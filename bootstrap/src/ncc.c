#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>

static void
crash_handler(int sig)
{
    void *bt[200];
    int   n = backtrace(bt, 200);
    fprintf(stderr, "\n=== SIGNAL %d ===\n", sig);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    _exit(1);
}

#include "argv_parse.h"
#include "compile.h"
#include "modernize.h"

#ifdef NCC_HAS_TRASHHEAP
extern void th_init(void);
#else
#define th_init()
#endif

static void print_ncc_help(void);

static inline void
signal_setup(void)
{
    static char        *altstack = NULL;
    static const size_t alt_sz   = 64 * 1024;
    static stack_t      ss;
    struct sigaction    sa = {.sa_handler = crash_handler, .sa_flags = SA_ONSTACK};

    if (altstack == NULL) {
        altstack = malloc(alt_sz);
        if (altstack == NULL) {
            abort();
        }
        ss = (stack_t){.ss_sp = altstack, .ss_size = alt_sz};
    }

    sigaltstack(&ss, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

[[noreturn]] void
compiler_passthrough(ncc_argv_t *ctx)
{
    char *exe = ncc_find_compiler();

    ncc_exec_compiler(exe, ctx->argv);
    abort();
}

ncc_buf_t *
ncc_invoke_preprocessor(ncc_argv_t *ctx, char *compiler, ncc_buf_t *input)
{
    bool use_no_blocks = ncc_compiler_supports_no_blocks(compiler);

    // We need space for original args + "-E" + optional "-fno-blocks" + null terminator
    int   nargs = ctx->argc + 2 + (use_no_blocks ? 1 : 0);
    char *preproc_argv[nargs + 1];

    for (int i = 0; i < nargs + 1; i++) {
        preproc_argv[i] = nullptr;
    }

    // Copy original args
    memcpy(preproc_argv, ctx->argv, sizeof(char *) * ctx->argc);
    int tail = ctx->argc;

    if (ctx->has_c && ctx->flag_c_index > 0) {
        // Explicit -c in argv: replace it with -E
        preproc_argv[ctx->flag_c_index] = (char *)"-E";
    }
    else if (ctx->e_count == 0) {
        // No -E in original argv (implicit mode or --dump-tokens): append -E
        preproc_argv[tail++] = (char *)"-E";
    }

    // Add -fno-blocks to disable block syntax in headers (clang only)
    if (use_no_blocks) {
        preproc_argv[tail++] = "-fno-blocks";
    }

    int outspec_index = ctx->source_indices[0];

    if (ctx->flag_o_index > 0) {
        // We need to erase -ofoo or -o foo, if provided (we want
        // output on stdout).
        // If filename is in same arg (-ofoo), skip 1 arg; otherwise skip 2 (-o foo)
        int    args_to_skip  = ctx->filename_in_same_arg ? 1 : 2;
        int    num_to_move   = nargs - ctx->flag_o_index - args_to_skip;
        int    bytes_to_move = num_to_move * sizeof(char *);
        char **start_of_flag = &preproc_argv[ctx->flag_o_index];
        char **move_start    = &preproc_argv[ctx->flag_o_index + args_to_skip];

        memmove(start_of_flag, move_start, bytes_to_move);

        assert(nargs > 0);
        // Clear the now-unused slots at the end
        for (int j = 0; j < args_to_skip; j++) {
            preproc_argv[nargs - 1 - j] = (char *)nullptr;
        }

        // Adjust source index if it was after the -o flag we just removed
        if (outspec_index > ctx->flag_o_index) {
            outspec_index -= args_to_skip;
        }
    }

    // Always use stdin for the preprocessor so we can pass our wrapped input.
    preproc_argv[outspec_index] = "-";

    // Prepend a line marker so the CPP attributes its output to the original
    // source file instead of "<stdin>".
    if (!ctx->has_stdin && ctx->sources[0]) {
        char line_marker[4096];
        int  n = snprintf(line_marker, sizeof(line_marker), "# 1 \"%s\"\n", ctx->sources[0]);
        ncc_buf_t *prefixed = ncc_buf_alloc(n + input->len);
        memcpy(prefixed->data, line_marker, n);
        memcpy(prefixed->data + n, input->data, input->len);
        prefixed->len = n + input->len;
        base_dealloc(input);
        input = prefixed;
    }

    // The number is the child's fd; the parent should write to pipe0,
    // and read from pipe1.
    int pipe0[2];
    int pipe1[2];

    if (pipe(pipe0) || pipe(pipe1)) {
sys_err:
        fprintf(stderr, "%s: %s\n", preproc_argv[0], strerror(errno));
        abort();
    }

    pid_t pid = fork();

    if (pid < 0) {
        goto sys_err;
    }

    if (!pid) {
        close(pipe0[1]);
        close(pipe1[0]);
        // Always redirect stdin - we pass our wrapped input through stdin
        dup2(pipe0[0], STDIN_FILENO);
        close(pipe0[0]);
        dup2(pipe1[1], STDOUT_FILENO);
        ncc_exec_compiler(compiler, preproc_argv);
        goto sys_err;
    }
    close(pipe0[0]);
    close(pipe1[1]);

    // Ignore SIGPIPE so we can handle write errors ourselves
    signal(SIGPIPE, SIG_IGN);

    ncc_buf_t *preproc_output = ncc_buf_alloc(0);

    // Always write our (possibly wrapped) input to the preprocessor's stdin
    {
        // Set both pipe ends to non-blocking to avoid deadlock with large files
        int flags0 = fcntl(pipe0[1], F_GETFL, 0);
        int flags1 = fcntl(pipe1[0], F_GETFL, 0);
        fcntl(pipe0[1], F_SETFL, flags0 | O_NONBLOCK);
        fcntl(pipe1[0], F_SETFL, flags1 | O_NONBLOCK);

        // Use poll() to handle concurrent read/write to avoid deadlock
        // when both input and output exceed pipe buffer sizes
        const char *write_ptr    = (const char *)input->data;
        size_t      write_remain = input->len;

        struct pollfd fds[2];
        fds[0].fd     = pipe0[1]; // write to child stdin
        fds[0].events = POLLOUT;
        fds[1].fd     = pipe1[0]; // read from child stdout
        fds[1].events = POLLIN;

        int write_fd_open = 1;
        int read_fd_open  = 1;

        while (write_fd_open || read_fd_open) {
            fds[0].events  = write_fd_open ? POLLOUT : 0;
            fds[1].events  = read_fd_open ? POLLIN : 0;
            fds[0].revents = 0;
            fds[1].revents = 0;

            int ret = poll(fds, 2, -1);
            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                }
                fprintf(stderr, "%s: poll error: %s\n", preproc_argv[0], strerror(errno));
                abort();
            }

            // Handle writes to child stdin
            if (write_fd_open && (fds[0].revents & (POLLOUT | POLLERR | POLLHUP))) {
                if (write_remain > 0) {
                    ssize_t written = write(pipe0[1], write_ptr, write_remain);
                    if (written > 0) {
                        write_ptr += written;
                        write_remain -= written;
                    }
                    else if (written < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // Try again later
                        }
                        else if (errno == EPIPE) {
                            // Child closed stdin, stop writing
                            close(pipe0[1]);
                            write_fd_open = 0;
                        }
                        else {
                            fprintf(stderr,
                                    "%s: write error: %s\n",
                                    preproc_argv[0],
                                    strerror(errno));
                            abort();
                        }
                    }
                }
                if (write_fd_open && write_remain == 0) {
                    close(pipe0[1]); // Signal EOF to child
                    write_fd_open = 0;
                }
            }

            // Handle reads from child stdout
            if (read_fd_open && (fds[1].revents & (POLLIN | POLLERR | POLLHUP))) {
                char    read_buf[8192];
                ssize_t n = read(pipe1[0], read_buf, sizeof(read_buf));
                if (n > 0) {
                    preproc_output = ncc_buf_concat(preproc_output, read_buf, n);
                }
                else if (n == 0) {
                    close(pipe1[0]);
                    read_fd_open = 0;
                }
                else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    fprintf(stderr, "%s: read error: %s\n", preproc_argv[0], strerror(errno));
                    abort();
                }
            }
        }
    }

    // Wait for child to finish
    int status;
    waitpid(pid, &status, 0);

    // Check if preprocessor failed (e.g., due to #error directive)
    // This is critical for CMake feature detection tests
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        exit(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        exit(128 + WTERMSIG(status));
    }

    return preproc_output;
}

