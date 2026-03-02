// json_tokenizer.c — JSON tokenizer (RFC 8259).

#include "parsers/json_tokenizer.h"
#include "parsers/scan_recipes.h"
#include <assert.h>
#include <string.h>

// ============================================================================
// Internal: JSON whitespace (space, tab, LF, CR per RFC 8259)
// ============================================================================

static void
skip_json_whitespace(n00b_scanner_t *s)
{
    while (!n00b_scan_at_eof(s)) {
        n00b_codepoint_t cp = n00b_scan_peek(s, 0);

        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') {
            n00b_scan_advance(s);
        }
        else {
            break;
        }
    }
}

// ============================================================================
// Internal: JSON number
//
// JSON numbers: [-] digits ['.' digits] [('e'|'E') ['+'/'-'] digits]
// No leading zeros except for "0" itself. No hex/octal/binary.
// ============================================================================

static bool
is_digit(n00b_codepoint_t cp)
{
    return cp >= '0' && cp <= '9';
}

static bool
scan_json_number(n00b_scanner_t *s)
{
    n00b_scan_mark(s);
    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    // Optional minus.
    if (cp == '-') {
        n00b_scan_advance(s);

        if (n00b_scan_at_eof(s)) {
            return false;
        }

        cp = n00b_scan_peek(s, 0);
    }

    if (!is_digit(cp)) {
        return false;
    }

    // Integer part.
    if (cp == '0') {
        n00b_scan_advance(s);
    }
    else {
        while (!n00b_scan_at_eof(s) && is_digit(n00b_scan_peek(s, 0))) {
            n00b_scan_advance(s);
        }
    }

    // Fractional part.
    if (!n00b_scan_at_eof(s) && n00b_scan_peek(s, 0) == '.') {
        n00b_scan_advance(s);

        if (n00b_scan_at_eof(s) || !is_digit(n00b_scan_peek(s, 0))) {
            return false;
        }

        while (!n00b_scan_at_eof(s) && is_digit(n00b_scan_peek(s, 0))) {
            n00b_scan_advance(s);
        }
    }

    // Exponent part.
    if (!n00b_scan_at_eof(s)) {
        cp = n00b_scan_peek(s, 0);

        if (cp == 'e' || cp == 'E') {
            n00b_scan_advance(s);

            if (!n00b_scan_at_eof(s)) {
                cp = n00b_scan_peek(s, 0);

                if (cp == '+' || cp == '-') {
                    n00b_scan_advance(s);
                }
            }

            if (n00b_scan_at_eof(s) || !is_digit(n00b_scan_peek(s, 0))) {
                return false;
            }

            while (!n00b_scan_at_eof(s) && is_digit(n00b_scan_peek(s, 0))) {
                n00b_scan_advance(s);
            }
        }
    }

    n00b_scan_emit(s, .token_type = "NUMBER");
    return true;
}

// ============================================================================
// JSON tokenizer callback
// ============================================================================

bool
n00b_json_tokenize(n00b_scanner_t *s)
{
    skip_json_whitespace(s);

    if (n00b_scan_at_eof(s)) {
        return false;
    }

    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    // Structural characters.
    switch (cp) {
    case '{': case '}': case '[': case ']': case ':': case ',':
        n00b_scan_mark(s);
        n00b_scan_advance(s);
        n00b_scan_emit(s);
        return true;
    default:
        break;
    }

    // String literal.
    if (cp == '"') {
        n00b_scan_mark(s);
        n00b_option_t(n00b_string_t *) val = n00b_scan_string_double(s);

        if (n00b_option_is_set(val)) {
            n00b_scan_emit(s, .token_type = "STRING_LIT",
                            .contents    = val);
            return true;
        }
    }

    // Number (starts with digit or '-').
    if (is_digit(cp) || cp == '-') {
        if (scan_json_number(s)) {
            return true;
        }
    }

    // Keyword literals: true, false, null.
    if (cp == 't') {
        n00b_scan_mark(s);
        size_t matched = n00b_scan_match_str(s, "true");

        if (matched) {
            n00b_scan_emit(s);
            return true;
        }
    }

    if (cp == 'f') {
        n00b_scan_mark(s);
        size_t matched = n00b_scan_match_str(s, "false");

        if (matched) {
            n00b_scan_emit(s);
            return true;
        }
    }

    if (cp == 'n') {
        n00b_scan_mark(s);
        size_t matched = n00b_scan_match_str(s, "null");

        if (matched) {
            n00b_scan_emit(s);
            return true;
        }
    }

    // Error recovery: advance one character and emit OTHER.
    n00b_scan_mark(s);
    n00b_scan_advance(s);
    n00b_scan_emit(s, .tid = N00B_TOK_OTHER);
    return true;
}
