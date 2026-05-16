// n00b_tokenizer.c — N00b language tokenizer callback for the scanner framework.
//
// Ported from n00b-noai/src/compiler/lex.c. The n00b language uses:
// - Newlines as statement terminators (significant whitespace)
// - `#` and `//` for line comments, `/* */` for block comments
// - Python-like keyword set (if/elif/else/for/while/etc.)
// - Single/double/triple-quoted strings with escape sequences
// - Long literals: [=encoder[contents]=]'modifier (Lua-inspired, level >= 1)
// - Literal modifiers (e.g., `42'hex`, `[==[data]==]'sometype`)
// - `and`/`or` for logical operators (not `&&`/`||`)

#include "n00b/n00b_tokenizer.h"
#include "parsers/scan_recipes.h"
#include "parsers/token_stream.h"
#include "core/alloc.h"

#include <string.h>

// ============================================================================
// Internal: skip non-newline whitespace
// ============================================================================

static void
skip_non_nl_ws(n00b_scanner_t *s)
{
    while (!n00b_scan_at_eof(s)) {
        uint8_t b = n00b_scan_peek_byte(s, 0);

        if (b == ' ' || b == '\t' || b == '\r') {
            n00b_scan_advance(s);
        }
        else {
            break;
        }
    }
}

// lookup_or_default removed — replaced by hash-based token ID scheme.

// ============================================================================
// Internal: get last emitted token (for post-emit patching)
// ============================================================================

static n00b_token_info_t *
last_emitted_token(n00b_scanner_t *s)
{
    n00b_token_stream_t *ts = s->stream;

    if (ts && ts->token_count > 0) {
        return ts->tokens[ts->token_count - 1];
    }

    return nullptr;
}

// ============================================================================
// Internal: try to scan a literal modifier ('identifier after a literal)
// ============================================================================

// try_scan_modifier is no longer needed — the grammar handles the
// trailing 'type-spec via: %EMBED (%"'" <type-spec>)?
// The tokenizer's main loop naturally scans the tick and identifier.

// ============================================================================
// Internal: trim leading/trailing whitespace from a byte range
// ============================================================================

static void
trim_whitespace(const char *data, size_t len, size_t *out_start, size_t *out_len)
{
    size_t start = 0;
    size_t end   = len;

    while (start < end
           && (data[start] == ' ' || data[start] == '\t'
               || data[start] == '\n' || data[start] == '\r')) {
        start++;
    }

    while (end > start
           && (data[end - 1] == ' ' || data[end - 1] == '\t'
               || data[end - 1] == '\n' || data[end - 1] == '\r')) {
        end--;
    }

    *out_start = start;
    *out_len   = end - start;
}

// ============================================================================
// Internal: scan a long literal [=...encoder?[contents]=...]
// ============================================================================

