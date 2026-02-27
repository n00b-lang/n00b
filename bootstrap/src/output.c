/**
 * @file output.c
 * @brief Final pipeline stage: emit transformed C to the underlying compiler.
 *
 * Walks the transformed parse tree via `emit_tree()`, pipes the result
 * to the real compiler (`clang`), and generates Meson-compatible `.d`
 * dependency files when `-MD`/`-MF`/`-MQ` flags are present.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "compile.h"
#include "emit.h"
#include "pipe_io.h"

/**
 * @brief Get the output filename from command-line arguments.
 * @param argv Parsed command-line arguments
 * @return Output filename, or nullptr for stdout
 */
static char *
get_output_filename(ncc_argv_t *argv)
{
    if (argv->flag_o_index <= 0) {
        return nullptr;
    }

    if (argv->filename_in_same_arg) {
        return argv->argv[argv->flag_o_index] + 2; // skip "-o"
    }

    return argv->argv[argv->flag_o_index + 1];
}

/**
 * @brief Emit transformed code to a FILE stream.
 *
 * Bootstrap: if ctx->tree is non-null, walks the tnode_t tree via emit_tree().
 * Main NCC: ctx->tree is always nullptr (passthrough from slay_pprint output).
 */
static void
emit_to_stream(compile_ctx_t *ctx, FILE *out)
{
    if (ctx->tree == nullptr) {
        // Pass-through mode: write raw preprocessed input
        fwrite(ctx->lex_state.input->data, 1, ctx->lex_state.input->len, out);
        return;
    }

    emit_ctx_t emit;
    emit_init(&emit, &ctx->lex_state, out);
    emit_file_directive(&emit);
    emit_tree(&emit, ctx->tree);
    emit_finish(&emit);
}

/**
 * @brief Generate a proper dependency file from the original source.
 *
 * ncc preprocesses source before compiling, so the normal -MD/-MF/-MQ
 * flags produce empty dep files (clang sees preprocessed code via stdin
 * with no `#include` directives).  This runs a separate `-M` pass on the
 * original source to capture real header dependencies.
 *
 * @param ctx Compilation context (must have valid compiler and argv).
 */
