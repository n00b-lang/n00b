// c_tokenizer.c — C23/ncc tokenizer callback for the scanner framework.
//
// Production C tokenizer with support for:
// - #pragma ncc off/on tracking
// - String encoding prefixes (L, u, U, u8)
// - C integer/float suffixes (preserved in token value)
// - Hex float literals (0x1.0p-3)
// - Leading-dot floats (.5f)
// - #line markers preserved as leading trivia
// - All C23 operators

#include "slay/c_tokenizer.h"
#include "parsers/scan_recipes.h"
#include "parsers/token_stream.h"
#include "core/alloc.h"

#include <stdlib.h>
#include <string.h>

// ============================================================================
// State management
// ============================================================================

n00b_c_tokenizer_state_t *
n00b_c_tokenizer_state_new(void)
{
    n00b_c_tokenizer_state_t *st = n00b_alloc(n00b_c_tokenizer_state_t);
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

// ============================================================================
// Internal: mark last emitted token with system_header flag
// ============================================================================

static void
mark_system_header(n00b_scanner_t *s)
{
    n00b_c_tokenizer_state_t *st = s->user_state;

    if (!st) {
        return;
    }

    if (st->ncc_off || st->in_system_header) {
        n00b_token_stream_t *ts = s->stream;

        if (ts && ts->token_count > 0) {
            ts->tokens[ts->token_count - 1]->system_header = true;
        }
    }
}

// ============================================================================
// Internal: skip preprocessor directive (as trivia)
// ============================================================================

static void
skip_pp_directive(n00b_scanner_t *s)
{
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

// ============================================================================
// Internal: helper to check word boundary after a keyword match
// ============================================================================

static bool
is_word_boundary(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'
        || ch == '\0';
}

// ============================================================================
// Internal: check #pragma ncc off/on and update state (Fix 17: word boundaries)
// ============================================================================

static bool
check_pragma_ncc(n00b_scanner_t *s, size_t directive_start)
{
    if (!s->user_state) {
        return false;
    }

    const char *base = s->input + directive_start;
    size_t      len  = s->cursor - directive_start;

    size_t pos = 0;

    while (pos < len && (base[pos] == ' ' || base[pos] == '\t')) {
        pos++;
    }

    if (pos + 6 > len || memcmp(base + pos, "pragma", 6) != 0) {
        return false;
    }

    // Word boundary check after "pragma".
    if (pos + 6 < len && !is_word_boundary(base[pos + 6])) {
        return false;
    }

    pos += 6;

    while (pos < len && (base[pos] == ' ' || base[pos] == '\t')) {
        pos++;
    }

    if (pos + 3 > len || memcmp(base + pos, "ncc", 3) != 0) {
        return false;
    }

    // Word boundary check after "ncc".
    if (pos + 3 < len && !is_word_boundary(base[pos + 3])) {
        return false;
    }

    pos += 3;

    while (pos < len && (base[pos] == ' ' || base[pos] == '\t')) {
        pos++;
    }

    n00b_c_tokenizer_state_t *st = s->user_state;

    if (pos + 3 <= len && memcmp(base + pos, "off", 3) == 0) {
        if (pos + 3 >= len || is_word_boundary(base[pos + 3])) {
            st->ncc_off = true;
            return true;
        }
    }

    if (pos + 2 <= len && memcmp(base + pos, "on", 2) == 0) {
        if (pos + 2 >= len || is_word_boundary(base[pos + 2])) {
            st->ncc_off = false;
            return true;
        }
    }

    return false;
}

// ============================================================================
// Internal: preserve #line marker as leading trivia (Fix 5)
// ============================================================================

static void
handle_line_marker(n00b_scanner_t *s)
{
    // Mark before '#' so we capture the entire line.
    n00b_scan_mark(s);

    // Scan to end of line.
    while (!n00b_scan_at_eof(s) && n00b_scan_peek_byte(s, 0) != '\n') {
        n00b_scan_advance(s);
    }

    if (!n00b_scan_at_eof(s)) {
        n00b_scan_advance(s); // consume the newline
    }

    // Extract verbatim text and add as leading trivia for the next token.
    n00b_string_t text = n00b_scan_extract(s);
    n00b_scan_add_leading_trivia(s, text);

    // Parse the line marker to update tokenizer state.
    // Format: # <linenum> "filename" [flags]
    if (!s->user_state) {
        return;
    }

    n00b_c_tokenizer_state_t *st = s->user_state;

    const char *p   = text.data;
    const char *end = text.data + text.u8_bytes;

    // Skip '#' and whitespace.
    if (p < end && *p == '#') {
        p++;
    }

    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }

    // Parse line number.
    uint32_t linenum = 0;
    bool     got_num = false;

    while (p < end && *p >= '0' && *p <= '9') {
        linenum = linenum * 10 + (uint32_t)(*p - '0');
        p++;
        got_num = true;
    }

    if (got_num) {
        s->line   = linenum;
        s->column = 1;
    }

    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }

    // Parse filename.
    if (p < end && *p == '"') {
        p++;
        const char *fname_start = p;

        while (p < end && *p != '"') {
            p++;
        }

        size_t fname_len = (size_t)(p - fname_start);

        if (fname_len > 0) {
            char *fname = malloc(fname_len + 1);
            memcpy(fname, fname_start, fname_len);
            fname[fname_len] = '\0';
            st->current_file = fname;
        }

        if (p < end && *p == '"') {
            p++;
        }
    }

    // Parse flags (1 = push, 2 = pop, 3 = system header, 4 = extern C).
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }

        if (p < end && *p == '3') {
            st->in_system_header = true;
        }
        else if (p < end && (*p == '1' || *p == '2')) {
            // Flag 1 (entering new file) or 2 (returning) — reset
            // system header flag; flag 3 will set it again if needed.
            st->in_system_header = false;
        }

        if (p < end && *p >= '0' && *p <= '9') {
            p++;
        }
        else {
            break;
        }
    }
}

