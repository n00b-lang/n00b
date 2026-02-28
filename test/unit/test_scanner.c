/*
 * test_scanner.c — Tests for the streaming tokenizer API:
 *   scanner, token stream, scanning recipes, and integration.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "parsers/scan_recipes.h"
#include "parsers/token_stream.h"
#include "text/strings/string_ops.h"

// ============================================================================
// Helpers
// ============================================================================

enum {
    TOK_NUMBER = 1,
    TOK_IDENT  = 100,
    TOK_INT    = 101,
    TOK_FLOAT  = 102,
    TOK_STRING = 103,
};

#define TOK_PLUS   n00b_token_id_from_text("+", 1)
#define TOK_MINUS  n00b_token_id_from_text("-", 1)
#define TOK_STAR   n00b_token_id_from_text("*", 1)
#define TOK_LPAREN n00b_token_id_from_text("(", 1)
#define TOK_RPAREN n00b_token_id_from_text(")", 1)

static n00b_buffer_t *
buf(const char *str)
{
    return n00b_buffer_from_bytes((char *)str, (int64_t)strlen(str));
}

// A trivial tokenizer: yields each byte as its own token.
static bool
byte_scan(n00b_scanner_t *s)
{
    if (n00b_scan_at_eof(s)) {
        return false;
    }

    n00b_scan_mark(s);
    n00b_scan_advance(s);
    n00b_scan_emit(s, .tid = (int64_t)(uint8_t)s->input[s->mark]);

    return true;
}

// A calculator tokenizer: numbers, single-char operators, whitespace trivia.
static bool
calc_scan(n00b_scanner_t *s)
{
    n00b_scan_skip_whitespace(s);

    if (n00b_scan_at_eof(s)) {
        return false;
    }

    n00b_scan_mark(s);
    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    if (cp >= '0' && cp <= '9') {
        n00b_option_t(n00b_string_t) val = n00b_scan_integer(s);

        n00b_scan_emit(s, .tid = TOK_NUMBER, .contents = val);
        return true;
    }

    n00b_scan_advance(s);
    n00b_scan_emit(s);

    return true;
}

// A tokenizer with comments for trivia tests.
static bool
comment_scan(n00b_scanner_t *s)
{
    n00b_scan_skip_whitespace(s);

    if (n00b_scan_at_eof(s)) {
        return false;
    }

    // Line comment: //
    if (n00b_scan_peek(s, 0) == '/' && n00b_scan_peek_byte(s, 1) == '/') {
        n00b_scan_skip_line_comment(s);
        return true;
    }

    // Block comment: /* ... */
    if (n00b_scan_peek(s, 0) == '/'
        && n00b_scan_peek_byte(s, 1) == '*') {
        n00b_scan_skip_block_comment(s, "/*", "*/");
        return true;
    }

    n00b_scan_mark(s);
    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    if (cp >= '0' && cp <= '9') {
        n00b_option_t(n00b_string_t) val = n00b_scan_integer(s);

        n00b_scan_emit(s, .tid = TOK_NUMBER, .contents = val);
        return true;
    }

    n00b_scan_advance(s);
    n00b_scan_emit(s);

    return true;
}

// ============================================================================
// Scanner tests
// ============================================================================

static void
test_scanner_new(void)
{
    n00b_buffer_t  *b = buf("hello");
    n00b_scanner_t *s = n00b_scanner_new(b, byte_scan, nullptr);

    assert(s != nullptr);
    assert(s->input_len == 5);
    assert(s->cursor == 0);
    assert(s->line == 1);
    assert(s->column == 1);

    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scanner new\n");
}

static void
test_scan_peek(void)
{
    n00b_buffer_t  *b = buf("AB");
    n00b_scanner_t *s = n00b_scanner_new(b, byte_scan, nullptr);

    assert(n00b_scan_peek(s, 0) == 'A');
    assert(n00b_scan_peek(s, 1) == 'B');
    assert(n00b_scan_peek(s, 2) == 0);  // Past EOF.

    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan peek\n");
}

