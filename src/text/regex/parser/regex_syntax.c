// regex_syntax.c — typed translation of resharp-c's hir::translate path.
//
// Per § 0a-Z + § 7.5: the algorithm tracks upstream Rust regex-syntax 0.8.10
// (Translator/Hir/Utf8Sequences + the sorted-range interval-set / ClassSet
// recursion).  Every primitive is translated to n00b at the call site:
//
//   xmalloc / xcalloc / xrealloc -> n00b_alloc / n00b_alloc_array +
//                                   alloc-new + memcpy (D13) + n00b_free
//   xfree                        -> n00b_free
//   xstrdup                      -> n00b_alloc_array(char, n+1) + memcpy
//   PANIC / PANIC_FMT            -> n00b_panic(fmt, ...)            (D9)
//   REQUIRE / NOT_NULL           -> n00b_require(cond, msg)         (D8)
//   pthread Mutex / MUTEX_INIT   -> n00b_mutex_t + sys_mutex_init,
//                                   guarded by an atomic state flag for
//                                   the lazy double-checked init pattern
//   _Atomic(bool) direct ops     -> n00b_atomic_load / n00b_atomic_store /
//                                   n00b_atomic_cas (D4)
//   ckd_mul_sz / ckd_add_sz      -> <stdckdint.h>'s ckd_mul / ckd_add
//                                   directly (§ 15(C))
//   regex_syntax_unicode_tables  -> DROP entirely.  The Phase 4.5 unicode
//                                   range/by-name surface (n00b_unicode_*)
//                                   replaces the per-property tables.  The
//                                   simple case-fold equivalence table is
//                                   built lazily by inverting
//                                   n00b_unicode_casefold_cp() over the
//                                   full codepoint space.
//
// Cross-file dependency: the rs_* AST POD types (rs_Span, rs_Literal,
// rs_ClassUnicode, rs_ClassBracketed, rs_ClassSet, rs_Translator, rs_Hir,
// rs_Utf8Sequences …) are declared in `internal/regex/ast.h`, ported in
// parallel as Phase 6/ast.c per § 7.5.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdckdint.h>
#include <string.h>  // memcpy / memmove (D13)
#include <stdatomic.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/atomic.h"
#include "core/runtime.h"
#include "core/rt_access.h"
#include "core/thread.h"  // n00b_thread_id, transitively required by core/lock_common.h
#include "core/mutex.h"
#include "util/assert.h"
#include "util/panic.h"
#include "util/utf8.h"
#include "text/regex/ctx.h"
#include "text/unicode/types.h"
#include "text/unicode/properties.h"
#include "text/unicode/query.h"
#include "text/unicode/segmentation.h"
#include "text/unicode/casemap.h"

#include "internal/regex/ast.h"

// ===========================================================================
// Capacity helpers — checked-arithmetic wrappers around <stdckdint.h>.
// On overflow we route to n00b_panic so the abort path is loud and consistent
// (mirrors algebra.c).
// ===========================================================================

[[noreturn]] static inline void
regex_syntax_capacity_overflow(void)
{
    n00b_panic("regex_syntax.c: capacity overflow");
}

static inline size_t
safe_mul_sz(size_t a, size_t b)
{
    size_t r;
    if (ckd_mul(&r, a, b)) {
        regex_syntax_capacity_overflow();
    }
    return r;
}

static inline size_t
safe_add_sz(size_t a, size_t b)
{
    size_t r;
    if (ckd_add(&r, a, b)) {
        regex_syntax_capacity_overflow();
    }
    return r;
}

// ===========================================================================
// Helpers
// ===========================================================================

// xstrdup_or_null — used for the heap-owned name/value strings in
// rs_ClassUnicode.  Per § 7.5 char *-internal narrow license, we keep these
// as plain char * inside the parser AST.
static char *
strdup_or_null(const char *s)
{
    if (!s) {
        return nullptr;
    }
    size_t n   = strlen(s);
    char  *out = n00b_alloc_array(char, safe_add_sz(n, 1));
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static char *
strdup_cstr(const char *s)
{
    size_t n   = strlen(s);
    char  *out = n00b_alloc_array(char, safe_add_sz(n, 1));
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

// UTF-8 encode a scalar value into out[]; returns byte length (1..4).  This
// is the same algorithm as n00b_utf8_encode_codepoint() from util/utf8.h,
// but local copy preserves the surrogate handling resharp-c relied on (no
// surrogate validation — surrogate halves are encoded byte-for-byte; the
// caller has already filtered them).  Match resharp-c's contract.
static size_t
encode_utf8(uint32_t c, uint8_t out[4])
{
    if (c < 0x80u) {
        out[0] = (uint8_t)c;
        return 1;
    }
    if (c < 0x800u) {
        out[0] = (uint8_t)(0xC0u | (c >> 6));
        out[1] = (uint8_t)(0x80u | (c & 0x3Fu));
        return 2;
    }
    if (c < 0x10000u) {
        out[0] = (uint8_t)(0xE0u | (c >> 12));
        out[1] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu));
        out[2] = (uint8_t)(0x80u | (c & 0x3Fu));
        return 3;
    }
    out[0] = (uint8_t)(0xF0u | (c >> 18));
    out[1] = (uint8_t)(0x80u | ((c >> 12) & 0x3Fu));
    out[2] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu));
    out[3] = (uint8_t)(0x80u | (c & 0x3Fu));
    return 4;
}

// ===========================================================================
// Span / Position
// ===========================================================================

rs_Position
rs_Position_new(size_t offset, size_t line, size_t column)
{
    return (rs_Position){.offset = offset, .line = line, .column = column};
}

rs_Span
rs_Span_new(rs_Position start, rs_Position end)
{
    return (rs_Span){.start = start, .end = end};
}

rs_Span
rs_Span_splat(rs_Position p)
{
    return (rs_Span){.start = p, .end = p};
}

// regex-syntax AST has no `combine` method — it is a parser-side helper
// that builds a fresh span from two positions.  Equivalent to Span_new.
rs_Span
rs_Span_combine(rs_Position start, rs_Position end)
{
    return (rs_Span){.start = start, .end = end};
}

// ===========================================================================
// Literal
// ===========================================================================

rs_Literal
rs_Literal_make(rs_Span span, rs_LiteralKind kind, uint32_t c)
{
    return (rs_Literal){
        .span         = span,
        .kind         = kind,
        .hex_kind     = RS_HEX_LITERAL_X,
        .special_kind = RS_SPECIAL_LITERAL_BELL,
        .c            = c,
    };
}

rs_Literal
rs_Literal_make_special(rs_Span span, rs_SpecialLiteralKind sk, uint32_t c)
{
    return (rs_Literal){
        .span         = span,
        .kind         = RS_LITERAL_KIND_SPECIAL,
        .hex_kind     = RS_HEX_LITERAL_X,
        .special_kind = sk,
        .c            = c,
    };
}

rs_Literal
rs_Literal_make_hex_fixed(rs_Span span, rs_HexLiteralKind hk, uint32_t c)
{
    return (rs_Literal){
        .span         = span,
        .kind         = RS_LITERAL_KIND_HEX_FIXED,
        .hex_kind     = hk,
        .special_kind = RS_SPECIAL_LITERAL_BELL,
        .c            = c,
    };
}

rs_Literal
rs_Literal_make_hex_brace(rs_Span span, rs_HexLiteralKind hk, uint32_t c)
{
    return (rs_Literal){
        .span         = span,
        .kind         = RS_LITERAL_KIND_HEX_BRACE,
        .hex_kind     = hk,
        .special_kind = RS_SPECIAL_LITERAL_BELL,
        .c            = c,
    };
}

rs_Literal
rs_Literal_make_octal(rs_Span span, uint32_t c)
{
    return (rs_Literal){
        .span         = span,
        .kind         = RS_LITERAL_KIND_OCTAL,
        .hex_kind     = RS_HEX_LITERAL_X,
        .special_kind = RS_SPECIAL_LITERAL_BELL,
        .c            = c,
    };
}

void
rs_Literal_set_span_start(rs_Literal *l, rs_Position start)
{
    n00b_require(l != nullptr, "rs_Literal_set_span_start: l");
    l->span.start = start;
}

const rs_Span *
rs_Literal_span(const rs_Literal *l)
{
    n00b_require(l != nullptr, "rs_Literal_span: l");
    return &l->span;
}

rs_Literal
rs_Literal_clone(const rs_Literal *l)
{
    n00b_require(l != nullptr, "rs_Literal_clone: l");
    return *l;
}

// ===========================================================================
// HexLiteralKind
// ===========================================================================

size_t
rs_HexLiteralKind_digits(rs_HexLiteralKind k)
{
    switch (k) {
    case RS_HEX_LITERAL_X:
        return 2;
    case RS_HEX_LITERAL_UNICODE_SHORT:
        return 4;
    case RS_HEX_LITERAL_UNICODE_LONG:
        return 8;
    }
    n00b_panic("rs_HexLiteralKind_digits: invalid kind");
}

// ===========================================================================
// ClassPerl
// ===========================================================================

rs_ClassPerl
rs_ClassPerl_make(rs_Span span, rs_ClassPerlKind kind, bool negated)
{
    return (rs_ClassPerl){.span = span, .kind = kind, .negated = negated};
}

void
rs_ClassPerl_set_span_start(rs_ClassPerl *c, rs_Position start)
{
    n00b_require(c != nullptr, "rs_ClassPerl_set_span_start: c");
    c->span.start = start;
}

const rs_Span *
rs_ClassPerl_span(const rs_ClassPerl *c)
{
    n00b_require(c != nullptr, "rs_ClassPerl_span: c");
    return &c->span;
}

// ===========================================================================
// ClassAscii / ClassAsciiKind (regex-syntax AST:810,854)
// ===========================================================================

rs_ClassAscii
rs_ClassAscii_make(rs_Span span, int kind, bool negated)
{
    return (rs_ClassAscii){.span = span, .kind = kind, .negated = negated};
}

bool
rs_ClassAsciiKind_from_name(const char *name, size_t n, int *out_kind)
{
    n00b_require(name != nullptr, "rs_ClassAsciiKind_from_name: name");
    n00b_require(out_kind != nullptr, "rs_ClassAsciiKind_from_name: out_kind");
    struct entry {
        const char *s;
        int         k;
    };
    static const struct entry table[] = {
        {"alnum",  RS_CLASS_ASCII_KIND_ALNUM },
        {"alpha",  RS_CLASS_ASCII_KIND_ALPHA },
        {"ascii",  RS_CLASS_ASCII_KIND_ASCII },
        {"blank",  RS_CLASS_ASCII_KIND_BLANK },
        {"cntrl",  RS_CLASS_ASCII_KIND_CNTRL },
        {"digit",  RS_CLASS_ASCII_KIND_DIGIT },
        {"graph",  RS_CLASS_ASCII_KIND_GRAPH },
        {"lower",  RS_CLASS_ASCII_KIND_LOWER },
        {"print",  RS_CLASS_ASCII_KIND_PRINT },
        {"punct",  RS_CLASS_ASCII_KIND_PUNCT },
        {"space",  RS_CLASS_ASCII_KIND_SPACE },
        {"upper",  RS_CLASS_ASCII_KIND_UPPER },
        {"word",   RS_CLASS_ASCII_KIND_WORD  },
        {"xdigit", RS_CLASS_ASCII_KIND_XDIGIT},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        size_t slen = strlen(table[i].s);
        if (slen == n && memcmp(name, table[i].s, n) == 0) {
            *out_kind = table[i].k;
            return true;
        }
    }
    return false;
}

// ===========================================================================
// ClassUnicode
// ===========================================================================

rs_ClassUnicode
rs_ClassUnicode_make_named(rs_Span span, bool negated, char *name_owned)
{
    return (rs_ClassUnicode){
        .span       = span,
        .negated    = negated,
        .kind_tag   = RS_CLASS_UNICODE_KIND_NAMED,
        .one_letter = 0,
        .name       = name_owned,
        .value      = nullptr,
        .op         = RS_CLASS_UNICODE_OP_EQUAL,
    };
}

rs_ClassUnicode
rs_ClassUnicode_make_named_value(rs_Span span, bool negated,
                                 rs_ClassUnicodeOpKind op,
                                 char *name_owned, char *value_owned)
{
    return (rs_ClassUnicode){
        .span       = span,
        .negated    = negated,
        .kind_tag   = RS_CLASS_UNICODE_KIND_NAMED_VALUE,
        .one_letter = 0,
        .name       = name_owned,
        .value      = value_owned,
        .op         = op,
    };
}

rs_ClassUnicode
rs_ClassUnicode_make_one_letter(rs_Span span, bool negated, uint32_t c)
{
    return (rs_ClassUnicode){
        .span       = span,
        .negated    = negated,
        .kind_tag   = RS_CLASS_UNICODE_KIND_ONE_LETTER,
        .one_letter = c,
        .name       = nullptr,
        .value      = nullptr,
        .op         = RS_CLASS_UNICODE_OP_EQUAL,
    };
}

void
rs_ClassUnicode_set_span_start(rs_ClassUnicode *c, rs_Position start)
{
    n00b_require(c != nullptr, "rs_ClassUnicode_set_span_start: c");
    c->span.start = start;
}

const rs_Span *
rs_ClassUnicode_span(const rs_ClassUnicode *c)
{
    n00b_require(c != nullptr, "rs_ClassUnicode_span: c");
    return &c->span;
}

rs_ClassUnicode
rs_ClassUnicode_clone(const rs_ClassUnicode *c)
{
    n00b_require(c != nullptr, "rs_ClassUnicode_clone: c");
    return (rs_ClassUnicode){
        .span       = c->span,
        .negated    = c->negated,
        .kind_tag   = c->kind_tag,
        .one_letter = c->one_letter,
        .name       = strdup_or_null(c->name),
        .value      = strdup_or_null(c->value),
        .op         = c->op,
    };
}

// lib.c declares this with a local enum return type; we match by integer.
typedef enum {
    LOCAL_RS_CUI_NAMED,
    LOCAL_RS_CUI_NAMED_VALUE,
    LOCAL_RS_CUI_ONE_LETTER,
} local_rs_ClassUnicode_kind_tag;

local_rs_ClassUnicode_kind_tag
rs_ClassUnicode_kind_tag_of(const rs_ClassUnicode *c)
{
    n00b_require(c != nullptr, "rs_ClassUnicode_kind_tag_of: c");
    switch (c->kind_tag) {
    case RS_CLASS_UNICODE_KIND_NAMED:
        return LOCAL_RS_CUI_NAMED;
    case RS_CLASS_UNICODE_KIND_NAMED_VALUE:
        return LOCAL_RS_CUI_NAMED_VALUE;
    case RS_CLASS_UNICODE_KIND_ONE_LETTER:
        return LOCAL_RS_CUI_ONE_LETTER;
    }
    n00b_panic("rs_ClassUnicode_kind_tag_of: invalid tag");
}

const char *
rs_ClassUnicode_named(const rs_ClassUnicode *c, size_t *len)
{
    n00b_require(c != nullptr, "rs_ClassUnicode_named: c");
    n00b_require(len != nullptr, "rs_ClassUnicode_named: len");
    if (c->kind_tag == RS_CLASS_UNICODE_KIND_NAMED && c->name) {
        *len = strlen(c->name);
        return c->name;
    }
    *len = 0;
    return nullptr;
}

// ===========================================================================
// ClassSetRange
// ===========================================================================

rs_ClassSetRange
rs_ClassSetRange_make(rs_Span span, rs_Literal start, rs_Literal end)
{
    return (rs_ClassSetRange){.span = span, .start = start, .end = end};
}

bool
rs_ClassSetRange_is_valid(const rs_ClassSetRange *r)
{
    n00b_require(r != nullptr, "rs_ClassSetRange_is_valid: r");
    return r->start.c <= r->end.c;
}

// ===========================================================================
// ClassSetUnion
// ===========================================================================

rs_ClassSetUnion
rs_ClassSetUnion_new(rs_Span span)
{
    return (rs_ClassSetUnion){
        .span  = span,
        .items = nullptr,
        .len   = 0,
        .cap   = 0,
    };
}

