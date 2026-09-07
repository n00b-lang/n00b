
#include "scanner/scan_builtins.h"
#include "lib/buffer.h"
#include "unicode/encoding.h"
#include <assert.h>
#include <string.h>

static int
hex_val(core_codepoint_t cp)
{
    if (cp >= '0' && cp <= '9') return (int)(cp - '0');
    if (cp >= 'a' && cp <= 'f') return (int)(cp - 'a' + 10);
    if (cp >= 'A' && cp <= 'F') return (int)(cp - 'A' + 10);
    return -1;
}

static void
sb_push_cp(core_buffer_t *sb, core_codepoint_t cp)
{
    char enc[4];
    uint32_t n = unicode_utf8_encode(cp, enc);

    core_buffer_append(sb, enc, n);
}

static core_option_t(core_string_t)
sb_finish(core_buffer_t *sb)
{
    core_string_t str = core_string_from_raw(sb->data, (int64_t)sb->byte_len);
    core_free(sb->data);
    core_free(sb);

    return core_option_set(core_string_t, str);
}

static void
sb_discard(core_buffer_t *sb)
{
    core_buffer_free(sb);
}

static bool
scan_hex_digits(parse_scanner_t *s, int count, core_codepoint_t *out)
{
    core_codepoint_t val = 0;

    for (int i = 0; i < count; i++) {
        core_codepoint_t cp = scan_peek(s, 0);
        int hv              = hex_val(cp);

        if (hv < 0) {
            return false;
        }

        val = (val << 4) | (core_codepoint_t)hv;
        scan_advance(s);
    }

    *out = val;
    return true;
}

static core_option_t(core_string_t)
scan_quoted_string(parse_scanner_t *s, core_codepoint_t quote_cp)
{
    core_codepoint_t cp = scan_peek(s, 0);

    if (cp != quote_cp) {
        return core_option_none(core_string_t);
    }

    scan_mark(s);
    scan_advance(s);

    core_buffer_t *sb = core_buffer_empty();

    while (!scan_at_eof(s)) {
        cp = scan_peek(s, 0);

        if (cp == quote_cp) {
            scan_advance(s);
            return sb_finish(sb);
        }

        if (cp == '\\') {
            scan_advance(s);

            if (scan_at_eof(s)) {
                sb_discard(sb);
                return core_option_none(core_string_t);
            }

            cp = scan_peek(s, 0);
            scan_advance(s);

            switch (cp) {
            case '\\': core_buffer_putc(sb,'\\'); break;
            case 'n':  core_buffer_putc(sb,'\n'); break;
            case 't':  core_buffer_putc(sb,'\t'); break;
            case 'r':  core_buffer_putc(sb,'\r'); break;
            case '0':  core_buffer_putc(sb,'\0'); break;
            case '\'': core_buffer_putc(sb,'\''); break;
            case '"':  core_buffer_putc(sb,'"');  break;
            case 'x': {
                core_codepoint_t val;
                if (!scan_hex_digits(s, 2, &val)) {
                    sb_discard(sb);
                    return core_option_none(core_string_t);
                }
                core_buffer_putc(sb,(char)(uint8_t)val);
                break;
            }
            case 'u': {
                core_codepoint_t val;
                if (!scan_hex_digits(s, 4, &val)) {
                    sb_discard(sb);
                    return core_option_none(core_string_t);
                }
                sb_push_cp(sb,val);
                break;
            }
            case 'U': {
                core_codepoint_t val;
                if (!scan_hex_digits(s, 8, &val)) {
                    sb_discard(sb);
                    return core_option_none(core_string_t);
                }
                sb_push_cp(sb,val);
                break;
            }
            default:
                core_buffer_putc(sb,'\\');
                sb_push_cp(sb,cp);
                break;
            }
        }
        else {
            size_t before = scan_offset(s);

            scan_advance(s);

            size_t after  = scan_offset(s);
            size_t nbytes = after - before;

            core_buffer_append(sb, s->input + before, nbytes);
        }
    }

    sb_discard(sb);
    return core_option_none(core_string_t);
}

core_option_t(core_string_t)
scan_string_double(parse_scanner_t *s)
{
    return scan_quoted_string(s, '"');
}

core_option_t(core_string_t)
scan_string_single(parse_scanner_t *s)
{
    return scan_quoted_string(s, '\'');
}

core_option_t(core_string_t)
scan_string_raw(parse_scanner_t *s, core_string_t quote)
{
    size_t qlen = quote.u8_bytes;

    if (s->cursor + qlen > s->input_len
        || memcmp(s->input + s->cursor, quote.data, qlen) != 0) {
        return core_option_none(core_string_t);
    }

    scan_mark(s);
    scan_advance_bytes(s, qlen);

    size_t content_start = s->cursor;

    while (s->cursor + qlen <= s->input_len) {
        if (memcmp(s->input + s->cursor, quote.data, qlen) == 0) {
            size_t  content_end = s->cursor;
            size_t  len         = content_end - content_start;

            core_string_t str = core_string_from_raw(s->input + content_start,
                                                      (int64_t)len);

            scan_advance_bytes(s, qlen);
            return core_option_set(core_string_t, str);
        }

        scan_advance(s);
    }

    return core_option_none(core_string_t);
}