static void
test_scan_peek_multibyte(void)
{
    // UTF-8: é = 0xC3 0xA9 (U+00E9).
    n00b_buffer_t  *b = buf("\xC3\xA9X");
    n00b_scanner_t *s = n00b_scanner_new(b, byte_scan, nullptr);

    assert(n00b_scan_peek(s, 0) == 0xE9);  // é at byte 0.
    assert(n00b_scan_peek(s, 2) == 'X');    // X at byte 2.

    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan peek multibyte\n");
}

static void
test_scan_peek_negative_offset(void)
{
    n00b_buffer_t  *b = buf("ABCDE");
    n00b_scanner_t *s = n00b_scanner_new(b, byte_scan, nullptr);

    // At cursor 0, negative offset should return 0 (not underflow).
    assert(n00b_scan_peek(s, -1) == 0);
    assert(n00b_scan_peek_byte(s, -1) == 0);

    // Advance to position 3, then peek back.
    n00b_scan_advance_n(s, 3);
    assert(n00b_scan_peek(s, -1) == 'C');
    assert(n00b_scan_peek_byte(s, -1) == 'C');

    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan peek negative offset\n");
}

static void
test_scan_advance(void)
{
    n00b_buffer_t  *b = buf("A\nB");
    n00b_scanner_t *s = n00b_scanner_new(b, byte_scan, nullptr);

    assert(s->line == 1 && s->column == 1);

    n00b_scan_advance(s);  // 'A'
    assert(s->line == 1 && s->column == 2);

    n00b_scan_advance(s);  // '\n'
    assert(s->line == 2 && s->column == 1);

    n00b_scan_advance(s);  // 'B'
    assert(s->line == 2 && s->column == 2);

    assert(n00b_scan_at_eof(s));

    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan advance\n");
}

static void
test_scan_match_str(void)
{
    n00b_buffer_t  *b = buf("hello world");
    n00b_scanner_t *s = n00b_scanner_new(b, byte_scan, nullptr);

    assert(n00b_scan_match_str(s, "hello") == 5);
    assert(s->cursor == 5);
    assert(n00b_scan_match_str(s, "xyz") == 0);
    assert(s->cursor == 5);  // Unchanged on failure.

    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan match str\n");
}

static void
test_scan_match_class(void)
{
    n00b_buffer_t  *b = buf("9x");
    n00b_scanner_t *s = n00b_scanner_new(b, byte_scan, nullptr);

    assert(n00b_scan_match_class(s, N00B_CC_ASCII_DIGIT) > 0);
    assert(s->cursor == 1);
    assert(n00b_scan_match_class(s, N00B_CC_ASCII_DIGIT) == 0);
    assert(s->cursor == 1);

    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan match class\n");
}

static bool
is_upper(n00b_codepoint_t cp, void *ctx)
{
    (void)ctx;
    return cp >= 'A' && cp <= 'Z';
}

static void
test_scan_match_if(void)
{
    n00b_buffer_t  *b = buf("Az");
    n00b_scanner_t *s = n00b_scanner_new(b, byte_scan, nullptr);

    assert(n00b_scan_match_if(s, is_upper) > 0);
    assert(s->cursor == 1);
    assert(n00b_scan_match_if(s, is_upper) == 0);

    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan match if\n");
}

static bool
is_a(n00b_codepoint_t cp, void *ctx)
{
    (void)ctx;
    return cp == 'a';
}

static void
test_scan_skip_while(void)
{
    n00b_buffer_t  *b = buf("aaabbb");
    n00b_scanner_t *s = n00b_scanner_new(b, byte_scan, nullptr);

    assert(n00b_scan_skip_while(s, is_a) == 3);
    assert(s->cursor == 3);

    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan skip while\n");
}

static void
test_scan_mark_extract(void)
{
    n00b_buffer_t  *b = buf("hello world");
    n00b_scanner_t *s = n00b_scanner_new(b, byte_scan, nullptr);

    n00b_scan_advance_n(s, 6);  // Skip "hello "
    n00b_scan_mark(s);
    n00b_scan_advance_n(s, 5);  // Read "world"

    n00b_string_t text = n00b_scan_extract(s);

    assert(n00b_unicode_str_eq(text, *r"world"));
    assert(n00b_scan_mark_len(s) == 5);

    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan mark extract\n");
}

