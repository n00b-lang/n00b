// c_tokenizer.c — C23/ncc tokenizer callback for the scanner framework.
//
// This is the production C tokenizer, generalized from the prototype in
// test_c_parse.c. It uses the scanner.h callback API and handles all
// C23 syntax plus ncc extensions.

#include "slay/c_tokenizer.h"
#include "parsers/scan_recipes.h"
#include "core/alloc.h"

#include <string.h>

// ============================================================================
// State management
// ============================================================================

n00b_c_tokenizer_state_t *
n00b_c_tokenizer_state_new(void)
{
    n00b_c_tokenizer_state_t *st = n00b_alloc(n00b_c_tokenizer_state_t);

    if (st) {
        memset(st, 0, sizeof(*st));
    }

    return st;
}

void
n00b_c_tokenizer_reset(n00b_scanner_t *s)
{
    if (!s || !s->user_state) {
        return;
    }

    n00b_c_tokenizer_state_t *st = s->user_state;
    st->ncc_off          = false;
    st->in_system_header = false;
    st->current_file     = NULL;
}

bool
n00b_c_tokenizer_is_ncc_off(n00b_scanner_t *s)
{
    if (!s || !s->user_state) {
        return false;
    }

    return ((n00b_c_tokenizer_state_t *)s->user_state)->ncc_off;
}

static bool
c_ident_start(uint8_t b)
{
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || b == '_';
}

static bool
c_ident_continue(uint8_t b)
{
    return c_ident_start(b) || (b >= '0' && b <= '9');
}

static n00b_option_t(n00b_string_t *)
c_scan_identifier(n00b_scanner_t *s)
{
    if (n00b_scan_at_eof(s) || !c_ident_start(n00b_scan_peek_byte(s, 0))) {
        return n00b_option_none(n00b_string_t *);
    }

    n00b_scan_mark(s);
    n00b_scan_advance(s);

    while (!n00b_scan_at_eof(s) && c_ident_continue(n00b_scan_peek_byte(s, 0))) {
        n00b_scan_advance(s);
    }

    return n00b_option_set(n00b_string_t *, n00b_scan_extract(s));
}

// ============================================================================
// Internal: skip preprocessor directive (as trivia)
// ============================================================================

static void
skip_pp_directive(n00b_scanner_t *s)
{
    // We're right after the '#'. Consume to end of line, handling
    // backslash-newline continuation.
    while (!n00b_scan_at_eof(s)) {
        uint8_t b = n00b_scan_peek_byte(s, 0);

        if (b == '\n') {
            n00b_scan_advance(s);
            break;
        }

        if (b == '\\' && n00b_scan_peek_byte(s, 1) == '\n') {
            n00b_scan_advance_n(s, 2);
            continue;
        }

        n00b_scan_advance(s);
    }
}

// Check for "#pragma ncc off" or "#pragma ncc on" and update state.
static bool
check_pragma_ncc(n00b_scanner_t *s, size_t directive_start)
{
    if (!s->user_state) {
        return false;
    }

    // We need to check the text from directive_start to the cursor.
    // The directive starts after '#', so we look at what follows.
    const char *base = s->input + directive_start;
    size_t      len  = s->cursor - directive_start;

    // Skip whitespace after '#'.
    size_t pos = 0;

    while (pos < len && (base[pos] == ' ' || base[pos] == '\t')) {
        pos++;
    }

    // Check for "pragma".
    if (pos + 6 > len || memcmp(base + pos, "pragma", 6) != 0) {
        return false;
    }

    pos += 6;

    // Skip whitespace.
    while (pos < len && (base[pos] == ' ' || base[pos] == '\t')) {
        pos++;
    }

    // Check for "ncc".
    if (pos + 3 > len || memcmp(base + pos, "ncc", 3) != 0) {
        return false;
    }

    pos += 3;

    // Skip whitespace.
    while (pos < len && (base[pos] == ' ' || base[pos] == '\t')) {
        pos++;
    }

    n00b_c_tokenizer_state_t *st = s->user_state;

    if (pos + 3 <= len && memcmp(base + pos, "off", 3) == 0) {
        st->ncc_off = true;
        return true;
    }

    if (pos + 2 <= len && memcmp(base + pos, "on", 2) == 0) {
        st->ncc_off = false;
        return true;
    }

    return false;
}

// ============================================================================
// Internal: check for string literal prefix (L, u, U, u8)
// ============================================================================

// Returns the prefix length (0, 1, or 2) if followed by '"' or '\''.
static int
string_prefix_len(n00b_scanner_t *s)
{
    uint8_t b0 = n00b_scan_peek_byte(s, 0);
    uint8_t b1 = n00b_scan_peek_byte(s, 1);
    uint8_t b2 = n00b_scan_peek_byte(s, 2);

    // u8"..." or u8'...'
    if (b0 == 'u' && b1 == '8' && (b2 == '"' || b2 == '\'')) {
        return 2;
    }

    // L, u, U followed by quote.
    if ((b0 == 'L' || b0 == 'u' || b0 == 'U')
        && (b1 == '"' || b1 == '\'')) {
        return 1;
    }

    return 0;
}

// ============================================================================
// Main tokenizer callback
// ============================================================================

