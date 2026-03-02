// shell_tokenizer.c — Shell/command-line tokenizer.

#include "parsers/shell_tokenizer.h"
#include "parsers/scan_recipes.h"
#include <assert.h>

// ============================================================================
// Internal helpers
// ============================================================================

static bool
is_shell_special(n00b_codepoint_t cp)
{
    switch (cp) {
    case '#': case '$': case '"': case '\'': case '\n':
    case '{': case '}': case '(': case ')': case '[': case ']':
    case '|': case '&': case ';': case '<': case '>': case '=':
    case ' ': case '\t': case '\r':
        return true;
    default:
        return false;
    }
}

static void
skip_non_newline_whitespace(n00b_scanner_t *s)
{
    while (!n00b_scan_at_eof(s)) {
        n00b_codepoint_t cp = n00b_scan_peek(s, 0);

        if (cp == ' ' || cp == '\t' || cp == '\r') {
            n00b_scan_advance(s);
        }
        else {
            break;
        }
    }
}

// ============================================================================
// Shell tokenizer callback
// ============================================================================

bool
n00b_shell_tokenize(n00b_scanner_t *s)
{
restart:
    skip_non_newline_whitespace(s);

    if (n00b_scan_at_eof(s)) {
        return false;
    }

    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    // Newline — significant in shell grammars.
    if (cp == '\n') {
        n00b_scan_mark(s);
        n00b_scan_advance(s);
        n00b_scan_emit(s, .token_type = "NEWLINE");
        return true;
    }

    // Line comment.
    if (cp == '#') {
        n00b_scan_advance(s);
        n00b_scan_skip_line_comment(s);
        goto restart;
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

    // Single-quoted string (raw, no escapes — shell style).
    if (cp == '\'') {
        n00b_scan_mark(s);
        n00b_option_t(n00b_string_t *) val = n00b_scan_string_raw(s, "'");

        if (n00b_option_is_set(val)) {
            n00b_scan_emit(s, .token_type = "STRING_LIT",
                            .contents    = val);
            return true;
        }
    }

    // Variable reference: ${name} or $name.
    if (cp == '$') {
        n00b_scan_mark(s);
        n00b_scan_advance(s);  // Skip '$'.

        if (!n00b_scan_at_eof(s) && n00b_scan_peek(s, 0) == '{') {
            // ${name} form.
            n00b_scan_advance(s);  // Skip '{'.

            while (!n00b_scan_at_eof(s) && n00b_scan_peek(s, 0) != '}') {
                n00b_scan_advance(s);
            }

            if (!n00b_scan_at_eof(s)) {
                n00b_scan_advance(s);  // Skip '}'.
            }

            n00b_scan_emit(s, .token_type = "VAR_REF");
            return true;
        }

        // $name form: scan identifier characters.
        if (!n00b_scan_at_eof(s) && n00b_unicode_is_id_start(n00b_scan_peek(s, 0))) {
            n00b_scan_advance(s);

            while (!n00b_scan_at_eof(s)
                   && n00b_unicode_is_id_continue(n00b_scan_peek(s, 0))) {
                n00b_scan_advance(s);
            }

            n00b_scan_emit(s, .token_type = "VAR_REF");
            return true;
        }

        // Bare '$' — emit as terminal.
        n00b_token_err_t err = n00b_scan_emit(s);

        if (err == N00B_TOK_OK) {
            return true;
        }

        n00b_scan_emit(s, .tid = N00B_TOK_OTHER);
        return true;
    }

    // Number.
    if (cp >= '0' && cp <= '9') {
        if (n00b_scan_number(s, "INTEGER", "FLOAT")) {
            return true;
        }
    }

    // Unquoted word: everything up to whitespace or special char.
    if (!is_shell_special(cp)) {
        n00b_scan_mark(s);

        while (!n00b_scan_at_eof(s) && !is_shell_special(n00b_scan_peek(s, 0))) {
            n00b_scan_advance(s);
        }

        // Try as keyword first.
        n00b_token_err_t err = n00b_scan_emit(s);

        if (err == N00B_TOK_OK) {
            return true;
        }

        n00b_scan_emit(s, .token_type = "WORD");
        return true;
    }

    // Single special character as terminal.
    n00b_scan_mark(s);
    n00b_scan_advance(s);

    n00b_token_err_t err = n00b_scan_emit(s);

    if (err == N00B_TOK_OK) {
        return true;
    }

    n00b_scan_emit(s, .tid = N00B_TOK_OTHER);
    return true;
}