static void
test_scan_emit(void)
{
    n00b_buffer_t         *b  = buf("AB");
    n00b_scanner_t        *s  = n00b_scanner_new(b, byte_scan, nullptr);
    n00b_token_stream_t   *ts = n00b_token_stream_new(s);

    n00b_scan_mark(s);
    n00b_scan_advance(s);
    n00b_scan_emit(s, .tid = 42);

    n00b_token_info_t *tok = n00b_stream_next(ts);

    assert(tok != nullptr);
    assert(tok->tid == 42);
    assert(n00b_option_is_set(tok->value));
    assert(n00b_unicode_str_eq(n00b_option_get(tok->value), *r"A"));
    assert(tok->line == 1);
    assert(tok->column == 1);
    assert(tok->endcol == 2);

    n00b_token_stream_free(ts);
    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan emit\n");
}

static void
test_scan_trivia(void)
{
    n00b_buffer_t       *b  = buf("  42");
    n00b_scanner_t      *s  = n00b_scanner_new(b, calc_scan, nullptr);
    n00b_token_stream_t *ts = n00b_token_stream_new(s);

    n00b_token_info_t *tok = n00b_stream_next(ts);

    assert(tok != nullptr);
    assert(tok->tid == TOK_NUMBER);
    assert(n00b_option_is_set(tok->value));
    assert(n00b_unicode_str_eq(n00b_option_get(tok->value), *r"42"));
    assert(tok->leading_trivia != nullptr);
    assert(tok->leading_trivia->text.u8_bytes == 2);

    n00b_token_stream_free(ts);
    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan trivia\n");
}

static void
test_scan_whitespace_skip(void)
{
    n00b_buffer_t  *b = buf("   x");
    n00b_scanner_t *s = n00b_scanner_new(b, byte_scan, nullptr);

    int32_t skipped = n00b_scan_skip_whitespace(s);

    assert(skipped == 3);
    assert(s->cursor == 3);
    assert(n00b_scan_peek(s, 0) == 'x');

    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan whitespace skip\n");
}

static void
test_scan_line_comment(void)
{
    n00b_buffer_t       *b  = buf("42// comment\n99");
    n00b_scanner_t      *s  = n00b_scanner_new(b, comment_scan, nullptr);
    n00b_token_stream_t *ts = n00b_token_stream_new(s);

    n00b_token_info_t *tok1 = n00b_stream_next(ts);

    assert(tok1 != nullptr);
    assert(tok1->tid == TOK_NUMBER);
    assert(n00b_option_is_set(tok1->value));
    assert(n00b_unicode_str_eq(n00b_option_get(tok1->value), *r"42"));

    n00b_token_info_t *tok2 = n00b_stream_next(ts);

    assert(tok2 != nullptr);
    assert(tok2->tid == TOK_NUMBER);
    assert(n00b_option_is_set(tok2->value));
    assert(n00b_unicode_str_eq(n00b_option_get(tok2->value), *r"99"));

    n00b_token_stream_free(ts);
    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan line comment\n");
}

static void
test_scan_block_comment(void)
{
    n00b_buffer_t       *b  = buf("/* hi */42");
    n00b_scanner_t      *s  = n00b_scanner_new(b, comment_scan, nullptr);
    n00b_token_stream_t *ts = n00b_token_stream_new(s);

    n00b_token_info_t *tok = n00b_stream_next(ts);

    assert(tok != nullptr);
    assert(tok->tid == TOK_NUMBER);
    assert(n00b_option_is_set(tok->value));
    assert(n00b_unicode_str_eq(n00b_option_get(tok->value), *r"42"));
    assert(tok->leading_trivia != nullptr);

    n00b_token_stream_free(ts);
    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan block comment\n");
}

static void
test_scan_skip_until_str(void)
{
    n00b_buffer_t  *b = buf("hello world end");
    n00b_scanner_t *s = n00b_scanner_new(b, byte_scan, nullptr);

    // Found case.
    n00b_result_t(size_t) r = n00b_scan_skip_until_str(s, "world");

    assert(n00b_result_is_ok(r));
    assert(n00b_result_get(r) == 6);  // "hello " = 6 bytes.
    assert(n00b_scan_peek(s, 0) == 'w');

    n00b_scanner_free(s);
    n00b_buffer_free(b);

    // Not found case.
    b = buf("hello");
    s = n00b_scanner_new(b, byte_scan, nullptr);

    r = n00b_scan_skip_until_str(s, "xyz");

    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_ERR_SCAN_NOT_FOUND);
    assert(n00b_scan_at_eof(s));

    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan skip until str\n");
}