static void
generate_depfile(compile_ctx_t *ctx)
{
    ncc_argv_t *argv = ctx->argv;

    if (!argv->has_dep_flags || !argv->dep_file || !argv->num_sources) {
        return;
    }

    int   max_args = argv->argc + 10;
    char *dep_argv[max_args];
    int   j = 0;

    dep_argv[j++] = ctx->compiler;
    dep_argv[j++] = "-M";
    dep_argv[j++] = "-MF";
    dep_argv[j++] = argv->dep_file;
    dep_argv[j++] = "-fno-blocks";

    if (argv->dep_target_q) {
        dep_argv[j++] = "-MQ";
        dep_argv[j++] = argv->dep_target_q;
    }

    if (argv->dep_target) {
        dep_argv[j++] = "-MT";
        dep_argv[j++] = argv->dep_target;
    }

    if (!argv->has_c23) {
        dep_argv[j++] = "-std=c23";
    }

    for (int i = 1; i < argv->argc; i++) {
        char *arg = argv->argv[i];

        // Skip the source file (added explicitly at the end).
        if (i == argv->source_indices[0]) {
            continue;
        }

        // Skip compilation/output/dep flags we handle ourselves.
        if (!strcmp(arg, "-c") || !strcmp(arg, "-MD") || !strcmp(arg, "-MMD")) {
            continue;
        }

        if (i == argv->flag_o_index) {
            if (!argv->filename_in_same_arg && i + 1 < argv->argc) {
                i++;
            }
            continue;
        }

        if (strncmp(arg, "-MF", 3) == 0) {
            if (strlen(arg) == 3 && i + 1 < argv->argc) {
                i++;
            }
            continue;
        }

        if (strncmp(arg, "-MQ", 3) == 0) {
            if (strlen(arg) == 3 && i + 1 < argv->argc) {
                i++;
            }
            continue;
        }

        if (strncmp(arg, "-MT", 3) == 0) {
            if (strlen(arg) == 3 && i + 1 < argv->argc) {
                i++;
            }
            continue;
        }

        dep_argv[j++] = arg;
    }

    // Use the real source file so the preprocessor sees actual #includes.
    dep_argv[j++] = argv->sources[0];
    dep_argv[j]   = nullptr;

    pid_t pid = fork();
    if (pid < 0) {
        // Non-fatal: deps won't be tracked but compilation proceeds.
        return;
    }

    if (pid == 0) {
        execvp(ctx->compiler, dep_argv);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
}

/**
 * @brief Invoke the compiler with transformed source on stdin.
 *
 * Builds a new argv that:
 * - Uses the compiler path
 * - Replaces the source file with "-x c -" (read C from stdin)
 * - Keeps all other flags (-c, -o, warning flags, etc.)
 * - Adds -o flag if not specified (to avoid creating -.o)
 *
 * Uses non-blocking I/O to avoid deadlock if the compiler stops
 * reading (e.g., on syntax error).
 */
static void
invoke_compiler(compile_ctx_t *ctx)
{
    ncc_argv_t *argv = ctx->argv;

    // Generate proper dependency file before compilation.
    // Must run first since the main compilation strips dep flags.
    generate_depfile(ctx);

    // Determine if we need to inject -std=c23:
    // - ncc implies C23 code, so always use C23
    // - This covers both ncc transformations and C23 features in the codebase
    // - Only inject if the caller didn't already specify a C23 compatible standard
    bool need_c23 = !argv->has_c23;

    // Check if we need to inject -o flag (when -c is used but no -o specified)
    // Since we're using stdin, the compiler would create "-.o" otherwise
    bool   need_output_flag = (argv->has_c && argv->flag_o_index <= 0);
    char  *output_name      = nullptr;
    size_t output_name_len  = 0;

    if (need_output_flag && argv->sources[0]) {
        // Compute output name: basename of source with .o extension
        const char *source = argv->sources[0];
        const char *slash  = strrchr(source, '/');
        const char *base   = slash ? slash + 1 : source;
        const char *dot    = strrchr(base, '.');
        size_t      baselen;
        if (dot && dot > base) {
            baselen = dot - base;
        }
        else {
            baselen = strlen(base);
        }
        output_name_len = baselen + 3; // ".o" + null
        output_name     = base_alloc(output_name_len);
        memcpy(output_name, base, baselen);
        memcpy(output_name + baselen, ".o", 3);
    }

    // Build new argv: compiler + original args with source replaced by "-x c -"
    // We need space for: original args + "-x" + "c" + possibly "-std=c23" + possibly "-o <file>"
    // + "-Wno-odr" (always added to suppress spurious ODR warnings from alignas)
    // The source file slot will become "-"
    int extra_args = 3; // -x c -Wno-odr
    if (need_c23) {
        extra_args += 1; // -std=c23
    }
    if (need_output_flag && output_name) {
        extra_args += 2; // -o <file>
    }
    int   nargs = argv->argc + extra_args;
    char *new_argv[nargs + 1];

    int j = 0;
    new_argv[j++] = ctx->compiler;

    // Suppress ODR warnings that fire on structs with alignas attributes
    // when #line directives point to the same source location
    new_argv[j++] = "-Wno-odr";

    // Inject -std=c23 only if ncc transformations require it
    if (need_c23) {
        new_argv[j++] = "-std=c23";
    }

    for (int i = 1; i < argv->argc; i++) {
        if (i == argv->source_indices[0]) {
            // Replace source file with "-x c -"
            new_argv[j++] = "-x";
            new_argv[j++] = "c";
            new_argv[j++] = "-";
            // Add -o flag if needed
            if (need_output_flag && output_name) {
                new_argv[j++] = "-o";
                new_argv[j++] = output_name;
            }
        }
        else {
            char *arg = argv->argv[i];
            // Skip -save-temps flags since we're feeding from stdin
            // and the compiler would try to create "-.i" files
            if (strcmp(arg, "-save-temps") == 0 || strncmp(arg, "-save-temps=", 12) == 0) {
                continue;
            }
            // Skip any -std= flags only if we're injecting -std=c23 for transforms.
            // This prevents original -std=c11 etc. from overriding our injected flag.
            // If no transforms, preserve the caller's -std flag.
            if (need_c23 && strncmp(arg, "-std=", 5) == 0) {
                continue;
            }
            // Skip dependency flags (already handled by generate_depfile).
            if (argv->has_dep_flags) {
                if (!strcmp(arg, "-MD") || !strcmp(arg, "-MMD")) {
                    continue;
                }
                if (strncmp(arg, "-MF", 3) == 0) {
                    if (strlen(arg) == 3 && i + 1 < argv->argc) {
                        i++;
                    }
                    continue;
                }
                if (strncmp(arg, "-MQ", 3) == 0) {
                    if (strlen(arg) == 3 && i + 1 < argv->argc) {
                        i++;
                    }
                    continue;
                }
                if (strncmp(arg, "-MT", 3) == 0) {
                    if (strlen(arg) == 3 && i + 1 < argv->argc) {
                        i++;
                    }
                    continue;
                }
            }
            new_argv[j++] = arg;
        }
    }
    new_argv[j] = nullptr;

    // First, emit transformed code to a memory buffer
    char  *buf     = nullptr;
    size_t buf_len = 0;
    FILE  *memfile = open_memstream(&buf, &buf_len);
    if (!memfile) {
        (void)fprintf(stderr, "%s: open_memstream: %s\n", argv->argv[0], strerror(errno));
        abort();
    }
    emit_to_stream(ctx, memfile);
    (void)fclose(memfile);

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        (void)fprintf(stderr, "%s: pipe: %s\n", argv->argv[0], strerror(errno));
        abort();
    }

    // Pipe to capture compiler stderr for post-processing.
    int err_pipe[2];
    if (pipe(err_pipe) < 0) {
        (void)fprintf(stderr, "%s: pipe: %s\n", argv->argv[0], strerror(errno));
        abort();
    }

    pid_t pid = fork();
    if (pid < 0) {
        (void)fprintf(stderr, "%s: fork: %s\n", argv->argv[0], strerror(errno));
        abort();
    }

    if (pid == 0) {
        // Child: read from pipe, exec compiler
        close(pipefd[1]);
        close(err_pipe[0]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        dup2(err_pipe[1], STDERR_FILENO);
        close(err_pipe[1]);
        execvp(ctx->compiler, new_argv);
        // If exec fails, write to the error pipe (our stderr is the pipe now)
        dprintf(STDERR_FILENO, "%s: exec %s: %s\n",
                argv->argv[0], ctx->compiler, strerror(errno));
        _exit(127);
    }

    // Parent: write buffered code to pipe, no read-back needed
    close(pipefd[0]);
    close(err_pipe[1]);
    ncc_pipe_io(pipefd[1], -1, buf, buf_len, argv->argv[0]);
    base_dealloc(buf);

    // Read captured compiler stderr
    char  *captured_err = nullptr;
    size_t err_size;
    FILE  *ef = open_memstream(&captured_err, &err_size);
    if (ef) {
        char    ebuf[4096];
        ssize_t n;
        while ((n = read(err_pipe[0], ebuf, sizeof(ebuf))) > 0) {
            fwrite(ebuf, 1, n, ef);
        }
        fclose(ef);
    }
    close(err_pipe[0]);

    // Wait for compiler to finish
    int status;
    waitpid(pid, &status, 0);

    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    if (exit_code != 0 && captured_err && *captured_err) {
        const char *source = argv->has_stdin
            ? "<stdin>" : (argv->sources[0] ? argv->sources[0] : "<unknown>");

        // Check for common patterns and add guidance
        if (strstr(captured_err, "redefinition of")) {
            fprintf(stderr,
                    "ncc: note: 'redefinition' errors usually mean a type "
                    "is defined in multiple included headers.\n"
                    "ncc: note: use 'ncc -E' to inspect the "
                    "transformed output.\n");
        }

        // Print clang's errors, replacing <stdin> with the source filename
        char *p = captured_err;
        while (*p) {
            if (strncmp(p, "<stdin>", 7) == 0) {
                fputs(source, stderr);
                p += 7;
            }
            else {
                fputc(*p, stderr);
                p++;
            }
        }
        // Ensure final newline
        if (err_size > 0 && captured_err[err_size - 1] != '\n') {
            fputc('\n', stderr);
        }
    }
    else if (exit_code != 0 && captured_err) {
        // Empty stderr but non-zero exit — just report
        fputs(captured_err, stderr);
    }

    base_dealloc(captured_err);
    exit(exit_code);
}

void
final_output(compile_ctx_t *ctx)
{
    ncc_argv_t *argv = ctx->argv;

    // If -c was specified, we need to invoke the compiler with
    // the transformed code
    if (argv->has_c) {
        invoke_compiler(ctx);
        // invoke_compiler does not return
    }

    // Otherwise (-E mode), write to stdout or specified output file
    char *outfile = get_output_filename(argv);
    FILE *out     = stdout;

    if (outfile) {
        out = fopen(outfile, "w");
        if (!out) {
            (void)fprintf(stderr,
                          "%s: %s: %s\n",
                          argv->argv[0],
                          outfile,
                          strerror(errno));
            abort();
        }
    }

    emit_to_stream(ctx, out);

    if (out != stdout) {
        (void)fclose(out);
    }
}
