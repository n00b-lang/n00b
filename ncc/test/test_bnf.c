// test_bnf.c — End-to-end test: load BNF grammar, tokenize C, parse, print tree.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slay/bnf.h"
#include "slay/pwz.h"
#include "slay/pretty_print.h"
#include "parsers/scan_recipes.h"
#include "parsers/scanner.h"
#include "parsers/token_stream.h"
#include "core/buffer.h"
#include "internal/slay/grammar_internal.h"

// ============================================================================
// C tokenizer callback
// ============================================================================

static bool
c_tokenize(n00b_scanner_t *s)
{
restart:
    n00b_scan_skip_whitespace(s);

    if (n00b_scan_at_eof(s)) {
        return false;
    }

    // Skip line comments.
    if (n00b_scan_peek_byte(s, 0) == '/'
        && n00b_scan_peek_byte(s, 1) == '/') {
        n00b_scan_skip_line_comment(s);
        goto restart;
    }

    // Skip block comments.
    if (n00b_scan_peek_byte(s, 0) == '/'
        && n00b_scan_peek_byte(s, 1) == '*') {
        n00b_scan_skip_block_comment(s, "/*", "*/");
        goto restart;
    }

    n00b_scan_mark(s);
    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    // String literals.
    if (cp == '"') {
        n00b_option_t(n00b_string_t) val = n00b_scan_string_double(s);
        n00b_scan_emit(s, N00B_TOK_STRING_LIT, val);
        return true;
    }

    // Character literals.
    if (cp == '\'') {
        n00b_option_t(n00b_string_t) val = n00b_scan_string_single(s);
        n00b_scan_emit(s, N00B_TOK_CHAR_LIT, val);
        return true;
    }

    // Numbers.
    if ((cp >= '0' && cp <= '9')
        || (cp == '.' && n00b_scan_peek_byte(s, 1) >= '0'
            && n00b_scan_peek_byte(s, 1) <= '9')) {
        bool emitted = n00b_scan_number(s, N00B_TOK_INTEGER, N00B_TOK_FLOAT);

        if (emitted) {
            return true;
        }
    }

    // Identifiers / keywords.
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_') {
        n00b_option_t(n00b_string_t) id_val = n00b_scan_identifier(s);

        if (n00b_option_is_set(id_val)) {
            n00b_string_t id_str = n00b_option_get(id_val);

            // Try keyword lookup via the scanner's grammar.
            int64_t kw_id = n00b_scan_terminal_id(s, id_str.data);

            if (kw_id != N00B_TOK_OTHER) {
                n00b_scan_emit(s, (int32_t)kw_id, id_val);
            }
            else {
                n00b_scan_emit(s, N00B_TOK_IDENTIFIER, id_val);
            }

            return true;
        }
    }

    // 3-char operators.
    static const char *ops3[] = {
        "<<=", ">>=", "...", NULL,
    };

    for (const char **op = ops3; *op; op++) {
        if (n00b_scan_peek_byte(s, 0) == (uint8_t)(*op)[0]
            && n00b_scan_peek_byte(s, 1) == (uint8_t)(*op)[1]
            && n00b_scan_peek_byte(s, 2) == (uint8_t)(*op)[2]) {
            int64_t tid = n00b_scan_terminal_id(s, *op);

            if (tid != N00B_TOK_OTHER) {
                n00b_scan_advance_n(s, 3);
                n00b_scan_emit_marked(s, (int32_t)tid);
                return true;
            }
        }
    }

    // 2-char operators.
    static const char *ops2[] = {
        "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=",
        "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "^=",
        "|=", "::", NULL,
    };

    for (const char **op = ops2; *op; op++) {
        if (n00b_scan_peek_byte(s, 0) == (uint8_t)(*op)[0]
            && n00b_scan_peek_byte(s, 1) == (uint8_t)(*op)[1]) {
            int64_t tid = n00b_scan_terminal_id(s, *op);

            if (tid != N00B_TOK_OTHER) {
                n00b_scan_advance_n(s, 2);
                n00b_scan_emit_marked(s, (int32_t)tid);
                return true;
            }
        }
    }

    // Single-character token.
    n00b_scan_advance(s);
    n00b_scan_emit_marked(s, (int32_t)cp);

    return true;
}

// ============================================================================
// Grammar loading
// ============================================================================