static void
test_scan_unterminated_block_comment(void)
{
    // Unterminated block comment — should return false and consume all input.
    n00b_buffer_t  *b = buf("/* no close");
    n00b_scanner_t *s = n00b_scanner_new(b, byte_scan, nullptr);

    bool found = n00b_scan_skip_block_comment(s, "/*", "*/");

    assert(!found);
    assert(n00b_scan_at_eof(s));
    // Trivia should still be collected.
    assert(s->pending_leading != nullptr);

    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] scan unterminated block comment\n");
}

// ============================================================================
// Token stream tests
// ============================================================================

static void
test_stream_next(void)
{
    n00b_buffer_t       *b  = buf("1 + 2");
    n00b_scanner_t      *s  = n00b_scanner_new(b, calc_scan, nullptr);
    n00b_token_stream_t *ts = n00b_token_stream_new(s);

    n00b_token_info_t *t1 = n00b_stream_next(ts);

    assert(t1 && t1->tid == TOK_NUMBER);

    n00b_token_info_t *t2 = n00b_stream_next(ts);

    assert(t2 && t2->tid == TOK_PLUS);

    n00b_token_info_t *t3 = n00b_stream_next(ts);

    assert(t3 && t3->tid == TOK_NUMBER);

    n00b_token_info_t *t4 = n00b_stream_next(ts);

    assert(t4 == nullptr);

    n00b_token_stream_free(ts);
    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] stream next\n");
}

static void
test_stream_peek(void)
{
    n00b_buffer_t       *b  = buf("1 + 2");
    n00b_scanner_t      *s  = n00b_scanner_new(b, calc_scan, nullptr);
    n00b_token_stream_t *ts = n00b_token_stream_new(s);

    n00b_token_info_t *p0 = n00b_stream_peek(ts, 0);

    assert(p0 && p0->tid == TOK_NUMBER);

    n00b_token_info_t *p1 = n00b_stream_peek(ts, 1);

    assert(p1 && p1->tid == TOK_PLUS);

    n00b_token_info_t *again = n00b_stream_peek(ts, 0);

    assert(again == p0);

    n00b_token_stream_free(ts);
    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] stream peek\n");
}

static void
test_stream_rewind(void)
{
    n00b_buffer_t       *b  = buf("1 + 2");
    n00b_scanner_t      *s  = n00b_scanner_new(b, calc_scan, nullptr);
    n00b_token_stream_t *ts = n00b_token_stream_new(s);

    n00b_token_info_t *t1 = n00b_stream_next(ts);
    n00b_token_info_t *t2 = n00b_stream_next(ts);

    (void)t2;

    assert(n00b_stream_rewind(ts, 2));

    n00b_token_info_t *r1 = n00b_stream_next(ts);

    assert(r1->tid == t1->tid);
    assert(n00b_option_is_set(r1->value));
    assert(n00b_option_is_set(t1->value));
    assert(n00b_unicode_str_eq(n00b_option_get(r1->value),
                               n00b_option_get(t1->value)));

    n00b_token_stream_free(ts);
    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] stream rewind\n");
}

static void
test_stream_rewind_overflow(void)
{
    n00b_buffer_t       *b  = buf("ABCD");
    n00b_scanner_t      *s  = n00b_scanner_new(b, byte_scan, nullptr);
    n00b_token_stream_t *ts = n00b_token_stream_new(s);

    for (int i = 0; i < 4; i++) {
        n00b_stream_next(ts);
    }

    // With growable list, all 4 tokens are retained.
    assert(n00b_stream_rewind(ts, 2));
    assert(n00b_stream_rewind(ts, 2));
    // Can't rewind past position 0.
    assert(!n00b_stream_rewind(ts, 1));

    n00b_token_stream_free(ts);
    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] stream rewind overflow\n");
}

