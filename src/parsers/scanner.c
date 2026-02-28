// scanner.c — Streaming tokenizer scanner implementation.

#include "parsers/scanner.h"
#include "parsers/token_stream.h"
#include "internal/slay/grammar_internal.h"
#include "core/string.h"
#include <assert.h>
#include <string.h>

// ============================================================================
// Internal: UTF-8 decode at position
// ============================================================================

static n00b_codepoint_t
decode_at(n00b_scanner_t *s, size_t off, int *cp_len)
{
    if (off >= s->input_len) {
        *cp_len = 0;
        return 0;
    }

    uint32_t pos = (uint32_t)off;
    int32_t  cp  = n00b_unicode_utf8_decode(s->input, (uint32_t)s->input_len, &pos);

    if (cp < 0) {
        *cp_len = 1;
        return (n00b_codepoint_t)(uint8_t)s->input[off];
    }

    *cp_len = (int)(pos - (uint32_t)off);
    return (n00b_codepoint_t)cp;
}

// ============================================================================
// Internal: character class matching
// ============================================================================

static bool
cc_match(n00b_char_class_t cc, n00b_codepoint_t cp)
{
    switch (cc) {
    case N00B_CC_ID_START:
        return n00b_unicode_is_id_start(cp);
    case N00B_CC_ID_CONTINUE:
        return n00b_unicode_is_id_continue(cp);
    case N00B_CC_ASCII_DIGIT:
        return cp >= '0' && cp <= '9';
    case N00B_CC_UNICODE_DIGIT: {
        n00b_unicode_gc_t gc = n00b_unicode_general_category(cp);
        return gc == N00B_UNICODE_GC_ND;
    }
    case N00B_CC_ASCII_UPPER:
        return cp >= 'A' && cp <= 'Z';
    case N00B_CC_ASCII_LOWER:
        return cp >= 'a' && cp <= 'z';
    case N00B_CC_ASCII_ALPHA:
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    case N00B_CC_WHITESPACE:
        return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r'
            || cp == '\f' || cp == '\v'
            || cp == 0x85   // NEL
            || cp == 0xA0   // NBSP
            || cp == 0x2028 // Line separator
            || cp == 0x2029; // Paragraph separator
    case N00B_CC_HEX_DIGIT:
        return (cp >= '0' && cp <= '9')
            || (cp >= 'a' && cp <= 'f')
            || (cp >= 'A' && cp <= 'F');
    case N00B_CC_NONZERO_DIGIT:
        return cp >= '1' && cp <= '9';
    case N00B_CC_PRINTABLE:
        return cp >= 0x20 && cp != 0x7F;
    case N00B_CC_NON_WS_PRINTABLE:
        return cp > 0x20 && cp != 0x7F;
    case N00B_CC_NON_NL_WS:
        return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\f' || cp == '\v';
    case N00B_CC_NON_NL_PRINTABLE:
        return cp >= 0x20 && cp != 0x7F && cp != '\n';
    case N00B_CC_JSON_STRING_CHAR:
        return cp >= 0x20 && cp != '"' && cp != '\\';
    case N00B_CC_REGEX_BODY_CHAR:
        return cp >= 0x20 && cp != '/' && cp != '\\';
    }
    return false;
}

// ============================================================================
// Internal: trivia allocation
// ============================================================================

static n00b_trivia_t *
make_trivia(n00b_string_t *text)
{
    n00b_trivia_t *t = n00b_alloc(n00b_trivia_t);

    t->text = text;
    t->next = nullptr;
    return t;
}

static void
free_trivia_chain(n00b_trivia_t *t)
{
    while (t) {
        n00b_trivia_t *next = t->next;
        // t->text.data is GC-managed; just free the node.
        n00b_free(t);
        t = next;
    }
}

static void
append_trivia(n00b_trivia_t **head, n00b_trivia_t *node)
{
    if (!*head) {
        *head = node;
        return;
    }

    n00b_trivia_t *tail = *head;

    while (tail->next) {
        tail = tail->next;
    }

    tail->next = node;
}