int
main(int argc, char *argv[], [[maybe_unused]] char *envp[])
{
    signal_setup();

    th_init();

    ncc_argv_t ctx;

    ncc_argv_parse(&ctx, argc, argv);

    // --ncc-help: print help and exit
    if (ctx.has_help) {
        print_ncc_help();
        exit(0);
    }

    // --no-ncc: explicit opt-out, passthrough to compiler
    if (ctx.has_no_ncc) {
        compiler_passthrough(&ctx);
    }

    // --dump-tokens implies -E (need to preprocess+lex, but not compile)
    if (ctx.has_dump_tokens && !ctx.has_E && !ctx.has_c) {
#ifndef NCC_BOOTSTRAP
        // Main NCC uses dragon_slay's tokenizer, not NCC's lex.c.
        // --dump-tokens is only supported in bootstrap mode.
        (void)fprintf(stderr, "ncc: --dump-tokens is only supported in ncc-bootstrap\n");
        exit(1);
#endif
        ctx.has_E        = true;
        ctx.implicit_cmd = true;
    }

    int effective_sources = ctx.num_sources + (ctx.has_stdin ? 1 : 0);

    // Passthrough: flags that produce non-C output
    if (ctx.passthrough_only) {
        compiler_passthrough(&ctx);
    }

    // Multiple sources or zero sources = linking or unknown command
    if (effective_sources != 1) {
        compiler_passthrough(&ctx);
    }

    // Exactly one source. If no -c and no -E, treat as -c (compile).
    if (!ctx.has_E && !ctx.has_c) {
        ctx.has_c        = true;
        ctx.implicit_cmd = true;
    }

    if (ctx.has_E && ctx.has_c) {
        fprintf(stderr, "%s:warning: passthrough: -E and -c both invoked.\n", argv[0]);
        compiler_passthrough(&ctx);
    }

    compile_file(&ctx);
}

