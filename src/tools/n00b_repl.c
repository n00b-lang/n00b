// n00b_repl.c — Interactive REPL for the n00b language.
//
// Parses n00b expressions/statements, runs annotation walk, generates
// MIR via the codegen session API, JIT-compiles, executes, and prints results.
//
// Uses a persistent n00b_cg_session_t so functions defined in one expression
// are visible in subsequent expressions.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include "parsers/scanner.h"
#include "parsers/token_stream.h"
#include "slay/annot_walk.h"
#include "n00b/n00b_compile.h"
#include "n00b/n00b_tokenizer.h"
#include "n00b/n00b_module_loader.h"
#include "slay/codegen.h"
#include "slay/diagnostic.h"
#include "n00b/n00b_type_map.h"
#include "slay/grammar.h"
#include "slay/n00b_parse.h"
#include "slay/parse_tree.h"
#include "slay/token.h"
#include "vendor/linenoise.h"

// ============================================================================
// REPL state
// ============================================================================

typedef struct {
    n00b_grammar_t      *grammar;
    n00b_cg_session_t   *session;
    char                *input_buf;
    size_t               input_len;
    size_t               input_cap;
    int                  eval_count;
} n00b_repl_state_t;

// ============================================================================
// Parse-attempt result
// ============================================================================

typedef enum {
    REPL_PARSE_OK,
    REPL_PARSE_INCOMPLETE,
    REPL_PARSE_ERROR,
} repl_parse_status_t;

// ============================================================================
// Input buffer management
// ============================================================================

static void
repl_buf_reset(n00b_repl_state_t *state)
{
    state->input_len = 0;

    if (state->input_buf) {
        state->input_buf[0] = '\0';
    }
}

static void
repl_buf_append(n00b_repl_state_t *state, const char *line)
{
    size_t line_len = strlen(line);
    size_t need     = state->input_len + line_len + 2; // +newline +nul

    if (need > state->input_cap) {
        size_t new_cap = need * 2;

        if (new_cap < 1024) {
            new_cap = 1024;
        }

        state->input_buf = realloc(state->input_buf, new_cap);
        state->input_cap = new_cap;
    }

    // Append newline between lines if we already have content.
    if (state->input_len > 0) {
        state->input_buf[state->input_len++] = '\n';
    }

    memcpy(state->input_buf + state->input_len, line, line_len);
    state->input_len += line_len;
    state->input_buf[state->input_len] = '\0';
}

// ============================================================================
// Parse attempt with incomplete detection
// ============================================================================

static repl_parse_status_t
try_parse(n00b_repl_state_t     *state,
          const char            *input,
          n00b_parse_result_t  **out)
{
    n00b_buffer_t       *buf = n00b_buffer_from_bytes((char *)input,
                                                       (int64_t)strlen(input));
    n00b_scanner_t      *s   = n00b_scanner_new(buf, n00b_lang_tokenize,
                                                  state->grammar);
    n00b_token_stream_t *ts  = n00b_token_stream_new(s);

    n00b_parse_result_t *r = n00b_grammar_parse(state->grammar, ts,
                                                 N00B_PARSE_MODE_DEFAULT);

    if (n00b_parse_result_ok(r)) {
        *out = r;
        return REPL_PARSE_OK;
    }

    // Check if incomplete: parser hit EOF but expected more tokens.
    n00b_error_location_t loc = n00b_parse_result_error_location(r);

    if (loc.got_id == N00B_TOK_EOF) {
        n00b_parse_result_free(r);
        *out = NULL;
        return REPL_PARSE_INCOMPLETE;
    }

    // Real syntax error at a non-EOF token.
    *out = r;
    return REPL_PARSE_ERROR;
}

// ============================================================================
// Codegen + JIT + execute (uses persistent session)
// ============================================================================

static void
generate_and_run(n00b_repl_state_t   *state,
                 n00b_parse_tree_t   *tree,
                 n00b_annot_result_t *annot)
{
    bool    ok  = false;
    int64_t val = n00b_cg_session_eval_tree(state->session, tree,
                                              .annot = annot, .ok = &ok);

    if (ok) {
        printf("=> %lld\n", (long long)val);
    }
    else {
        fprintf(stderr, "error: codegen/JIT failed\n");
    }
}

// ============================================================================
// REPL commands
// ============================================================================

