/**
 * @file src/parsers/toml.c
 * @brief Minimal TOML parser — implementation.
 *
 * Port of resharp-c/src/common/toml.c, types swapped at the boundary
 * to n00b's API (n00b_string_t for strings, n00b_list_t / n00b_dict_t
 * for arrays / tables, n00b_buffer_t for accumulating bytes, n00b_panic
 * for parse errors).  Internal parser state stays close to the
 * upstream algorithm.
 *
 * Scope: see parsers/toml.h.  This is a strict TOML subset chosen to
 * match what the regex-engine test fixtures use.
 */

#include "parsers/toml.h"

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include "core/rt_access.h"
#include "core/string.h"
#include "util/assert.h"
#include "util/panic.h"

#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

// ===========================================================================
// Last-error storage (per-thread).
// ===========================================================================
//
// `n00b_toml_last_error()` returns the most recent parse-error diagnostic
// for the calling thread.  Stored on the thread record so we don't add a
// new _Thread_local — matches the regex port's `regex_last_detail` slot.

static inline n00b_string_t **
toml_last_detail_slot(void)
{
    // Reuse the regex_last_detail slot — both come from the same kind of
    // last-error idiom and aren't read concurrently within a single
    // parse call.
    return &n00b_thread_self()->record->regex_last_detail;
}

static void
toml_set_last_error(n00b_string_t *msg)
{
    *toml_last_detail_slot() = msg;
}

n00b_string_t *
n00b_toml_last_error(void)
{
    return *toml_last_detail_slot();
}

// ===========================================================================
// Parse-error plumbing.
// ===========================================================================
//
// Internally a parser-error long-jumps out via `n00b_panic` (matches the
// upstream "callers never NULL-check" model for non-NULL ops).  At the
// public API boundary the caller gets `n00b_result_err(N00B_TOML_ERR_PARSE)`
// instead — handled by jmp_buf trampolines below.

#include <setjmp.h>

typedef struct {
    jmp_buf       jb;
    bool          active;
} toml_jmp_t;

// One slot is sufficient because parsing is single-threaded per call and
// recursion goes through the same trampoline.
static _Thread_local toml_jmp_t toml_jmp = { .active = false };

static inline void
toml_throw_parse_error(void)
{
    if (toml_jmp.active) {
        longjmp(toml_jmp.jb, 1);
    }
    // Top-level: no trampoline set up; degrade to panic.
    n00b_panic("toml: parse error (no trampoline active)");
}

// ===========================================================================
// Node constructors.
// ===========================================================================

static n00b_toml_node_t *
toml_new_int(int64_t v)
{
    n00b_toml_node_t *n = n00b_alloc(n00b_toml_node_t);
    n->type    = N00B_TOML_INT;
    n->integer = v;
    return n;
}

static n00b_toml_node_t *
toml_new_bool(bool v)
{
    n00b_toml_node_t *n = n00b_alloc(n00b_toml_node_t);
    n->type    = N00B_TOML_BOOL;
    n->boolean = v;
    return n;
}

static n00b_toml_node_t *
toml_new_string(n00b_string_t *s)
{
    n00b_toml_node_t *n = n00b_alloc(n00b_toml_node_t);
    n->type   = N00B_TOML_STRING;
    n->string = s;
    return n;
}

static n00b_toml_node_t *
toml_new_array(void)
{
    n00b_toml_node_t *n = n00b_alloc(n00b_toml_node_t);
    n->type  = N00B_TOML_ARRAY;
    n->array = n00b_list_new_private(n00b_toml_node_t *);
    return n;
}

static n00b_toml_node_t *
toml_new_table(void)
{
    n00b_toml_node_t *n = n00b_alloc(n00b_toml_node_t);
    n->type = N00B_TOML_TABLE;
    n00b_dict_init(&n->table,
                   .hash          = n00b_string_hash,
                   .skip_obj_hash = true);
    return n;
}