static const rs_Span *class_set_item_span(const rs_ClassSetItem *it);

void
rs_ClassSetUnion_push(rs_ClassSetUnion *u, rs_ClassSetItem item)
{
    n00b_require(u != nullptr, "rs_ClassSetUnion_push: u");
    if (u->len == 0) {
        const rs_Span *isp = class_set_item_span(&item);
        u->span.start      = isp->start;
    }
    {
        const rs_Span *isp = class_set_item_span(&item);
        u->span.end        = isp->end;
    }
    if (u->len == u->cap) {
        size_t newcap = u->cap == 0 ? 4 : safe_mul_sz(u->cap, 2);
        rs_ClassSetItem *new_items = n00b_alloc_array(rs_ClassSetItem, newcap);
        if (u->len > 0 && u->items != nullptr) {
            memcpy(new_items, u->items,
                   safe_mul_sz(u->len, sizeof(rs_ClassSetItem)));
        }
        if (u->items != nullptr) {
            n00b_free(u->items);
        }
        u->items = new_items;
        u->cap   = newcap;
    }
    u->items[u->len++] = item;
}

rs_ClassSetItem
rs_ClassSetUnion_into_item(rs_ClassSetUnion u)
{
    // regex-syntax AST:1264 — len 0 -> Empty(span); 1 -> first; else Union.
    if (u.len == 0) {
        rs_ClassSetItem out = (rs_ClassSetItem){.tag = RS_CLASS_SET_ITEM_EMPTY};
        out.empty_span      = u.span;
        if (u.items) {
            n00b_free(u.items);
        }
        return out;
    }
    if (u.len == 1) {
        rs_ClassSetItem only = u.items[0];
        n00b_free(u.items);
        return only;
    }
    rs_ClassSetItem out = (rs_ClassSetItem){.tag = RS_CLASS_SET_ITEM_UNION};
    out.union_          = u;
    return out;
}

bool
rs_ClassSetUnion_is_empty(const rs_ClassSetUnion *u)
{
    n00b_require(u != nullptr, "rs_ClassSetUnion_is_empty: u");
    return u->len == 0;
}

rs_Position
rs_ClassSetUnion_span_start(const rs_ClassSetUnion *u)
{
    n00b_require(u != nullptr, "rs_ClassSetUnion_span_start: u");
    return u->span.start;
}

// ===========================================================================
// ClassSetItem
// ===========================================================================

static const rs_Span *
class_set_item_span(const rs_ClassSetItem *it)
{
    n00b_require(it != nullptr, "class_set_item_span: it");
    switch (it->tag) {
    case RS_CLASS_SET_ITEM_EMPTY:
        return &it->empty_span;
    case RS_CLASS_SET_ITEM_LITERAL:
        return &it->literal.span;
    case RS_CLASS_SET_ITEM_RANGE:
        return &it->range->span;
    case RS_CLASS_SET_ITEM_ASCII:
        return &it->ascii.span;
    case RS_CLASS_SET_ITEM_UNICODE:
        return &it->unicode.span;
    case RS_CLASS_SET_ITEM_PERL:
        return &it->perl.span;
    case RS_CLASS_SET_ITEM_BRACKETED:
        return &it->bracketed->span;
    case RS_CLASS_SET_ITEM_UNION:
        return &it->union_.span;
    }
    n00b_panic("class_set_item_span: invalid tag");
}

rs_ClassSetItem
rs_ClassSetItem_literal(rs_Literal l)
{
    rs_ClassSetItem it = (rs_ClassSetItem){.tag = RS_CLASS_SET_ITEM_LITERAL};
    it.literal         = l;
    return it;
}

rs_ClassSetItem
rs_ClassSetItem_perl(rs_ClassPerl c)
{
    rs_ClassSetItem it = (rs_ClassSetItem){.tag = RS_CLASS_SET_ITEM_PERL};
    it.perl            = c;
    return it;
}

rs_ClassSetItem
rs_ClassSetItem_unicode(rs_ClassUnicode c)
{
    rs_ClassSetItem it = (rs_ClassSetItem){.tag = RS_CLASS_SET_ITEM_UNICODE};
    it.unicode         = c;
    return it;
}

rs_ClassSetItem
rs_ClassSetItem_ascii(rs_ClassAscii a)
{
    rs_ClassSetItem it = (rs_ClassSetItem){.tag = RS_CLASS_SET_ITEM_ASCII};
    it.ascii           = a;
    return it;
}

rs_ClassSetItem
rs_ClassSetItem_range(rs_ClassSetRange r)
{
    rs_ClassSetItem it = (rs_ClassSetItem){.tag = RS_CLASS_SET_ITEM_RANGE};
    it.range           = n00b_alloc(rs_ClassSetRange);
    *it.range          = r;
    return it;
}

rs_ClassSetItem
rs_ClassSetItem_bracketed(rs_ClassBracketed b)
{
    rs_ClassSetItem it = (rs_ClassSetItem){.tag = RS_CLASS_SET_ITEM_BRACKETED};
    it.bracketed       = n00b_alloc(rs_ClassBracketed);
    *it.bracketed      = b;
    return it;
}

bool
rs_ClassSetItem_is_perl(const rs_ClassSetItem *i)
{
    n00b_require(i != nullptr, "rs_ClassSetItem_is_perl: i");
    return i->tag == RS_CLASS_SET_ITEM_PERL;
}

rs_ClassPerl
rs_ClassSetItem_as_perl(const rs_ClassSetItem *i)
{
    n00b_require(i != nullptr, "rs_ClassSetItem_as_perl: i");
    n00b_require(i->tag == RS_CLASS_SET_ITEM_PERL,
                 "rs_ClassSetItem_as_perl: not perl");
    return i->perl;
}

bool
rs_ClassSetItem_is_union(const rs_ClassSetItem *i)
{
    n00b_require(i != nullptr, "rs_ClassSetItem_is_union: i");
    return i->tag == RS_CLASS_SET_ITEM_UNION;
}

size_t
rs_ClassSetItem_union_len(const rs_ClassSetItem *i)
{
    n00b_require(i != nullptr, "rs_ClassSetItem_union_len: i");
    n00b_require(i->tag == RS_CLASS_SET_ITEM_UNION,
                 "rs_ClassSetItem_union_len: not union");
    return i->union_.len;
}

const rs_ClassSetItem *
rs_ClassSetItem_union_at(const rs_ClassSetItem *i, size_t k)
{
    n00b_require(i != nullptr, "rs_ClassSetItem_union_at: i");
    n00b_require(i->tag == RS_CLASS_SET_ITEM_UNION,
                 "rs_ClassSetItem_union_at: not union");
    n00b_require(k < i->union_.len, "rs_ClassSetItem_union_at: out of range");
    return &i->union_.items[k];
}

// ===========================================================================
// ClassSet
// ===========================================================================

rs_ClassSet
rs_ClassSet_item(rs_ClassSetItem item)
{
    rs_ClassSet s = (rs_ClassSet){.tag = RS_CLASS_SET_TAG_ITEM};
    s.item        = item;
    return s;
}

rs_ClassSet
rs_ClassSet_union_(rs_ClassSetUnion u)
{
    // regex-syntax AST:1141: ClassSet::union(ast) -> Item(Union(ast)).
    rs_ClassSetItem it = (rs_ClassSetItem){.tag = RS_CLASS_SET_ITEM_UNION};
    it.union_          = u;
    return rs_ClassSet_item(it);
}

rs_ClassSet
rs_ClassSet_binary_op(rs_ClassSetBinaryOpKind kind,
                      rs_ClassSet              lhs,
                      rs_ClassSet              rhs,
                      rs_Span                  span)
{
    rs_ClassSet s = (rs_ClassSet){.tag = RS_CLASS_SET_TAG_BINARY_OP};
    s.binop_span  = span;
    s.binop_kind  = kind;
    s.lhs         = n00b_alloc(rs_ClassSet);
    *s.lhs        = lhs;
    s.rhs         = n00b_alloc(rs_ClassSet);
    *s.rhs        = rhs;
    return s;
}

rs_Span
rs_ClassSet_span(const rs_ClassSet *s)
{
    n00b_require(s != nullptr, "rs_ClassSet_span: s");
    if (s->tag == RS_CLASS_SET_TAG_ITEM) {
        return *class_set_item_span(&s->item);
    }
    return s->binop_span;
}

bool
rs_ClassSet_is_item(const rs_ClassSet *s)
{
    n00b_require(s != nullptr, "rs_ClassSet_is_item: s");
    return s->tag == RS_CLASS_SET_TAG_ITEM;
}

rs_ClassSetItem
rs_ClassSet_get_item(const rs_ClassSet *s)
{
    n00b_require(s != nullptr, "rs_ClassSet_get_item: s");
    n00b_require(s->tag == RS_CLASS_SET_TAG_ITEM,
                 "rs_ClassSet_get_item: not item");
    return s->item;
}

// ===========================================================================
// ClassBracketed
// ===========================================================================

rs_ClassBracketed
rs_ClassBracketed_make(rs_Span span, bool negated, rs_ClassSet kind)
{
    return (rs_ClassBracketed){.span = span, .negated = negated, .kind = kind};
}

void
rs_ClassBracketed_set_span_end(rs_ClassBracketed *b, rs_Position end)
{
    n00b_require(b != nullptr, "rs_ClassBracketed_set_span_end: b");
    b->span.end = end;
}

void
rs_ClassBracketed_set_kind(rs_ClassBracketed *b, rs_ClassSet kind)
{
    n00b_require(b != nullptr, "rs_ClassBracketed_set_kind: b");
    b->kind = kind;
}

rs_Span
rs_ClassBracketed_span(const rs_ClassBracketed *b)
{
    n00b_require(b != nullptr, "rs_ClassBracketed_span: b");
    return b->span;
}

bool
rs_ClassBracketed_negated(const rs_ClassBracketed *b)
{
    n00b_require(b != nullptr, "rs_ClassBracketed_negated: b");
    return b->negated;
}

rs_ClassSet
rs_ClassBracketed_kind_get(const rs_ClassBracketed *b)
{
    n00b_require(b != nullptr, "rs_ClassBracketed_kind_get: b");
    return b->kind;
}

// Forward decls for recursive deep clone (defined alongside the existing
// free helpers — rs_ClassSetItem / rs_ClassSet are recursive).
static rs_ClassSetItem class_set_item_clone(const rs_ClassSetItem *src);
static rs_ClassSet     class_set_clone(const rs_ClassSet *src);

rs_ClassBracketed
rs_ClassBracketed_clone(const rs_ClassBracketed *b)
{
    n00b_require(b != nullptr, "rs_ClassBracketed_clone: b");
    return (rs_ClassBracketed){
        .span    = b->span,
        .negated = b->negated,
        .kind    = class_set_clone(&b->kind),
    };
}

// ===========================================================================
// rs_Ast — minimal heap wrapper used as a translator input token.
// ===========================================================================

typedef enum {
    LOCAL_RS_AST_LITERAL,
    LOCAL_RS_AST_CLASS_UNICODE,
    LOCAL_RS_AST_CLASS_BRACKETED,
} local_rs_AstTag;

struct rs_Ast {
    local_rs_AstTag   tag;
    rs_Literal        literal;
    rs_ClassUnicode   class_unicode;
    rs_ClassBracketed class_bracketed;
};

// Forward decls for recursive free.
static void class_set_item_free(rs_ClassSetItem *it);
static void class_set_free(rs_ClassSet *s);

static void
class_bracketed_free_owned(rs_ClassBracketed *b)
{
    if (!b) {
        return;
    }
    class_set_free(&b->kind);
}

static void
class_set_item_free(rs_ClassSetItem *it)
{
    if (!it) {
        return;
    }
    switch (it->tag) {
    case RS_CLASS_SET_ITEM_RANGE:
        n00b_free(it->range);
        it->range = nullptr;
        break;
    case RS_CLASS_SET_ITEM_BRACKETED:
        class_bracketed_free_owned(it->bracketed);
        n00b_free(it->bracketed);
        it->bracketed = nullptr;
        break;
    case RS_CLASS_SET_ITEM_UNION:
        for (size_t i = 0; i < it->union_.len; i++) {
            class_set_item_free(&it->union_.items[i]);
        }
        n00b_free(it->union_.items);
        it->union_.items = nullptr;
        it->union_.len   = 0;
        it->union_.cap   = 0;
        break;
    case RS_CLASS_SET_ITEM_UNICODE:
        n00b_free(it->unicode.name);
        n00b_free(it->unicode.value);
        it->unicode.name  = nullptr;
        it->unicode.value = nullptr;
        break;
    default:
        break;
    }
}

static void
class_set_free(rs_ClassSet *s)
{
    if (!s) {
        return;
    }
    if (s->tag == RS_CLASS_SET_TAG_ITEM) {
        class_set_item_free(&s->item);
    }
    else {
        if (s->lhs) {
            class_set_free(s->lhs);
            n00b_free(s->lhs);
            s->lhs = nullptr;
        }
        if (s->rhs) {
            class_set_free(s->rhs);
            n00b_free(s->rhs);
            s->rhs = nullptr;
        }
    }
}

// Recursive deep clone, mirroring class_set_item_free / class_set_free.
// Heap-owned children are duplicated so the clone is independently freeable.
static rs_ClassSetItem
class_set_item_clone(const rs_ClassSetItem *src)
{
    rs_ClassSetItem dst = *src;
    switch (src->tag) {
    case RS_CLASS_SET_ITEM_RANGE:
        dst.range  = n00b_alloc(rs_ClassSetRange);
        *dst.range = *src->range;
        break;
    case RS_CLASS_SET_ITEM_BRACKETED:
        dst.bracketed  = n00b_alloc(rs_ClassBracketed);
        *dst.bracketed = (rs_ClassBracketed){
            .span    = src->bracketed->span,
            .negated = src->bracketed->negated,
            .kind    = class_set_clone(&src->bracketed->kind),
        };
        break;
    case RS_CLASS_SET_ITEM_UNION: {
        size_t n         = src->union_.len;
        dst.union_.span  = src->union_.span;
        dst.union_.len   = n;
        dst.union_.cap   = n;
        dst.union_.items = nullptr;
        if (n > 0) {
            dst.union_.items = n00b_alloc_array(rs_ClassSetItem, n);
            for (size_t i = 0; i < n; i++) {
                dst.union_.items[i] = class_set_item_clone(&src->union_.items[i]);
            }
        }
        break;
    }
    case RS_CLASS_SET_ITEM_UNICODE:
        dst.unicode.name  = strdup_or_null(src->unicode.name);
        dst.unicode.value = strdup_or_null(src->unicode.value);
        break;
    default:
        // EMPTY / LITERAL / ASCII / PERL: POD, plain struct copy is fine.
        break;
    }
    return dst;
}

static rs_ClassSet
class_set_clone(const rs_ClassSet *src)
{
    rs_ClassSet dst = *src;
    if (src->tag == RS_CLASS_SET_TAG_ITEM) {
        dst.item = class_set_item_clone(&src->item);
    }
    else {
        dst.lhs  = n00b_alloc(rs_ClassSet);
        *dst.lhs = class_set_clone(src->lhs);
        dst.rhs  = n00b_alloc(rs_ClassSet);
        *dst.rhs = class_set_clone(src->rhs);
    }
    return dst;
}

rs_Ast *
rs_Ast_literal_owned(rs_Literal lit)
{
    rs_Ast *a  = n00b_alloc(rs_Ast);
    a->tag     = LOCAL_RS_AST_LITERAL;
    a->literal = lit;
    return a;
}

rs_Ast *
rs_Ast_class_unicode_owned(rs_ClassUnicode cls)
{
    rs_Ast *a        = n00b_alloc(rs_Ast);
    a->tag           = LOCAL_RS_AST_CLASS_UNICODE;
    a->class_unicode = cls;
    return a;
}