// ============================================================================
// Internal: handle non-ncc preprocessor directives as trivia (Fix 5)
// ============================================================================

static void
handle_pp_directive_as_trivia(n00b_scanner_t *s)
{
    size_t pp_start = n00b_scan_offset(s);

    // Mark before '#'.
    n00b_scan_mark(s);
    n00b_scan_advance(s); // skip '#'
    skip_pp_directive(s);

    // Check for #pragma ncc.
    check_pragma_ncc(s, pp_start + 1);

    // Preserve as leading trivia.
    n00b_string_t text = n00b_scan_extract(s);
    n00b_scan_add_leading_trivia(s, text);
}

// ============================================================================
// Internal: string literal prefix (L, u, U, u8)
// ============================================================================

static int
string_prefix_len(n00b_scanner_t *s)
{
    uint8_t b0 = n00b_scan_peek_byte(s, 0);
    uint8_t b1 = n00b_scan_peek_byte(s, 1);
    uint8_t b2 = n00b_scan_peek_byte(s, 2);

    if (b0 == 'u' && b1 == '8' && (b2 == '"' || b2 == '\'')) {
        return 2;
    }

    if ((b0 == 'L' || b0 == 'u' || b0 == 'U')
        && (b1 == '"' || b1 == '\'')) {
        return 1;
    }

    return 0;
}

// ============================================================================
// Internal: scan C number literal with suffix (Fixes 2, 3, 6)
//
// Scans from mark to end of number including suffix.  Returns the
// token type (INTEGER or FLOAT) and emits the token with the full
// verbatim source text.
// ============================================================================