bool
n00b_c_tokenize(n00b_scanner_t *s)
{
restart:
    // Skip whitespace.
    n00b_scan_skip_whitespace(s);

    if (n00b_scan_at_eof(s)) {
        return false;
    }

    // Skip line comments (// ...).
    if (n00b_scan_peek_byte(s, 0) == '/'
        && n00b_scan_peek_byte(s, 1) == '/') {
        n00b_scan_skip_line_comment(s);
        goto restart;
    }

    // Skip block comments (/* ... */).
    if (n00b_scan_peek_byte(s, 0) == '/'
        && n00b_scan_peek_byte(s, 1) == '*') {
        n00b_scan_skip_block_comment(s, "/*", "*/");
        goto restart;
    }

    // Preprocessor directives: skip as trivia but check for #pragma ncc.
    if (n00b_scan_peek_byte(s, 0) == '#') {
        size_t pp_start = n00b_scan_offset(s);
        n00b_scan_advance(s);  // Past '#'.
        skip_pp_directive(s);
        check_pragma_ncc(s, pp_start + 1);
        goto restart;
    }

    n00b_scan_mark(s);
    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    // -----------------------------------------------------------------
    // String literals (with optional encoding prefix)
    // -----------------------------------------------------------------
    int pfx = string_prefix_len(s);

    if (pfx > 0) {
        n00b_scan_advance_n(s, pfx);
        cp = n00b_scan_peek(s, 0);
    }

    if (cp == '"' && pfx >= 0) {
        n00b_option_t(n00b_string_t *) val = n00b_scan_string_double(s);
        // The C grammar (grammars/c_ncc.bnf) declares the string
        // literal type as %STRING (not %STRING_LIT, which is the
        // n00b-language grammar's spelling). Emitting the wrong
        // type name makes n00b_scan_emit return BAD_TYPE_NAME and
        // silently drop the token, so any C construct containing a
        // string literal (notably gcc asm-labels: `int x __asm("y");`)
        // fails to parse against the merged grammar.
        n00b_scan_emit(s, .token_type = "STRING", .contents = val);
        return true;
    }

    // Character literals (with optional encoding prefix).
    if (cp == '\'' && pfx >= 0) {
        n00b_option_t(n00b_string_t *) val = n00b_scan_string_single(s);
        // Likewise %CHAR, not %CHAR_LIT — see the STRING note above.
        n00b_scan_emit(s, .token_type = "CHAR", .contents = val);
        return true;
    }

    // If we advanced past a prefix but didn't find a quote,
    // treat the prefix as an identifier start — reset mark.
    if (pfx > 0) {
        // The mark was set before the prefix; the cursor advanced past it.
        // We need to reconsider this as an identifier. The cursor is now
        // past the prefix chars. Fall through to identifier handling — the
        // prefix chars are valid identifier starts (L, u, U).
        // But we've already advanced, so we need to continue scanning
        // the rest of the identifier.
        cp = n00b_scan_peek_byte(s, 0);

        // Scan remaining identifier chars.
        while (!n00b_scan_at_eof(s)) {
            uint8_t b = n00b_scan_peek_byte(s, 0);

            if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z')
                || (b >= '0' && b <= '9') || b == '_') {
                n00b_scan_advance(s);
            }
            else {
                break;
            }
        }

        n00b_string_t *id_str = n00b_scan_extract(s);
        n00b_option_t(n00b_string_t *) id_val = n00b_option_set(
            n00b_string_t *, id_str);

        // Try as keyword (hashes text, checks grammar).
        n00b_token_err_t err = n00b_scan_emit(s, .contents = id_val);

        if (err == N00B_TOK_ERR_NOT_IN_GRAMMAR) {
            n00b_scan_emit(s, .token_type = "IDENTIFIER",
                           .contents = id_val);
        }

        return true;
    }

    // -----------------------------------------------------------------
    // Numbers
    // -----------------------------------------------------------------
    if ((cp >= '0' && cp <= '9')
        || (cp == '.' && n00b_scan_peek_byte(s, 1) >= '0'
            && n00b_scan_peek_byte(s, 1) <= '9')) {
        bool emitted = n00b_scan_number(s, "INTEGER", "FLOAT");

        if (emitted) {
            return true;
        }
    }

    // -----------------------------------------------------------------
    // Identifiers / keywords
    // -----------------------------------------------------------------
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_') {
        n00b_option_t(n00b_string_t *) id_val = c_scan_identifier(s);

        if (n00b_option_is_set(id_val)) {
            // Try as keyword (hashes text, checks grammar).
            n00b_token_err_t err = n00b_scan_emit(s, .contents = id_val);

            if (err == N00B_TOK_ERR_NOT_IN_GRAMMAR) {
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
        "<<=", ">>=", "...", NULL,
    };

    const char **op;
    for (op = ops3; *op; op++) {
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
        "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=",
        "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "^=",
        "|=", "::", "##", NULL,
    };

    for (op = ops2; *op; op++) {
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
        // Not in grammar — emit the generic OTHER token by token-id,
        // matching the other tokenizers (lisp/json/shell). Using
        // `.token_type = "OTHER"` is wrong: "OTHER" is not a
        // grammar-declared type name, so n00b_scan_emit returns
        // BAD_TYPE_NAME and the token is silently DROPPED — letting an
        // unknown byte vanish so a garbage file can parse clean (e.g. a
        // stray U+00A0). `.tid = N00B_TOK_OTHER` emits a real token the
        // grammar has no rule for, so the parse fails loudly instead.
        n00b_scan_emit(s, .tid = N00B_TOK_OTHER);
    }

    return true;
}