rs_Ast *
rs_Ast_class_bracketed_owned(rs_ClassBracketed cls)
{
    rs_Ast *a          = n00b_alloc(rs_Ast);
    a->tag             = LOCAL_RS_AST_CLASS_BRACKETED;
    a->class_bracketed = cls;
    return a;
}

void
rs_Ast_free(rs_Ast *a)
{
    if (!a) {
        return;
    }
    switch (a->tag) {
    case LOCAL_RS_AST_LITERAL:
        // Literal owns no heap data.
        break;
    case LOCAL_RS_AST_CLASS_UNICODE:
        n00b_free(a->class_unicode.name);
        n00b_free(a->class_unicode.value);
        break;
    case LOCAL_RS_AST_CLASS_BRACKETED:
        class_bracketed_free_owned(&a->class_bracketed);
        break;
    }
    n00b_free(a);
}

// ===========================================================================
// Sorted-range interval set
// Each set is canonical: sorted by `start`, no overlapping or contiguous
// ranges.  uint32_t for codepoints (skipping the surrogate hole on
// increment/decrement) and uint8_t for bytes.
// ===========================================================================

typedef struct {
    uint32_t start;
    uint32_t end;
} rs_urange_t;

typedef struct {
    rs_urange_t *data;
    size_t       len;
    size_t       cap;
} rs_uvec_t;

typedef struct {
    uint8_t start;
    uint8_t end;
} rs_brange_t;

typedef struct {
    rs_brange_t *data;
    size_t       len;
    size_t       cap;
} rs_bvec_t;

#define RS_UCP_MAX 0x10FFFFu

static uint32_t
ucp_inc(uint32_t c)
{
    if (c == 0xD7FFu) {
        return 0xE000u;
    }
    n00b_require(c < RS_UCP_MAX, "ucp_inc: overflow");
    return c + 1u;
}

static uint32_t
ucp_dec(uint32_t c)
{
    if (c == 0xE000u) {
        return 0xD7FFu;
    }
    n00b_require(c > 0, "ucp_dec: underflow");
    return c - 1u;
}

// ---- rs_uvec_t (Unicode range) helpers ------------------------------------

static void
uvec_init(rs_uvec_t *v)
{
    v->data = nullptr;
    v->len  = 0;
    v->cap  = 0;
}

static void
uvec_free(rs_uvec_t *v)
{
    if (v->data) {
        n00b_free(v->data);
    }
    v->data = nullptr;
    v->len  = 0;
    v->cap  = 0;
}

static void
uvec_reserve(rs_uvec_t *v, size_t need)
{
    if (need <= v->cap) {
        return;
    }
    size_t newcap = v->cap == 0 ? 4 : safe_mul_sz(v->cap, 2);
    while (newcap < need) {
        newcap = safe_mul_sz(newcap, 2);
    }
    rs_urange_t *new_data = n00b_alloc_array(rs_urange_t, newcap);
    if (v->len > 0 && v->data != nullptr) {
        memcpy(new_data, v->data, safe_mul_sz(v->len, sizeof(rs_urange_t)));
    }
    if (v->data != nullptr) {
        n00b_free(v->data);
    }
    v->data = new_data;
    v->cap  = newcap;
}

static void
uvec_push(rs_uvec_t *v, rs_urange_t r)
{
    uvec_reserve(v, safe_add_sz(v->len, 1));
    v->data[v->len++] = r;
}

static void
uvec_drain_prefix(rs_uvec_t *v, size_t n)
{
    n00b_require(n <= v->len, "uvec_drain_prefix: n out of range");
    if (n == 0) {
        return;
    }
    size_t remaining = v->len - n;
    if (remaining > 0) {
        memmove(v->data, v->data + n, remaining * sizeof(rs_urange_t));
    }
    v->len = remaining;
}

static int
urange_cmp(const void *a, const void *b)
{
    const rs_urange_t *ra = (const rs_urange_t *)a;
    const rs_urange_t *rb = (const rs_urange_t *)b;
    if (ra->start != rb->start) {
        return ra->start < rb->start ? -1 : 1;
    }
    if (ra->end != rb->end) {
        return ra->end < rb->end ? -1 : 1;
    }
    return 0;
}

static bool
urange_is_contiguous(rs_urange_t a, rs_urange_t b)
{
    uint64_t lo = a.start > b.start ? a.start : b.start;
    uint32_t mn = a.end < b.end ? a.end : b.end;
    return lo <= (uint64_t)mn + 1ull;
}

static bool
urange_intersect(rs_urange_t a, rs_urange_t b, rs_urange_t *out)
{
    uint32_t lo = a.start > b.start ? a.start : b.start;
    uint32_t hi = a.end < b.end ? a.end : b.end;
    if (lo <= hi) {
        *out = (rs_urange_t){lo, hi};
        return true;
    }
    return false;
}

static bool
urange_intersection_empty(rs_urange_t a, rs_urange_t b)
{
    uint32_t lo = a.start > b.start ? a.start : b.start;
    uint32_t hi = a.end < b.end ? a.end : b.end;
    return lo > hi;
}

static bool
urange_is_subset(rs_urange_t a, rs_urange_t b)
{
    return b.start <= a.start && a.start <= b.end && b.start <= a.end
           && a.end <= b.end;
}

// Difference: a - b returns 0, 1, or 2 ranges in out0 / out1.
static void
urange_difference(rs_urange_t a, rs_urange_t b, rs_urange_t *out0,
                  rs_urange_t *out1, int *n_out)
{
    if (urange_is_subset(a, b)) {
        *n_out = 0;
        return;
    }
    if (urange_intersection_empty(a, b)) {
        *out0  = a;
        *n_out = 1;
        return;
    }
    bool add_lower = b.start > a.start;
    bool add_upper = b.end < a.end;
    n00b_require(add_lower || add_upper, "urange_difference: invariant");
    int n = 0;
    if (add_lower) {
        out0[n++] = (rs_urange_t){a.start, ucp_dec(b.start)};
    }
    if (add_upper) {
        rs_urange_t r = {ucp_inc(b.end), a.end};
        if (n == 0) {
            out0[n++] = r;
        }
        else {
            out1[0] = r;
        }
    }
    *n_out = n;
}

// Sort + merge contiguous/overlapping; in-place via append-and-drain.
static void
uvec_canonicalize(rs_uvec_t *v)
{
    if (v->len <= 1) {
        return;
    }
    bool canonical = true;
    for (size_t i = 1; i < v->len; i++) {
        if (urange_cmp(&v->data[i - 1], &v->data[i]) >= 0
            || urange_is_contiguous(v->data[i - 1], v->data[i])) {
            canonical = false;
            break;
        }
    }
    if (canonical) {
        return;
    }

    qsort(v->data, v->len, sizeof(rs_urange_t), urange_cmp);
    size_t drain_end = v->len;
    for (size_t oldi = 0; oldi < drain_end; oldi++) {
        rs_urange_t cur = v->data[oldi];
        if (v->len > drain_end) {
            rs_urange_t *last = &v->data[v->len - 1];
            if (urange_is_contiguous(*last, cur)) {
                uint32_t lo = last->start < cur.start ? last->start : cur.start;
                uint32_t hi = last->end > cur.end ? last->end : cur.end;
                last->start = lo;
                last->end   = hi;
                continue;
            }
        }
        uvec_push(v, cur);
    }
    uvec_drain_prefix(v, drain_end);
}

static void
uvec_push_canonical(rs_uvec_t *v, rs_urange_t r)
{
    uvec_push(v, r);
    uvec_canonicalize(v);
}

static void
uvec_union(rs_uvec_t *v, const rs_uvec_t *other)
{
    if (other->len == 0) {
        return;
    }
    for (size_t i = 0; i < other->len; i++) {
        uvec_push(v, other->data[i]);
    }
    uvec_canonicalize(v);
}

static void
uvec_intersect(rs_uvec_t *v, const rs_uvec_t *other)
{
    if (v->len == 0) {
        return;
    }
    if (other->len == 0) {
        v->len = 0;
        return;
    }
    size_t drain_end = v->len;
    size_t a = 0, b = 0;
    for (;;) {
        rs_urange_t inter;
        if (urange_intersect(v->data[a], other->data[b], &inter)) {
            uvec_push(v, inter);
        }
        bool advance_a = v->data[a].end < other->data[b].end;
        if (advance_a) {
            a++;
            if (a >= drain_end) {
                break;
            }
        }
        else {
            b++;
            if (b >= other->len) {
                break;
            }
        }
    }
    uvec_drain_prefix(v, drain_end);
}

static void
uvec_difference(rs_uvec_t *v, const rs_uvec_t *other)
{
    if (v->len == 0 || other->len == 0) {
        return;
    }
    size_t drain_end = v->len;
    size_t a = 0, b = 0;
    while (a < drain_end && b < other->len) {
        if (other->data[b].end < v->data[a].start) {
            b++;
            continue;
        }
        if (v->data[a].end < other->data[b].start) {
            uvec_push(v, v->data[a]);
            a++;
            continue;
        }
        rs_urange_t range    = v->data[a];
        bool        consumed = false;
        while (b < other->len
               && !urange_intersection_empty(range, other->data[b])) {
            rs_urange_t old_range = range;
            rs_urange_t out0, out1;
            int         n_out;
            urange_difference(range, other->data[b], &out0, &out1, &n_out);
            if (n_out == 0) {
                a++;
                consumed = true;
                break;
            }
            if (n_out == 1) {
                range = out0;
            }
            else {
                uvec_push(v, out0);
                range = out1;
            }
            if (other->data[b].end > old_range.end) {
                break;
            }
            b++;
        }
        if (consumed) {
            continue;
        }
        uvec_push(v, range);
        a++;
    }
    while (a < drain_end) {
        uvec_push(v, v->data[a]);
        a++;
    }
    uvec_drain_prefix(v, drain_end);
}

static void
uvec_clone_into(rs_uvec_t *dst, const rs_uvec_t *src)
{
    uvec_reserve(dst, src->len);
    if (src->len > 0) {
        memcpy(dst->data, src->data, src->len * sizeof(rs_urange_t));
    }
    dst->len = src->len;
}

static void
uvec_symmetric_difference(rs_uvec_t *v, const rs_uvec_t *other)
{
    rs_uvec_t intersection;
    uvec_init(&intersection);
    uvec_clone_into(&intersection, v);
    uvec_intersect(&intersection, other);
    uvec_union(v, other);
    uvec_difference(v, &intersection);
    uvec_free(&intersection);
}

static void
uvec_negate(rs_uvec_t *v)
{
    if (v->len == 0) {
        uvec_push(v, (rs_urange_t){0u, RS_UCP_MAX});
        return;
    }
    size_t drain_end = v->len;
    if (v->data[0].start > 0u) {
        uvec_push(v, (rs_urange_t){0u, ucp_dec(v->data[0].start)});
    }
    for (size_t i = 1; i < drain_end; i++) {
        uint32_t lo = ucp_inc(v->data[i - 1].end);
        uint32_t hi = ucp_dec(v->data[i].start);
        uvec_push(v, (rs_urange_t){lo, hi});
    }
    if (v->data[drain_end - 1].end < RS_UCP_MAX) {
        uint32_t lo = ucp_inc(v->data[drain_end - 1].end);
        uvec_push(v, (rs_urange_t){lo, RS_UCP_MAX});
    }
    uvec_drain_prefix(v, drain_end);
}

// ---- rs_bvec_t (byte range) helpers ---------------------------------------

static void
bvec_init(rs_bvec_t *v)
{
    v->data = nullptr;
    v->len  = 0;
    v->cap  = 0;
}

static void
bvec_free(rs_bvec_t *v)
{
    if (v->data) {
        n00b_free(v->data);
    }
    v->data = nullptr;
    v->len  = 0;
    v->cap  = 0;
}

static void
bvec_reserve(rs_bvec_t *v, size_t need)
{
    if (need <= v->cap) {
        return;
    }
    size_t newcap = v->cap == 0 ? 4 : safe_mul_sz(v->cap, 2);
    while (newcap < need) {
        newcap = safe_mul_sz(newcap, 2);
    }
    rs_brange_t *new_data = n00b_alloc_array(rs_brange_t, newcap);
    if (v->len > 0 && v->data != nullptr) {
        memcpy(new_data, v->data, safe_mul_sz(v->len, sizeof(rs_brange_t)));
    }
    if (v->data != nullptr) {
        n00b_free(v->data);
    }
    v->data = new_data;
    v->cap  = newcap;
}

static void
bvec_push(rs_bvec_t *v, rs_brange_t r)
{
    bvec_reserve(v, safe_add_sz(v->len, 1));
    v->data[v->len++] = r;
}

static void
bvec_drain_prefix(rs_bvec_t *v, size_t n)
{
    n00b_require(n <= v->len, "bvec_drain_prefix: n out of range");
    if (n == 0) {
        return;
    }
    size_t remaining = v->len - n;
    if (remaining > 0) {
        memmove(v->data, v->data + n, remaining * sizeof(rs_brange_t));
    }
    v->len = remaining;
}

static int
brange_cmp(const void *a, const void *b)
{
    const rs_brange_t *ra = (const rs_brange_t *)a;
    const rs_brange_t *rb = (const rs_brange_t *)b;
    if (ra->start != rb->start) {
        return ra->start < rb->start ? -1 : 1;
    }
    if (ra->end != rb->end) {
        return ra->end < rb->end ? -1 : 1;
    }
    return 0;
}

static bool
brange_is_contiguous(rs_brange_t a, rs_brange_t b)
{
    uint16_t lo = a.start > b.start ? a.start : b.start;
    uint16_t mn = a.end < b.end ? a.end : b.end;
    return lo <= (uint16_t)(mn + 1u);
}

static bool
brange_intersect(rs_brange_t a, rs_brange_t b, rs_brange_t *out)
{
    uint8_t lo = a.start > b.start ? a.start : b.start;
    uint8_t hi = a.end < b.end ? a.end : b.end;
    if (lo <= hi) {
        *out = (rs_brange_t){lo, hi};
        return true;
    }
    return false;
}

static bool
brange_intersection_empty(rs_brange_t a, rs_brange_t b)
{
    uint8_t lo = a.start > b.start ? a.start : b.start;
    uint8_t hi = a.end < b.end ? a.end : b.end;
    return lo > hi;
}

static bool
brange_is_subset(rs_brange_t a, rs_brange_t b)
{
    return b.start <= a.start && a.start <= b.end && b.start <= a.end
           && a.end <= b.end;
}

static void
brange_difference(rs_brange_t a, rs_brange_t b, rs_brange_t *out0,
                  rs_brange_t *out1, int *n_out)
{
    if (brange_is_subset(a, b)) {
        *n_out = 0;
        return;
    }
    if (brange_intersection_empty(a, b)) {
        *out0  = a;
        *n_out = 1;
        return;
    }
    bool add_lower = b.start > a.start;
    bool add_upper = b.end < a.end;
    n00b_require(add_lower || add_upper, "brange_difference: invariant");
    int n = 0;
    if (add_lower) {
        n00b_require(b.start > 0, "brange_difference: lo underflow");
        out0[n++] = (rs_brange_t){a.start, (uint8_t)(b.start - 1u)};
    }
    if (add_upper) {
        n00b_require(b.end < 255u, "brange_difference: hi overflow");
        rs_brange_t r = {(uint8_t)(b.end + 1u), a.end};
        if (n == 0) {
            out0[n++] = r;
        }
        else {
            out1[0] = r;
        }
    }
    *n_out = n;
}

static void
bvec_canonicalize(rs_bvec_t *v)
{
    if (v->len <= 1) {
        return;
    }
    bool canonical = true;
    for (size_t i = 1; i < v->len; i++) {
        if (brange_cmp(&v->data[i - 1], &v->data[i]) >= 0
            || brange_is_contiguous(v->data[i - 1], v->data[i])) {
            canonical = false;
            break;
        }
    }
    if (canonical) {
        return;
    }

    qsort(v->data, v->len, sizeof(rs_brange_t), brange_cmp);
    size_t drain_end = v->len;
    for (size_t oldi = 0; oldi < drain_end; oldi++) {
        rs_brange_t cur = v->data[oldi];
        if (v->len > drain_end) {
            rs_brange_t *last = &v->data[v->len - 1];
            if (brange_is_contiguous(*last, cur)) {
                uint8_t lo = last->start < cur.start ? last->start : cur.start;
                uint8_t hi = last->end > cur.end ? last->end : cur.end;
                last->start = lo;
                last->end   = hi;
                continue;
            }
        }
        bvec_push(v, cur);
    }
    bvec_drain_prefix(v, drain_end);
}