static void
test_stream_lookback(void)
{
    n00b_buffer_t       *b  = buf("1 + 2");
    n00b_scanner_t      *s  = n00b_scanner_new(b, calc_scan, nullptr);
    n00b_token_stream_t *ts = n00b_token_stream_new(s);

    n00b_stream_next(ts);  // "1"
    n00b_stream_next(ts);  // "+"

    n00b_token_info_t *lb = n00b_stream_lookback(ts, 1);

    assert(lb != nullptr);
    assert(lb->tid == TOK_PLUS);

    lb = n00b_stream_lookback(ts, 2);
    assert(lb != nullptr);
    assert(lb->tid == TOK_NUMBER);

    n00b_token_stream_free(ts);
    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] stream lookback\n");
}

static void
test_stream_save_restore(void)
{
    n00b_buffer_t       *b  = buf("1 + 2");
    n00b_scanner_t      *s  = n00b_scanner_new(b, calc_scan, nullptr);
    n00b_token_stream_t *ts = n00b_token_stream_new(s);

    n00b_stream_next(ts);  // "1"
    n00b_stream_mark_t m = n00b_stream_save(ts);

    n00b_stream_next(ts);  // "+"
    n00b_stream_next(ts);  // "2"

    assert(n00b_stream_restore(ts, m));

    n00b_token_info_t *tok = n00b_stream_next(ts);

    assert(tok && tok->tid == TOK_PLUS);

    n00b_stream_commit(m);

    n00b_token_stream_free(ts);
    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] stream save restore\n");
}

static void
test_stream_save_evict(void)
{
    // With growable list, all tokens are retained so restore always works.
    n00b_buffer_t       *b  = buf("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    n00b_scanner_t      *s  = n00b_scanner_new(b, byte_scan, nullptr);
    n00b_token_stream_t *ts = n00b_token_stream_new(s);

    n00b_stream_mark_t m = n00b_stream_save(ts);

    for (int i = 0; i < 36; i++) {
        n00b_stream_next(ts);
    }

    // Restore to position 0 should succeed (all tokens retained).
    assert(n00b_stream_restore(ts, m));

    // Verify we can re-read from position 0.
    n00b_token_info_t *tok = n00b_stream_next(ts);
    assert(tok != nullptr);

    n00b_token_stream_free(ts);
    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] stream save evict\n");
}

static void
test_stream_collect(void)
{
    n00b_buffer_t       *b  = buf("1 + 2");
    n00b_scanner_t      *s  = n00b_scanner_new(b, calc_scan, nullptr);
    n00b_token_stream_t *ts = n00b_token_stream_new(s);

    n00b_list_t(n00b_token_info_t) tl = n00b_stream_collect(ts);

    assert(n00b_list_len(tl) == 3);

    n00b_token_info_t t0 = n00b_list_get(tl, 0);
    assert(t0.tid == TOK_NUMBER);

    n00b_token_info_t t1 = n00b_list_get(tl, 1);
    assert(t1.tid == TOK_PLUS);

    n00b_list_free(tl);
    n00b_token_stream_free(ts);
    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] stream collect\n");
}

static void
test_stream_foreach(void)
{
    n00b_buffer_t       *b  = buf("1 + 2 - 3");
    n00b_scanner_t      *s  = n00b_scanner_new(b, calc_scan, nullptr);
    n00b_token_stream_t *ts = n00b_token_stream_new(s);

    int count = 0;

    n00b_stream_foreach(ts, tok) {
        count++;
        (void)tok;
    }

    assert(count == 5);  // 1, +, 2, -, 3

    n00b_token_stream_free(ts);
    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] stream foreach\n");
}

// ============================================================================
// Recipe tests
// ============================================================================

typedef struct {
    n00b_buffer_t       *buf;
    n00b_scanner_t      *scanner;
    n00b_token_stream_t *stream;
} recipe_ctx_t;

static bool
noop_scan(n00b_scanner_t *s)
{
    (void)s;
    return false;
}

static recipe_ctx_t
recipe_setup(const char *input)
{
    recipe_ctx_t ctx;

    ctx.buf     = buf(input);
    ctx.scanner = n00b_scanner_new(ctx.buf, noop_scan, nullptr);
    ctx.stream  = n00b_token_stream_new(ctx.scanner);

    return ctx;
}

static void
recipe_teardown(recipe_ctx_t *ctx)
{
    n00b_token_stream_free(ctx->stream);
    n00b_scanner_free(ctx->scanner);
    n00b_buffer_free(ctx->buf);
}