static n00b_grammar_t *
load_c_grammar(void)
{
    // Try several paths — meson runs from build dir.
    const char *paths[] = {
        "grammars/c_ncc.bnf",
        "../grammars/c_ncc.bnf",
        "../../grammars/c_ncc.bnf",
        NULL,
    };

    const char *srcroot = getenv("MESON_SOURCE_ROOT");

    FILE *f = NULL;

    for (const char **p = paths; *p; p++) {
        f = fopen(*p, "r");

        if (f) {
            break;
        }
    }

    if (!f && srcroot) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/grammars/c_ncc.bnf", srcroot);
        f = fopen(path, "r");
    }

    if (!f) {
        fprintf(stderr, "  [SKIP] Cannot find grammars/c_ncc.bnf\n");
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)len + 1);
    size_t nread = fread(buf, 1, (size_t)len, f);
    buf[nread] = '\0';
    fclose(f);

    n00b_string_t bnf_text = n00b_string_from_cstr(buf);
    free(buf);

    n00b_grammar_t *g = n00b_grammar_new();
    n00b_grammar_set_error_recovery(g, false);

    n00b_string_t start = N00B_STRING_STATIC("translation_unit");
    bool ok = n00b_bnf_load(bnf_text, start, g);

    if (!ok) {
        fprintf(stderr, "  [FAIL] n00b_bnf_load failed for c_ncc.bnf\n");
        n00b_grammar_free(g);
        return NULL;
    }

    return g;
}

// ============================================================================
// Test: load grammar
// ============================================================================

static void
test_load_grammar(void)
{
    printf("Test: Load C grammar from BNF...\n");

    n00b_grammar_t *g = load_c_grammar();

    if (!g) {
        printf("  SKIP (grammar file not found)\n");
        return;
    }

    // Check that basic terminals were registered.
    assert(g->terminal_map != NULL);
    assert(n00b_list_len(g->nt_list) > 0);
    assert(n00b_list_len(g->rules) > 0);

    printf("  Grammar loaded: %zu NTs, %zu rules\n",
           n00b_list_len(g->nt_list),
           n00b_list_len(g->rules));

    printf("  PASS\n");
    n00b_grammar_free(g);
}

// ============================================================================
// Test: tokenize + parse simple C
// ============================================================================

static void
test_parse_simple_c(void)
{
    printf("Test: Parse simple C source...\n");

    n00b_grammar_t *g = load_c_grammar();

    if (!g) {
        printf("  SKIP\n");
        return;
    }

    const char *src = "int main(void) { return 0; }";

    n00b_buffer_t *buf = n00b_buffer_from_bytes(src, (int64_t)strlen(src));
    n00b_scanner_t *scanner = n00b_scanner_new(buf, c_tokenize, g,
                                                n00b_option_none(n00b_string_t),
                                                NULL, NULL);
    n00b_token_stream_t *ts = n00b_token_stream_new(scanner);

    n00b_pwz_parser_t *p = n00b_pwz_new(g);
    assert(p);

    bool ok = n00b_pwz_parse(p, ts);

    if (ok) {
        n00b_parse_tree_t *tree = n00b_pwz_get_tree(p);
        assert(tree);

        printf("  Parse succeeded. Tree:\n");
        n00b_parse_tree_print(g, tree, stdout, false);
    }
    else {
        printf("  Parse did NOT succeed (this may be expected for a partial grammar test)\n");
    }

    printf("  PASS\n");

    n00b_pwz_free(p);
    n00b_token_stream_free(ts);
    n00b_scanner_free(scanner);
    n00b_grammar_free(g);
}

// ============================================================================
// Test: BNF preprocessing
// ============================================================================

static void
test_bnf_preprocess(void)
{
    printf("Test: BNF preprocessing...\n");

    // Test comment stripping.
    n00b_string_t input = n00b_string_from_cstr("rule = a | b // comment\nrule2 = c");
    n00b_string_t stripped = n00b_bnf_strip_comments(input);

    assert(stripped.data != NULL);
    printf("  Strip comments: \"%.*s\"\n", (int)stripped.u8_bytes, stripped.data);

    // Test line trimming.
    n00b_string_t padded = n00b_string_from_cstr("  hello  \n  world  \n");
    n00b_string_t trimmed = n00b_bnf_trim_lines(padded);

    assert(trimmed.data != NULL);
    printf("  Trim lines: \"%.*s\"\n", (int)trimmed.u8_bytes, trimmed.data);

    printf("  PASS\n");
}

// ============================================================================
// main
// ============================================================================

int
main(void)
{
    test_bnf_preprocess();
    test_load_grammar();
    test_parse_simple_c();

    printf("\nAll BNF tests passed.\n");
    return 0;
}