static void
bvec_push_canonical(rs_bvec_t *v, rs_brange_t r)
{
    bvec_push(v, r);
    bvec_canonicalize(v);
}

static void
bvec_union(rs_bvec_t *v, const rs_bvec_t *other)
{
    if (other->len == 0) {
        return;
    }
    for (size_t i = 0; i < other->len; i++) {
        bvec_push(v, other->data[i]);
    }
    bvec_canonicalize(v);
}

static void
bvec_intersect(rs_bvec_t *v, const rs_bvec_t *other)
{
    if (v->len == 0) {
        return;
    }
    if (other->len == 0) {
        v->len = 0;
        return;
    }
    size_t drain_end = v->len;
    size_t a = 0, b = 0;
    for (;;) {
        rs_brange_t inter;
        if (brange_intersect(v->data[a], other->data[b], &inter)) {
            bvec_push(v, inter);
        }
        bool advance_a = v->data[a].end < other->data[b].end;
        if (advance_a) {
            a++;
            if (a >= drain_end) {
                break;
            }
        }
        else {
            b++;
            if (b >= other->len) {
                break;
            }
        }
    }
    bvec_drain_prefix(v, drain_end);
}

static void
bvec_difference(rs_bvec_t *v, const rs_bvec_t *other)
{
    if (v->len == 0 || other->len == 0) {
        return;
    }
    size_t drain_end = v->len;
    size_t a = 0, b = 0;
    while (a < drain_end && b < other->len) {
        if (other->data[b].end < v->data[a].start) {
            b++;
            continue;
        }
        if (v->data[a].end < other->data[b].start) {
            bvec_push(v, v->data[a]);
            a++;
            continue;
        }
        rs_brange_t range    = v->data[a];
        bool        consumed = false;
        while (b < other->len
               && !brange_intersection_empty(range, other->data[b])) {
            rs_brange_t old_range = range;
            rs_brange_t out0, out1;
            int         n_out;
            brange_difference(range, other->data[b], &out0, &out1, &n_out);
            if (n_out == 0) {
                a++;
                consumed = true;
                break;
            }
            if (n_out == 1) {
                range = out0;
            }
            else {
                bvec_push(v, out0);
                range = out1;
            }
            if (other->data[b].end > old_range.end) {
                break;
            }
            b++;
        }
        if (consumed) {
            continue;
        }
        bvec_push(v, range);
        a++;
    }
    while (a < drain_end) {
        bvec_push(v, v->data[a]);
        a++;
    }
    bvec_drain_prefix(v, drain_end);
}

static void
bvec_clone_into(rs_bvec_t *dst, const rs_bvec_t *src)
{
    bvec_reserve(dst, src->len);
    if (src->len > 0) {
        memcpy(dst->data, src->data, src->len * sizeof(rs_brange_t));
    }
    dst->len = src->len;
}

static void
bvec_symmetric_difference(rs_bvec_t *v, const rs_bvec_t *other)
{
    rs_bvec_t intersection;
    bvec_init(&intersection);
    bvec_clone_into(&intersection, v);
    bvec_intersect(&intersection, other);
    bvec_union(v, other);
    bvec_difference(v, &intersection);
    bvec_free(&intersection);
}

static void
bvec_negate(rs_bvec_t *v)
{
    if (v->len == 0) {
        bvec_push(v, (rs_brange_t){0u, 255u});
        return;
    }
    size_t drain_end = v->len;
    if (v->data[0].start > 0u) {
        bvec_push(v, (rs_brange_t){0u, (uint8_t)(v->data[0].start - 1u)});
    }
    for (size_t i = 1; i < drain_end; i++) {
        uint8_t lo = (uint8_t)(v->data[i - 1].end + 1u);
        uint8_t hi = (uint8_t)(v->data[i].start - 1u);
        bvec_push(v, (rs_brange_t){lo, hi});
    }
    if (v->data[drain_end - 1].end < 255u) {
        uint8_t lo = (uint8_t)(v->data[drain_end - 1].end + 1u);
        bvec_push(v, (rs_brange_t){lo, 255u});
    }
    bvec_drain_prefix(v, drain_end);
}

// ===========================================================================
// Hir — sum type over {Empty, Literal, Class, Concat}.
// ===========================================================================

typedef enum {
    LOCAL_RS_HIR_EMPTY,
    LOCAL_RS_HIR_LITERAL,
    LOCAL_RS_HIR_CLASS,
    LOCAL_RS_HIR_LOOK,
    LOCAL_RS_HIR_REPETITION,
    LOCAL_RS_HIR_CAPTURE,
    LOCAL_RS_HIR_CONCAT,
    LOCAL_RS_HIR_ALTERNATION,
} local_rs_HirKindTag;

typedef enum {
    LOCAL_RS_HIR_CLASS_UNICODE,
    LOCAL_RS_HIR_CLASS_BYTES,
} local_rs_HirClassTag;

struct rs_Hir {
    local_rs_HirKindTag kind;
    // LITERAL
    uint8_t *lit_bytes;
    size_t   lit_len;
    // CLASS
    local_rs_HirClassTag class_tag;
    rs_uvec_t            cls_unicode;
    rs_bvec_t            cls_bytes;
    // CONCAT
    struct rs_Hir **concat;
    size_t          concat_len;
};

static rs_Hir *
hir_new_empty(void)
{
    rs_Hir *h = n00b_alloc(rs_Hir);
    h->kind   = LOCAL_RS_HIR_EMPTY;
    return h;
}

static rs_Hir *
hir_new_literal(const uint8_t *bytes, size_t len)
{
    rs_Hir *h = n00b_alloc(rs_Hir);
    h->kind   = LOCAL_RS_HIR_LITERAL;
    if (len > 0) {
        h->lit_bytes = n00b_alloc_array(uint8_t, len);
        memcpy(h->lit_bytes, bytes, len);
    }
    h->lit_len = len;
    return h;
}

static rs_Hir *
hir_new_class_unicode(rs_uvec_t v)
{
    rs_Hir *h      = n00b_alloc(rs_Hir);
    h->kind        = LOCAL_RS_HIR_CLASS;
    h->class_tag   = LOCAL_RS_HIR_CLASS_UNICODE;
    h->cls_unicode = v;
    return h;
}

static rs_Hir *
hir_new_class_bytes(rs_bvec_t v)
{
    rs_Hir *h    = n00b_alloc(rs_Hir);
    h->kind      = LOCAL_RS_HIR_CLASS;
    h->class_tag = LOCAL_RS_HIR_CLASS_BYTES;
    h->cls_bytes = v;
    return h;
}

void
rs_Hir_free(rs_Hir *h)
{
    if (!h) {
        return;
    }
    switch (h->kind) {
    case LOCAL_RS_HIR_LITERAL:
        n00b_free(h->lit_bytes);
        break;
    case LOCAL_RS_HIR_CLASS:
        uvec_free(&h->cls_unicode);
        bvec_free(&h->cls_bytes);
        break;
    case LOCAL_RS_HIR_CONCAT:
        for (size_t i = 0; i < h->concat_len; i++) {
            rs_Hir_free(h->concat[i]);
        }
        n00b_free(h->concat);
        break;
    default:
        break;
    }
    n00b_free(h);
}

local_rs_HirKindTag
rs_Hir_kind(const rs_Hir *h)
{
    if (!h) {
        return LOCAL_RS_HIR_EMPTY;
    }
    return h->kind;
}

local_rs_HirClassTag
rs_Hir_class_tag(const rs_Hir *h)
{
    n00b_require(h != nullptr, "rs_Hir_class_tag: h");
    n00b_require(h->kind == LOCAL_RS_HIR_CLASS,
                 "rs_Hir_class_tag: not a class");
    return h->class_tag;
}

const uint8_t *
rs_Hir_literal_bytes(const rs_Hir *h, size_t *len)
{
    n00b_require(len != nullptr, "rs_Hir_literal_bytes: len");
    if (!h || h->kind != LOCAL_RS_HIR_LITERAL) {
        *len = 0;
        return nullptr;
    }
    *len = h->lit_len;
    return h->lit_bytes;
}

size_t
rs_Hir_class_unicode_ranges(const rs_Hir *h, uint32_t *out_starts,
                            uint32_t *out_ends, size_t cap)
{
    if (!h || h->kind != LOCAL_RS_HIR_CLASS
        || h->class_tag != LOCAL_RS_HIR_CLASS_UNICODE) {
        return 0;
    }
    if (!out_starts || !out_ends) {
        return h->cls_unicode.len;
    }
    size_t n = h->cls_unicode.len < cap ? h->cls_unicode.len : cap;
    for (size_t i = 0; i < n; i++) {
        out_starts[i] = h->cls_unicode.data[i].start;
        out_ends[i]   = h->cls_unicode.data[i].end;
    }
    return h->cls_unicode.len;
}

size_t
rs_Hir_class_bytes_ranges(const rs_Hir *h, uint8_t *out_starts,
                          uint8_t *out_ends, size_t cap)
{
    if (!h || h->kind != LOCAL_RS_HIR_CLASS
        || h->class_tag != LOCAL_RS_HIR_CLASS_BYTES) {
        return 0;
    }
    if (!out_starts || !out_ends) {
        return h->cls_bytes.len;
    }
    size_t n = h->cls_bytes.len < cap ? h->cls_bytes.len : cap;
    for (size_t i = 0; i < n; i++) {
        out_starts[i] = h->cls_bytes.data[i].start;
        out_ends[i]   = h->cls_bytes.data[i].end;
    }
    return h->cls_bytes.len;
}

size_t
rs_Hir_concat_count(const rs_Hir *h)
{
    if (!h || h->kind != LOCAL_RS_HIR_CONCAT) {
        return 0;
    }
    return h->concat_len;
}

const rs_Hir *
rs_Hir_concat_child(const rs_Hir *h, size_t i)
{
    if (!h || h->kind != LOCAL_RS_HIR_CONCAT) {
        return nullptr;
    }
    n00b_require(i < h->concat_len, "rs_Hir_concat_child: out of range");
    return h->concat[i];
}

// regex-syntax HIR:659 — Hir::dot factories collapse into Class.
rs_Hir *
rs_Hir_dot_any_char(void)
{
    rs_uvec_t v;
    uvec_init(&v);
    uvec_push(&v, (rs_urange_t){0u, RS_UCP_MAX});
    return hir_new_class_unicode(v);
}

rs_Hir *
rs_Hir_dot_any_char_except_lf(void)
{
    rs_uvec_t v;
    uvec_init(&v);
    uvec_push(&v, (rs_urange_t){0u, 0x09u});
    uvec_push(&v, (rs_urange_t){0x0Bu, RS_UCP_MAX});
    return hir_new_class_unicode(v);
}

rs_Hir *
rs_Hir_dot_any_byte_except_lf(void)
{
    rs_bvec_t v;
    bvec_init(&v);
    bvec_push(&v, (rs_brange_t){0u, 0x09u});
    bvec_push(&v, (rs_brange_t){0x0Bu, 0xFFu});
    return hir_new_class_bytes(v);
}

// ===========================================================================
// Unicode-property resolution
// Supports: Perl shortcuts (\d \s \w), one-letter and two-letter general
// categories, and the long-form GC names (case-insensitive, hyphens and
// underscores ignored, UAX44-LM3-style normalisation).  Also the ASCII /
// Any / Assigned pseudo-categories.
// Script, Script_Extension, Age, and arbitrary binary-property queries are
// resolved via n00b_unicode_*.  Grapheme/Word/Sentence/Line break too.
// ===========================================================================

// UAX44-LM3 normalisation: drop spaces / underscores / hyphens, lowercase
// ASCII letters.  Operates in place; returns new length.
static size_t
normalize_symbolic(char *buf, size_t n)
{
    size_t start          = 0;
    bool   starts_with_is = false;
    if (n >= 2) {
        char a = buf[0], b = buf[1];
        if ((a == 'i' || a == 'I') && (b == 's' || b == 'S')) {
            starts_with_is = true;
            start          = 2;
        }
    }
    size_t w = 0;
    for (size_t i = start; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == ' ' || c == '_' || c == '-') {
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            buf[w++] = (char)(c + ('a' - 'A'));
        }
        else if (c <= 0x7F) {
            buf[w++] = (char)c;
        }
    }
    if (starts_with_is && w == 1 && buf[0] == 'c' && n >= 3) {
        buf[0] = 'i';
        buf[1] = 's';
        buf[2] = 'c';
        w      = 3;
    }
    return w;
}

typedef struct {
    const char *alias;
    const char *canonical;
} gc_alias_t;
static const gc_alias_t GC_ALIASES[] = {
    {"l",                       "Letter"               },
    {"n",                       "Number"               },
    {"p",                       "Punctuation"          },
    {"s",                       "Symbol"               },
    {"z",                       "Separator"            },
    {"m",                       "Mark"                 },
    {"c",                       "Other"                },
    {"lu",                      "Uppercase_Letter"     },
    {"ll",                      "Lowercase_Letter"     },
    {"lt",                      "Titlecase_Letter"     },
    {"lm",                      "Modifier_Letter"      },
    {"lo",                      "Other_Letter"         },
    {"lc",                      "Cased_Letter"         },
    {"mn",                      "Nonspacing_Mark"      },
    {"mc",                      "Spacing_Mark"         },
    {"me",                      "Enclosing_Mark"       },
    {"nd",                      "Decimal_Number"       },
    {"nl",                      "Letter_Number"        },
    {"no",                      "Other_Number"         },
    {"pc",                      "Connector_Punctuation"},
    {"pd",                      "Dash_Punctuation"     },
    {"ps",                      "Open_Punctuation"     },
    {"pe",                      "Close_Punctuation"    },
    {"pi",                      "Initial_Punctuation"  },
    {"pf",                      "Final_Punctuation"    },
    {"po",                      "Other_Punctuation"    },
    {"sm",                      "Math_Symbol"          },
    {"sc",                      "Currency_Symbol"      },
    {"sk",                      "Modifier_Symbol"      },
    {"so",                      "Other_Symbol"         },
    {"zs",                      "Space_Separator"      },
    {"zl",                      "Line_Separator"       },
    {"zp",                      "Paragraph_Separator"  },
    {"cc",                      "Control"              },
    {"cf",                      "Format"               },
    {"co",                      "Private_Use"          },
    {"cn",                      "Unassigned"           },
    {"letter",                  "Letter"               },
    {"casedletter",             "Cased_Letter"         },
    {"uppercaseletter",         "Uppercase_Letter"     },
    {"lowercaseletter",         "Lowercase_Letter"     },
    {"titlecaseletter",         "Titlecase_Letter"     },
    {"modifierletter",          "Modifier_Letter"      },
    {"otherletter",             "Other_Letter"         },
    {"mark",                    "Mark"                 },
    {"nonspacingmark",          "Nonspacing_Mark"      },
    {"spacingmark",             "Spacing_Mark"         },
    {"enclosingmark",           "Enclosing_Mark"       },
    {"number",                  "Number"               },
    {"decimalnumber",           "Decimal_Number"       },
    {"letternumber",            "Letter_Number"        },
    {"othernumber",             "Other_Number"         },
    {"punctuation",             "Punctuation"          },
    {"connectorpunctuation",    "Connector_Punctuation"},
    {"dashpunctuation",         "Dash_Punctuation"     },
    {"openpunctuation",         "Open_Punctuation"     },
    {"closepunctuation",        "Close_Punctuation"    },
    {"initialpunctuation",      "Initial_Punctuation"  },
    {"finalpunctuation",        "Final_Punctuation"    },
    {"otherpunctuation",        "Other_Punctuation"    },
    {"symbol",                  "Symbol"               },
    {"mathsymbol",              "Math_Symbol"          },
    {"currencysymbol",          "Currency_Symbol"      },
    {"modifiersymbol",          "Modifier_Symbol"      },
    {"othersymbol",             "Other_Symbol"         },
    {"separator",               "Separator"            },
    {"spaceseparator",          "Space_Separator"      },
    {"lineseparator",           "Line_Separator"       },
    {"paragraphseparator",      "Paragraph_Separator"  },
    {"other",                   "Other"                },
    {"control",                 "Control"              },
    {"format",                  "Format"               },
    {"privateuse",              "Private_Use"          },
    {"unassigned",              "Unassigned"           },
};
static constexpr size_t GC_ALIASES_LEN = sizeof(GC_ALIASES) / sizeof(GC_ALIASES[0]);

