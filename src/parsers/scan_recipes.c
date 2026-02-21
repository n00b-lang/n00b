// scan_recipes.c — Higher-level scanning recipes for common token patterns.

#include "parsers/scan_recipes.h"
#include "util/utf8.h"
#include <assert.h>
#include <string.h>

// ============================================================================
// Internal: hex digit value
// ============================================================================

static int
hex_val(n00b_codepoint_t cp)
{
    if (cp >= '0' && cp <= '9') return (int)(cp - '0');
    if (cp >= 'a' && cp <= 'f') return (int)(cp - 'a' + 10);
    if (cp >= 'A' && cp <= 'F') return (int)(cp - 'A' + 10);
    return -1;
}

// ============================================================================
// Internal: dynamic string builder for decoded strings
// ============================================================================

typedef struct {
    char   *buf;
    size_t  len;
    size_t  cap;
    int64_t cp_count;
} strbuf_t;

static void
sb_init(strbuf_t *sb)
{
    sb->cap      = 64;
    sb->buf      = n00b_alloc_array(char, sb->cap);
    sb->len      = 0;
    sb->cp_count = 0;
}

static void
sb_push_raw(strbuf_t *sb, char c)
{
    if (sb->len >= sb->cap) {
        size_t new_cap = sb->cap * 2;
        char *new_buf  = n00b_alloc_array(char, new_cap);

        memcpy(new_buf, sb->buf, sb->len);
        n00b_free(sb->buf);
        sb->buf = new_buf;
        sb->cap = new_cap;
    }

    sb->buf[sb->len++] = c;
}

static void
sb_push(strbuf_t *sb, char c)
{
    sb_push_raw(sb, c);
    sb->cp_count++;
}

static void
sb_push_cp(strbuf_t *sb, n00b_codepoint_t cp)
{
    uint8_t enc[4];
    int     n = n00b_utf8_encode_codepoint(cp, enc);

    for (int i = 0; i < n; i++) {
        sb_push_raw(sb, (char)enc[i]);
    }

    sb->cp_count++;
}

static n00b_option_t(n00b_string_t)
sb_finish(strbuf_t *sb)
{
    n00b_string_t str = n00b_string_from_raw(sb->buf, (int64_t)sb->len);
    n00b_free(sb->buf);
    sb->buf      = nullptr;
    sb->len      = 0;
    sb->cap      = 0;
    sb->cp_count = 0;

    return n00b_option_set(n00b_string_t, str);
}

static void
sb_discard(strbuf_t *sb)
{
    n00b_free(sb->buf);
    sb->buf      = nullptr;
    sb->len      = 0;
    sb->cap      = 0;
    sb->cp_count = 0;
}

// ============================================================================
// Internal: scan N hex digits into a codepoint
// ============================================================================

static bool
scan_hex_digits(n00b_scanner_t *s, int count, n00b_codepoint_t *out)
{
    n00b_codepoint_t val = 0;

    for (int i = 0; i < count; i++) {
        n00b_codepoint_t cp = n00b_scan_peek(s, 0);
        int hv              = hex_val(cp);

        if (hv < 0) {
            return false;
        }

        val = (val << 4) | (n00b_codepoint_t)hv;
        n00b_scan_advance(s);
    }

    *out = val;
    return true;
}

// ============================================================================
// Internal: quoted string with escape processing
// ============================================================================