static void
test_recipe_string_double(void)
{
    recipe_ctx_t ctx = recipe_setup("\"hello\\nworld\"");

    n00b_scan_mark(ctx.scanner);

    n00b_option_t(n00b_string_t) val = n00b_scan_string_double(ctx.scanner);

    assert(n00b_option_is_set(val));
    assert(n00b_unicode_str_eq(n00b_option_get(val), *r"hello\nworld"));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe string double\n");
}

static void
test_recipe_string_single(void)
{
    recipe_ctx_t ctx = recipe_setup("'tab\\there'");

    n00b_scan_mark(ctx.scanner);

    n00b_option_t(n00b_string_t) val = n00b_scan_string_single(ctx.scanner);

    assert(n00b_option_is_set(val));
    assert(n00b_unicode_str_eq(n00b_option_get(val), *r"tab\there"));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe string single\n");
}

static void
test_recipe_string_unterminated(void)
{
    recipe_ctx_t ctx = recipe_setup("\"hello");

    n00b_option_t(n00b_string_t) val = n00b_scan_string_double(ctx.scanner);

    assert(!n00b_option_is_set(val));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe string unterminated\n");
}

static void
test_recipe_string_bad_hex_escape(void)
{
    // \xGG is not valid hex.
    recipe_ctx_t ctx = recipe_setup("\"\\xGG\"");

    n00b_option_t(n00b_string_t) val = n00b_scan_string_double(ctx.scanner);

    assert(!n00b_option_is_set(val));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe string bad hex escape\n");
}

static void
test_recipe_string_unicode_escape(void)
{
    // \u00E9 = é
    recipe_ctx_t ctx = recipe_setup("\"caf\\u00E9\"");

    n00b_option_t(n00b_string_t) val = n00b_scan_string_double(ctx.scanner);

    assert(n00b_option_is_set(val));
    assert(n00b_unicode_str_eq(n00b_option_get(val), *r"caf\xC3\xA9"));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe string unicode escape\n");
}

static void
test_recipe_integer_decimal(void)
{
    recipe_ctx_t ctx = recipe_setup("12345");

    n00b_option_t(n00b_string_t) val = n00b_scan_integer(ctx.scanner);

    assert(n00b_option_is_set(val));
    assert(n00b_unicode_str_eq(n00b_option_get(val), *r"12345"));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe integer decimal\n");
}

static void
test_recipe_integer_hex(void)
{
    recipe_ctx_t ctx = recipe_setup("0xFF");

    n00b_option_t(n00b_string_t) val = n00b_scan_integer(ctx.scanner);

    assert(n00b_option_is_set(val));
    assert(n00b_unicode_str_eq(n00b_option_get(val), *r"0xFF"));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe integer hex\n");
}

static void
test_recipe_integer_binary(void)
{
    recipe_ctx_t ctx = recipe_setup("0b1010");

    n00b_option_t(n00b_string_t) val = n00b_scan_integer(ctx.scanner);

    assert(n00b_option_is_set(val));
    assert(n00b_unicode_str_eq(n00b_option_get(val), *r"0b1010"));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe integer binary\n");
}

static void
test_recipe_integer_octal(void)
{
    recipe_ctx_t ctx = recipe_setup("0o777");

    n00b_option_t(n00b_string_t) val = n00b_scan_integer(ctx.scanner);

    assert(n00b_option_is_set(val));
    assert(n00b_unicode_str_eq(n00b_option_get(val), *r"0o777"));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe integer octal\n");
}

static void
test_recipe_integer_separators(void)
{
    recipe_ctx_t ctx = recipe_setup("1_000_000");

    n00b_option_t(n00b_string_t) val = n00b_scan_integer(ctx.scanner);

    assert(n00b_option_is_set(val));
    assert(n00b_unicode_str_eq(n00b_option_get(val), *r"1_000_000"));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe integer separators\n");
}

static void
test_recipe_integer_no_digits_after_prefix(void)
{
    // "0x" with no hex digits after — should return none.
    recipe_ctx_t ctx = recipe_setup("0xZZ");

    n00b_option_t(n00b_string_t) val = n00b_scan_integer(ctx.scanner);

    assert(!n00b_option_is_set(val));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe integer no digits after prefix\n");
}