typedef enum {
    GC_PSEUDO_NONE,
    GC_PSEUDO_ASCII,
    GC_PSEUDO_ANY,
    GC_PSEUDO_ASSIGNED,
} gc_pseudo_t;

static gc_pseudo_t
resolve_pseudo(const char *norm)
{
    if (strcmp(norm, "ascii") == 0) {
        return GC_PSEUDO_ASCII;
    }
    if (strcmp(norm, "any") == 0) {
        return GC_PSEUDO_ANY;
    }
    if (strcmp(norm, "assigned") == 0) {
        return GC_PSEUDO_ASSIGNED;
    }
    return GC_PSEUDO_NONE;
}

static const char *
resolve_gc_alias(const char *norm)
{
    for (size_t i = 0; i < GC_ALIASES_LEN; i++) {
        if (strcmp(norm, GC_ALIASES[i].alias) == 0) {
            return GC_ALIASES[i].canonical;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Unicode property -> rs_uchar_pair_t adapters (shim over n00b unicode).
// The n00b unicode lib exposes property data as `n00b_codepoint_pair_t`
// arrays (lo/hi field names — same layout as rs_uchar_pair_t which uses
// start/end).  Field types match (uint32_t each), so pointer reinterpret
// is sound; we contain the casts in `wrap_pair_ptr()` below.
// All cached `rs_named_pairset_t` records live in dynamically grown slots
// and reference data owned by n00b unicode (program-lifetime).  `name` is
// strdup'd from the canonical key.
// ---------------------------------------------------------------------------

// `rs_uchar_pair_t` and `rs_named_pairset_t` are defined in
// `include/text/regex/ctx.h` — the regex subsystem bundle reaches them
// via the runtime.

static const rs_uchar_pair_t *
wrap_pair_ptr(const n00b_codepoint_pair_t *r)
{
    // n00b_codepoint_pair_t is { lo, hi } and rs_uchar_pair_t is
    // { start, end }; both are two adjacent uint32_t in the same order.
    static_assert(sizeof(n00b_codepoint_pair_t) == sizeof(rs_uchar_pair_t),
                  "n00b_codepoint_pair_t must match rs_uchar_pair_t layout");
    return (const rs_uchar_pair_t *)r;
}

// `cached_pairset_t` is defined as `n00b_regex_cached_pairset_t` in
// `include/text/regex/ctx.h`; the cache + len + cap + mutex all live
// on `n00b_runtime_t::regex_ctx`.  Alias the canonical name here.
typedef n00b_regex_cached_pairset_t cached_pairset_t;

typedef _Atomic(uint32_t) once_state_t;

// Get the per-runtime regex subsystem context.  Eager-allocated in
// n00b_init() and registered as a GC root.
static inline n00b_regex_ctx_t *
_rctx(void)
{
    return n00b_get_runtime()->regex_ctx;
}

static void
ensure_named_cache_mutex(void)
{
    n00b_regex_ctx_t *ctx = _rctx();
    uint32_t          expected = 0;
    if (n00b_atomic_cas(&ctx->named_cache_mutex_init, &expected, 1)) {
        n00b_sys_mutex_init(&ctx->named_cache_mutex, (char *)__FILE__);
        n00b_atomic_store(&ctx->named_cache_mutex_init, 2);
        return;
    }
    while (n00b_atomic_load(&ctx->named_cache_mutex_init) != 2) {
        // spin until winner finishes init
    }
}

static const rs_named_pairset_t *
intern_pairset(const char                  *canonical,
               const n00b_codepoint_pair_t *ranges,
               size_t                       n)
{
    ensure_named_cache_mutex();
    n00b_regex_ctx_t *ctx = _rctx();
    n00b_mutex_lock(&ctx->named_cache_mutex);

    for (size_t i = 0; i < ctx->named_cache_len; i++) {
        if (strcmp(ctx->named_cache[i].name, canonical) == 0) {
            const rs_named_pairset_t *out = &ctx->named_cache[i].set;
            n00b_mutex_unlock(&ctx->named_cache_mutex);
            return out;
        }
    }
    if (ctx->named_cache_len == ctx->named_cache_cap) {
        size_t newcap = ctx->named_cache_cap == 0
                            ? 16
                            : safe_mul_sz(ctx->named_cache_cap, 2);
        cached_pairset_t *new_cache = n00b_alloc_array(cached_pairset_t, newcap);
        if (ctx->named_cache_len > 0 && ctx->named_cache != nullptr) {
            memcpy(new_cache, ctx->named_cache,
                   safe_mul_sz(ctx->named_cache_len, sizeof(cached_pairset_t)));
        }
        if (ctx->named_cache != nullptr) {
            n00b_free(ctx->named_cache);
        }
        ctx->named_cache     = new_cache;
        ctx->named_cache_cap = newcap;
    }
    cached_pairset_t *e = &ctx->named_cache[ctx->named_cache_len++];
    e->name             = strdup_cstr(canonical);
    e->set.name         = e->name;
    e->set.pairs        = wrap_pair_ptr(ranges);
    e->set.pairs_len    = n;
    const rs_named_pairset_t *out = &e->set;
    n00b_mutex_unlock(&ctx->named_cache_mutex);
    return out;
}

// Build a tagged cache key in @p out_buf (a stack buffer).  Returns nullptr
// if the formatted key won't fit; otherwise out_buf is NUL-terminated and a
// pointer to it is returned.
static const char *
build_tagged_key(char *out_buf, size_t out_cap, const char *prefix,
                 const char *name)
{
    size_t pl = strlen(prefix);
    size_t nl = strlen(name);
    if (safe_add_sz(pl, safe_add_sz(nl, 1)) > out_cap) {
        return nullptr;
    }
    memcpy(out_buf, prefix, pl);
    memcpy(out_buf + pl, name, nl);
    out_buf[pl + nl] = '\0';
    return out_buf;
}

// Resolve a GC name to ranges.  Tries the base GC enum first (loose match),
// then derived composite names (Letter, Mark, ...).  Caches the resulting
// pairset.  Returns nullptr if `canonical` does not name a GC.
static const rs_named_pairset_t *
rs_lookup_gencat(const char *canonical)
{
    if (!canonical) {
        return nullptr;
    }

    // 1. Try base enum.
    n00b_unicode_gc_t gc;
    if (n00b_unicode_gc_by_name(canonical, &gc)) {
        const n00b_codepoint_pair_t *r;
        size_t                       n;
        n00b_unicode_general_category_ranges(gc, &r, &n);
        return intern_pairset(canonical, r, n);
    }

    // 2. Try derived (Letter, Mark, Number, Punctuation, Symbol, Separator,
    //    Other, Cased_Letter).
    const n00b_codepoint_pair_t *r;
    size_t                       n;
    if (n00b_unicode_gc_derived_ranges(canonical, &r, &n)) {
        return intern_pairset(canonical, r, n);
    }
    return nullptr;
}

// Resolve a Script name (loose-matched).
static const rs_named_pairset_t *
rs_lookup_script(const char *name)
{
    if (!name) {
        return nullptr;
    }
    n00b_unicode_script_t sc;
    if (!n00b_unicode_script_by_name(name, &sc)) {
        return nullptr;
    }
    const n00b_codepoint_pair_t *r;
    size_t                       n;
    n00b_unicode_script_ranges(sc, &r, &n);
    char        key[160];
    const char *k = build_tagged_key(key, sizeof(key), "sc:", name);
    if (!k) {
        return nullptr;
    }
    return intern_pairset(k, r, n);
}

// Resolve a Script_Extensions name (loose-matched).
static const rs_named_pairset_t *
rs_lookup_script_ext(const char *name)
{
    if (!name) {
        return nullptr;
    }
    n00b_unicode_script_t sc;
    if (!n00b_unicode_script_by_name(name, &sc)) {
        return nullptr;
    }
    const n00b_codepoint_pair_t *r;
    size_t                       n;
    n00b_unicode_script_extensions_ranges(sc, &r, &n);
    char        key[160];
    const char *k = build_tagged_key(key, sizeof(key), "scx:", name);
    if (!k) {
        return nullptr;
    }
    return intern_pairset(k, r, n);
}

// Resolve a Block name (loose-matched).
static const rs_named_pairset_t *
rs_lookup_block(const char *name)
{
    if (!name) {
        return nullptr;
    }
    n00b_unicode_block_t bl;
    if (!n00b_unicode_block_by_name(name, &bl)) {
        return nullptr;
    }
    const n00b_codepoint_pair_t *r;
    size_t                       n;
    n00b_unicode_block_ranges_for(bl, &r, &n);
    char        key[200];
    const char *k = build_tagged_key(key, sizeof(key), "blk:", name);
    if (!k) {
        return nullptr;
    }
    return intern_pairset(k, r, n);
}

// Resolve a binary property name (loose-matched).
static const rs_named_pairset_t *
rs_lookup_property(const char *name)
{
    if (!name) {
        return nullptr;
    }
    n00b_unicode_property_t prop;
    if (!n00b_unicode_property_by_name(name, &prop)) {
        return nullptr;
    }
    const n00b_codepoint_pair_t *r;
    size_t                       n;
    n00b_unicode_property_ranges(prop, &r, &n);
    char        key[160];
    const char *k = build_tagged_key(key, sizeof(key), "prop:", name);
    if (!k) {
        return nullptr;
    }
    return intern_pairset(k, r, n);
}

// Resolve a Bidi_Class name (loose-matched).
static const rs_named_pairset_t *
rs_lookup_bidi_class(const char *name)
{
    if (!name) {
        return nullptr;
    }
    n00b_unicode_bidi_class_t bc;
    if (!n00b_unicode_bidi_class_by_name(name, &bc)) {
        return nullptr;
    }
    const n00b_codepoint_pair_t *r;
    size_t                       n;
    n00b_unicode_bidi_class_ranges(bc, &r, &n);
    char        key[96];
    const char *k = build_tagged_key(key, sizeof(key), "bc:", name);
    if (!k) {
        return nullptr;
    }
    return intern_pairset(k, r, n);
}

// Resolve a Grapheme_Cluster_Break name (loose-matched).
static const rs_named_pairset_t *
rs_lookup_grapheme_break(const char *name)
{
    if (!name) {
        return nullptr;
    }
    n00b_unicode_gcb_t v;
    if (!n00b_unicode_gcb_by_name(name, &v)) {
        return nullptr;
    }
    const n00b_codepoint_pair_t *r;
    size_t                       n;
    n00b_unicode_grapheme_break_ranges(v, &r, &n);
    char        key[96];
    const char *k = build_tagged_key(key, sizeof(key), "gcb:", name);
    if (!k) {
        return nullptr;
    }
    return intern_pairset(k, r, n);
}

// Resolve a Word_Break name (loose-matched).
static const rs_named_pairset_t *
rs_lookup_word_break(const char *name)
{
    if (!name) {
        return nullptr;
    }
    n00b_unicode_wb_t v;
    if (!n00b_unicode_wb_by_name(name, &v)) {
        return nullptr;
    }
    const n00b_codepoint_pair_t *r;
    size_t                       n;
    n00b_unicode_word_break_ranges(v, &r, &n);
    char        key[96];
    const char *k = build_tagged_key(key, sizeof(key), "wb:", name);
    if (!k) {
        return nullptr;
    }
    return intern_pairset(k, r, n);
}

// Resolve a Sentence_Break name (loose-matched).
static const rs_named_pairset_t *
rs_lookup_sentence_break(const char *name)
{
    if (!name) {
        return nullptr;
    }
    n00b_unicode_sb_t v;
    if (!n00b_unicode_sb_by_name(name, &v)) {
        return nullptr;
    }
    const n00b_codepoint_pair_t *r;
    size_t                       n;
    n00b_unicode_sentence_break_ranges(v, &r, &n);
    char        key[96];
    const char *k = build_tagged_key(key, sizeof(key), "sb:", name);
    if (!k) {
        return nullptr;
    }
    return intern_pairset(k, r, n);
}

// Resolve a Line_Break name (loose-matched).
static const rs_named_pairset_t *
rs_lookup_line_break(const char *name)
{
    if (!name) {
        return nullptr;
    }
    n00b_unicode_lb_t v;
    if (!n00b_unicode_lb_by_name(name, &v)) {
        return nullptr;
    }
    const n00b_codepoint_pair_t *r;
    size_t                       n;
    n00b_unicode_line_break_ranges(v, &r, &n);
    char        key[96];
    const char *k = build_tagged_key(key, sizeof(key), "lb:", name);
    if (!k) {
        return nullptr;
    }
    return intern_pairset(k, r, n);
}

// Resolve an Age name like "12.0" or "V12_0".  Returns false if not found.
static bool
rs_lookup_age(const char             *name,
              const rs_uchar_pair_t **out_pairs,
              size_t                 *out_len)
{
    const n00b_codepoint_pair_t *r;
    size_t                       n;
    if (!n00b_unicode_age_ranges(name, &r, &n)) {
        return false;
    }
    *out_pairs = wrap_pair_ptr(r);
    *out_len   = n;
    return true;
}

// Per-class Perl-kind ranges, materialized as needed.  Idempotent: the
// underlying n00b unicode caches make repeat calls cheap.

static const rs_uchar_pair_t *
rs_perl_decimal_pairs(size_t *out_len)
{
    // \d == \p{Nd} == GC=Decimal_Number.
    const n00b_codepoint_pair_t *r;
    size_t                       n;
    n00b_unicode_general_category_ranges(N00B_UNICODE_GC_ND, &r, &n);
    *out_len = n;
    return wrap_pair_ptr(r);
}

static const rs_uchar_pair_t *
rs_perl_space_pairs(size_t *out_len)
{
    // \s == \p{White_Space}.
    const n00b_codepoint_pair_t *r;
    size_t                       n;
    n00b_unicode_property_ranges(N00B_UNICODE_PROP_WHITE_SPACE, &r, &n);
    *out_len = n;
    return wrap_pair_ptr(r);
}

// \w == \p{Alphabetic} ∪ \p{M} ∪ \p{Nd} ∪ \p{Pc} ∪ \p{Join_Control}.
// Cache + init flag live on `n00b_runtime_t::regex_ctx`.
static void
rs_perl_word_init_locked(void)
{
    n00b_regex_ctx_t *ctx = _rctx();
    rs_uvec_t v;
    uvec_init(&v);

    // \p{Alphabetic}
    {
        const n00b_codepoint_pair_t *r;
        size_t                       n;
        n00b_unicode_property_ranges(N00B_UNICODE_PROP_ALPHABETIC, &r, &n);
        for (size_t i = 0; i < n; i++) {
            uvec_push(&v, (rs_urange_t){r[i].lo, r[i].hi});
        }
    }
    // \p{M} = derived "Mark" = Mn ∪ Mc ∪ Me
    for (n00b_unicode_gc_t g = N00B_UNICODE_GC_MN; g <= N00B_UNICODE_GC_ME;
         g++) {
        const n00b_codepoint_pair_t *r;
        size_t                       n;
        n00b_unicode_general_category_ranges(g, &r, &n);
        for (size_t i = 0; i < n; i++) {
            uvec_push(&v, (rs_urange_t){r[i].lo, r[i].hi});
        }
    }
    // \p{Nd}
    {
        const n00b_codepoint_pair_t *r;
        size_t                       n;
        n00b_unicode_general_category_ranges(N00B_UNICODE_GC_ND, &r, &n);
        for (size_t i = 0; i < n; i++) {
            uvec_push(&v, (rs_urange_t){r[i].lo, r[i].hi});
        }
    }
    // \p{Pc}
    {
        const n00b_codepoint_pair_t *r;
        size_t                       n;
        n00b_unicode_general_category_ranges(N00B_UNICODE_GC_PC, &r, &n);
        for (size_t i = 0; i < n; i++) {
            uvec_push(&v, (rs_urange_t){r[i].lo, r[i].hi});
        }
    }
    // \p{Join_Control}
    {
        const n00b_codepoint_pair_t *r;
        size_t                       n;
        n00b_unicode_property_ranges(N00B_UNICODE_PROP_JOIN_CONTROL, &r, &n);
        for (size_t i = 0; i < n; i++) {
            uvec_push(&v, (rs_urange_t){r[i].lo, r[i].hi});
        }
    }
    uvec_canonicalize(&v);

    ctx->perl_word_pairs = n00b_alloc_array(rs_uchar_pair_t, v.len);
    for (size_t i = 0; i < v.len; i++) {
        ctx->perl_word_pairs[i].start = v.data[i].start;
        ctx->perl_word_pairs[i].end   = v.data[i].end;
    }
    ctx->perl_word_pairs_n = v.len;
    uvec_free(&v);
}

static void
ensure_perl_word(void)
{
    n00b_regex_ctx_t *ctx = _rctx();
    uint32_t expected = 0;
    if (n00b_atomic_cas(&ctx->perl_word_init, &expected, 1)) {
        rs_perl_word_init_locked();
        n00b_atomic_store(&ctx->perl_word_init, 2);
        return;
    }
    while (n00b_atomic_load(&ctx->perl_word_init) != 2) {
        // spin until winner finishes init
    }
}

static const rs_uchar_pair_t *
rs_perl_word_pairs(size_t *out_len)
{
    ensure_perl_word();
    n00b_regex_ctx_t *ctx = _rctx();
    *out_len = ctx->perl_word_pairs_n;
    return ctx->perl_word_pairs;
}

static const rs_named_pairset_t *
find_gencat(const char *canonical)
{
    return rs_lookup_gencat(canonical);
}

static void
uvec_append_pairs(rs_uvec_t *v, const rs_uchar_pair_t *pairs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uvec_push(v, (rs_urange_t){pairs[i].start, pairs[i].end});
    }
    uvec_canonicalize(v);
}

static bool
resolve_gencat_into(rs_uvec_t *out, const char *canonical)
{
    const rs_named_pairset_t *p = find_gencat(canonical);
    if (!p) {
        return false;
    }
    uvec_append_pairs(out, p->pairs, p->pairs_len);
    return true;
}

// Try to resolve a normalized name as a binary (boolean) property.
// Per regex-syntax (the unicode module):309, "cf", "sc", and "lc" are
// excluded from the binary lookup so they fall through to the GC abbreviation
// lookup (Format, Currency_Symbol, Cased_Letter respectively).
static bool
try_resolve_binary_property(const char *norm, rs_uvec_t *out)
{
    if (strcmp(norm, "cf") == 0) {
        return false;
    }
    if (strcmp(norm, "sc") == 0) {
        return false;
    }
    if (strcmp(norm, "lc") == 0) {
        return false;
    }
    const rs_named_pairset_t *p = rs_lookup_property(norm);
    if (!p) {
        return false;
    }
    uvec_append_pairs(out, p->pairs, p->pairs_len);
    return true;
}

static bool
try_resolve_gc_name(const char *norm, rs_uvec_t *out)
{
    // Pseudo categories first (Rust's gencat() short-circuits these).
    gc_pseudo_t ps = resolve_pseudo(norm);
    if (ps == GC_PSEUDO_ASCII) {
        uvec_push(out, (rs_urange_t){0u, 0x7Fu});
        uvec_canonicalize(out);
        return true;
    }
    if (ps == GC_PSEUDO_ANY) {
        uvec_push(out, (rs_urange_t){0u, RS_UCP_MAX});
        uvec_canonicalize(out);
        return true;
    }
    if (ps == GC_PSEUDO_ASSIGNED) {
        rs_uvec_t un;
        uvec_init(&un);
        const rs_named_pairset_t *p = find_gencat("Unassigned");
        if (!p) {
            uvec_free(&un);
            return false;
        }
        uvec_append_pairs(&un, p->pairs, p->pairs_len);
        uvec_negate(&un);
        for (size_t i = 0; i < un.len; i++) {
            uvec_push(out, un.data[i]);
        }
        uvec_canonicalize(out);
        uvec_free(&un);
        return true;
    }
    const char *canonical = resolve_gc_alias(norm);
    if (!canonical) {
        return false;
    }
    return resolve_gencat_into(out, canonical);
}

// Translator entry point — mirrors regex-syntax 0.8.10's
// `ClassQuery::canonicalize` + `unicode::class()`.
// Resolution order for OneLetter / Named (no '=' in source):
//   1) binary property (skipping cf/sc/lc, which collide with GC abbrevs)
//   2) general_category (incl. pseudo "ascii"/"any"/"assigned")
//   3) script
//   4) (extension) block via the legacy `\p{In…}` shorthand
// Resolution order for ByValue (`\p{name=value}`): dispatch on canonical
// name to GC / Script / Script_Extensions / Age / Block / Bidi_Class /
// segmentation breaks.
static void
resolve_unicode_class(const rs_ClassUnicode *cu, rs_uvec_t *out, bool *ok)
{
    *ok = false;

    if (cu->kind_tag == RS_CLASS_UNICODE_KIND_NAMED_VALUE) {
        // ByValue.
        if (!cu->name || !cu->value) {
            return;
        }

        char   nbuf[64];
        size_t nl = strlen(cu->name);
        if (nl >= sizeof(nbuf)) {
            return;
        }
        memcpy(nbuf, cu->name, nl);
        size_t new_nl = normalize_symbolic(nbuf, nl);
        nbuf[new_nl]  = '\0';

        char   vbuf[160];
        size_t vl = strlen(cu->value);
        if (vl >= sizeof(vbuf)) {
            return;
        }
        memcpy(vbuf, cu->value, vl);
        size_t new_vl = normalize_symbolic(vbuf, vl);
        vbuf[new_vl]  = '\0';

        if (strcmp(nbuf, "gc") == 0 || strcmp(nbuf, "generalcategory") == 0) {
            if (!try_resolve_gc_name(vbuf, out)) {
                return;
            }
            *ok = true;
            return;
        }
        if (strcmp(nbuf, "sc") == 0 || strcmp(nbuf, "script") == 0) {
            const rs_named_pairset_t *p = rs_lookup_script(cu->value);
            if (!p) {
                return;
            }
            uvec_append_pairs(out, p->pairs, p->pairs_len);
            *ok = true;
            return;
        }
        if (strcmp(nbuf, "scx") == 0
            || strcmp(nbuf, "scriptextensions") == 0) {
            const rs_named_pairset_t *p = rs_lookup_script_ext(cu->value);
            if (!p) {
                return;
            }
            uvec_append_pairs(out, p->pairs, p->pairs_len);
            *ok = true;
            return;
        }
        if (strcmp(nbuf, "age") == 0) {
            const rs_uchar_pair_t *pairs;
            size_t                 n;
            if (!rs_lookup_age(cu->value, &pairs, &n)) {
                return;
            }
            uvec_append_pairs(out, pairs, n);
            *ok = true;
            return;
        }
        if (strcmp(nbuf, "blk") == 0 || strcmp(nbuf, "block") == 0
            || strcmp(nbuf, "in") == 0) {
            const rs_named_pairset_t *p = rs_lookup_block(cu->value);
            if (!p) {
                return;
            }
            uvec_append_pairs(out, p->pairs, p->pairs_len);
            *ok = true;
            return;
        }
        if (strcmp(nbuf, "bc") == 0 || strcmp(nbuf, "bidiclass") == 0) {
            const rs_named_pairset_t *p = rs_lookup_bidi_class(cu->value);
            if (!p) {
                return;
            }
            uvec_append_pairs(out, p->pairs, p->pairs_len);
            *ok = true;
            return;
        }
        if (strcmp(nbuf, "gcb") == 0
            || strcmp(nbuf, "graphemeclusterbreak") == 0) {
            const rs_named_pairset_t *p = rs_lookup_grapheme_break(cu->value);
            if (!p) {
                return;
            }
            uvec_append_pairs(out, p->pairs, p->pairs_len);
            *ok = true;
            return;
        }
        if (strcmp(nbuf, "wb") == 0 || strcmp(nbuf, "wordbreak") == 0) {
            const rs_named_pairset_t *p = rs_lookup_word_break(cu->value);
            if (!p) {
                return;
            }
            uvec_append_pairs(out, p->pairs, p->pairs_len);
            *ok = true;
            return;
        }
        if (strcmp(nbuf, "sb") == 0 || strcmp(nbuf, "sentencebreak") == 0) {
            const rs_named_pairset_t *p = rs_lookup_sentence_break(cu->value);
            if (!p) {
                return;
            }
            uvec_append_pairs(out, p->pairs, p->pairs_len);
            *ok = true;
            return;
        }
        if (strcmp(nbuf, "lb") == 0 || strcmp(nbuf, "linebreak") == 0) {
            const rs_named_pairset_t *p = rs_lookup_line_break(cu->value);
            if (!p) {
                return;
            }
            uvec_append_pairs(out, p->pairs, p->pairs_len);
            *ok = true;
            return;
        }
        // Unknown property name — leave *ok = false.
        return;
    }

    // OneLetter / Named.
    char        buf[128];
    const char *raw     = nullptr;
    size_t      raw_len = 0;
    char        one_letter_buf[8];
    if (cu->kind_tag == RS_CLASS_UNICODE_KIND_ONE_LETTER) {
        uint8_t enc[4];
        size_t  n = encode_utf8(cu->one_letter, enc);
        if (n >= sizeof(one_letter_buf)) {
            return;
        }
        memcpy(one_letter_buf, enc, n);
        one_letter_buf[n] = '\0';
        raw               = one_letter_buf;
        raw_len           = n;
    }
    else if (cu->kind_tag == RS_CLASS_UNICODE_KIND_NAMED) {
        if (!cu->name) {
            return;
        }
        raw     = cu->name;
        raw_len = strlen(cu->name);
    }
    else {
        return;
    }

    if (raw_len >= sizeof(buf)) {
        return;
    }
    memcpy(buf, raw, raw_len);
    size_t nlen = normalize_symbolic(buf, raw_len);
    buf[nlen]   = '\0';

    // Step 1: binary property (with the cf/sc/lc carve-out).
    if (try_resolve_binary_property(buf, out)) {
        *ok = true;
        return;
    }
    // Step 2: GC.
    if (try_resolve_gc_name(buf, out)) {
        *ok = true;
        return;
    }
    // Step 3: Script.
    {
        // Try via the canonical (name-as-given) lookup which handles loose
        // matching itself.  Pass cu->name so the "Is" prefix from
        // normalize_symbolic doesn't cripple us — but for robustness fall
        // back to the normalized buf if needed.
        const rs_named_pairset_t *p = nullptr;
        if (cu->kind_tag == RS_CLASS_UNICODE_KIND_NAMED && cu->name) {
            p = rs_lookup_script(cu->name);
        }
        if (!p) {
            p = rs_lookup_script(buf);
        }
        if (p) {
            uvec_append_pairs(out, p->pairs, p->pairs_len);
            *ok = true;
            return;
        }
    }
    // Step 4 (extension): Block via the `\p{In...}` shorthand.  Try the
    // original (un-normalized) name with an "In" prefix stripped.
    if (cu->kind_tag == RS_CLASS_UNICODE_KIND_NAMED && cu->name) {
        const char *orig = cu->name;
        size_t      ol   = strlen(orig);
        if (ol >= 3 && (orig[0] == 'I' || orig[0] == 'i')
            && (orig[1] == 'n' || orig[1] == 'N')) {
            const rs_named_pairset_t *p = rs_lookup_block(orig + 2);
            if (p) {
                uvec_append_pairs(out, p->pairs, p->pairs_len);
                *ok = true;
                return;
            }
        }
    }
}

// Perl Unicode classes (\d, \s, \w) — regex-syntax translate:1081.
static void
resolve_perl_unicode(rs_ClassPerlKind kind, rs_uvec_t *out)
{
    size_t                 n;
    const rs_uchar_pair_t *pairs;
    switch (kind) {
    case RS_CLASS_PERL_KIND_DIGIT:
        pairs = rs_perl_decimal_pairs(&n);
        break;
    case RS_CLASS_PERL_KIND_SPACE:
        pairs = rs_perl_space_pairs(&n);
        break;
    case RS_CLASS_PERL_KIND_WORD:
        pairs = rs_perl_word_pairs(&n);
        break;
    default:
        return;
    }
    uvec_append_pairs(out, pairs, n);
}

// Perl byte classes (regex-syntax translate:1103, ascii_class subset).
static void
resolve_perl_bytes(rs_ClassPerlKind kind, rs_bvec_t *out)
{
    switch (kind) {
    case RS_CLASS_PERL_KIND_DIGIT:
        bvec_push(out, (rs_brange_t){'0', '9'});
        break;
    case RS_CLASS_PERL_KIND_SPACE:
        bvec_push(out, (rs_brange_t){'\t', '\t'});
        bvec_push(out, (rs_brange_t){'\n', '\n'});
        bvec_push(out, (rs_brange_t){0x0B, 0x0B});
        bvec_push(out, (rs_brange_t){0x0C, 0x0C});
        bvec_push(out, (rs_brange_t){'\r', '\r'});
        bvec_push(out, (rs_brange_t){' ', ' '});
        break;
    case RS_CLASS_PERL_KIND_WORD:
        bvec_push(out, (rs_brange_t){'0', '9'});
        bvec_push(out, (rs_brange_t){'A', 'Z'});
        bvec_push(out, (rs_brange_t){'_', '_'});
        bvec_push(out, (rs_brange_t){'a', 'z'});
        break;
    }
    bvec_canonicalize(out);
}

// regex-syntax translate:1319 — ASCII class byte ranges.
static void
ascii_class_bytes(int kind, rs_bvec_t *out)
{
    switch (kind) {
    case RS_CLASS_ASCII_KIND_ALNUM:
        bvec_push(out, (rs_brange_t){'0', '9'});
        bvec_push(out, (rs_brange_t){'A', 'Z'});
        bvec_push(out, (rs_brange_t){'a', 'z'});
        break;
    case RS_CLASS_ASCII_KIND_ALPHA:
        bvec_push(out, (rs_brange_t){'A', 'Z'});
        bvec_push(out, (rs_brange_t){'a', 'z'});
        break;
    case RS_CLASS_ASCII_KIND_ASCII:
        bvec_push(out, (rs_brange_t){0x00, 0x7F});
        break;
    case RS_CLASS_ASCII_KIND_BLANK:
        bvec_push(out, (rs_brange_t){'\t', '\t'});
        bvec_push(out, (rs_brange_t){' ', ' '});
        break;
    case RS_CLASS_ASCII_KIND_CNTRL:
        bvec_push(out, (rs_brange_t){0x00, 0x1F});
        bvec_push(out, (rs_brange_t){0x7F, 0x7F});
        break;
    case RS_CLASS_ASCII_KIND_DIGIT:
        bvec_push(out, (rs_brange_t){'0', '9'});
        break;
    case RS_CLASS_ASCII_KIND_GRAPH:
        bvec_push(out, (rs_brange_t){'!', '~'});
        break;
    case RS_CLASS_ASCII_KIND_LOWER:
        bvec_push(out, (rs_brange_t){'a', 'z'});
        break;
    case RS_CLASS_ASCII_KIND_PRINT:
        bvec_push(out, (rs_brange_t){' ', '~'});
        break;
    case RS_CLASS_ASCII_KIND_PUNCT:
        bvec_push(out, (rs_brange_t){'!', '/'});
        bvec_push(out, (rs_brange_t){':', '@'});
        bvec_push(out, (rs_brange_t){'[', '`'});
        bvec_push(out, (rs_brange_t){'{', '~'});
        break;
    case RS_CLASS_ASCII_KIND_SPACE:
        bvec_push(out, (rs_brange_t){'\t', '\t'});
        bvec_push(out, (rs_brange_t){'\n', '\n'});
        bvec_push(out, (rs_brange_t){0x0B, 0x0B});
        bvec_push(out, (rs_brange_t){0x0C, 0x0C});
        bvec_push(out, (rs_brange_t){'\r', '\r'});
        bvec_push(out, (rs_brange_t){' ', ' '});
        break;
    case RS_CLASS_ASCII_KIND_UPPER:
        bvec_push(out, (rs_brange_t){'A', 'Z'});
        break;
    case RS_CLASS_ASCII_KIND_WORD:
        bvec_push(out, (rs_brange_t){'0', '9'});
        bvec_push(out, (rs_brange_t){'A', 'Z'});
        bvec_push(out, (rs_brange_t){'_', '_'});
        bvec_push(out, (rs_brange_t){'a', 'z'});
        break;
    case RS_CLASS_ASCII_KIND_XDIGIT:
        bvec_push(out, (rs_brange_t){'0', '9'});
        bvec_push(out, (rs_brange_t){'A', 'F'});
        bvec_push(out, (rs_brange_t){'a', 'f'});
        break;
    }
    bvec_canonicalize(out);
}

static void
ascii_class_unicode(int kind, rs_uvec_t *out)
{
    rs_bvec_t bv;
    bvec_init(&bv);
    ascii_class_bytes(kind, &bv);
    for (size_t i = 0; i < bv.len; i++) {
        uvec_push(out, (rs_urange_t){bv.data[i].start, bv.data[i].end});
    }
    uvec_canonicalize(out);
    bvec_free(&bv);
}

// ===========================================================================
// Simple case folding.
//
// resharp-c embedded an auto-generated CaseFolding.txt-derived table of the
// simple-fold equivalence classes (a single key codepoint -> array of
// equivalent codepoints).  We rebuild that table at first use by inverting
// `n00b_unicode_casefold_cp(cp)` over the BMP + supplementary planes:
//
//   - For each cp in [0..0x10FFFF] (skipping the surrogate hole) call
//     n00b_unicode_casefold_cp(cp).  The fold value is the canonical
//     representative of the equivalence class.
//   - Bucket cps by their fold value.
//   - For each cp whose bucket has size >= 2, that cp's fold-equivalents
//     are the OTHER members of its bucket.
//
// This recovers the same simple-fold equivalence relation that
// regex-syntax's CaseFoldingSimple table encodes (UCD CaseFolding.txt
// status C+S), but using only the public n00b unicode surface.
// ===========================================================================

// `rs_case_fold_entry_t` is defined as `n00b_regex_case_fold_entry_t`
// in `include/text/regex/ctx.h`.  Alias the canonical name here.
typedef n00b_regex_case_fold_entry_t rs_case_fold_entry_t;

static int
cmp_uint32(const void *a, const void *b)
{
    uint32_t ua = *(const uint32_t *)a;
    uint32_t ub = *(const uint32_t *)b;
    if (ua != ub) {
        return ua < ub ? -1 : 1;
    }
    return 0;
}

static int
cmp_case_fold_entry_by_key(const void *a, const void *b)
{
    const rs_case_fold_entry_t *ea = (const rs_case_fold_entry_t *)a;
    const rs_case_fold_entry_t *eb = (const rs_case_fold_entry_t *)b;
    if (ea->key != eb->key) {
        return ea->key < eb->key ? -1 : 1;
    }
    return 0;
}

// Build the simple-fold equivalence table by inverting
// n00b_unicode_casefold_cp.  Two passes:
//   pass 1: for every cp, collect (cp, fold_cp) pairs where fold_cp != cp;
//   pass 2: bucket by fold_cp, then for every cp in each bucket of size >= 2
//           record (cp -> [bucket members - {cp}]).
// The bucket itself also contains the fold representative — that is one
// of the equivalents (it's the canonical cp every other member maps to).
static void
case_fold_init_locked(void)
{
    // Pass 1 — count bucket sizes (indexed by fold_cp).  Allocate the
    // count array as a sparse uint16_t table over 0x110000 entries
    // (~2 MiB; transient — freed at the end of init).
    static constexpr size_t CP_RANGE = 0x110000u;
    uint16_t *count = n00b_alloc_array(uint16_t, CP_RANGE);

    // Pass 1a: count.  cp itself is part of each bucket where it appears.
    // We iterate all cps including those that fold to themselves; the
    // bucket size includes the canonical cp.
    for (uint32_t cp = 0; cp < CP_RANGE; cp++) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) {
            continue;
        }
        uint32_t f = n00b_unicode_casefold_cp(cp);
        // Bucket cps by their fold representative.
        n00b_require(f < CP_RANGE, "case_fold: fold cp out of range");
        if (count[f] < UINT16_MAX) {
            count[f]++;
        }
    }

    // Pass 2 — build bucket-member arrays.  bucket_members[f] holds the
    // list of cps that fold to f, including f itself.  Sparse alloc:
    // only buckets with size >= 2 are interesting (singletons fold to
    // themselves and contribute no equivalence-class entries).
    uint32_t **bucket_members = n00b_alloc_array(uint32_t *, CP_RANGE);
    uint16_t  *bucket_fill    = n00b_alloc_array(uint16_t, CP_RANGE);
    for (uint32_t f = 0; f < CP_RANGE; f++) {
        if (count[f] >= 2) {
            bucket_members[f] = n00b_alloc_array(uint32_t, count[f]);
        }
    }

    for (uint32_t cp = 0; cp < CP_RANGE; cp++) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) {
            continue;
        }
        uint32_t f = n00b_unicode_casefold_cp(cp);
        if (count[f] >= 2) {
            bucket_members[f][bucket_fill[f]++] = cp;
        }
    }

    // Pass 3 — count number of (key, fold-list) entries we'll emit:
    // for each bucket with size N >= 2, every member becomes a key with
    // (N - 1) folds.  Sum of N across all multi-buckets gives total keys.
    size_t total_keys = 0;
    for (uint32_t f = 0; f < CP_RANGE; f++) {
        if (count[f] >= 2) {
            total_keys = safe_add_sz(total_keys, count[f]);
        }
    }

    rs_case_fold_entry_t *entries = nullptr;
    if (total_keys > 0) {
        entries = n00b_alloc_array(rs_case_fold_entry_t, total_keys);
    }
    size_t out_idx = 0;

    for (uint32_t f = 0; f < CP_RANGE; f++) {
        if (count[f] < 2) {
            continue;
        }
        uint32_t *members = bucket_members[f];
        size_t    n       = count[f];
        // Members may not be sorted (they are added in cp-iteration order,
        // which is sorted, but be defensive).
        qsort(members, n, sizeof(uint32_t), cmp_uint32);
        for (size_t k = 0; k < n; k++) {
            uint32_t key   = members[k];
            size_t   nfold = n - 1;
            // The simple-fold entries n.fold.len fits in 8 bits since the
            // largest fold equivalence class in Unicode 16 has < 8 members.
            n00b_require(nfold < 256, "case_fold: bucket overflow");
            uint32_t *fold = nullptr;
            if (nfold > 0) {
                fold = n00b_alloc_array(uint32_t, nfold);
                size_t w = 0;
                for (size_t j = 0; j < n; j++) {
                    if (members[j] != key) {
                        fold[w++] = members[j];
                    }
                }
            }
            entries[out_idx++] = (rs_case_fold_entry_t){
                .key      = key,
                .fold     = fold,
                .fold_len = (uint8_t)nfold,
            };
        }
    }

    // Sort entries by key for binary search.
    if (out_idx > 1) {
        qsort(entries, out_idx, sizeof(rs_case_fold_entry_t),
              cmp_case_fold_entry_by_key);
    }

    n00b_regex_ctx_t *ctx = _rctx();
    ctx->case_fold_table     = entries;
    ctx->case_fold_table_len = out_idx;

    // Drop transient buckets.
    for (uint32_t f = 0; f < CP_RANGE; f++) {
        if (bucket_members[f]) {
            n00b_free(bucket_members[f]);
        }
    }
    n00b_free(bucket_members);
    n00b_free(bucket_fill);
    n00b_free(count);
}