// ===========================================================================
// Parser state.
// ===========================================================================

typedef struct {
    const char *src;   // NUL-terminated input
    size_t      pos;   // byte offset into src
    size_t      line;  // 1-based
    size_t      col;   // 1-based
} toml_parser_t;

[[noreturn, gnu::format(printf, 2, 3)]]
static void
toml_error(const toml_parser_t *p, const char *fmt, ...)
{
    char    body[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    char full[512];
    snprintf(full, sizeof(full),
             "toml: parse error at line %zu column %zu: %s",
             p->line, p->col, body);
    toml_set_last_error(n00b_string_from_raw(full, (int64_t)strlen(full)));
    toml_throw_parse_error();
}

// ===========================================================================
// Lexer primitives.
// ===========================================================================

static char
toml_peek(const toml_parser_t *p)
{
    return p->src[p->pos];
}

static char
toml_peek_at(const toml_parser_t *p, size_t offset)
{
    size_t      i = 0;
    const char *s = p->src + p->pos;
    while (i < offset && s[i] != '\0') i++;
    if (i < offset) return '\0';
    return s[i];
}

static bool
toml_at_end(const toml_parser_t *p)
{
    return p->src[p->pos] == '\0';
}

static char
toml_advance(toml_parser_t *p)
{
    char c = p->src[p->pos];
    if (c == '\0') return '\0';
    p->pos += 1;
    if (c == '\n') {
        p->line += 1;
        p->col = 1;
    }
    else if (c == '\r') {
        if (p->src[p->pos] == '\n') {
            p->pos += 1;
        }
        p->line += 1;
        p->col = 1;
        c = '\n';
    }
    else {
        p->col += 1;
    }
    return c;
}

static void
toml_skip_ws(toml_parser_t *p)
{
    for (;;) {
        char c = toml_peek(p);
        if (c == ' ' || c == '\t') {
            toml_advance(p);
        }
        else {
            break;
        }
    }
}

static void
toml_skip_ws_nl_comments(toml_parser_t *p)
{
    for (;;) {
        char c = toml_peek(p);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            toml_advance(p);
        }
        else if (c == '#') {
            for (;;) {
                char d = toml_peek(p);
                if (d == '\0' || d == '\n' || d == '\r') break;
                toml_advance(p);
            }
        }
        else {
            break;
        }
    }
}

static void
toml_expect_eol(toml_parser_t *p)
{
    toml_skip_ws(p);
    char c = toml_peek(p);
    if (c == '#') {
        for (;;) {
            char d = toml_peek(p);
            if (d == '\0' || d == '\n' || d == '\r') break;
            toml_advance(p);
        }
        c = toml_peek(p);
    }
    if (c == '\0' || c == '\n' || c == '\r') return;
    toml_error(p, "expected end of line, got '%c'", c);
}

// ===========================================================================
// Byte buffer helpers — small wrappers around n00b_buffer_t for the
// "append one byte" / "append a UTF-8 codepoint" idiom the string-parsing
// helpers use.
// ===========================================================================

static void
buf_push_byte(n00b_buffer_t *buf, unsigned char b)
{
    size_t old = (size_t)n00b_buffer_len(buf);
    n00b_buffer_resize(buf, old + 1);
    buf->data[old] = (char)b;
}

static void
buf_push_utf8(n00b_buffer_t *buf, uint32_t cp)
{
    if (cp < 0x80u) {
        buf_push_byte(buf, (unsigned char)cp);
    }
    else if (cp < 0x800u) {
        buf_push_byte(buf, (unsigned char)(0xC0u | (cp >> 6)));
        buf_push_byte(buf, (unsigned char)(0x80u | (cp & 0x3Fu)));
    }
    else if (cp < 0x10000u) {
        buf_push_byte(buf, (unsigned char)(0xE0u | (cp >> 12)));
        buf_push_byte(buf, (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu)));
        buf_push_byte(buf, (unsigned char)(0x80u | (cp & 0x3Fu)));
    }
    else {
        buf_push_byte(buf, (unsigned char)(0xF0u | (cp >> 18)));
        buf_push_byte(buf, (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu)));
        buf_push_byte(buf, (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu)));
        buf_push_byte(buf, (unsigned char)(0x80u | (cp & 0x3Fu)));
    }
}