static void
test_recipe_float(void)
{
    recipe_ctx_t ctx = recipe_setup("3.14");

    n00b_option_t(n00b_string_t) val = n00b_scan_float(ctx.scanner);

    assert(n00b_option_is_set(val));
    assert(n00b_unicode_str_eq(n00b_option_get(val), *r"3.14"));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe float\n");
}

static void
test_recipe_float_exponent(void)
{
    recipe_ctx_t ctx = recipe_setup("1e10");

    n00b_option_t(n00b_string_t) val = n00b_scan_float(ctx.scanner);

    assert(n00b_option_is_set(val));
    assert(n00b_unicode_str_eq(n00b_option_get(val), *r"1e10"));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe float exponent\n");
}

static void
test_recipe_float_full(void)
{
    recipe_ctx_t ctx = recipe_setup("2.5E-3");

    n00b_option_t(n00b_string_t) val = n00b_scan_float(ctx.scanner);

    assert(n00b_option_is_set(val));
    assert(n00b_unicode_str_eq(n00b_option_get(val), *r"2.5E-3"));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe float full\n");
}

static void
test_recipe_float_integer_only(void)
{
    // "42" is not a float (no dot, no exponent).
    recipe_ctx_t ctx = recipe_setup("42");

    n00b_option_t(n00b_string_t) val = n00b_scan_float(ctx.scanner);

    assert(!n00b_option_is_set(val));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe float integer only\n");
}

static void
test_recipe_number_emits_correctly(void)
{
    // n00b_scan_number should emit float for "3.14" and int for "42".
    recipe_ctx_t ctx = recipe_setup("3.14");

    bool ok = n00b_scan_number(ctx.scanner, "INTEGER", "FLOAT");

    assert(ok);

    n00b_token_info_t *tok = n00b_stream_next(ctx.stream);

    assert(tok != nullptr);
    // The tid will be whatever the literal_type_map assigns for "FLOAT".
    // Without a grammar, the scan_number call won't find the type name,
    // so we just verify a token was emitted.
    assert(n00b_option_is_set(tok->value));
    assert(n00b_unicode_str_eq(n00b_option_get(tok->value), *r"3.14"));

    recipe_teardown(&ctx);

    // Integer case.
    ctx = recipe_setup("42");
    ok  = n00b_scan_number(ctx.scanner, "INTEGER", "FLOAT");

    assert(ok);

    tok = n00b_stream_next(ctx.stream);

    assert(tok != nullptr);
    assert(n00b_option_is_set(tok->value));
    assert(n00b_unicode_str_eq(n00b_option_get(tok->value), *r"42"));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe number emits correctly\n");
}

static void
test_recipe_identifier(void)
{
    recipe_ctx_t ctx = recipe_setup("foo_bar123");

    n00b_option_t(n00b_string_t) val = n00b_scan_identifier(ctx.scanner);

    assert(n00b_option_is_set(val));
    assert(n00b_unicode_str_eq(n00b_option_get(val), *r"foo_bar123"));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe identifier\n");
}

static void
test_recipe_identifier_unicode(void)
{
    recipe_ctx_t ctx = recipe_setup("caf\xC3\xA9");

    n00b_option_t(n00b_string_t) val = n00b_scan_identifier(ctx.scanner);

    assert(n00b_option_is_set(val));
    assert(n00b_unicode_str_eq(n00b_option_get(val), *r"caf\xC3\xA9"));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe identifier unicode\n");
}

static void
test_recipe_identifier_not_at_start(void)
{
    // "123abc" — digit is not ID_Start, should return none.
    recipe_ctx_t ctx = recipe_setup("123abc");

    n00b_option_t(n00b_string_t) val = n00b_scan_identifier(ctx.scanner);

    assert(!n00b_option_is_set(val));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe identifier not at start\n");
}

static void
test_recipe_empty_input(void)
{
    recipe_ctx_t ctx = recipe_setup("");

    assert(!n00b_option_is_set(n00b_scan_string_double(ctx.scanner)));
    assert(!n00b_option_is_set(n00b_scan_integer(ctx.scanner)));
    assert(!n00b_option_is_set(n00b_scan_float(ctx.scanner)));
    assert(!n00b_option_is_set(n00b_scan_identifier(ctx.scanner)));

    recipe_teardown(&ctx);
    printf("  [PASS] recipe empty input\n");
}