static void
ensure_case_fold(void)
{
    n00b_regex_ctx_t *ctx = _rctx();
    uint32_t expected = 0;
    if (n00b_atomic_cas(&ctx->case_fold_init, &expected, 1)) {
        case_fold_init_locked();
        n00b_atomic_store(&ctx->case_fold_init, 2);
        return;
    }
    while (n00b_atomic_load(&ctx->case_fold_init) != 2) {
        // spin until winner finishes init
    }
}

static const rs_case_fold_entry_t *
case_fold_lookup(uint32_t cp)
{
    ensure_case_fold();
    n00b_regex_ctx_t *ctx = _rctx();
    if (ctx->case_fold_table_len == 0) {
        return nullptr;
    }
    size_t lo = 0, hi = ctx->case_fold_table_len;
    while (lo < hi) {
        size_t   mid = lo + (hi - lo) / 2;
        uint32_t k   = ctx->case_fold_table[mid].key;
        if (k == cp) {
            return &ctx->case_fold_table[mid];
        }
        if (k < cp) {
            lo = mid + 1;
        }
        else {
            hi = mid;
        }
    }
    return nullptr;
}

static bool
case_fold_overlaps(uint32_t start, uint32_t end)
{
    ensure_case_fold();
    n00b_regex_ctx_t *ctx = _rctx();
    if (ctx->case_fold_table_len == 0) {
        return false;
    }
    size_t lo = 0, hi = ctx->case_fold_table_len;
    while (lo < hi) {
        size_t   mid = lo + (hi - lo) / 2;
        uint32_t k   = ctx->case_fold_table[mid].key;
        if (start <= k && k <= end) {
            return true;
        }
        if (k > end) {
            hi = mid;
        }
        else {
            lo = mid + 1;
        }
    }
    return false;
}

