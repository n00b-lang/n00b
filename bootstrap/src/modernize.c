/**
 * @file modernize.c
 * @brief `--modernize` pipeline: upgrade C11/C17 source to C23.
 *
 * Orchestrates a two-phase transformation:
 *   - **Phase A** (`mod_token_xforms.c`): token-level rewrites on original source
 *   - **Phase B** (`mod_tree_xforms.c`): tree-level rewrites on preprocessed source
 *
 * After transforms, reconstructs the source text and optionally pipes it
 * through `clang-format`.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "ncc_limits.h"
#include "modernize.h"
#include "buf.h"
#include "lex.h"
#include "pipe_io.h"
#include "token.h"

// Phase A token transforms (from mod_token_xforms.c)
extern void mod_token_xforms(lex_t *state, int *insert_after_ix, char **header_text,
                             bool conservative_overflow, const modernize_skip_t *skip);

// Phase B tree transforms (from mod_tree_xforms.c)
extern int mod_tree_xforms(ncc_argv_t *ctx, ncc_buf_t *source, ncc_buf_t **result);

/**
 * @brief Reconstruct source text from a modified token array.
 *
 * Walks the token array and emits text for each token:
 * - skip_emit tokens are skipped
 * - tokens with replacement use replacement text
 * - normal tokens use original source text
 * Whitespace, comments, and preprocessor directives are preserved as-is.
 *
 * If header_text is non-null, it is inserted after the token at insert_after_ix
 * (or at the beginning if insert_after_ix == -1).
 */
static ncc_buf_t *
reconstruct_source(lex_t *state, int insert_after_ix, const char *header_text)
{
    ncc_buf_t *out = ncc_buf_alloc(state->input->len + 256);

    // Insert headers at the beginning if needed
    if (header_text && insert_after_ix == -1) {
        out = ncc_buf_concat(out, (char *)header_text, strlen(header_text));
    }

    for (int i = 0; i < state->num_toks; i++) {
        tok_t *tok = &state->toks[i];

        if (tok->skip_emit) {
            continue;
        }

        if (tok->replacement) {
            out = ncc_buf_concat(out, tok->replacement->data, tok->replacement->len);
        }
        else {
            out = ncc_buf_concat(out, state->input->data + tok->offset, tok->len);
        }

        // Insert headers after this token
        if (header_text && i == insert_after_ix) {
            out = ncc_buf_concat(out, (char *)header_text, strlen(header_text));
        }
    }

    return out;
}

/**
 * @brief Pipe source through clang-format.
 */
static ncc_buf_t *
run_clang_format(ncc_buf_t *input, const char *source_file)
{
    int pipe_in[2];
    int pipe_out[2];

    if (pipe(pipe_in) || pipe(pipe_out)) {
        fprintf(stderr, "modernize: pipe: %s\n", strerror(errno));
        return input; // Fall back to unformatted
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "modernize: fork: %s\n", strerror(errno));
        close(pipe_in[0]);
        close(pipe_in[1]);
        close(pipe_out[0]);
        close(pipe_out[1]);
        return input;
    }

    if (pid == 0) {
        close(pipe_in[1]);
        close(pipe_out[0]);
        dup2(pipe_in[0], STDIN_FILENO);
        close(pipe_in[0]);
        dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_out[1]);

        const char *style_env = getenv("NCC_CLANG_FORMAT_STYLE");
        if (style_env) {
            char style_arg[NCC_ATTR_BUF];
            snprintf(style_arg, sizeof(style_arg), "--style=%s", style_env);
            execlp("clang-format", "clang-format", style_arg, (char *)nullptr);
        }
        else if (source_file) {
            execlp("clang-format", "clang-format",
                   "--assume-filename", source_file, (char *)nullptr);
        }
        else {
            execlp("clang-format", "clang-format", "--style=file",
                   (char *)nullptr);
        }
        // If clang-format not found, just cat stdin to stdout
        execlp("cat", "cat", (char *)nullptr);
        _exit(127);
    }

    close(pipe_in[0]);
    close(pipe_out[1]);

    ncc_buf_t *result = ncc_pipe_io(pipe_in[1], pipe_out[0],
                                    input->data, input->len,
                                    "clang-format");

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        fprintf(stderr, "modernize: clang-format exited with status %d\n",
                WEXITSTATUS(status));
        base_dealloc(result);
        return input;
    }

    base_dealloc(input);
    return result;
}