static bool
scan_c_number(n00b_scanner_t *s)
{
    // Mark is already set by the caller before the first digit/dot.
    uint8_t b0 = n00b_scan_peek_byte(s, 0);
    bool    is_float = false;

    // Leading dot float: .5, .123e4
    if (b0 == '.') {
        is_float = true;
        n00b_scan_advance(s); // skip '.'

        // Consume decimal digits.
        while (!n00b_scan_at_eof(s)) {
            uint8_t d = n00b_scan_peek_byte(s, 0);
            if (d >= '0' && d <= '9') {
                n00b_scan_advance(s);
            }
            else {
                break;
            }
        }

        // Optional exponent.
        uint8_t e = n00b_scan_peek_byte(s, 0);
        if (e == 'e' || e == 'E') {
            n00b_scan_advance(s);
            uint8_t sign = n00b_scan_peek_byte(s, 0);
            if (sign == '+' || sign == '-') {
                n00b_scan_advance(s);
            }
            while (!n00b_scan_at_eof(s)) {
                uint8_t d = n00b_scan_peek_byte(s, 0);
                if (d >= '0' && d <= '9') {
                    n00b_scan_advance(s);
                }
                else {
                    break;
                }
            }
        }

        goto suffix;
    }

    // Hex prefix: 0x / 0X
    if (b0 == '0'
        && (n00b_scan_peek_byte(s, 1) == 'x'
            || n00b_scan_peek_byte(s, 1) == 'X')) {
        n00b_scan_advance_n(s, 2); // skip 0x

        // Hex digits.
        while (!n00b_scan_at_eof(s)) {
            uint8_t d = n00b_scan_peek_byte(s, 0);
            if ((d >= '0' && d <= '9') || (d >= 'a' && d <= 'f')
                || (d >= 'A' && d <= 'F')) {
                n00b_scan_advance(s);
            }
            else {
                break;
            }
        }

        // Hex float: '.' + hex digits and/or 'p'/'P' exponent.
        if (n00b_scan_peek_byte(s, 0) == '.') {
            is_float = true;
            n00b_scan_advance(s);

            while (!n00b_scan_at_eof(s)) {
                uint8_t d = n00b_scan_peek_byte(s, 0);
                if ((d >= '0' && d <= '9') || (d >= 'a' && d <= 'f')
                    || (d >= 'A' && d <= 'F')) {
                    n00b_scan_advance(s);
                }
                else {
                    break;
                }
            }
        }

        uint8_t p = n00b_scan_peek_byte(s, 0);
        if (p == 'p' || p == 'P') {
            is_float = true;
            n00b_scan_advance(s);
            uint8_t sign = n00b_scan_peek_byte(s, 0);
            if (sign == '+' || sign == '-') {
                n00b_scan_advance(s);
            }
            while (!n00b_scan_at_eof(s)) {
                uint8_t d = n00b_scan_peek_byte(s, 0);
                if (d >= '0' && d <= '9') {
                    n00b_scan_advance(s);
                }
                else {
                    break;
                }
            }
        }

        goto suffix;
    }

    // Binary prefix: 0b / 0B
    if (b0 == '0'
        && (n00b_scan_peek_byte(s, 1) == 'b'
            || n00b_scan_peek_byte(s, 1) == 'B')) {
        n00b_scan_advance_n(s, 2);

        while (!n00b_scan_at_eof(s)) {
            uint8_t d = n00b_scan_peek_byte(s, 0);
            if (d == '0' || d == '1') {
                n00b_scan_advance(s);
            }
            else {
                break;
            }
        }

        goto suffix;
    }

    // Decimal (or octal starting with 0) integer or float.
    while (!n00b_scan_at_eof(s)) {
        uint8_t d = n00b_scan_peek_byte(s, 0);
        if (d >= '0' && d <= '9') {
            n00b_scan_advance(s);
        }
        else {
            break;
        }
    }

    // Decimal point(s) -> float / pp-number (e.g. 10.13.4).
    // Loop to consume multiple dot-digit sequences (pp-number grammar).
    while (n00b_scan_peek_byte(s, 0) == '.') {
        // Make sure next char isn't '..' (ellipsis).
        uint8_t after_dot = n00b_scan_peek_byte(s, 1);
        if (after_dot == '.') {
            break;
        }
        is_float = true;
        n00b_scan_advance(s); // skip '.'

        while (!n00b_scan_at_eof(s)) {
            uint8_t d = n00b_scan_peek_byte(s, 0);
            if (d >= '0' && d <= '9') {
                n00b_scan_advance(s);
            }
            else {
                break;
            }
        }
    }

    // Exponent.
    {
        uint8_t e = n00b_scan_peek_byte(s, 0);
        if (e == 'e' || e == 'E') {
            is_float = true;
            n00b_scan_advance(s);
            uint8_t sign = n00b_scan_peek_byte(s, 0);
            if (sign == '+' || sign == '-') {
                n00b_scan_advance(s);
            }
            while (!n00b_scan_at_eof(s)) {
                uint8_t d = n00b_scan_peek_byte(s, 0);
                if (d >= '0' && d <= '9') {
                    n00b_scan_advance(s);
                }
                else {
                    break;
                }
            }
        }
    }

suffix:
    // Consume C number suffix: u/U/l/L/f/F/i (imaginary) and C23 wb/WB.
    for (;;) {
        uint8_t ch = n00b_scan_peek_byte(s, 0);
        if (ch == 'u' || ch == 'U' || ch == 'l' || ch == 'L'
            || ch == 'f' || ch == 'F' || ch == 'i') {
            n00b_scan_advance(s);
        }
        else if ((ch == 'w' || ch == 'W')
                 && (n00b_scan_peek_byte(s, 1) == 'b'
                     || n00b_scan_peek_byte(s, 1) == 'B')) {
            n00b_scan_advance_n(s, 2);
        }
        else {
            break;
        }
    }

    // Extract verbatim source text and emit.
    n00b_string_t             val_str = n00b_scan_extract(s);
    n00b_option_t(n00b_string_t) val  = n00b_option_set(n00b_string_t, val_str);

    int32_t tid = is_float ? (int32_t)N00B_TOK_FLOAT
                           : (int32_t)N00B_TOK_INTEGER;
    n00b_scan_emit(s, tid, val);
    mark_system_header(s);

    return true;
}

