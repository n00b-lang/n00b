// lisp_tokenizer.c — Lisp/S-expression tokenizer.

#include "parsers/lisp_tokenizer.h"
#include "parsers/scan_recipes.h"
#include <assert.h>

// ============================================================================
// Internal helpers
// ============================================================================

static bool
is_lisp_delimiter(n00b_codepoint_t cp)
{
    switch (cp) {
    case '(': case ')': case '"': case ';':
    case ' ': case '\t': case '\n': case '\r':
        return true;
    default:
        return false;
    }
}

// ============================================================================
// Lisp tokenizer callback
// ============================================================================

bool
n00b_lisp_tokenize(n00b_scanner_t *s)
{
restart:
    n00b_scan_skip_whitespace(s);

    if (n00b_scan_at_eof(s)) {
        return false;
    }

    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    // Line comment.
    if (cp == ';') {
        n00b_scan_advance(s);
        n00b_scan_skip_line_comment(s);
        goto restart;
    }

    // Parentheses — emit as terminals.
    if (cp == '(' || cp == ')') {
        n00b_scan_mark(s);
        n00b_scan_advance(s);
        n00b_scan_emit(s);
        return true;
    }

    // Double-quoted string.
    if (cp == '"') {
        n00b_scan_mark(s);
        n00b_option_t(n00b_string_t *) val = n00b_scan_string_double(s);

        if (n00b_option_is_set(val)) {
            n00b_scan_emit(s, .token_type = "STRING_LIT",
                            .contents    = val);
            return true;
        }
    }

    // Boolean literals: #t, #f.
    if (cp == '#') {
        n00b_codepoint_t next = n00b_scan_peek(s, 1);

        if (next == 't' || next == 'f') {
            n00b_scan_mark(s);
            n00b_scan_advance(s);
            n00b_scan_advance(s);

            // Check that the next char is a delimiter or EOF.
            if (n00b_scan_at_eof(s) || is_lisp_delimiter(n00b_scan_peek(s, 0))) {
                n00b_scan_emit(s);
                return true;
            }

            // Not a boolean — fall through to symbol scanning below.
            // We already advanced past #t/#f, so continue scanning
            // as a symbol.
            goto scan_symbol_tail;
        }
    }

    // Number: digit, or +/- followed by digit.
    if (cp >= '0' && cp <= '9') {
        if (n00b_scan_number(s, "INTEGER", "FLOAT")) {
            return true;
        }
    }

    if ((cp == '+' || cp == '-')) {
        n00b_codepoint_t next = n00b_scan_peek(s, 1);

        if (next >= '0' && next <= '9') {
            // Signed number: mark at sign, scan sign + digits manually
            // (can't use n00b_scan_number because it calls mark internally).
            n00b_scan_mark(s);
            n00b_scan_advance(s);    // Skip sign.

            // Scan digits.
            bool has_dot      = false;
            bool has_exponent = false;

            while (!n00b_scan_at_eof(s)) {
                n00b_codepoint_t d = n00b_scan_peek(s, 0);

                if (d >= '0' && d <= '9') {
                    n00b_scan_advance(s);
                }
                else if (d == '.' && !has_dot) {
                    n00b_codepoint_t after = n00b_scan_peek(s, 1);

                    if (after >= '0' && after <= '9') {
                        has_dot = true;
                        n00b_scan_advance(s);
                    }
                    else {
                        break;
                    }
                }
                else if ((d == 'e' || d == 'E') && !has_exponent) {
                    has_exponent = true;
                    n00b_scan_advance(s);

                    n00b_codepoint_t esign = n00b_scan_peek(s, 0);

                    if (esign == '+' || esign == '-') {
                        n00b_scan_advance(s);
                    }
                }
                else {
                    break;
                }
            }

            const char *type_name = (has_dot || has_exponent) ? "FLOAT" : "INTEGER";
            n00b_scan_emit(s, .token_type = type_name);
            return true;
        }
    }

    // Symbol: everything except whitespace, parens, quotes, semicolons.
    n00b_scan_mark(s);

scan_symbol_tail:
    while (!n00b_scan_at_eof(s) && !is_lisp_delimiter(n00b_scan_peek(s, 0))) {
        n00b_scan_advance(s);
    }

    if (n00b_scan_mark_len(s) > 0) {
        // Try as keyword first.
        n00b_token_err_t err = n00b_scan_emit(s);

        if (err == N00B_TOK_OK) {
            return true;
        }

        n00b_scan_emit(s, .token_type = "SYMBOL");
        return true;
    }

    // Should not reach here; advance past unknown character.
    n00b_scan_mark(s);
    n00b_scan_advance(s);
    n00b_scan_emit(s, .tid = N00B_TOK_OTHER);
    return true;
}