static bool
try_scan_long_literal(n00b_scanner_t *s)
{
    // Must start with [ followed by at least one =.
    if (n00b_scan_peek_byte(s, 0) != '[' || n00b_scan_peek_byte(s, 1) != '=') {
        return false;
    }

    // Save position for rollback.
    size_t   saved_cursor = s->cursor;
    uint32_t saved_line   = s->line;
    uint32_t saved_col    = s->column;

    n00b_scan_advance(s);  // Skip opening [.

    // Count = signs.
    int32_t level = 0;

    while (!n00b_scan_at_eof(s) && n00b_scan_peek_byte(s, 0) == '=') {
        n00b_scan_advance(s);
        level++;
    }

    // After the = signs, we expect either an identifier (encoder) or [.
    n00b_string_t *encoder = nullptr;

    if (n00b_scan_at_eof(s)) {
        goto rollback;
    }

    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    if (cp != '[') {
        // Try to scan an encoder tag (ASCII identifier).
        if (!((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')
              || cp == '_')) {
            goto rollback;
        }

        n00b_scan_mark(s);
        n00b_scan_advance(s);

        while (!n00b_scan_at_eof(s)) {
            cp = n00b_scan_peek(s, 0);

            if (!((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')
                  || (cp >= '0' && cp <= '9') || cp == '_')) {
                break;
            }

            n00b_scan_advance(s);
        }

        encoder = n00b_scan_extract(s);


        // Must be followed by [.
        if (n00b_scan_at_eof(s) || n00b_scan_peek_byte(s, 0) != '[') {
            goto rollback;
        }
    }

    n00b_scan_advance(s);  // Skip the inner [.

    // Now scan contents until we find ] followed by exactly `level` = signs
    // followed by ].
    size_t content_start = s->cursor;

    while (!n00b_scan_at_eof(s)) {
        if (n00b_scan_peek_byte(s, 0) != ']') {
            n00b_scan_advance(s);
            continue;
        }

        // Potential closing delimiter: ] + level = signs + ].
        size_t close_cursor = s->cursor;

        n00b_scan_advance(s);  // Skip ].

        int32_t eq_count = 0;

        while (!n00b_scan_at_eof(s) && n00b_scan_peek_byte(s, 0) == '='
               && eq_count < level) {
            n00b_scan_advance(s);
            eq_count++;
        }

        if (eq_count == level && !n00b_scan_at_eof(s)
            && n00b_scan_peek_byte(s, 0) == ']') {
            // Found the closing delimiter.
            size_t content_end = close_cursor;

            n00b_scan_advance(s);  // Skip final ].

            // Extract content, trimming leading/trailing whitespace.
            const char *raw     = s->input + content_start;
            size_t      raw_len = content_end - content_start;
            size_t      trim_start, trim_len;

            trim_whitespace(raw, raw_len, &trim_start, &trim_len);

            n00b_string_t *contents = n00b_string_from_raw(
                raw + trim_start, (int64_t)trim_len);

            // Emit the token.
            n00b_scan_emit(s, .token_type = "EMBED",
                            .contents = n00b_option_set(n00b_string_t *, contents));

            // Set encoder on the token's encoding field.
            if (encoder) {
                n00b_token_info_t *tok = last_emitted_token(s);

                if (tok) {
                    tok->encoding = n00b_option_set(n00b_string_t *, encoder);
                }
            }

            // Trailing 'modifier (e.g., 'ffi) is handled by the grammar:
            //   %EMBED (%"'" <type-spec>)?
            // The main scan loop will tokenize the ' and identifier.

            return true;
        }

        // Not a match — restore to just after the ] and keep scanning.
        // (We already advanced past the ] and some ='s; just continue.)
    }

    // Unterminated long literal — fall through to rollback.

rollback:
    s->cursor = saved_cursor;
    s->line   = saved_line;
    s->column = saved_col;
    return false;
}

// ============================================================================
// Main tokenizer callback
// ============================================================================

bool
n00b_lang_tokenize(n00b_scanner_t *s)
{
restart:
    // Skip non-newline whitespace.
    skip_non_nl_ws(s);

    if (n00b_scan_at_eof(s)) {
        return false;
    }

    uint8_t b0 = n00b_scan_peek_byte(s, 0);

    // -----------------------------------------------------------------
    // Newlines — significant in n00b
    // -----------------------------------------------------------------
    if (b0 == '\n') {
        n00b_scan_mark(s);
        n00b_scan_advance(s);
        n00b_scan_emit(s, .token_type = "NEWLINE");
        return true;
    }

    // -----------------------------------------------------------------
    // Line comments: # or //
    // -----------------------------------------------------------------
    if (b0 == '#') {
        n00b_scan_skip_line_comment(s);
        goto restart;
    }

    if (b0 == '/' && n00b_scan_peek_byte(s, 1) == '/') {
        n00b_scan_skip_line_comment(s);
        goto restart;
    }

    // Block comments: /* ... */
    if (b0 == '/' && n00b_scan_peek_byte(s, 1) == '*') {
        n00b_scan_skip_block_comment(s, "/*", "*/");
        goto restart;
    }

    n00b_scan_mark(s);
    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    // -----------------------------------------------------------------
    // String literals
    // -----------------------------------------------------------------
    if (cp == '"') {
        // Check for triple-quote.
        if (n00b_scan_peek_byte(s, 1) == '"'
            && n00b_scan_peek_byte(s, 2) == '"') {
            // Triple-quoted string: scan to closing """.
            n00b_scan_advance_n(s, 3);  // Past opening """.

            while (!n00b_scan_at_eof(s)) {
                if (n00b_scan_peek_byte(s, 0) == '"'
                    && n00b_scan_peek_byte(s, 1) == '"'
                    && n00b_scan_peek_byte(s, 2) == '"') {
                    n00b_scan_advance_n(s, 3);
                    break;
                }

                n00b_scan_advance(s);
            }

            n00b_string_t *text = n00b_scan_extract(s);
            n00b_scan_emit(s, .token_type = "STRING_LIT",
                            .contents = n00b_option_set(n00b_string_t *, text));
            return true;
        }

        n00b_option_t(n00b_string_t *) val = n00b_scan_string_double(s);
        n00b_scan_emit(s, .token_type = "STRING_LIT", .contents = val);
        return true;
    }

    if (cp == '\'') {
        // Could be a char literal ('x'), or a modifier tick (42'u8).
        // Try as a char literal first; if that fails, fall through to
        // the single-char handler which will emit ' as a standalone token.
        size_t   save_cur = s->cursor;
        uint32_t save_ln  = s->line;
        uint32_t save_col = s->column;

        n00b_option_t(n00b_string_t *) val = n00b_scan_string_single(s);

        if (n00b_option_is_set(val)) {
            n00b_scan_emit(s, .token_type = "CHAR_LIT", .contents = val);
            return true;
        }

        // Not a char literal — restore cursor and fall through to
        // single-char handler, which will emit ' as a tick token.
        s->cursor = save_cur;
        s->line   = save_ln;
        s->column = save_col;
    }

    // -----------------------------------------------------------------
    // Long literals: [=...encoder?[contents]=...]'modifier?
    // -----------------------------------------------------------------
    if (cp == '[' && n00b_scan_peek_byte(s, 1) == '=') {
        if (try_scan_long_literal(s)) {
            return true;
        }
    }

    // -----------------------------------------------------------------
    // Hex literals (0x...)
    // -----------------------------------------------------------------
    if (cp == '0'
        && (n00b_scan_peek_byte(s, 1) == 'x'
            || n00b_scan_peek_byte(s, 1) == 'X')) {
        n00b_option_t(n00b_string_t *) hval = n00b_scan_integer(s);

        if (n00b_option_is_set(hval)) {
            n00b_scan_emit(s, .token_type = "HEX_LIT", .contents = hval);
            return true;
        }
    }

    // -----------------------------------------------------------------
    // Numbers (decimal integers and floats)
    // -----------------------------------------------------------------
    if ((cp >= '0' && cp <= '9')
        || (cp == '.' && n00b_scan_peek_byte(s, 1) >= '0'
            && n00b_scan_peek_byte(s, 1) <= '9')) {
        bool emitted = n00b_scan_number(s, "INT_LIT", "FLOAT_LIT");

        if (emitted) {
            return true;
        }
    }

    // -----------------------------------------------------------------
    // Identifiers / keywords
    // -----------------------------------------------------------------
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_') {
        n00b_option_t(n00b_string_t *) id_val = n00b_scan_identifier(s,
                                       .allow_underscore_start = true);

        if (n00b_option_is_set(id_val)) {
            // Try as a keyword first (hashes the text, checks grammar).
            n00b_token_err_t err = n00b_scan_emit(s, .contents = id_val);

            if (err == N00B_TOK_ERR_NOT_IN_GRAMMAR) {
                // Not a keyword — emit as IDENTIFIER.
                n00b_scan_emit(s, .token_type = "IDENTIFIER",
                               .contents = id_val);
            }

            return true;
        }
    }

    // -----------------------------------------------------------------
    // Multi-character operators (longest match first)
    // -----------------------------------------------------------------

    // 3-char operators.
    static const char *ops3[] = {
        "<<=", ">>=", NULL,
    };

    for (const char **op = ops3; *op; op++) {
        if (n00b_scan_peek_byte(s, 0) == (uint8_t)(*op)[0]
            && n00b_scan_peek_byte(s, 1) == (uint8_t)(*op)[1]
            && n00b_scan_peek_byte(s, 2) == (uint8_t)(*op)[2]) {
            size_t   save_cur = s->cursor;
            uint32_t save_ln  = s->line;
            uint32_t save_col = s->column;

            n00b_scan_advance_n(s, 3);
            n00b_token_err_t err = n00b_scan_emit(s);

            if (err == N00B_TOK_OK) {
                return true;
            }

            // Not in grammar — restore cursor.
            s->cursor = save_cur;
            s->line   = save_ln;
            s->column = save_col;
        }
    }

    // 2-char operators.
    static const char *ops2[] = {
        "->", "<<", ">>", "<=", ">=", "==", "!=",
        "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
        "**",
        NULL,
    };

    for (const char **op = ops2; *op; op++) {
        if (n00b_scan_peek_byte(s, 0) == (uint8_t)(*op)[0]
            && n00b_scan_peek_byte(s, 1) == (uint8_t)(*op)[1]) {
            size_t   save_cur = s->cursor;
            uint32_t save_ln  = s->line;
            uint32_t save_col = s->column;

            n00b_scan_advance_n(s, 2);
            n00b_token_err_t err = n00b_scan_emit(s);

            if (err == N00B_TOK_OK) {
                return true;
            }

            // Not in grammar — restore cursor.
            s->cursor = save_cur;
            s->line   = save_ln;
            s->column = save_col;
        }
    }

    // -----------------------------------------------------------------
    // Single-character tokens
    // -----------------------------------------------------------------
    n00b_scan_advance(s);

    // Try to emit as a fixed-text terminal (hashes the single char).
    n00b_token_err_t err = n00b_scan_emit(s);

    if (err != N00B_TOK_OK) {
        // Not in grammar — emit as generic OTHER token.
        n00b_scan_emit(s, .token_type = "OTHER");
    }

    return true;
}