// ============================================================================
// Internal: scan string/char literal preserving raw source (BONUS fix)
//
// Instead of decoding escapes, we capture verbatim source text
// (including quotes and encoding prefix) so the emitter can
// reproduce the literal faithfully.
// ============================================================================

static void
scan_raw_string(n00b_scanner_t *s, int32_t tid)
{
    // Mark was set before the encoding prefix (if any) by the caller.
    // We've already advanced past the prefix.  Now at the opening quote.
    uint8_t quote = n00b_scan_peek_byte(s, 0);
    n00b_scan_advance(s); // skip opening quote

    while (!n00b_scan_at_eof(s)) {
        uint8_t b = n00b_scan_peek_byte(s, 0);

        if (b == (uint8_t)quote) {
            n00b_scan_advance(s); // consume closing quote
            break;
        }

        if (b == '\\') {
            n00b_scan_advance(s); // skip backslash
            if (!n00b_scan_at_eof(s)) {
                n00b_scan_advance(s); // skip escaped character
            }
            continue;
        }

        n00b_scan_advance(s);
    }

    n00b_string_t             val_str = n00b_scan_extract(s);
    n00b_option_t(n00b_string_t) val  = n00b_option_set(n00b_string_t, val_str);
    n00b_scan_emit(s, tid, val);
    mark_system_header(s);
}

// ============================================================================
// Main tokenizer callback
// ============================================================================