static n00b_option_t(n00b_string_t)
scan_quoted_string(n00b_scanner_t *s, n00b_codepoint_t quote_cp)
{
    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    if (cp != quote_cp) {
        return n00b_option_none(n00b_string_t);
    }

    n00b_scan_mark(s);
    n00b_scan_advance(s);  // Skip opening quote.

    strbuf_t sb;
    sb_init(&sb);

    while (!n00b_scan_at_eof(s)) {
        cp = n00b_scan_peek(s, 0);

        if (cp == quote_cp) {
            n00b_scan_advance(s);  // Skip closing quote.
            return sb_finish(&sb);
        }

        if (cp == '\\') {
            n00b_scan_advance(s);  // Skip backslash.

            if (n00b_scan_at_eof(s)) {
                sb_discard(&sb);
                return n00b_option_none(n00b_string_t);
            }

            cp = n00b_scan_peek(s, 0);
            n00b_scan_advance(s);

            switch (cp) {
            case '\\': sb_push(&sb, '\\'); break;
            case 'n':  sb_push(&sb, '\n'); break;
            case 't':  sb_push(&sb, '\t'); break;
            case 'r':  sb_push(&sb, '\r'); break;
            case '0':  sb_push(&sb, '\0'); break;
            case '\'': sb_push(&sb, '\''); break;
            case '"':  sb_push(&sb, '"');  break;
            case 'x': {
                n00b_codepoint_t val;
                if (!scan_hex_digits(s, 2, &val)) {
                    sb_discard(&sb);
                    return n00b_option_none(n00b_string_t);
                }
                sb_push(&sb, (char)(uint8_t)val);
                break;
            }
            case 'u': {
                n00b_codepoint_t val;
                if (!scan_hex_digits(s, 4, &val)) {
                    sb_discard(&sb);
                    return n00b_option_none(n00b_string_t);
                }
                sb_push_cp(&sb, val);
                break;
            }
            case 'U': {
                n00b_codepoint_t val;
                if (!scan_hex_digits(s, 8, &val)) {
                    sb_discard(&sb);
                    return n00b_option_none(n00b_string_t);
                }
                sb_push_cp(&sb, val);
                break;
            }
            default:
                sb_push(&sb, '\\');
                sb_push_cp(&sb, cp);
                break;
            }
        }
        else {
            size_t before = n00b_scan_offset(s);

            n00b_scan_advance(s);

            size_t after  = n00b_scan_offset(s);
            size_t nbytes = after - before;

            // Push raw bytes for a single codepoint.
            for (size_t i = 0; i < nbytes; i++) {
                sb_push_raw(&sb, s->input[before + i]);
            }
            sb.cp_count++;
        }
    }

    // Unterminated string.
    sb_discard(&sb);
    return n00b_option_none(n00b_string_t);
}

// ============================================================================
// String recipes
// ============================================================================

n00b_option_t(n00b_string_t)
n00b_scan_string_double(n00b_scanner_t *s)
{
    return scan_quoted_string(s, '"');
}

n00b_option_t(n00b_string_t)
n00b_scan_string_single(n00b_scanner_t *s)
{
    return scan_quoted_string(s, '\'');
}

n00b_option_t(n00b_string_t)
n00b_scan_string_raw(n00b_scanner_t *s, const char *quote)
{
    size_t qlen = strlen(quote);

    if (s->cursor + qlen > s->input_len
        || memcmp(s->input + s->cursor, quote, qlen) != 0) {
        return n00b_option_none(n00b_string_t);
    }

    n00b_scan_mark(s);
    n00b_scan_advance_bytes(s, qlen);

    size_t content_start = s->cursor;

    while (s->cursor + qlen <= s->input_len) {
        if (memcmp(s->input + s->cursor, quote, qlen) == 0) {
            size_t  content_end = s->cursor;
            size_t  len         = content_end - content_start;

            n00b_string_t str = n00b_string_from_raw(s->input + content_start,
                                                      (int64_t)len);

            n00b_scan_advance_bytes(s, qlen);
            return n00b_option_set(n00b_string_t, str);
        }

        n00b_scan_advance(s);
    }

    // Unterminated.
    return n00b_option_none(n00b_string_t);
}

// ============================================================================
// Number recipes
// ============================================================================

static bool
is_digit(n00b_codepoint_t cp)
{
    return cp >= '0' && cp <= '9';
}

static bool
is_hex(n00b_codepoint_t cp)
{
    return (cp >= '0' && cp <= '9')
        || (cp >= 'a' && cp <= 'f')
        || (cp >= 'A' && cp <= 'F');
}

static bool
is_binary(n00b_codepoint_t cp)
{
    return cp == '0' || cp == '1';
}

static bool
is_octal(n00b_codepoint_t cp)
{
    return cp >= '0' && cp <= '7';
}

static int32_t
skip_digits(n00b_scanner_t *s, bool (*pred)(n00b_codepoint_t))
{
    int32_t count = 0;

    while (!n00b_scan_at_eof(s)) {
        n00b_codepoint_t cp = n00b_scan_peek(s, 0);

        if (cp == '_') {
            n00b_scan_advance(s);
            continue;
        }

        if (!pred(cp)) {
            break;
        }

        n00b_scan_advance(s);
        count++;
    }

    return count;
}