// ============================================================================
// Integration test
// ============================================================================

static void
test_calculator_tokenizer(void)
{
    n00b_buffer_t       *b  = buf("123 + 456 * (7 - 8)");
    n00b_scanner_t      *s  = n00b_scanner_new(b, calc_scan, nullptr);
    n00b_token_stream_t *ts = n00b_token_stream_new(s);

    int64_t expected_tids[] = {
        TOK_NUMBER, TOK_PLUS, TOK_NUMBER, TOK_STAR,
        TOK_LPAREN, TOK_NUMBER, TOK_MINUS, TOK_NUMBER, TOK_RPAREN,
    };
    n00b_string_t *expected_values[] = {
        r"123", r"+", r"456", r"*", r"(", r"7", r"-", r"8", r")",
    };
    int expected_count = 9;

    int i = 0;

    n00b_stream_foreach(ts, tok) {
        assert(i < expected_count);
        assert(tok->tid == expected_tids[i]);
        assert(n00b_option_is_set(tok->value));
        assert(n00b_unicode_str_eq(n00b_option_get(tok->value),
                                   *expected_values[i]));
        i++;
    }

    assert(i == expected_count);

    n00b_token_stream_free(ts);
    n00b_scanner_free(s);
    n00b_buffer_free(b);
    printf("  [PASS] calculator tokenizer\n");
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_scanner:\n");
    fflush(stdout);

    // Scanner tests
    test_scanner_new();                    fflush(stdout);
    test_scan_peek();                      fflush(stdout);
    test_scan_peek_multibyte();            fflush(stdout);
    test_scan_peek_negative_offset();      fflush(stdout);
    test_scan_advance();                   fflush(stdout);
    test_scan_match_str();                 fflush(stdout);
    test_scan_match_class();               fflush(stdout);
    test_scan_match_if();                  fflush(stdout);
    test_scan_skip_while();                fflush(stdout);
    test_scan_mark_extract();              fflush(stdout);
    test_scan_emit();                      fflush(stdout);
    test_scan_trivia();                    fflush(stdout);
    test_scan_whitespace_skip();           fflush(stdout);
    test_scan_line_comment();              fflush(stdout);
    test_scan_block_comment();             fflush(stdout);
    test_scan_skip_until_str();            fflush(stdout);
    test_scan_unterminated_block_comment(); fflush(stdout);

    // Token stream tests
    test_stream_next();              fflush(stdout);
    test_stream_peek();              fflush(stdout);
    test_stream_rewind();            fflush(stdout);
    test_stream_rewind_overflow();   fflush(stdout);
    test_stream_lookback();          fflush(stdout);
    test_stream_save_restore();      fflush(stdout);
    test_stream_save_evict();        fflush(stdout);
    test_stream_collect();           fflush(stdout);
    test_stream_foreach();           fflush(stdout);

    // Recipe tests
    test_recipe_string_double();            fflush(stdout);
    test_recipe_string_single();            fflush(stdout);
    test_recipe_string_unterminated();      fflush(stdout);
    test_recipe_string_bad_hex_escape();    fflush(stdout);
    test_recipe_string_unicode_escape();    fflush(stdout);
    test_recipe_integer_decimal();          fflush(stdout);
    test_recipe_integer_hex();              fflush(stdout);
    test_recipe_integer_binary();           fflush(stdout);
    test_recipe_integer_octal();            fflush(stdout);
    test_recipe_integer_separators();       fflush(stdout);
    test_recipe_integer_no_digits_after_prefix(); fflush(stdout);
    test_recipe_float();                    fflush(stdout);
    test_recipe_float_exponent();           fflush(stdout);
    test_recipe_float_full();               fflush(stdout);
    test_recipe_float_integer_only();       fflush(stdout);
    test_recipe_number_emits_correctly();   fflush(stdout);
    test_recipe_identifier();               fflush(stdout);
    test_recipe_identifier_unicode();       fflush(stdout);
    test_recipe_identifier_not_at_start();  fflush(stdout);
    test_recipe_empty_input();              fflush(stdout);

    // Integration
    test_calculator_tokenizer(); fflush(stdout);

    printf("All scanner tests passed.\n");
    n00b_shutdown();
    return 0;
}