bool
n00b_c_tokenize(n00b_scanner_t *s)
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

    // Preprocessor directives: preserve as leading trivia.
    if (n00b_scan_peek_byte(s, 0) == '#') {
        uint8_t next = n00b_scan_peek_byte(s, 1);

        // Line markers from clang -E: # <num> "file" [flags]
        if (next == ' ' || (next >= '0' && next <= '9')) {
            handle_line_marker(s);
            goto restart;
        }

        // Other preprocessor directives (including #pragma).
        handle_pp_directive_as_trivia(s);
        goto restart;
    }

    n00b_scan_mark(s);
    n00b_codepoint_t cp = n00b_scan_peek(s, 0);

    // -----------------------------------------------------------------
    // String literals (with optional encoding prefix) — raw source
    // -----------------------------------------------------------------
    int pfx = string_prefix_len(s);

    if (pfx > 0) {
        n00b_scan_advance_n(s, pfx);
        cp = n00b_scan_peek(s, 0);
    }

    if (cp == '"' && pfx >= 0) {
        scan_raw_string(s, (int32_t)N00B_TOK_STRING_LIT);
        return true;
    }

    if (cp == '\'' && pfx >= 0) {
        scan_raw_string(s, (int32_t)N00B_TOK_CHAR_LIT);
        return true;
    }

    // If we advanced past a prefix but didn't find a quote,
    // treat the prefix as an identifier start.
    if (pfx > 0) {
        cp = n00b_scan_peek_byte(s, 0);

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

        n00b_string_t id_str = n00b_scan_extract(s);
        n00b_option_t(n00b_string_t) id_val = n00b_option_set(
            n00b_string_t, id_str);

        int64_t kw_id = n00b_scan_terminal_id(s, id_str.data);

        if (kw_id != N00B_TOK_OTHER) {
            n00b_scan_emit(s, (int32_t)kw_id, id_val);
        }
        else {
            n00b_scan_emit(s, (int32_t)N00B_TOK_IDENTIFIER, id_val);
        }

        mark_system_header(s);
        return true;
    }

    // -----------------------------------------------------------------
    // Numbers (custom scanner: preserves suffixes, handles hex floats)
    // -----------------------------------------------------------------
    if ((cp >= '0' && cp <= '9')
        || (cp == '.' && n00b_scan_peek_byte(s, 1) >= '0'
            && n00b_scan_peek_byte(s, 1) <= '9')) {
        return scan_c_number(s);
    }

    // -----------------------------------------------------------------
    // Identifiers / keywords
    // -----------------------------------------------------------------
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_') {
        n00b_option_t(n00b_string_t) id_val = n00b_scan_identifier(s);

        if (n00b_option_is_set(id_val)) {
            n00b_string_t id_str = n00b_option_get(id_val);
            int64_t kw_id = n00b_scan_terminal_id(s, id_str.data);

            if (kw_id != N00B_TOK_OTHER) {
                n00b_scan_emit(s, (int32_t)kw_id, id_val);
            }
            else {
                n00b_scan_emit(s, (int32_t)N00B_TOK_IDENTIFIER, id_val);
            }

            mark_system_header(s);
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

    for (const char **op = ops3; *op; op++) {
        if (n00b_scan_peek_byte(s, 0) == (uint8_t)(*op)[0]
            && n00b_scan_peek_byte(s, 1) == (uint8_t)(*op)[1]
            && n00b_scan_peek_byte(s, 2) == (uint8_t)(*op)[2]) {
            int64_t tid = n00b_scan_terminal_id(s, *op);

            if (tid != N00B_TOK_OTHER) {
                n00b_scan_advance_n(s, 3);
                n00b_scan_emit_marked(s, (int32_t)tid);
                mark_system_header(s);
                return true;
            }
        }
    }

    // 2-char operators.
    static const char *ops2[] = {
        "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=",
        "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "^=",
        "|=", "::", "##", NULL,
    };

    for (const char **op = ops2; *op; op++) {
        if (n00b_scan_peek_byte(s, 0) == (uint8_t)(*op)[0]
            && n00b_scan_peek_byte(s, 1) == (uint8_t)(*op)[1]) {
            int64_t tid = n00b_scan_terminal_id(s, *op);

            if (tid != N00B_TOK_OTHER) {
                n00b_scan_advance_n(s, 2);
                n00b_scan_emit_marked(s, (int32_t)tid);
                mark_system_header(s);
                return true;
            }
        }
    }

    // -----------------------------------------------------------------
    // Single-character token
    // -----------------------------------------------------------------
    n00b_scan_advance(s);
    n00b_scan_emit_marked(s, (int32_t)cp);
    mark_system_header(s);

    return true;
}