// ===========================================================================
// String parsing.
// ===========================================================================

static int
toml_hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static uint32_t
toml_read_hex(toml_parser_t *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
        char c = toml_peek(p);
        int  d = toml_hex_digit(c);
        if (d < 0) {
            toml_error(p, "expected %d hex digits in escape", n);
        }
        v = (v << 4) | (uint32_t)d;
        toml_advance(p);
    }
    return v;
}

static void
toml_parse_basic_string(toml_parser_t *p, n00b_buffer_t *buf)
{
    for (;;) {
        char c = toml_peek(p);
        if (c == '\0') {
            toml_error(p, "unterminated basic string");
        }
        if (c == '\n' || c == '\r') {
            toml_error(p, "unescaped newline in basic string");
        }
        if (c == '"') {
            toml_advance(p);
            return;
        }
        if (c == '\\') {
            toml_advance(p);
            char esc = toml_peek(p);
            switch (esc) {
            case '"':  buf_push_byte(buf, '"');  toml_advance(p); break;
            case '\\': buf_push_byte(buf, '\\'); toml_advance(p); break;
            case 'n':  buf_push_byte(buf, '\n'); toml_advance(p); break;
            case 'r':  buf_push_byte(buf, '\r'); toml_advance(p); break;
            case 't':  buf_push_byte(buf, '\t'); toml_advance(p); break;
            case 'b':  buf_push_byte(buf, '\b'); toml_advance(p); break;
            case 'f':  buf_push_byte(buf, '\f'); toml_advance(p); break;
            case '0':  buf_push_byte(buf, '\0'); toml_advance(p); break;
            case '/':  buf_push_byte(buf, '/');  toml_advance(p); break;
            case 'x': {
                toml_advance(p);
                uint32_t v = toml_read_hex(p, 2);
                buf_push_byte(buf, (unsigned char)v);
                break;
            }
            case 'u': {
                toml_advance(p);
                uint32_t v = toml_read_hex(p, 4);
                if (v >= 0xD800u && v <= 0xDFFFu) {
                    toml_error(p, "surrogate code point in \\u escape");
                }
                buf_push_utf8(buf, v);
                break;
            }
            case 'U': {
                toml_advance(p);
                uint32_t v = toml_read_hex(p, 8);
                if (v > 0x10FFFFu
                    || (v >= 0xD800u && v <= 0xDFFFu)) {
                    toml_error(p, "invalid code point in \\U escape");
                }
                buf_push_utf8(buf, v);
                break;
            }
            case '\0':
                toml_error(p, "unterminated escape sequence");
            default:
                toml_error(p, "unknown escape '\\%c'", esc);
            }
            continue;
        }
        buf_push_byte(buf, (unsigned char)c);
        toml_advance(p);
    }
}

static void
toml_parse_literal_string(toml_parser_t *p, n00b_buffer_t *buf)
{
    for (;;) {
        char c = toml_peek(p);
        if (c == '\0') {
            toml_error(p, "unterminated literal string");
        }
        if (c == '\n' || c == '\r') {
            toml_error(p, "unescaped newline in literal string");
        }
        if (c == '\'') {
            toml_advance(p);
            return;
        }
        buf_push_byte(buf, (unsigned char)c);
        toml_advance(p);
    }
}