static bool
is_hex(core_codepoint_t cp)
{
    return (cp >= '0' && cp <= '9')
        || (cp >= 'a' && cp <= 'f')
        || (cp >= 'A' && cp <= 'F');
}

static bool
is_binary(core_codepoint_t cp)
{
    return cp == '0' || cp == '1';
}

static bool
is_octal(core_codepoint_t cp)
{
    return cp >= '0' && cp <= '7';
}

static int32_t
skip_digits(parse_scanner_t *s, bool (*pred)(core_codepoint_t))
{
    int32_t count = 0;

    while (!scan_at_eof(s)) {
        core_codepoint_t cp = scan_peek(s, 0);

        if (cp == '_') {
            scan_advance(s);
            continue;
        }

        if (!pred(cp)) {
            break;
        }

        scan_advance(s);
        count++;
    }

    return count;
}

core_option_t(core_string_t)
scan_integer(parse_scanner_t *s)
{
    core_codepoint_t cp = scan_peek(s, 0);

    if (!unicode_cp_is_ascii_digit(cp)) {
        return core_option_none(core_string_t);
    }

    scan_mark(s);

    if (cp == '0') {
        core_codepoint_t next = scan_peek(s, 1);

        if (next == 'x' || next == 'X') {
            scan_advance(s);
            scan_advance(s);

            if (skip_digits(s, is_hex) == 0) {
                return core_option_none(core_string_t);
            }

            return core_option_set(core_string_t, scan_extract(s));
        }

        if (next == 'o' || next == 'O') {
            scan_advance(s);
            scan_advance(s);

            if (skip_digits(s, is_octal) == 0) {
                return core_option_none(core_string_t);
            }

            return core_option_set(core_string_t, scan_extract(s));
        }

        if (next == 'b' || next == 'B') {
            scan_advance(s);
            scan_advance(s);

            if (skip_digits(s, is_binary) == 0) {
                return core_option_none(core_string_t);
            }

            return core_option_set(core_string_t, scan_extract(s));
        }
    }

    skip_digits(s, unicode_cp_is_ascii_digit);
    return core_option_set(core_string_t, scan_extract(s));
}

core_option_t(core_string_t)
scan_float(parse_scanner_t *s)
{
    core_codepoint_t cp = scan_peek(s, 0);

    if (!unicode_cp_is_ascii_digit(cp)) {
        return core_option_none(core_string_t);
    }

    scan_mark(s);
    skip_digits(s, unicode_cp_is_ascii_digit);

    bool has_dot      = false;
    bool has_exponent = false;

    if (scan_peek(s, 0) == '.') {
        core_codepoint_t after_dot = scan_peek(s, 1);

        if (unicode_cp_is_ascii_digit(after_dot)) {
            has_dot = true;
            scan_advance(s);
            skip_digits(s, unicode_cp_is_ascii_digit);
        }
    }

    cp = scan_peek(s, 0);

    if (cp == 'e' || cp == 'E') {
        has_exponent = true;
        scan_advance(s);

        cp = scan_peek(s, 0);

        if (cp == '+' || cp == '-') {
            scan_advance(s);
        }

        if (skip_digits(s, unicode_cp_is_ascii_digit) == 0) {
            return core_option_none(core_string_t);
        }
    }

    if (!has_dot && !has_exponent) {
        return core_option_none(core_string_t);
    }

    return core_option_set(core_string_t, scan_extract(s));
}

bool
scan_number(parse_scanner_t *s, int32_t int_tid, int32_t float_tid)
{
    core_codepoint_t cp = scan_peek(s, 0);

    if (!unicode_cp_is_ascii_digit(cp)) {
        return false;
    }

    size_t   saved_cursor = s->cursor;
    uint32_t saved_line   = s->line;
    uint32_t saved_col    = s->column;

    scan_mark(s);

    core_option_t(core_string_t) fval = scan_float(s);

    if (core_option_is_set(fval)) {
        scan_emit(s, float_tid, fval);
        return true;
    }

    s->cursor = saved_cursor;
    s->line   = saved_line;
    s->column = saved_col;

    scan_mark(s);

    core_option_t(core_string_t) ival = scan_integer(s);

    if (core_option_is_set(ival)) {
        scan_emit(s, int_tid, ival);
        return true;
    }

    s->cursor = saved_cursor;
    s->line   = saved_line;
    s->column = saved_col;

    return false;
}

core_option_t(core_string_t)
scan_identifier(parse_scanner_t *s)
{
    if (scan_at_eof(s)) {
        return core_option_none(core_string_t);
    }

    core_codepoint_t cp = scan_peek(s, 0);

    if (!unicode_is_id_start(cp)) {
        return core_option_none(core_string_t);
    }

    scan_mark(s);
    scan_advance(s);

    while (!scan_at_eof(s)) {
        cp = scan_peek(s, 0);

        if (!unicode_is_id_continue(cp)) {
            break;
        }

        scan_advance(s);
    }

    return core_option_set(core_string_t, scan_extract(s));
}