// ============================================================================
// Scanner lifecycle
// ============================================================================

n00b_scanner_t *
n00b_scanner_new(n00b_buffer_t  *buf,
                 n00b_scan_cb_t  cb,
                 n00b_grammar_t *grammar)
    _kargs
{
    n00b_option_t(n00b_string_t *) file;
    void                        *state;
    n00b_scan_reset_cb_t         reset_cb;
}
{
    assert(buf);
    assert(cb);

    n00b_scanner_t *s = n00b_alloc(n00b_scanner_t);

    s->input     = buf->data;
    s->input_len = buf->byte_len;
    s->cursor    = 0;
    s->line      = 1;
    s->column    = 1;
    s->file      = file;
    s->cb        = cb;
    s->reset_cb  = reset_cb;
    s->user_state = state;
    s->grammar   = grammar;
    s->at_eof    = false;

    // Cache the tokenizer in the grammar so template parsing can reuse it.
    if (grammar && !grammar->tokenize_cb) {
        grammar->tokenize_cb = cb;
    }

    // Mark defaults to start of input.
    s->mark      = 0;
    s->mark_line = 1;
    s->mark_col  = 1;

    // Terminal ID cache is lazily initialized.
    s->terminal_ids = nullptr;

    return s;
}

void
n00b_scanner_free(n00b_scanner_t *s)
{
    if (!s) {
        return;
    }

    // Free pending trivia nodes (text data is GC-managed).
    free_trivia_chain(s->pending_leading);
    free_trivia_chain(s->pending_trailing);

    // The terminal_ids dict is GC-managed (n00b_alloc); no explicit free needed.

    n00b_free(s);
}

void
n00b_scanner_reset(n00b_scanner_t *s)
{
    if (!s) {
        return;
    }

    s->cursor   = 0;
    s->line     = 1;
    s->column   = 1;
    s->mark     = 0;
    s->mark_line = 1;
    s->mark_col  = 1;
    s->at_eof   = false;

    // Free pending trivia.
    free_trivia_chain(s->pending_leading);
    free_trivia_chain(s->pending_trailing);
    s->pending_leading  = nullptr;
    s->pending_trailing = nullptr;

    // Reset user state if callback is set.
    if (s->reset_cb) {
        s->reset_cb(s);
    }
}

// ============================================================================
// Cursor inspection
// ============================================================================

n00b_codepoint_t
n00b_scan_peek(n00b_scanner_t *s, int32_t offset)
{
    // Guard against negative offsets that would underflow.
    if (offset < 0 && (size_t)(-offset) > s->cursor) {
        return 0;
    }

    size_t pos = s->cursor + (size_t)offset;
    int    cp_len;

    return decode_at(s, pos, &cp_len);
}

uint8_t
n00b_scan_peek_byte(n00b_scanner_t *s, int32_t offset)
{
    if (offset < 0 && (size_t)(-offset) > s->cursor) {
        return 0;
    }

    size_t pos = s->cursor + (size_t)offset;

    if (pos >= s->input_len) {
        return 0;
    }

    return (uint8_t)s->input[pos];
}

bool
n00b_scan_at_eof(n00b_scanner_t *s)
{
    return s->cursor >= s->input_len;
}

size_t
n00b_scan_remaining(n00b_scanner_t *s)
{
    if (s->cursor >= s->input_len) {
        return 0;
    }

    return s->input_len - s->cursor;
}

size_t
n00b_scan_offset(n00b_scanner_t *s)
{
    return s->cursor;
}

// ============================================================================
// Cursor movement
// ============================================================================

void
n00b_scan_advance(n00b_scanner_t *s)
{
    if (s->cursor >= s->input_len) {
        return;
    }

    int              cp_len;
    n00b_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    s->cursor += (size_t)cp_len;

    if (cp == '\n') {
        s->line++;
        s->column = 1;
    }
    else {
        s->column++;
    }
}

void
n00b_scan_advance_n(n00b_scanner_t *s, int32_t n)
{
    for (int32_t i = 0; i < n && s->cursor < s->input_len; i++) {
        n00b_scan_advance(s);
    }
}

