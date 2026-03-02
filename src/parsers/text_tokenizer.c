// text_tokenizer.c — General-purpose text tokenizer (default).

#include "parsers/text_tokenizer.h"
#include "parsers/scan_recipes.h"
#include <assert.h>

bool
n00b_text_tokenize(n00b_scanner_t *s)
{
restart:
    n00b_scan_skip_whitespace(s);

    if (n00b_scan_at_eof(s)) {
        return false;
    }

    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

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

    // Single-quoted string.
    if (cp == '\'') {
        n00b_scan_mark(s);
        n00b_option_t(n00b_string_t *) val = n00b_scan_string_single(s);

        if (n00b_option_is_set(val)) {
            n00b_scan_emit(s, .token_type = "STRING_LIT",
                            .contents    = val);
            return true;
        }
    }

    // Numbers: digit, or '.' followed by digit.
    if ((cp >= '0' && cp <= '9')
        || (cp == '.' && n00b_scan_peek(s, 1) >= '0'
            && n00b_scan_peek(s, 1) <= '9')) {
        if (n00b_scan_number(s, "INTEGER", "FLOAT")) {
            return true;
        }
    }

    // UAX#31 identifier (also handles keyword lookup via grammar).
    if (n00b_unicode_is_id_start(cp)) {
        n00b_scan_mark(s);
        n00b_option_t(n00b_string_t *) id = n00b_scan_identifier(s);

        if (n00b_option_is_set(id)) {
            // Try to emit as a keyword/terminal first.
            n00b_token_err_t err = n00b_scan_emit(s);

            if (err == N00B_TOK_OK) {
                return true;
            }

            // Not a keyword — emit as IDENTIFIER.
            n00b_scan_emit(s, .token_type = "IDENTIFIER");
            return true;
        }
    }

    // Line comment: // or #
    if (cp == '/' && n00b_scan_peek(s, 1) == '/') {
        n00b_scan_advance(s);
        n00b_scan_advance(s);
        n00b_scan_skip_line_comment(s);
        goto restart;
    }

    if (cp == '/' && n00b_scan_peek(s, 1) == '*') {
        n00b_scan_skip_block_comment(s, "/*", "*/");
        goto restart;
    }

    // Single punctuation codepoint as a terminal.
    n00b_scan_mark(s);
    n00b_scan_advance(s);

    // Try as fixed-text terminal.
    n00b_token_err_t err = n00b_scan_emit(s);

    if (err == N00B_TOK_OK) {
        return true;
    }

    // Not in grammar — emit as OTHER.
    n00b_scan_emit(s, .tid = N00B_TOK_OTHER);
    return true;
}