static void
toml_parse_multiline_literal_string(toml_parser_t *p, n00b_buffer_t *buf)
{
    if (toml_peek(p) == '\n' || toml_peek(p) == '\r') {
        toml_advance(p);
    }
    for (;;) {
        char c = toml_peek(p);
        if (c == '\0') {
            toml_error(p, "unterminated multi-line literal string");
        }
        if (c == '\'') {
            if (toml_peek_at(p, 1) == '\'' && toml_peek_at(p, 2) == '\'') {
                toml_advance(p);
                toml_advance(p);
                toml_advance(p);
                if (toml_peek(p) == '\'') {
                    buf_push_byte(buf, '\'');
                    toml_advance(p);
                    if (toml_peek(p) == '\'') {
                        buf_push_byte(buf, '\'');
                        toml_advance(p);
                    }
                }
                return;
            }
            buf_push_byte(buf, '\'');
            toml_advance(p);
            continue;
        }
        buf_push_byte(buf, (unsigned char)c);
        toml_advance(p);
    }
}

static void
toml_parse_multiline_basic_string(toml_parser_t *p, n00b_buffer_t *buf)
{
    if (toml_peek(p) == '\n' || toml_peek(p) == '\r') {
        toml_advance(p);
    }
    for (;;) {
        char c = toml_peek(p);
        if (c == '\0') {
            toml_error(p, "unterminated multi-line basic string");
        }
        if (c == '"') {
            if (toml_peek_at(p, 1) == '"' && toml_peek_at(p, 2) == '"') {
                toml_advance(p);
                toml_advance(p);
                toml_advance(p);
                if (toml_peek(p) == '"') {
                    buf_push_byte(buf, '"');
                    toml_advance(p);
                    if (toml_peek(p) == '"') {
                        buf_push_byte(buf, '"');
                        toml_advance(p);
                    }
                }
                return;
            }
            buf_push_byte(buf, '"');
            toml_advance(p);
            continue;
        }
        if (c == '\\') {
            // Line-ending-backslash: backslash, any space/tab, then a
            // newline — consume the whole run plus following whitespace
            // (including newlines).
            size_t look = 1;
            for (;;) {
                char d = toml_peek_at(p, look);
                if (d == ' ' || d == '\t') { look++; continue; }
                break;
            }
            char trailing = toml_peek_at(p, look);
            if (trailing == '\n' || trailing == '\r') {
                for (size_t i = 0; i < look; ++i) toml_advance(p);
                toml_advance(p);
                for (;;) {
                    char e = toml_peek(p);
                    if (e == ' ' || e == '\t' || e == '\n' || e == '\r') {
                        toml_advance(p);
                    }
                    else {
                        break;
                    }
                }
                continue;
            }
            toml_advance(p);
            char esc = toml_peek(p);
            switch (esc) {
            case '"':  buf_push_byte(buf, '"');  toml_advance(p); break;
            case '\\': buf_push_byte(buf, '\\'); toml_advance(p); break;
            case 'n':  buf_push_byte(buf, '\n'); toml_advance(p); break;
            case 'r':  buf_push_byte(buf, '\r'); toml_advance(p); break;
            case 't':  buf_push_byte(buf, '\t'); toml_advance(p); break;
            case 'b':  buf_push_byte(buf, '\b'); toml_advance(p); break;
            case 'f':  buf_push_byte(buf, '\f'); toml_advance(p); break;
            case '0':  buf_push_byte(buf, '\0'); toml_advance(p); break;
            case '/':  buf_push_byte(buf, '/');  toml_advance(p); break;
            case 'x': {
                toml_advance(p);
                uint32_t v = toml_read_hex(p, 2);
                buf_push_byte(buf, (unsigned char)v);
                break;
            }
            case 'u': {
                toml_advance(p);
                uint32_t v = toml_read_hex(p, 4);
                if (v >= 0xD800u && v <= 0xDFFFu) {
                    toml_error(p, "surrogate code point in \\u escape");
                }
                buf_push_utf8(buf, v);
                break;
            }
            case 'U': {
                toml_advance(p);
                uint32_t v = toml_read_hex(p, 8);
                if (v > 0x10FFFFu
                    || (v >= 0xD800u && v <= 0xDFFFu)) {
                    toml_error(p, "invalid code point in \\U escape");
                }
                buf_push_utf8(buf, v);
                break;
            }
            case '\0':
                toml_error(p, "unterminated escape sequence");
            default:
                toml_error(p, "unknown escape '\\%c'", esc);
            }
            continue;
        }
        buf_push_byte(buf, (unsigned char)c);
        toml_advance(p);
    }
}