void
n00b_scan_advance_bytes(n00b_scanner_t *s, size_t n)
{
    size_t end = s->cursor + n;

    if (end > s->input_len) {
        end = s->input_len;
    }

    size_t delta = end - s->cursor;

    s->cursor = end;
    s->column += (uint32_t)delta;
}

// ============================================================================
// Matching helpers
// ============================================================================

size_t
n00b_scan_match_str(n00b_scanner_t *s, const char *lit)
{
    size_t len = strlen(lit);

    if (s->cursor + len > s->input_len) {
        return 0;
    }

    if (memcmp(s->input + s->cursor, lit, len) != 0) {
        return 0;
    }

    for (size_t i = 0; i < len; ) {
        int              cp_len;
        n00b_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        s->cursor += (size_t)cp_len;
        i += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
    }

    return len;
}

size_t
n00b_scan_match_if(n00b_scanner_t *s, n00b_cp_predicate_fn pred)
{
    if (s->cursor >= s->input_len) {
        return 0;
    }

    int              cp_len;
    n00b_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    if (!pred(cp, nullptr)) {
        return 0;
    }

    s->cursor += (size_t)cp_len;

    if (cp == '\n') {
        s->line++;
        s->column = 1;
    }
    else {
        s->column++;
    }

    return (size_t)cp_len;
}

size_t
n00b_scan_match_class(n00b_scanner_t *s, n00b_char_class_t cc)
{
    if (s->cursor >= s->input_len) {
        return 0;
    }

    int              cp_len;
    n00b_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    if (!cc_match(cc, cp)) {
        return 0;
    }

    s->cursor += (size_t)cp_len;

    if (cp == '\n') {
        s->line++;
        s->column = 1;
    }
    else {
        s->column++;
    }

    return (size_t)cp_len;
}

size_t
n00b_scan_match_cp(n00b_scanner_t *s, n00b_codepoint_t target)
{
    if (s->cursor >= s->input_len) {
        return 0;
    }

    int              cp_len;
    n00b_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    if (cp != target) {
        return 0;
    }

    s->cursor += (size_t)cp_len;

    if (cp == '\n') {
        s->line++;
        s->column = 1;
    }
    else {
        s->column++;
    }

    return (size_t)cp_len;
}

int32_t
n00b_scan_skip_while(n00b_scanner_t *s, n00b_cp_predicate_fn pred)
{
    int32_t count = 0;

    while (s->cursor < s->input_len) {
        int              cp_len;
        n00b_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (!pred(cp, nullptr)) {
            break;
        }

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }

        count++;
    }

    return count;
}

int32_t
n00b_scan_skip_class(n00b_scanner_t *s, n00b_char_class_t cc)
{
    int32_t count = 0;

    while (s->cursor < s->input_len) {
        int              cp_len;
        n00b_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (!cc_match(cc, cp)) {
            break;
        }

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }

        count++;
    }

    return count;
}

size_t
n00b_scan_skip_until_cp(n00b_scanner_t *s, n00b_codepoint_t stop)
{
    size_t start = s->cursor;

    while (s->cursor < s->input_len) {
        int              cp_len;
        n00b_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (cp == stop) {
            break;
        }

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
    }

    return s->cursor - start;
}

n00b_result_t(size_t)
n00b_scan_skip_until_str(n00b_scanner_t *s, const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t start      = s->cursor;

    while (s->cursor + needle_len <= s->input_len) {
        if (memcmp(s->input + s->cursor, needle, needle_len) == 0) {
            return n00b_result_ok(size_t, s->cursor - start);
        }

        int              cp_len;
        n00b_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
    }

    // Needle not found; advance to end.
    while (s->cursor < s->input_len) {
        n00b_scan_advance(s);
    }

    return n00b_result_err(size_t, N00B_ERR_SCAN_NOT_FOUND);
}

// ============================================================================
// Mark / extract
// ============================================================================

void
n00b_scan_mark(n00b_scanner_t *s)
{
    s->mark      = s->cursor;
    s->mark_line = s->line;
    s->mark_col  = s->column;
}

