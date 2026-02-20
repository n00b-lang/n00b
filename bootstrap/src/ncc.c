/**
 * @file ncc.c
 * @brief NCC entry point: argument parsing, preprocessor invocation, dispatch.
 *
 * Parses the command line, decides whether to pass through to the
 * underlying compiler or invoke the NCC pipeline, and provides
 * `ncc_invoke_preprocessor()` which forks clang with `-E` and
 * shuttles data through pipes using `poll()`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <sys/wait.h>

#include "ncc_limits.h"

static void
crash_handler(int sig)
{
    void *bt[NCC_BACKTRACE_DEPTH];
    int   n = backtrace(bt, NCC_BACKTRACE_DEPTH);
    fprintf(stderr, "\n=== SIGNAL %d ===\n", sig);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    _exit(1);
}

#include "argv_parse.h"
#include "compile.h"
#include "modernize.h"
#include "pipe_io.h"

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

    execvp(exe, ctx->argv);
    abort();
}

ncc_buf_t *
ncc_invoke_preprocessor(ncc_argv_t *ctx, char *compiler, ncc_buf_t *input)
{
    // We need space for original args + "-E" + "-fno-blocks" + null terminator
    int   nargs = ctx->argc + 3;
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

    // Add -fno-blocks to disable block syntax in headers
    preproc_argv[tail++] = "-fno-blocks";

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
    {
        char  line_marker[NCC_LINE_MARKER_BUF];
        int   lm_len = 0;

        if (!ctx->has_stdin && ctx->sources[0]) {
            lm_len = snprintf(line_marker, sizeof(line_marker),
                              "# 1 \"%s\"\n", ctx->sources[0]);
        }

        if (lm_len > 0) {
            ncc_buf_t *prefixed = ncc_buf_alloc(lm_len + input->len);
            memcpy(prefixed->data, line_marker, lm_len);
            memcpy(prefixed->data + lm_len, input->data, input->len);
            prefixed->len = lm_len + input->len;
            base_dealloc(input);
            input = prefixed;
        }
    }

    // Fork preprocessor: parent writes input to stdin, reads stdout.
    int pipe0[2]; // parent→child stdin
    int pipe1[2]; // child stdout→parent

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
        dup2(pipe0[0], STDIN_FILENO);
        close(pipe0[0]);
        dup2(pipe1[1], STDOUT_FILENO);
        execvp(compiler, preproc_argv);
        goto sys_err;
    }
    close(pipe0[0]);
    close(pipe1[1]);

    ncc_buf_t *preproc_output = ncc_pipe_io(pipe0[1], pipe1[0],
                                            input->data, input->len,
                                            preproc_argv[0]);

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

    // --modernize / --modernize-overflow: transform source, no compile
    if (ctx.has_modernize) {
        modernize_file(&ctx);
        exit(0);
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