n00b_option_t(n00b_string_t)
n00b_scan_integer(n00b_scanner_t *s)
{
    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    if (!is_digit(cp)) {
        return n00b_option_none(n00b_string_t);
    }

    n00b_scan_mark(s);

    if (cp == '0') {
        n00b_codepoint_t next = n00b_scan_peek(s, 1);

        if (next == 'x' || next == 'X') {
            n00b_scan_advance(s);
            n00b_scan_advance(s);

            if (skip_digits(s, is_hex) == 0) {
                return n00b_option_none(n00b_string_t);
            }

            return n00b_option_set(n00b_string_t, n00b_scan_extract(s));
        }

        if (next == 'o' || next == 'O') {
            n00b_scan_advance(s);
            n00b_scan_advance(s);

            if (skip_digits(s, is_octal) == 0) {
                return n00b_option_none(n00b_string_t);
            }

            return n00b_option_set(n00b_string_t, n00b_scan_extract(s));
        }

        if (next == 'b' || next == 'B') {
            n00b_scan_advance(s);
            n00b_scan_advance(s);

            if (skip_digits(s, is_binary) == 0) {
                return n00b_option_none(n00b_string_t);
            }

            return n00b_option_set(n00b_string_t, n00b_scan_extract(s));
        }
    }

    skip_digits(s, is_digit);
    return n00b_option_set(n00b_string_t, n00b_scan_extract(s));
}

n00b_option_t(n00b_string_t)
n00b_scan_float(n00b_scanner_t *s)
{
    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    if (!is_digit(cp)) {
        return n00b_option_none(n00b_string_t);
    }

    n00b_scan_mark(s);
    skip_digits(s, is_digit);

    bool has_dot      = false;
    bool has_exponent = false;

    if (n00b_scan_peek(s, 0) == '.') {
        n00b_codepoint_t after_dot = n00b_scan_peek(s, 1);

        if (is_digit(after_dot)) {
            has_dot = true;
            n00b_scan_advance(s);
            skip_digits(s, is_digit);
        }
    }

    cp = n00b_scan_peek(s, 0);

    if (cp == 'e' || cp == 'E') {
        has_exponent = true;
        n00b_scan_advance(s);

        cp = n00b_scan_peek(s, 0);

        if (cp == '+' || cp == '-') {
            n00b_scan_advance(s);
        }

        if (skip_digits(s, is_digit) == 0) {
            return n00b_option_none(n00b_string_t);
        }
    }

    if (!has_dot && !has_exponent) {
        return n00b_option_none(n00b_string_t);
    }

    return n00b_option_set(n00b_string_t, n00b_scan_extract(s));
}

bool
n00b_scan_number(n00b_scanner_t *s, int32_t int_tid, int32_t float_tid)
{
    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    if (!is_digit(cp)) {
        return false;
    }

    // Save position to try float first, then fallback to integer.
    size_t   saved_cursor = s->cursor;
    uint32_t saved_line   = s->line;
    uint32_t saved_col    = s->column;

    // Try float.
    n00b_scan_mark(s);

    n00b_option_t(n00b_string_t) fval = n00b_scan_float(s);

    if (n00b_option_is_set(fval)) {
        n00b_scan_emit(s, float_tid, fval);
        return true;
    }

    // Float failed. GC handles any strings allocated during partial
    // scanning — no manual cleanup needed.

    // Restore and try integer.
    s->cursor = saved_cursor;
    s->line   = saved_line;
    s->column = saved_col;

    n00b_scan_mark(s);

    n00b_option_t(n00b_string_t) ival = n00b_scan_integer(s);

    if (n00b_option_is_set(ival)) {
        n00b_scan_emit(s, int_tid, ival);
        return true;
    }

    // Restore cursor on failure.
    s->cursor = saved_cursor;
    s->line   = saved_line;
    s->column = saved_col;

    return false;
}

// ============================================================================
// Identifier recipe
// ============================================================================

n00b_option_t(n00b_string_t)
n00b_scan_identifier(n00b_scanner_t *s)
{
    if (n00b_scan_at_eof(s)) {
        return n00b_option_none(n00b_string_t);
    }

    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    if (!n00b_unicode_is_id_start(cp)) {
        return n00b_option_none(n00b_string_t);
    }

    n00b_scan_mark(s);
    n00b_scan_advance(s);

    while (!n00b_scan_at_eof(s)) {
        cp = n00b_scan_peek(s, 0);

        if (!n00b_unicode_is_id_continue(cp)) {
            break;
        }

        n00b_scan_advance(s);
    }

    return n00b_option_set(n00b_string_t, n00b_scan_extract(s));
}