n00b_string_t *
n00b_scan_extract(n00b_scanner_t *s)
{
    size_t len = s->cursor - s->mark;

    return n00b_string_from_raw(s->input + s->mark, (int64_t)len);
}

size_t
n00b_scan_mark_len(n00b_scanner_t *s)
{
    return s->cursor - s->mark;
}

// ============================================================================
// Token emission
// ============================================================================

// Internal: emit a fully-resolved token into the stream.
static void
scan_emit_internal(n00b_scanner_t *s, int64_t resolved_tid,
                   n00b_option_t(n00b_string_t *) value)
{
    n00b_token_stream_t *ts = s->stream;

    // Grow token array if needed.
    if (ts->token_count >= ts->token_cap) {
        int32_t new_cap = ts->token_cap ? ts->token_cap * 2 : 128;
        n00b_token_info_t **new_tokens = n00b_alloc_array(n00b_token_info_t *, new_cap);

        if (ts->tokens && ts->token_count > 0) {
            memcpy(new_tokens, ts->tokens,
                   (size_t)ts->token_count * sizeof(n00b_token_info_t *));
        }

        n00b_free(ts->tokens);
        ts->tokens    = new_tokens;
        ts->token_cap = new_cap;
    }

    n00b_token_info_t *tok = n00b_alloc(n00b_token_info_t);
    memset(tok, 0, sizeof(*tok));

    tok->tid             = resolved_tid;
    tok->value           = value;
    tok->file            = s->file;
    tok->line            = s->mark_line;
    tok->column          = s->mark_col;
    tok->endcol          = s->column;
    tok->index           = ts->next_index++;
    tok->leading_trivia  = s->pending_leading;
    tok->trailing_trivia = s->pending_trailing;

    s->pending_leading  = nullptr;
    s->pending_trailing = nullptr;

    ts->tokens[ts->token_count++] = tok;

    // Reset mark to current cursor.
    s->mark      = s->cursor;
    s->mark_line = s->line;
    s->mark_col  = s->column;
}

n00b_token_err_t
n00b_scan_emit(n00b_scanner_t *s)
    _kargs
{
    n00b_option_t(n00b_string_t *) contents;
    bool                          use_mark   = true;
    const char                   *token_type = nullptr;
    int64_t                       tid        = 0;
}
{
    assert(s->stream);

    // 1. Get token text.
    n00b_option_t(n00b_string_t *) value = n00b_option_none(n00b_string_t *);
    n00b_string_t *text = nullptr;
    bool have_text = false;

    if (n00b_option_is_set(contents)) {
        text = n00b_option_get(contents);
        value = contents;
        have_text = true;
    }
    else if (use_mark && s->cursor > s->mark) {
        text = n00b_scan_extract(s);
        value = n00b_option_set(n00b_string_t *, text);
        have_text = true;
    }

    // 2. Determine token ID.
    int64_t resolved_tid;

    if (tid != 0) {
        // Explicit ID provided — use directly.
        resolved_tid = tid;
    }
    else if (token_type) {
        // Named literal type: look up in grammar's literal_type_map.
        if (!s->grammar || !s->grammar->literal_type_map) {
            // No grammar — hash the type name as fallback ID.
            resolved_tid = n00b_token_id_from_text(token_type,
                                                    strlen(token_type));
        }
        else {
            n00b_string_t *tt_str = n00b_string_from_cstr(token_type);
            n00b_string_t *tt_ptr = tt_str;
            bool           found  = false;
            int64_t        val    = n00b_dict_get(s->grammar->literal_type_map,
                                                  tt_ptr, &found);

            if (!found) {
                return N00B_TOK_ERR_BAD_TYPE_NAME;
            }

            resolved_tid = val;
        }
    }
    else {
        // Fixed-text terminal: hash text and optionally validate.
        if (!have_text) {
            return N00B_TOK_ERR_NO_TEXT;
        }

        resolved_tid = n00b_token_id_from_text(text->data, text->u8_bytes);

        // Validate against grammar if present.
        if (s->grammar && s->grammar->valid_tokens) {
            if (!n00b_dict_contains(s->grammar->valid_tokens, resolved_tid)) {
                return N00B_TOK_ERR_NOT_IN_GRAMMAR;
            }
        }
    }

    // 3. Emit.
    scan_emit_internal(s, resolved_tid, value);
    return N00B_TOK_OK;
}