static void
uvec_case_fold_simple(rs_uvec_t *v)
{
    size_t orig = v->len;
    for (size_t i = 0; i < orig; i++) {
        rs_urange_t r = v->data[i];
        if (!case_fold_overlaps(r.start, r.end)) {
            continue;
        }
        for (uint32_t cp = r.start;; cp++) {
            if (!(cp >= 0xD800u && cp <= 0xDFFFu)) {
                const rs_case_fold_entry_t *e = case_fold_lookup(cp);
                if (e) {
                    for (size_t k = 0; k < e->fold_len; k++) {
                        uvec_push(v, (rs_urange_t){e->fold[k], e->fold[k]});
                    }
                }
            }
            if (cp == r.end) {
                break;
            }
        }
    }
    uvec_canonicalize(v);
}

// regex-syntax HIR:1399 — bytes case_fold_simple is purely ASCII (a-z ↔ A-Z).
static void
bvec_case_fold_simple(rs_bvec_t *v)
{
    size_t orig = v->len;
    for (size_t i = 0; i < orig; i++) {
        rs_brange_t r = v->data[i];
        for (unsigned int b = r.start;; b++) {
            if (b >= 'a' && b <= 'z') {
                bvec_push(v, (rs_brange_t){(uint8_t)(b - 32u), (uint8_t)(b - 32u)});
            }
            else if (b >= 'A' && b <= 'Z') {
                bvec_push(v, (rs_brange_t){(uint8_t)(b + 32u), (uint8_t)(b + 32u)});
            }
            if (b == r.end) {
                break;
            }
        }
    }
    bvec_canonicalize(v);
}

// ===========================================================================
// Translator
// ===========================================================================

// regex-syntax translate:1154 — fold first, then negate.
static bool
unicode_fold_and_negate(const rs_Translator *t, bool negated, rs_uvec_t *cls)
{
    if (t->case_insensitive) {
        uvec_case_fold_simple(cls);
    }
    if (negated) {
        uvec_negate(cls);
    }
    return true;
}

static bool
bytes_fold_and_negate(const rs_Translator *t, bool negated, rs_bvec_t *cls)
{
    if (t->case_insensitive) {
        bvec_case_fold_simple(cls);
    }
    if (negated) {
        bvec_negate(cls);
    }
    return true;
}

static bool translate_class_set(const rs_Translator *t,
                                const rs_ClassSet   *set,
                                rs_uvec_t           *out_u,
                                rs_bvec_t           *out_b);