// ===========================================================================
// Key parsing — bare keys and quoted keys.  Returns an owned n00b_string_t.
// ===========================================================================

static bool
toml_is_bare_key_char(char c)
{
    return (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9')
        || c == '_' || c == '-';
}

static n00b_string_t *
toml_parse_key(toml_parser_t *p)
{
    char           c   = toml_peek(p);
    n00b_buffer_t *buf = n00b_buffer_empty();
    if (c == '"') {
        toml_advance(p);
        toml_parse_basic_string(p, buf);
    }
    else if (c == '\'') {
        toml_advance(p);
        toml_parse_literal_string(p, buf);
    }
    else if (toml_is_bare_key_char(c)) {
        while (toml_is_bare_key_char(toml_peek(p))) {
            buf_push_byte(buf, (unsigned char)toml_peek(p));
            toml_advance(p);
        }
    }
    else {
        toml_error(p, "expected key, got '%c'", c);
    }
    if (n00b_buffer_len(buf) == 0) {
        toml_error(p, "empty key");
    }
    return n00b_buffer_to_string(buf);
}

// ===========================================================================
// Value parsing.
// ===========================================================================

static n00b_toml_node_t *toml_parse_value(toml_parser_t *p);

static n00b_toml_node_t *
toml_parse_integer(toml_parser_t *p)
{
    bool neg = false;
    char c   = toml_peek(p);
    if (c == '+' || c == '-') {
        neg = (c == '-');
        toml_advance(p);
        c = toml_peek(p);
    }
    if (!(c >= '0' && c <= '9')) {
        toml_error(p, "expected digit, got '%c'", c);
    }

    uint64_t acc                 = 0;
    bool     saw_digit           = false;
    bool     last_was_underscore = false;
    for (;;) {
        char d = toml_peek(p);
        if (d == '_') {
            if (!saw_digit || last_was_underscore) {
                toml_error(p, "underscore must be between digits");
            }
            last_was_underscore = true;
            toml_advance(p);
            continue;
        }
        if (!(d >= '0' && d <= '9')) break;
        uint64_t digit = (uint64_t)(d - '0');
        uint64_t cap   = neg ? (uint64_t)9223372036854775808ULL
                             : (uint64_t)9223372036854775807ULL;
        if (acc > cap / 10ULL
            || (acc == cap / 10ULL && digit > cap % 10ULL)) {
            toml_error(p, "integer literal out of range");
        }
        acc                 = acc * 10ULL + digit;
        saw_digit           = true;
        last_was_underscore = false;
        toml_advance(p);
    }
    if (!saw_digit) {
        toml_error(p, "integer literal has no digits");
    }
    if (last_was_underscore) {
        toml_error(p, "trailing underscore in integer literal");
    }

    int64_t out;
    if (neg) {
        if (acc == (uint64_t)9223372036854775808ULL) {
            out = INT64_MIN;
        }
        else {
            out = -(int64_t)acc;
        }
    }
    else {
        out = (int64_t)acc;
    }
    return toml_new_int(out);
}

static bool
toml_match_keyword(toml_parser_t *p, const char *kw)
{
    size_t n = strlen(kw);
    for (size_t i = 0; i < n; ++i) {
        if (toml_peek_at(p, i) != kw[i]) return false;
    }
    char after = toml_peek_at(p, n);
    if (toml_is_bare_key_char(after)) return false;
    for (size_t i = 0; i < n; ++i) toml_advance(p);
    return true;
}

static n00b_toml_node_t *
toml_parse_array(toml_parser_t *p)
{
    n00b_require(toml_peek(p) == '[',
                 "toml_parse_array: caller must position at '['");
    toml_advance(p);

    n00b_toml_node_t *arr = toml_new_array();
    for (;;) {
        toml_skip_ws_nl_comments(p);
        char c = toml_peek(p);
        if (c == ']') {
            toml_advance(p);
            return arr;
        }
        if (c == '\0') {
            toml_error(p, "unterminated array");
        }
        n00b_toml_node_t *elem = toml_parse_value(p);
        n00b_list_push(arr->array, elem);
        toml_skip_ws_nl_comments(p);
        c = toml_peek(p);
        if (c == ',') {
            toml_advance(p);
            continue;
        }
        if (c == ']') {
            toml_advance(p);
            return arr;
        }
        toml_error(p, "expected ',' or ']' in array, got '%c'", c);
    }
}

static n00b_toml_node_t *
toml_parse_value(toml_parser_t *p)
{
    toml_skip_ws(p);
    char c = toml_peek(p);
    if (c == '"') {
        if (toml_peek_at(p, 1) == '"' && toml_peek_at(p, 2) == '"') {
            toml_advance(p);
            toml_advance(p);
            toml_advance(p);
            n00b_buffer_t *buf = n00b_buffer_empty();
            toml_parse_multiline_basic_string(p, buf);
            return toml_new_string(n00b_buffer_to_string(buf));
        }
        toml_advance(p);
        n00b_buffer_t *buf = n00b_buffer_empty();
        toml_parse_basic_string(p, buf);
        return toml_new_string(n00b_buffer_to_string(buf));
    }
    if (c == '\'') {
        if (toml_peek_at(p, 1) == '\'' && toml_peek_at(p, 2) == '\'') {
            toml_advance(p);
            toml_advance(p);
            toml_advance(p);
            n00b_buffer_t *buf = n00b_buffer_empty();
            toml_parse_multiline_literal_string(p, buf);
            return toml_new_string(n00b_buffer_to_string(buf));
        }
        toml_advance(p);
        n00b_buffer_t *buf = n00b_buffer_empty();
        toml_parse_literal_string(p, buf);
        return toml_new_string(n00b_buffer_to_string(buf));
    }
    if (c == '[') {
        return toml_parse_array(p);
    }
    if (c == '+' || c == '-' || (c >= '0' && c <= '9')) {
        return toml_parse_integer(p);
    }
    if (c == 't' && toml_match_keyword(p, "true"))  return toml_new_bool(true);
    if (c == 'f' && toml_match_keyword(p, "false")) return toml_new_bool(false);
    toml_error(p, "expected value, got '%c'", c);
}

// ===========================================================================
// Statement parsing.
// ===========================================================================

typedef n00b_dict_t(n00b_string_t *, n00b_toml_node_t *) toml_table_dict_t;

static n00b_toml_node_t *
toml_table_lookup(const n00b_toml_node_t *tbl, n00b_string_t *key)
{
    bool found = false;
    n00b_toml_node_t *v = n00b_dict_get(
        (toml_table_dict_t *)&((n00b_toml_node_t *)tbl)->table, key, &found);
    return found ? v : nullptr;
}

static void
toml_table_insert(n00b_toml_node_t *tbl,
                  n00b_string_t    *key,
                  n00b_toml_node_t *val)
{
    n00b_dict_put(&tbl->table, key, val);
}

static void
toml_parse_assignment(toml_parser_t *p, n00b_toml_node_t *cur_tbl)
{
    n00b_string_t *key = toml_parse_key(p);
    toml_skip_ws(p);
    if (toml_peek(p) != '=') {
        toml_error(p, "expected '=' after key");
    }
    toml_advance(p);
    n00b_toml_node_t *val = toml_parse_value(p);
    if (toml_table_lookup(cur_tbl, key) != nullptr) {
        toml_error(p, "duplicate key \"%s\"", key->data);
    }
    toml_table_insert(cur_tbl, key, val);
    toml_expect_eol(p);
}

static n00b_toml_node_t *
toml_parse_header(toml_parser_t *p, n00b_toml_node_t *root)
{
    n00b_require(toml_peek(p) == '[',
                 "toml_parse_header: caller must position at '['");
    toml_advance(p);
    bool is_array_of_tables = false;
    if (toml_peek(p) == '[') {
        is_array_of_tables = true;
        toml_advance(p);
    }
    toml_skip_ws(p);
    n00b_string_t *name = toml_parse_key(p);
    toml_skip_ws(p);
    if (toml_peek(p) != ']') {
        toml_error(p, "expected ']' in table header");
    }
    toml_advance(p);
    if (is_array_of_tables) {
        if (toml_peek(p) != ']') {
            toml_error(p, "expected ']]' in array-of-tables header");
        }
        toml_advance(p);
    }
    toml_expect_eol(p);

    if (is_array_of_tables) {
        n00b_toml_node_t *arr = toml_table_lookup(root, name);
        if (arr == nullptr) {
            arr = toml_new_array();
            toml_table_insert(root, name, arr);
        }
        else if (arr->type != N00B_TOML_ARRAY) {
            toml_error(p,
                       "[[...]] header conflicts with prior key kind");
        }
        n00b_toml_node_t *fresh = toml_new_table();
        n00b_list_push(arr->array, fresh);
        return fresh;
    }

    if (toml_table_lookup(root, name) != nullptr) {
        toml_error(p, "duplicate table header");
    }
    n00b_toml_node_t *fresh = toml_new_table();
    toml_table_insert(root, name, fresh);
    return fresh;
}

// ===========================================================================
// Top-level parse driver.
// ===========================================================================

n00b_result_t(n00b_toml_node_t *)
n00b_toml_parse(n00b_string_t *src)
{
    n00b_require(src != nullptr, "n00b_toml_parse: src");
    toml_set_last_error(nullptr);

    // Set up the parse-error trampoline.
    toml_jmp.active = true;
    if (setjmp(toml_jmp.jb) != 0) {
        toml_jmp.active = false;
        return n00b_result_err(n00b_toml_node_t *, N00B_TOML_ERR_PARSE);
    }

    toml_parser_t p = {
        .src  = (const char *)src->data,
        .pos  = 0,
        .line = 1,
        .col  = 1,
    };
    n00b_toml_node_t *root = toml_new_table();
    n00b_toml_node_t *cur  = root;

    for (;;) {
        toml_skip_ws_nl_comments(&p);
        if (toml_at_end(&p)) break;
        char c = toml_peek(&p);
        if (c == '[') {
            cur = toml_parse_header(&p, root);
        }
        else {
            toml_parse_assignment(&p, cur);
        }
    }
    toml_jmp.active = false;
    return n00b_result_ok(n00b_toml_node_t *, root);
}

// ===========================================================================
// File-driver: slurp `path` via the conduit subsystem then parse.
// ===========================================================================

n00b_result_t(n00b_toml_node_t *)
n00b_toml_parse_file(n00b_string_t *path)
{
    n00b_require(path != nullptr, "n00b_toml_parse_file: path");

    // Synchronous slurp via POSIX read into an n00b_buffer_t — same
    // pattern n00b uses internally for whole-file reads (see
    // src/vfs/cache.c:read_file).  The conduit IO event loop is for
    // streaming / async clients; for a one-shot fixture read it's
    // overhead we don't need.
    int fd = open((const char *)path->data, O_RDONLY);
    if (fd < 0) {
        toml_set_last_error(n00b_string_from_cstr(
            "toml: file open failed"));
        return n00b_result_err(n00b_toml_node_t *, N00B_TOML_ERR_IO);
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        toml_set_last_error(n00b_string_from_cstr("toml: fstat failed"));
        return n00b_result_err(n00b_toml_node_t *, N00B_TOML_ERR_IO);
    }
    size_t         size = (size_t)st.st_size;
    n00b_buffer_t *buf  = n00b_buffer_new((int64_t)size);
    if (size > 0) {
        n00b_buffer_resize(buf, size);
        char  *dst   = buf->data;
        size_t total = 0;
        while (total < size) {
            ssize_t r = read(fd, dst + total, size - total);
            if (r <= 0) break;
            total += (size_t)r;
        }
        if (total != size) {
            close(fd);
            toml_set_last_error(n00b_string_from_cstr("toml: short read"));
            return n00b_result_err(n00b_toml_node_t *, N00B_TOML_ERR_IO);
        }
    }
    close(fd);

    n00b_string_t *src = n00b_buffer_to_string(buf);
    return n00b_toml_parse(src);
}

// ===========================================================================
// Public accessors.
// ===========================================================================

int64_t
n00b_toml_as_int(const n00b_toml_node_t *v)
{
    n00b_require(v != nullptr, "n00b_toml_as_int: v");
    n00b_require(v->type == N00B_TOML_INT, "n00b_toml_as_int: type mismatch");
    return v->integer;
}

bool
n00b_toml_as_bool(const n00b_toml_node_t *v)
{
    n00b_require(v != nullptr, "n00b_toml_as_bool: v");
    n00b_require(v->type == N00B_TOML_BOOL, "n00b_toml_as_bool: type mismatch");
    return v->boolean;
}

n00b_string_t *
n00b_toml_as_string(const n00b_toml_node_t *v)
{
    n00b_require(v != nullptr, "n00b_toml_as_string: v");
    n00b_require(v->type == N00B_TOML_STRING,
                 "n00b_toml_as_string: type mismatch");
    return v->string;
}

size_t
n00b_toml_array_len(const n00b_toml_node_t *v)
{
    n00b_require(v != nullptr, "n00b_toml_array_len: v");
    n00b_require(v->type == N00B_TOML_ARRAY,
                 "n00b_toml_array_len: type mismatch");
    return n00b_list_len(((n00b_toml_node_t *)v)->array);
}

n00b_toml_node_t *
n00b_toml_array_get(const n00b_toml_node_t *v, size_t i)
{
    n00b_require(v != nullptr, "n00b_toml_array_get: v");
    n00b_require(v->type == N00B_TOML_ARRAY,
                 "n00b_toml_array_get: type mismatch");
    size_t n = n00b_list_len(((n00b_toml_node_t *)v)->array);
    n00b_require(i < n, "n00b_toml_array_get: out of bounds");
    return n00b_list_get(((n00b_toml_node_t *)v)->array, i);
}

n00b_toml_node_t *
n00b_toml_table_get(const n00b_toml_node_t *v, n00b_string_t *key)
{
    n00b_require(v != nullptr, "n00b_toml_table_get: v");
    n00b_require(v->type == N00B_TOML_TABLE,
                 "n00b_toml_table_get: type mismatch");
    return toml_table_lookup(v, key);
}

n00b_toml_node_t *
n00b_toml_table_get_cstr(const n00b_toml_node_t *v, const char *key)
{
    return n00b_toml_table_get(v, n00b_string_from_cstr(key));
}

n00b_toml_node_t *
n00b_toml_table_array_of(const n00b_toml_node_t *v, const char *name)
{
    n00b_toml_node_t *got = n00b_toml_table_get_cstr(v, name);
    if (got == nullptr) return nullptr;
    n00b_require(got->type == N00B_TOML_ARRAY,
                 "n00b_toml_table_array_of: not an array-of-tables");
    return got;
}