static bool
dispatch_command(const char *line)
{
    // Skip leading whitespace after ':'.
    while (*line == ' ' || *line == '\t') {
        line++;
    }

    if (strcmp(line, "quit") == 0 || strcmp(line, "q") == 0) {
        return true; // Signal exit.
    }

    if (strcmp(line, "clear") == 0 || strcmp(line, "c") == 0) {
        linenoiseClearScreen();
        return false;
    }

    if (strcmp(line, "help") == 0 || strcmp(line, "h") == 0) {
        printf("n00b REPL commands:\n");
        printf("  :quit, :q     Exit the REPL\n");
        printf("  :clear, :c    Clear the screen\n");
        printf("  :help, :h     Show this help\n");
        printf("\n");
        printf("Enter n00b expressions to evaluate them.\n");
        printf("Multi-line input: incomplete expressions auto-continue.\n");
        printf("Press Ctrl-D to exit.\n");
        return false;
    }

    fprintf(stderr, "Unknown command: :%s\n", line);
    fprintf(stderr, "Type :help for available commands.\n");
    return false;
}

// ============================================================================
// Main REPL loop
// ============================================================================

int
n00b_repl_run(n00b_grammar_t *grammar)
{
    n00b_repl_state_t state = {
        .grammar    = grammar,
        .session    = n00b_cg_session_new(grammar, .type_map = n00b_type_map),
        .input_buf  = NULL,
        .input_len  = 0,
        .input_cap  = 0,
        .eval_count = 0,
    };

    linenoiseSetMultiLine(1);
    linenoiseHistorySetMaxLen(100);

    printf("n00b REPL (type :help for commands, Ctrl-D to exit)\n");

    const char *prompt = "n00b> ";

    while (true) {
        char *line = linenoise(prompt);

        if (!line) {
            // Ctrl-D or EOF.
            printf("\n");
            break;
        }

        // Skip empty lines when not in continuation.
        if (line[0] == '\0' && state.input_len == 0) {
            linenoiseFree(line);
            continue;
        }

        // REPL commands (only at top level, not in continuation).
        if (line[0] == ':' && state.input_len == 0) {
            bool should_quit = dispatch_command(line + 1);
            linenoiseFree(line);

            if (should_quit) {
                break;
            }

            continue;
        }

        // Append to input buffer.
        repl_buf_append(&state, line);
        linenoiseFree(line);

        // Try to parse.
        n00b_parse_result_t *parse_result = NULL;
        repl_parse_status_t  status = try_parse(&state, state.input_buf,
                                                 &parse_result);

        switch (status) {
        case REPL_PARSE_INCOMPLETE:
            // Need more input.
            prompt = "  ... ";
            continue;

        case REPL_PARSE_ERROR: {
            // Print error.
            n00b_string_t *err = n00b_parse_result_error_string(parse_result);
            fprintf(stderr, "Parse error: %.*s\n",
                    (int)err->u8_bytes, err->data);

            n00b_string_t *expected = n00b_parse_result_expected_string(
                parse_result);

            if (expected->u8_bytes > 0) {
                fprintf(stderr, "Expected: %.*s\n",
                        (int)expected->u8_bytes, expected->data);
            }

            n00b_parse_result_free(parse_result);
            repl_buf_reset(&state);
            prompt = "n00b> ";
            continue;
        }

        case REPL_PARSE_OK:
            break;
        }

        // Add to history.
        linenoiseHistoryAdd(state.input_buf);

        // Get parse tree.
        n00b_parse_tree_t *tree = n00b_parse_result_tree(parse_result);

        // Annotation walk.
        n00b_annot_result_t *annot = n00b_compile_walk(grammar, tree);

        if (!annot) {
            fprintf(stderr, "error: annotation walk failed\n");
            n00b_parse_result_free(parse_result);
            repl_buf_reset(&state);
            prompt = "n00b> ";
            continue;
        }

        // Resolve use statements (load imported modules).
        n00b_resolve_use_stmts(state.session, grammar, tree, annot);

        // Codegen + JIT + execute (reuses persistent session).
        generate_and_run(&state, tree, annot);

        // Cleanup.
        if (annot->symtab) {
            n00b_symtab_free(annot->symtab);
        }

        n00b_parse_result_free(parse_result);
        repl_buf_reset(&state);
        prompt = "n00b> ";
    }

    n00b_cg_session_free(state.session);
    free(state.input_buf);

    return 0;
}