static bool
translate_class_set_item(const rs_Translator   *t,
                         const rs_ClassSetItem *it,
                         rs_uvec_t             *out_u,
                         rs_bvec_t             *out_b)
{
    bool unicode_mode = t->unicode;
    switch (it->tag) {
    case RS_CLASS_SET_ITEM_EMPTY:
        return true;
    case RS_CLASS_SET_ITEM_LITERAL:
        if (unicode_mode) {
            uvec_push_canonical(
                out_u, (rs_urange_t){it->literal.c, it->literal.c});
        }
        else {
            if (it->literal.c > 0xFFu) {
                return false;
            }
            bvec_push_canonical(
                out_b,
                (rs_brange_t){(uint8_t)it->literal.c, (uint8_t)it->literal.c});
        }
        return true;
    case RS_CLASS_SET_ITEM_RANGE:
        if (unicode_mode) {
            uvec_push_canonical(
                out_u, (rs_urange_t){it->range->start.c, it->range->end.c});
        }
        else {
            if (it->range->start.c > 0xFFu || it->range->end.c > 0xFFu) {
                return false;
            }
            bvec_push_canonical(
                out_b, (rs_brange_t){(uint8_t)it->range->start.c,
                                     (uint8_t)it->range->end.c});
        }
        return true;
    case RS_CLASS_SET_ITEM_ASCII:
        if (unicode_mode) {
            rs_uvec_t tmp;
            uvec_init(&tmp);
            ascii_class_unicode(it->ascii.kind, &tmp);
            if (it->ascii.negated) {
                uvec_negate(&tmp);
            }
            uvec_union(out_u, &tmp);
            uvec_free(&tmp);
        }
        else {
            rs_bvec_t tmp;
            bvec_init(&tmp);
            ascii_class_bytes(it->ascii.kind, &tmp);
            if (it->ascii.negated) {
                bvec_negate(&tmp);
            }
            bvec_union(out_b, &tmp);
            bvec_free(&tmp);
        }
        return true;
    case RS_CLASS_SET_ITEM_UNICODE: {
        if (!unicode_mode) {
            return false;
        }
        rs_uvec_t tmp;
        uvec_init(&tmp);
        bool ok;
        resolve_unicode_class(&it->unicode, &tmp, &ok);
        if (!ok) {
            uvec_free(&tmp);
            return false;
        }
        // hir_unicode_class applies fold+negate to the resolved class.
        unicode_fold_and_negate(t, it->unicode.negated, &tmp);
        uvec_union(out_u, &tmp);
        uvec_free(&tmp);
        return true;
    }
    case RS_CLASS_SET_ITEM_PERL:
        if (unicode_mode) {
            rs_uvec_t tmp;
            uvec_init(&tmp);
            resolve_perl_unicode(it->perl.kind, &tmp);
            // regex-syntax translate:1095 — Perl Unicode classes are already
            // closed under simple case folding; skip fold.
            if (it->perl.negated) {
                uvec_negate(&tmp);
            }
            uvec_union(out_u, &tmp);
            uvec_free(&tmp);
        }
        else {
            rs_bvec_t tmp;
            bvec_init(&tmp);
            resolve_perl_bytes(it->perl.kind, &tmp);
            if (it->perl.negated) {
                bvec_negate(&tmp);
            }
            bvec_union(out_b, &tmp);
            bvec_free(&tmp);
        }
        return true;
    case RS_CLASS_SET_ITEM_BRACKETED:
        if (unicode_mode) {
            rs_uvec_t inner;
            uvec_init(&inner);
            rs_bvec_t inner_b;
            bvec_init(&inner_b);
            if (!translate_class_set(t, &it->bracketed->kind, &inner, &inner_b)) {
                uvec_free(&inner);
                bvec_free(&inner_b);
                return false;
            }
            bvec_free(&inner_b);
            unicode_fold_and_negate(t, it->bracketed->negated, &inner);
            uvec_union(out_u, &inner);
            uvec_free(&inner);
        }
        else {
            rs_uvec_t inner_u;
            uvec_init(&inner_u);
            rs_bvec_t inner;
            bvec_init(&inner);
            if (!translate_class_set(t, &it->bracketed->kind, &inner_u, &inner)) {
                uvec_free(&inner_u);
                bvec_free(&inner);
                return false;
            }
            uvec_free(&inner_u);
            bytes_fold_and_negate(t, it->bracketed->negated, &inner);
            bvec_union(out_b, &inner);
            bvec_free(&inner);
        }
        return true;
    case RS_CLASS_SET_ITEM_UNION:
        for (size_t k = 0; k < it->union_.len; k++) {
            if (!translate_class_set_item(t, &it->union_.items[k], out_u,
                                          out_b)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

// translate_class_set: fold a set tree into a single class.  For BinaryOp,
// recursively translate lhs and rhs separately, fold case in each side, then
// apply the operator.
static bool
translate_class_set(const rs_Translator *t, const rs_ClassSet *set,
                    rs_uvec_t *out_u, rs_bvec_t *out_b)
{
    if (set->tag == RS_CLASS_SET_TAG_ITEM) {
        return translate_class_set_item(t, &set->item, out_u, out_b);
    }
    if (t->unicode) {
        rs_uvec_t lhs;
        uvec_init(&lhs);
        rs_uvec_t rhs;
        uvec_init(&rhs);
        rs_bvec_t lhs_b;
        bvec_init(&lhs_b);
        rs_bvec_t rhs_b;
        bvec_init(&rhs_b);
        if (!translate_class_set(t, set->lhs, &lhs, &lhs_b)) {
            uvec_free(&lhs);
            uvec_free(&rhs);
            bvec_free(&lhs_b);
            bvec_free(&rhs_b);
            return false;
        }
        if (!translate_class_set(t, set->rhs, &rhs, &rhs_b)) {
            uvec_free(&lhs);
            uvec_free(&rhs);
            bvec_free(&lhs_b);
            bvec_free(&rhs_b);
            return false;
        }
        bvec_free(&lhs_b);
        bvec_free(&rhs_b);
        if (t->case_insensitive) {
            uvec_case_fold_simple(&lhs);
            uvec_case_fold_simple(&rhs);
        }
        switch (set->binop_kind) {
        case RS_CLASS_SET_BINARY_OP_INTERSECTION:
            uvec_intersect(&lhs, &rhs);
            break;
        case RS_CLASS_SET_BINARY_OP_DIFFERENCE:
            uvec_difference(&lhs, &rhs);
            break;
        case RS_CLASS_SET_BINARY_OP_SYMMETRIC_DIFFERENCE:
            uvec_symmetric_difference(&lhs, &rhs);
            break;
        }
        uvec_union(out_u, &lhs);
        uvec_free(&lhs);
        uvec_free(&rhs);
        return true;
    }
    rs_uvec_t lhs_u;
    uvec_init(&lhs_u);
    rs_uvec_t rhs_u;
    uvec_init(&rhs_u);
    rs_bvec_t lhs;
    bvec_init(&lhs);
    rs_bvec_t rhs;
    bvec_init(&rhs);
    if (!translate_class_set(t, set->lhs, &lhs_u, &lhs)) {
        uvec_free(&lhs_u);
        uvec_free(&rhs_u);
        bvec_free(&lhs);
        bvec_free(&rhs);
        return false;
    }
    if (!translate_class_set(t, set->rhs, &rhs_u, &rhs)) {
        uvec_free(&lhs_u);
        uvec_free(&rhs_u);
        bvec_free(&lhs);
        bvec_free(&rhs);
        return false;
    }
    uvec_free(&lhs_u);
    uvec_free(&rhs_u);
    if (t->case_insensitive) {
        bvec_case_fold_simple(&lhs);
        bvec_case_fold_simple(&rhs);
    }
    switch (set->binop_kind) {
    case RS_CLASS_SET_BINARY_OP_INTERSECTION:
        bvec_intersect(&lhs, &rhs);
        break;
    case RS_CLASS_SET_BINARY_OP_DIFFERENCE:
        bvec_difference(&lhs, &rhs);
        break;
    case RS_CLASS_SET_BINARY_OP_SYMMETRIC_DIFFERENCE:
        bvec_symmetric_difference(&lhs, &rhs);
        break;
    }
    bvec_union(out_b, &lhs);
    bvec_free(&lhs);
    bvec_free(&rhs);
    return true;
}

// regex-syntax translate:385 — Literal: in unicode mode, encode the codepoint
// as UTF-8 bytes; in byte mode, emit the byte directly.  When
// case_insensitive, expand to a class.
static bool
translate_literal(const rs_Translator *t, const rs_Literal *lit, rs_Hir **out)
{
    uint32_t cp = lit->c;
    if (t->case_insensitive) {
        if (t->unicode) {
            if (case_fold_overlaps(cp, cp)) {
                rs_uvec_t v;
                uvec_init(&v);
                uvec_push(&v, (rs_urange_t){cp, cp});
                uvec_case_fold_simple(&v);
                *out = hir_new_class_unicode(v);
                return true;
            }
        }
        else {
            if (cp < 0x80u
                && ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'))) {
                rs_bvec_t v;
                bvec_init(&v);
                bvec_push(&v, (rs_brange_t){(uint8_t)cp, (uint8_t)cp});
                bvec_case_fold_simple(&v);
                *out = hir_new_class_bytes(v);
                return true;
            }
        }
    }
    // Default: encode UTF-8 bytes.
    uint8_t buf[4];
    size_t  n = encode_utf8(cp, buf);
    *out      = hir_new_literal(buf, n);
    return true;
}

bool
rs_Translator_translate(rs_Translator *t, const char *pattern,
                        const rs_Ast *ast, rs_Hir **out)
{
    n00b_require(t != nullptr, "rs_Translator_translate: t");
    n00b_require(out != nullptr, "rs_Translator_translate: out");
    (void)pattern;
    if (!ast) {
        *out = hir_new_empty();
        return true;
    }
    switch (ast->tag) {
    case LOCAL_RS_AST_LITERAL:
        return translate_literal(t, &ast->literal, out);
    case LOCAL_RS_AST_CLASS_UNICODE: {
        if (!t->unicode) {
            return false;
        }
        rs_uvec_t v;
        uvec_init(&v);
        bool ok;
        resolve_unicode_class(&ast->class_unicode, &v, &ok);
        if (!ok) {
            uvec_free(&v);
            return false;
        }
        unicode_fold_and_negate(t, ast->class_unicode.negated, &v);
        *out = hir_new_class_unicode(v);
        return true;
    }
    case LOCAL_RS_AST_CLASS_BRACKETED: {
        if (t->unicode) {
            rs_uvec_t v;
            uvec_init(&v);
            rs_bvec_t bv;
            bvec_init(&bv);
            if (!translate_class_set(t, &ast->class_bracketed.kind, &v, &bv)) {
                uvec_free(&v);
                bvec_free(&bv);
                return false;
            }
            bvec_free(&bv);
            unicode_fold_and_negate(t, ast->class_bracketed.negated, &v);
            *out = hir_new_class_unicode(v);
            return true;
        }
        rs_uvec_t uv;
        uvec_init(&uv);
        rs_bvec_t v;
        bvec_init(&v);
        if (!translate_class_set(t, &ast->class_bracketed.kind, &uv, &v)) {
            uvec_free(&uv);
            bvec_free(&v);
            return false;
        }
        uvec_free(&uv);
        bytes_fold_and_negate(t, ast->class_bracketed.negated, &v);
        *out = hir_new_class_bytes(v);
        return true;
    }
    }
    return false;
}

// ===========================================================================
// TranslatorBuilder
// ===========================================================================

rs_TranslatorBuilder
rs_TranslatorBuilder_new(void)
{
    // regex-syntax translate:26 defaults: utf8=true, unicode=true,
    // case_insensitive=false.
    return (rs_TranslatorBuilder){
        .case_insensitive = false,
        .unicode          = true,
        .utf8             = true,
    };
}

void
rs_TranslatorBuilder_unicode(rs_TranslatorBuilder *b, bool v)
{
    n00b_require(b != nullptr, "rs_TranslatorBuilder_unicode: b");
    b->unicode = v;
}

void
rs_TranslatorBuilder_utf8(rs_TranslatorBuilder *b, bool v)
{
    n00b_require(b != nullptr, "rs_TranslatorBuilder_utf8: b");
    b->utf8 = v;
}

void
rs_TranslatorBuilder_case_insensitive(rs_TranslatorBuilder *b, bool v)
{
    n00b_require(b != nullptr, "rs_TranslatorBuilder_case_insensitive: b");
    b->case_insensitive = v;
}

rs_Translator
rs_TranslatorBuilder_build(rs_TranslatorBuilder *b)
{
    n00b_require(b != nullptr, "rs_TranslatorBuilder_build: b");
    return (rs_Translator){
        .case_insensitive = b->case_insensitive,
        .unicode          = b->unicode,
        .utf8             = b->utf8,
    };
}

void
rs_Translator_free(rs_Translator *t)
{
    // No heap state in the translator itself.
    (void)t;
}

// ===========================================================================
// Utf8Sequences
// One call to rs_Utf8Sequences_next emits ONE Utf8Sequence (a tuple of 1..4
// byte ranges).  The internal state is a stack of ScalarRange { start, end }.
// On each next() the top range is popped and recursively split (by surrogate
// hole, by UTF-8 byte-length class boundary, then by shared-prefix /
// continuation-byte boundary) until the remaining range encodes uniformly,
// at which point its two endpoints are encoded and emitted as a sequence.
// ===========================================================================

// Internal ScalarRange (mirrors Rust struct of same name).
typedef struct {
    uint32_t start;
    uint32_t end;
} local_rs_ScalarRange;

// Stack depth bound.  The Rust algorithm's stack peaks at a handful of
// entries: each split adds at most one new entry while reducing the active
// range, and the total splits performed across the lifetime of a single
// next() call are bounded by the number of byte-length classes (4) and the
// number of continuation-byte positions (3).  16 leaves a comfortable margin
// over any path observed for any input range in [0, 0x10FFFF].
#define UTF8_SEQ_STACK_CAP 16

struct rs_Utf8Sequences {
    local_rs_ScalarRange stack[UTF8_SEQ_STACK_CAP];
    size_t               depth;
};

// rs_Utf8Range lives in ast.h with a struct tag so every TU sees the
// same type; earlier revisions had a local_rs_Utf8Range typedef here that
// was technically a distinct type from lib.c's rs_Utf8Range, which
// clang LTO + TBAA exploited to drop byte-range stores in negated
// character classes (truncating 0x80-0xBF). Keep using the canonical
// type from the header.

// max_scalar_value(nbytes): maximum codepoint encodable in `nbytes` UTF-8
// bytes.
static uint32_t
local_rs_max_scalar_value(int nbytes)
{
    switch (nbytes) {
    case 1:
        return 0x0000007Fu;
    case 2:
        return 0x000007FFu;
    case 3:
        return 0x0000FFFFu;
    case 4:
        return 0x0010FFFFu;
    default:
        return 0u;
    }
}

static void
local_rs_push(rs_Utf8Sequences *it, uint32_t start, uint32_t end)
{
    n00b_require(it != nullptr, "Utf8Sequences push");
    if (it->depth >= UTF8_SEQ_STACK_CAP) {
        // Should be unreachable for any valid scalar range; panic loudly so
        // any divergence from the Rust algorithm surfaces immediately.
        n00b_panic("rs_Utf8Sequences: internal stack overflow");
    }
    it->stack[it->depth].start = start;
    it->stack[it->depth].end   = end;
    it->depth++;
}

rs_Utf8Sequences *
rs_Utf8Sequences_new(uint32_t start, uint32_t end)
{
    rs_Utf8Sequences *it = n00b_alloc(rs_Utf8Sequences);
    it->depth            = 0;
    local_rs_push(it, start, end);
    return it;
}

void
rs_Utf8Sequences_free(rs_Utf8Sequences *it)
{
    n00b_free(it);
}

bool
rs_Utf8Sequences_next(rs_Utf8Sequences *it, rs_Utf8Range *out, size_t *n_out)
{
    n00b_require(it != nullptr, "rs_Utf8Sequences_next: it");
    // Mirror of Utf8Sequences::next() in utf8upstream.  Two nested loops:
    //   TOP   — pops a fresh range from the stack each iteration.
    //   INNER — keeps reshaping the active range r in place, pushing the
    //           tail half onto the stack each time we split.
    while (it->depth > 0) {
        local_rs_ScalarRange r = it->stack[--it->depth];

        for (;;) {
            // Step 1: surrogate-hole split.  ScalarRange::split: if the
            // range straddles [0xD800, 0xDFFF], yield (low=[start, 0xD7FF],
            // high=[0xE000, end]).
            if (r.start < 0xE000u && r.end > 0xD7FFu) {
                local_rs_push(it, 0xE000u, r.end);
                r.end = 0xD7FFu;
                continue;  // INNER
            }
            // Step 2: range invalid (start > end after a previous split's
            // tail-end-1 went underwater) — fall back to popping fresh.
            if (r.start > r.end) {
                break;  // continue TOP (outer while loop pops next)
            }
            // Step 3: byte-length class split.  If the range straddles the
            // boundary between two UTF-8 byte-length classes, split there.
            {
                bool restart_inner = false;
                for (int i = 1; i < 4; i++) {
                    uint32_t mx = local_rs_max_scalar_value(i);
                    if (r.start <= mx && mx < r.end) {
                        local_rs_push(it, mx + 1u, r.end);
                        r.end         = mx;
                        restart_inner = true;
                        break;
                    }
                }
                if (restart_inner) {
                    continue;  // INNER
                }
            }
            // Step 4: ASCII shortcut.  If the entire range fits in one byte,
            // emit One(...).
            if (r.end <= 0x7Fu) {
                if (out) {
                    out[0].start = (uint8_t)r.start;
                    out[0].end   = (uint8_t)r.end;
                }
                if (n_out) {
                    *n_out = 1;
                }
                return true;
            }
            // Step 5: continuation-byte boundary splits.
            {
                bool restart_inner = false;
                for (int i = 1; i < 4; i++) {
                    uint32_t m = (1u << (6 * i)) - 1u;
                    if ((r.start & ~m) != (r.end & ~m)) {
                        if ((r.start & m) != 0u) {
                            local_rs_push(it, (r.start | m) + 1u, r.end);
                            r.end         = r.start | m;
                            restart_inner = true;
                            break;
                        }
                        if ((r.end & m) != m) {
                            local_rs_push(it, r.end & ~m, r.end);
                            r.end         = (r.end & ~m) - 1u;
                            restart_inner = true;
                            break;
                        }
                    }
                }
                if (restart_inner) {
                    continue;  // INNER
                }
            }
            // Step 6: emit.  start and end now encode to the same byte
            // length and have a uniform per-position byte-range structure.
            {
                uint8_t sbuf[4], ebuf[4];
                size_t  ns = encode_utf8(r.start, sbuf);
                size_t  ne = encode_utf8(r.end, ebuf);
                (void)ne;  // assert(ns == ne) holds by construction.
                if (out) {
                    for (size_t k = 0; k < ns; k++) {
                        out[k].start = sbuf[k];
                        out[k].end   = ebuf[k];
                    }
                }
                if (n_out) {
                    *n_out = ns;
                }
                return true;
            }
        }
    }
    if (n_out) {
        *n_out = 0;
    }
    return false;
}