void
modernize_parse_skip(modernize_skip_t *skip)
{
    *skip = (modernize_skip_t){};

    const char *env = getenv("NCC_MODERNIZE_SKIP");
    if (!env || !*env) {
        return;
    }

    // Work on a copy since we tokenize with strtok-like logic
    char *buf = base_strdup(env);
    char *p   = buf;

    while (*p) {
        // Skip leading whitespace and commas
        while (*p == ',' || *p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }

        // Find end of this name
        char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') {
            p++;
        }

        char saved = *p;
        *p = '\0';

        if (strcmp(start, "keywords") == 0) {
            skip->skip_keywords = true;
        }
        else if (strcmp(start, "includes") == 0) {
            skip->skip_includes = true;
        }
        else if (strcmp(start, "elifdef") == 0) {
            skip->skip_elifdef = true;
        }
        else if (strcmp(start, "attributes") == 0) {
            skip->skip_attributes = true;
        }
        else if (strcmp(start, "builtins") == 0) {
            skip->skip_builtins = true;
        }
        else if (strcmp(start, "empty-init") == 0) {
            skip->skip_empty_init = true;
        }
        else if (strcmp(start, "va-paste") == 0) {
            skip->skip_va_paste = true;
        }
        else if (strcmp(start, "va-start") == 0) {
            skip->skip_va_start = true;
        }
        else if (strcmp(start, "overflow") == 0) {
            skip->skip_overflow = true;
        }
        else if (strcmp(start, "nullptr") == 0) {
            skip->skip_nullptr = true;
        }
        else if (strcmp(start, "pragma-once") == 0) {
            skip->skip_pragma_once = true;
        }
        else {
            fprintf(stderr, "modernize: unknown skip group '%s' "
                    "(valid: keywords, includes, elifdef, attributes, builtins, "
                    "empty-init, va-paste, va-start, overflow, nullptr, "
                    "pragma-once)\n", start);
        }

        if (saved) {
            *p = saved;
        }
    }

    base_dealloc(buf);
}

/**
 * @brief Get the output filename from -o flag.
 */
static char *
get_output_filename(ncc_argv_t *ctx)
{
    if (ctx->flag_o_index <= 0) {
        return nullptr;
    }
    if (ctx->filename_in_same_arg) {
        return ctx->argv[ctx->flag_o_index] + 2;
    }
    return ctx->argv[ctx->flag_o_index + 1];
}

void
modernize_file(ncc_argv_t *ctx)
{
    if (ctx->num_sources < 1 && !ctx->has_stdin) {
        fprintf(stderr, "modernize: no source file specified\n");
        exit(1);
    }

    // Read input
    ncc_buf_t *input;

    if (ctx->has_stdin) {
        input = ncc_buf_read_stream(stdin);
    }
    else {
        input = ncc_buf_read_file_by_name(ctx->sources[0]);
        if (!input) {
            fprintf(stderr, "modernize: cannot open %s: %s\n",
                    ctx->sources[0], strerror(errno));
            exit(1);
        }
    }

    // Parse skip flags from NCC_MODERNIZE_SKIP env var
    modernize_skip_t skip;
    modernize_parse_skip(&skip);

    // Phase A: Pre-CPP token transforms
    lex_t lex_state;
    lex_init(&lex_state, input, ctx->has_stdin ? "«stdin»" : ctx->sources[0]);
    lex(&lex_state);

    int   insert_after_ix = -1;
    char *header_text     = nullptr;
    mod_token_xforms(&lex_state, &insert_after_ix, &header_text,
                     ctx->has_modernize_overflow, &skip);

    ncc_buf_t *source = reconstruct_source(&lex_state, insert_after_ix, header_text);
    base_dealloc(header_text);

    // Phase B: Post-CPP tree transforms (NULL -> nullptr)
    ncc_buf_t *phase_b_result = nullptr;
    int        tree_changes   = 0;
    if (!skip.skip_nullptr) {
        tree_changes = mod_tree_xforms(ctx, source, &phase_b_result);
    }

    if (tree_changes > 0 && phase_b_result) {
        base_dealloc(source);
        source = phase_b_result;
    }
    else {
        base_dealloc(phase_b_result);
    }

    // Phase C: clang-format
    source = run_clang_format(source, ctx->has_stdin ? nullptr : ctx->sources[0]);

    // Write output
    char *outfile = get_output_filename(ctx);
    FILE *out     = stdout;

    if (outfile) {
        out = fopen(outfile, "w");
        if (!out) {
            fprintf(stderr, "modernize: %s: %s\n", outfile, strerror(errno));
            exit(1);
        }
    }

    fwrite(source->data, 1, source->len, out);

    if (out != stdout) {
        fclose(out);
    }

    base_dealloc(source);
}