// ============================================================================
// Trivia helpers
// ============================================================================

void
n00b_scan_add_leading_trivia(n00b_scanner_t *s, n00b_string_t *text)
{
    n00b_trivia_t *t = make_trivia(text);

    append_trivia(&s->pending_leading, t);
}

void
n00b_scan_add_trailing_trivia(n00b_scanner_t *s, n00b_string_t *text)
{
    n00b_trivia_t *t = make_trivia(text);

    append_trivia(&s->pending_trailing, t);
}

int32_t
n00b_scan_skip_whitespace(n00b_scanner_t *s)
{
    size_t  start = s->cursor;
    int32_t count = 0;

    while (s->cursor < s->input_len) {
        int              cp_len;
        n00b_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (!cc_match(N00B_CC_WHITESPACE, cp)) {
            break;
        }

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }

        count++;
    }

    if (count > 0) {
        size_t byte_len = s->cursor - start;

        n00b_string_t *text = n00b_string_from_raw(s->input + start,
                                                    (int64_t)byte_len);
        n00b_scan_add_leading_trivia(s, text);
    }

    return count;
}

void
n00b_scan_skip_line_comment(n00b_scanner_t *s)
{
    size_t start = s->cursor;

    while (s->cursor < s->input_len) {
        int              cp_len;
        n00b_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (cp == '\n') {
            break;
        }

        s->cursor += (size_t)cp_len;
        s->column++;
    }

    size_t byte_len = s->cursor - start;

    if (byte_len > 0) {
        n00b_string_t *text = n00b_string_from_raw(s->input + start,
                                                    (int64_t)byte_len);
        n00b_scan_add_trailing_trivia(s, text);
    }
}

bool
n00b_scan_skip_block_comment(n00b_scanner_t *s,
                              const char     *opener,
                              const char     *closer)
{
    size_t opener_len = strlen(opener);
    size_t closer_len = strlen(closer);
    size_t start      = s->cursor;

    // Skip past opener.
    if (s->cursor + opener_len > s->input_len
        || memcmp(s->input + s->cursor, opener, opener_len) != 0) {
        return false;
    }

    for (size_t i = 0; i < opener_len; ) {
        int              cp_len;
        n00b_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        s->cursor += (size_t)cp_len;
        i += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
    }

    // Scan for closer.
    while (s->cursor + closer_len <= s->input_len) {
        if (memcmp(s->input + s->cursor, closer, closer_len) == 0) {
            // Skip past closer.
            for (size_t i = 0; i < closer_len; ) {
                int              cp_len;
                n00b_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

                s->cursor += (size_t)cp_len;
                i += (size_t)cp_len;

                if (cp == '\n') {
                    s->line++;
                    s->column = 1;
                }
                else {
                    s->column++;
                }
            }

            size_t byte_len = s->cursor - start;

            n00b_string_t *text = n00b_string_from_raw(s->input + start,
                                                        (int64_t)byte_len);
            n00b_scan_add_leading_trivia(s, text);
            return true;
        }

        int              cp_len;
        n00b_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
    }

    // Unterminated — consume the rest, still collect as trivia.
    while (s->cursor < s->input_len) {
        n00b_scan_advance(s);
    }

    size_t byte_len = s->cursor - start;

    n00b_string_t *text = n00b_string_from_raw(s->input + start,
                                                (int64_t)byte_len);
    n00b_scan_add_leading_trivia(s, text);
    return false;
}

// ============================================================================
// Grammar integration
// ============================================================================

bool
n00b_scan_token_valid(n00b_scanner_t *s, int64_t tid)
{
    if (!s->grammar || !s->grammar->valid_tokens) {
        return false;
    }

    return n00b_dict_contains(s->grammar->valid_tokens, tid);
}