static void
print_ncc_help(void)
{
    fprintf(stdout,
            "NCC - the N00b C Compiler wrapper\n"
            "\n"
            "NCC adds language extensions to C23. It sits between your source code\n"
            "and your real C compiler, transforming NCC extensions into standard C.\n"
            "\n"
            "Usage:\n"
            "  ncc file.c -o file.o                 Compile a single source file\n"
            "  ncc -c file.c -o file.o              Compile (explicit -c)\n"
            "  ncc -E file.c                        Preprocess with NCC transforms\n"
            "  ncc -E -E file.c                     Stop after C preprocessor (raw CPP)\n"
            "  ncc --dump-tokens file.c             Dump token list after lexing\n"
            "  ncc --no-ncc [options] ...            Disable NCC, pure passthrough\n"
            "\n"
            "NCC processes a single source file by default. With one source file\n"
            "and no -c or -E, ncc infers -c (compile). Multiple sources or no\n"
            "sources are passed through to the underlying compiler (linking etc.).\n"
            "\n"
            "Options:\n"
            "  -E             Preprocess and show NCC-transformed output\n"
            "  -E -E          Stop after C preprocessor, before NCC lex/transform\n"
            "  --dump-tokens  Dump token list after lexing + prefix transforms\n"
            "  --no-ncc       Disable NCC processing (pure compiler passthrough)\n"
            "  --modernize    Modernize C source to C23 (output source, no compile)\n"
            "  --modernize-overflow  Like --modernize, but emit comments for overflow\n"
            "                        check rewrites instead of auto-rewriting\n"
            "  --ncc-help     Show this help and exit\n"
            "\n"
            "Environment variables:\n"
            "  NCC_COMPILER   Override the underlying C compiler (checked before CC)\n"
            "  CC             Standard compiler selection (checked after NCC_COMPILER)\n"
            "  NCC_EXTENSIONS Override file extensions to process (e.g., .c.nc.h)\n"
            "  NCC_PACKAGE_MAP  Remap package prefixes (e.g., conduit=n00b,io=n00b_io)\n"
            "  NCC_MODERNIZE_SKIP  Comma-separated list of transform groups to skip\n"
            "                      (keywords,includes,elifdef,attributes,builtins,\n"
            "                       empty-init,va-paste,va-start,overflow,nullptr,\n"
            "                       pragma-once)\n"
            "  NCC_CLANG_FORMAT_STYLE  Override clang-format style for --modernize\n"
            "                          (e.g., llvm, google, file:/path/to/.clang-format)\n"
            "\n"
            "Language extensions: keyword arguments, variadic arguments, typeid(),\n"
            "typestr(), package namespacing, once functions, error propagation (!),\n"
            "literal modifiers, compile-time evaluation.\n"
            "\n"
            "For full documentation, see docs/transformation_semantics.md\n",
            stdout);
}
