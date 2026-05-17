// solver.c — regex transition-set solver and pretty-printer.
//
// Per § 0a-Z: typed translation of the algorithm from upstream Rust
// `FxHashMap<TSet, TSetId>` + `Vec<TSet>`, with resharp-c's `xalloc` /
// `HASHMAP` / `StrBuf` macro shims replaced by their n00b primitives:
//
//   FxHashMap<TSet, TSetId>  -> n00b_dict_t(TSet, TSetId)
//                               (hash = nullptr + skip_obj_hash = true ->
//                                n00b_hash_raw on the 32-byte key bits)
//   Vec<TSet>                -> raw n00b_alloc_array(TSet, N) + grow-via-
//                               alloc-new + memcpy (D13) (single-owner,
//                               unlocked, indexed by id; n00b_list_t would
//                               add unwanted locking)
//   StrBuf (byte builder)    -> n00b_buffer_t (D12) with .no_lock = true;
//                               formatted hex bytes routed through
//                               n00b_cformat (rich markup) since printf
//                               style does not exist in n00b.
//   resharp-c xalloc shims   -> n00b_alloc_array(char, len + 1) for
//                               cstrings; n00b_alloc_array(T, N) for
//                               typed arrays; n00b_free; geometric grow
//                               via alloc-new + memcpy (D13) + free-old.
//
// The pretty-printer's outputs are NUL-terminated `char *` heap
// allocations from the n00b runtime; the caller does not free them
// (the GC reclaims unreachable allocations).

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/dict.h"
#include "text/strings/format.h"
#include "internal/regex/solver.h"

// ============================================================
// TSet
// ============================================================

constexpr TSet EMPTY = { .words = { 0, 0, 0, 0 } };
constexpr TSet FULL  = {
    .words = { UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX },
};

[[nodiscard]] TSet
TSet_splat(uint64_t v)
{
    return (TSet){ .words = { v, v, v, v } };
}

[[nodiscard]] TSet
tset_from_bytes(const uint8_t *bytes, size_t len)
{
    TSet out = TSet_splat(0);
    for (size_t k = 0; k < len; ++k) {
        uint8_t b = bytes[k];
        out.words[(size_t)b / 64] |= (uint64_t)1 << ((size_t)b % 64);
    }
    return out;
}

[[nodiscard]] bool
TSet_contains_byte(const TSet *self, uint8_t b)
{
    return (self->words[(size_t)b / 64] & ((uint64_t)1 << ((size_t)b % 64))) != 0;
}

[[nodiscard]] TSet
TSet_bitand(TSet a, TSet b)
{
    return (TSet){ .words = {
        a.words[0] & b.words[0],
        a.words[1] & b.words[1],
        a.words[2] & b.words[2],
        a.words[3] & b.words[3],
    } };
}

[[nodiscard]] TSet
TSet_bitor(TSet a, TSet b)
{
    return (TSet){ .words = {
        a.words[0] | b.words[0],
        a.words[1] | b.words[1],
        a.words[2] | b.words[2],
        a.words[3] | b.words[3],
    } };
}

[[nodiscard]] TSet
TSet_not(TSet a)
{
    return (TSet){ .words = { ~a.words[0], ~a.words[1], ~a.words[2], ~a.words[3] } };
}

[[nodiscard]] bool
TSet_eq(TSet a, TSet b)
{
    return a.words[0] == b.words[0]
        && a.words[1] == b.words[1]
        && a.words[2] == b.words[2]
        && a.words[3] == b.words[3];
}

[[nodiscard]] static bool
tsetid_eq(TSetId a, TSetId b)
{
    return a.v == b.v;
}

// ============================================================
// ByteRangeSet — sorted, unique sequence of (u8, u8) ranges
// ============================================================

void
ByteRangeSet_init(ByteRangeSet *s)
{
    *s = (ByteRangeSet){};
}

void
ByteRangeSet_free(ByteRangeSet *s)
{
    if (s->data) {
        n00b_free(s->data);
    }
    *s = (ByteRangeSet){};
}

[[nodiscard]] static int
br_cmp(ByteRange a, ByteRange b)
{
    if (a.start != b.start) {
        return (a.start < b.start) ? -1 : 1;
    }
    if (a.end != b.end) {
        return (a.end < b.end) ? -1 : 1;
    }
    return 0;
}

void
ByteRangeSet_insert(ByteRangeSet *s, ByteRange r)
{
    // Binary search for insertion point.
    size_t lo = 0, hi = s->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int    c   = br_cmp(s->data[mid], r);
        if (c == 0) {
            return; // already present
        }
        if (c < 0) {
            lo = mid + 1;
        }
        else {
            hi = mid;
        }
    }
    if (s->len == s->cap) {
        size_t     new_cap = s->cap == 0 ? 8 : s->cap * 2;
        ByteRange *new_buf = n00b_alloc_array_with_opts(ByteRange, new_cap, &(n00b_alloc_opts_t){.scan_kind = N00B_GC_SCAN_KIND_NONE});
        if (s->len > 0) {
            memcpy(new_buf, s->data, s->len * sizeof(ByteRange));
        }
        if (s->data) {
            n00b_free(s->data);
        }
        s->data = new_buf;
        s->cap  = new_cap;
    }
    if (lo < s->len) {
        memmove(&s->data[lo + 1], &s->data[lo], (s->len - lo) * sizeof(ByteRange));
    }
    s->data[lo] = r;
    s->len += 1;
}

[[nodiscard]] bool
ByteRangeSet_is_empty(const ByteRangeSet *s)
{
    return s->len == 0;
}

[[nodiscard]] size_t
ByteRangeSet_len(const ByteRangeSet *s)
{
    return s->len;
}

[[nodiscard]] const ByteRange *
ByteRangeSet_first(const ByteRangeSet *s)
{
    return s->len == 0 ? nullptr : &s->data[0];
}

[[nodiscard]] const ByteRange *
ByteRangeSet_last(const ByteRangeSet *s)
{
    return s->len == 0 ? nullptr : &s->data[s->len - 1];
}

// ============================================================
// Solver
// ============================================================

// `TSetCache` is declared opaque in the header.  The full definition is
// produced here so the typed-dict macros can size keys/values at compile
// time.  Hashing is delegated to n00b_hash_raw on the 32-byte key bits via
// skip_obj_hash = true (the default hash path dereferences the key as a
// pointer-sized value; TSet keys are 32 bytes, so skip_obj_hash is required
// for correctness).  The dict is a named `inner` field so call sites can
// take its address explicitly.
struct TSetCache {
    n00b_dict_t(TSet, TSetId) inner;
};

[[nodiscard]] static TSetId
solver_init_entry(Solver *self, TSet inst)
{
    TSetId new_id = (TSetId){ .v = (uint32_t)self->array_len };
    n00b_dict_put(&self->cache->inner, inst, new_id);

    if (self->array_len == self->array_cap) {
        size_t nc       = self->array_cap == 0 ? 16 : self->array_cap * 2;
        TSet  *new_buf  = n00b_alloc_array_with_opts(TSet, nc,
            &(n00b_alloc_opts_t){
                .allocator = self->allocator,
                .scan_kind = N00B_GC_SCAN_KIND_NONE,
            });
        if (self->array_len > 0) {
            memcpy(new_buf, self->array, self->array_len * sizeof(TSet));
        }
        if (self->array) {
            n00b_free(self->array);
        }
        self->array     = new_buf;
        self->array_cap = nc;
    }
    self->array[self->array_len++] = inst;
    return new_id;
}

[[nodiscard]] Solver *
solver_new(n00b_allocator_t *allocator)
{
    Solver *s = n00b_alloc_with_opts(
        Solver, &(n00b_alloc_opts_t){.allocator = allocator});
    s->allocator = allocator;

    s->cache = n00b_alloc_with_opts(
        TSetCache, &(n00b_alloc_opts_t){.allocator = allocator});
    // The TSet key is 32 bytes — bigger than a pointer — so hashing the
    // raw key bits is the only correct choice.  skip_obj_hash = true with
    // hash = nullptr routes compute_hash() through n00b_hash_raw(key, ksz).
    n00b_dict_init(&s->cache->inner, .skip_obj_hash = true,
                   .allocator = allocator,
                   .scan_kind = N00B_GC_SCAN_KIND_NONE);

    (void)solver_init_entry(s, solver_empty()); // 0
    (void)solver_init_entry(s, solver_full());  // 1
    return s;
}

[[nodiscard]] Solver *
solver_default(void)
{
    return solver_new(nullptr);
}

void
solver_free(Solver *self)
{
    if (!self) {
        return;
    }
    // The dict's internal storage is GC-managed; releasing the wrapper is
    // sufficient.  Match the Rust drop ordering: cache, then array, then self.
    if (self->cache) {
        n00b_free(self->cache);
    }
    if (self->array) {
        n00b_free(self->array);
    }
    n00b_free(self);
}

[[nodiscard]] TSet
solver_get_set(const Solver *self, TSetId set_id)
{
    extern int  printf(const char *, ...);
    extern void abort(void);
    if ((size_t)set_id.v >= self->array_len) {
        printf("solver_get_set OOB: self=%p set_id.v=%u array_len=%zu "
               "array_cap=%zu array=%p\n",
               (const void *)self, set_id.v,
               self->array_len, self->array_cap,
               (const void *)self->array);
        abort();
    }
    return self->array[(size_t)set_id.v];
}

[[nodiscard]] const TSet *
solver_get_set_ref(const Solver *self, TSetId set_id)
{
    return &self->array[(size_t)set_id.v];
}

[[nodiscard]] TSetId
solver_get_id(Solver *self, TSet inst)
{
    bool   found;
    TSetId existing = n00b_dict_get(&self->cache->inner, inst, &found);
    if (found) {
        return existing;
    }
    return solver_init_entry(self, inst);
}

[[nodiscard]] bool
solver_has_bit_set(Solver *self, TSetId set_id, size_t idx, uint64_t bit)
{
    return (self->array[(size_t)set_id.v].words[idx] & bit) != 0;
}

[[nodiscard]] ByteRangeSet
solver_pp_collect_ranges(const TSet *tset)
{
    ByteRangeSet ranges;
    ByteRangeSet_init(&ranges);

    bool    have_rangestart = false;
    uint8_t rangestart      = 0;
    bool    have_prevchar   = false;
    uint8_t prevchar        = 0;

    for (size_t i = 0; i < 4; ++i) {
        for (size_t j = 0; j < 64; ++j) {
            uint64_t nthbit = (uint64_t)1 << j;
            if ((tset->words[i] & nthbit) != 0) {
                uint8_t cc = (uint8_t)(i * 64 + j);
                if (!have_rangestart) {
                    rangestart      = cc;
                    have_rangestart = true;
                    prevchar        = cc;
                    have_prevchar   = true;
                    continue;
                }
                if (have_rangestart && have_prevchar) {
                    if ((uint8_t)(prevchar + 1) == cc) {
                        prevchar = cc;
                        continue;
                    }
                    ByteRangeSet_insert(&ranges,
                        (ByteRange){ .start = rangestart, .end = prevchar });
                    rangestart = cc;
                    prevchar   = cc;
                }
            }
        }
    }
    if (have_rangestart && have_prevchar) {
        ByteRangeSet_insert(&ranges,
            (ByteRange){ .start = rangestart, .end = prevchar });
    }
    return ranges;
}

// ----- pretty-print helpers -----

// Allocate a NUL-terminated heap copy of @p src (managed by n00b runtime).
[[nodiscard]] static char *
cstr_dup(const char *src)
{
    size_t n   = strlen(src) + 1;
    char  *out = n00b_alloc_array_with_opts(char, n, &(n00b_alloc_opts_t){.scan_kind = N00B_GC_SCAN_KIND_NONE});
    memcpy(out, src, n);
    return out;
}

// Drain a buffer into a NUL-terminated heap-allocated `char *`.  The buffer's
// internal storage is not NUL-terminated, so we copy out and add the byte.
[[nodiscard]] static char *
buffer_into_cstr(n00b_buffer_t *buf)
{
    int64_t len  = 0;
    char   *src  = n00b_buffer_to_c(buf, &len);
    char   *out  = n00b_alloc_array_with_opts(char, (size_t)len + 1, &(n00b_alloc_opts_t){.scan_kind = N00B_GC_SCAN_KIND_NONE});
    if (len > 0) {
        memcpy(out, src, (size_t)len);
    }
    out[len] = '\0';
    return out;
}

// Append @p src (a NUL-terminated cstr) onto @p buf.
static void
buffer_push_cstr(n00b_buffer_t *buf, const char *src)
{
    size_t n = strlen(src);
    if (n == 0) {
        return;
    }
    n00b_buffer_concat(buf, n00b_buffer_from_bytes((char *)src, (int64_t)n));
}

// Append a single byte @p c onto @p buf.
static void
buffer_push_char(n00b_buffer_t *buf, char c)
{
    n00b_buffer_concat(buf, n00b_buffer_from_bytes(&c, 1));
}

// NOTE: Rust pp_byte uses `cfg!(feature = "graphviz")` — disabled here; can
// be wired back through a build flag in phase-2.
[[nodiscard]] static char *
solver_pp_byte(uint8_t b)
{
    char c = (char)b;
    switch (c) {
    case '\n':
        return cstr_dup("\\n");
    case '\r':
        return cstr_dup("\\r");
    case '\t':
        return cstr_dup("\\t");
    case ' ':
        return cstr_dup(" ");
    case '_': case '.': case '+': case '-': case '\\': case '&':
    case '|': case '~': case '{': case '}': case '[': case ']':
    case '(': case ')': case '*': case '?': case '^': case '$': {
        char buf[3] = { '\\', c, '\0' };
        return cstr_dup(buf);
    }
    default:
        // Rust: c.is_ascii_punctuation() || c.is_ascii_alphanumeric().
        // The union of those two Rust predicates is exactly the printable
        // ASCII range (0x21..=0x7E) excluding space — every codepoint in
        // that range is either alphanumeric or punctuation.  resharp-c
        // gates the same range with ispunct()|isalnum() (libc), which is
        // redundant once you have the range bound; we drop the libc call
        // since the dispatch instructions forbid <ctype.h>.
        if (b >= 0x21 && b <= 0x7E) {
            char buf[2] = { c, '\0' };
            return cstr_dup(buf);
        }
        else {
            // resharp-c: `xasprintf_cstr("\\x%02X", b)`.
            // n00b's formatter does not use printf-style `%`; the equivalent
            // rich-markup spec is `[|#:02X|]` — substitution, format-spec
            // `02X` (zero-padded width-2 uppercase hex).  n00b's rich-desc
            // parser treats `\` as a single-char escape (`\x` would emit
            // just `x`), so we double-escape the leading backslash to get
            // a literal `\x` prefix in the output.
            // See text/strings/format_spec.h / rich_desc.c.
            n00b_string_t *s = n00b_cformat("\\\\x[|#:02X|]", (int64_t)b);
            return cstr_dup(s->data);
        }
    }
}

// Format one (s, e) range pair for pp_content.
[[nodiscard]] static char *
display_range(uint8_t c, uint8_t c2)
{
    char *a = solver_pp_byte(c);
    if (c == c2) {
        return a;
    }
    char *b = solver_pp_byte(c2);
    // abs_diff for the "consecutive" check (skip the `-` separator on adjacent bytes).
    uint8_t diff = (c > c2) ? (uint8_t)(c - c2) : (uint8_t)(c2 - c);

    n00b_buffer_t *sb = n00b_alloc(n00b_buffer_t);
    n00b_buffer_init(sb, .length = 0, .no_lock = true);

    buffer_push_cstr(sb, a);
    if (diff == 1) {
        buffer_push_cstr(sb, b);
    }
    else {
        buffer_push_char(sb, '-');
        buffer_push_cstr(sb, b);
    }
    return buffer_into_cstr(sb);
}

// "\u{22a5}" UP TACK ⊥
static const char UTF8_UP_TACK[] = "\xE2\x8A\xA5";
// "\u{03c6}" GREEK SMALL LETTER PHI φ
static const char UTF8_PHI[]     = "\xCF\x86";

[[nodiscard]] static char *
solver_pp_content(const ByteRangeSet *ranges)
{
    if (ByteRangeSet_is_empty(ranges)) {
        return cstr_dup(UTF8_UP_TACK);
    }
    if (ByteRangeSet_len(ranges) == 1) {
        const ByteRange *r = ByteRangeSet_first(ranges);
        if (r->start == r->end) {
            return solver_pp_byte(r->start);
        }
        // single range, joined — falls through to the multi-range branch below.
    }
    if (ByteRangeSet_len(ranges) > 20) {
        return cstr_dup(UTF8_PHI);
    }

    n00b_buffer_t *sb = n00b_alloc(n00b_buffer_t);
    n00b_buffer_init(sb, .length = 0, .no_lock = true);

    for (size_t i = 0; i < ranges->len; ++i) {
        char *piece = display_range(ranges->data[i].start, ranges->data[i].end);
        buffer_push_cstr(sb, piece);
    }
    return buffer_into_cstr(sb);
}

[[nodiscard]] uint32_t
solver_pp_first(const Solver *self, const TSet *tset)
{
    (void)self;
    // tryn1: scan bits 0..32; tryn2: bits 33..64. Return first set in
    // priority order; fall back to U+22A5 (UP TACK).
    static const struct {
        int    kind;
        size_t i;
    } order[] = {
        { 2, 0 }, { 2, 1 }, { 1, 1 }, { 1, 2 },
        { 2, 2 }, { 1, 3 }, { 2, 3 }, { 1, 0 },
    };
    for (size_t k = 0; k < sizeof(order) / sizeof(order[0]); ++k) {
        size_t i      = order[k].i;
        size_t jstart = (order[k].kind == 1) ? 0u : 33u;
        size_t jend   = (order[k].kind == 1) ? 32u : 64u;
        for (size_t j = jstart; j < jend; ++j) {
            uint64_t nthbit = (uint64_t)1 << j;
            if ((tset->words[i] & nthbit) != 0) {
                return (uint32_t)(uint8_t)(i * 64 + j);
            }
        }
    }
    return 0x22A5u; // ⊥
}

void
solver_byte_ranges(const Solver *self, TSetId tset,
                   ByteRange **out, size_t *out_len)
{
    TSet         t  = solver_get_set(self, tset);
    ByteRangeSet rs = solver_pp_collect_ranges(&t);
    *out_len = rs.len;
    if (rs.len == 0) {
        *out = nullptr;
    }
    else {
        ByteRange *buf = n00b_alloc_array_with_opts(ByteRange, rs.len, &(n00b_alloc_opts_t){.scan_kind = N00B_GC_SCAN_KIND_NONE});
        memcpy(buf, rs.data, rs.len * sizeof(ByteRange));
        *out = buf;
    }
    ByteRangeSet_free(&rs);
}

[[nodiscard]] char *
solver_pp(const Solver *self, TSetId tset_id)
{
    if (tsetid_eq(tset_id, TSET_ID_FULL)) {
        return cstr_dup("_");
    }
    if (tsetid_eq(tset_id, TSET_ID_EMPTY)) {
        return cstr_dup(UTF8_UP_TACK);
    }

    TSet         tset   = solver_get_set(self, tset_id);
    ByteRangeSet ranges = solver_pp_collect_ranges(&tset);

    // NOTE: Rust's `pp` calls `ranges.first().unwrap()` BEFORE checking
    // `ranges.is_empty()`, which means an empty `ranges` would panic and the
    // later `if ranges.is_empty()` branch is dead code. We deviate by checking
    // empty first and returning UP TACK; reachable inputs are unaffected, but
    // this swallows the panic Rust would raise on a degenerate empty TSet.
    if (ByteRangeSet_is_empty(&ranges)) {
        ByteRangeSet_free(&ranges);
        return cstr_dup(UTF8_UP_TACK);
    }

    uint8_t rstart = ByteRangeSet_first(&ranges)->start;
    uint8_t rend   = ByteRangeSet_last(&ranges)->end;

    if (ranges.len >= 2 && rstart == 0 && rend == 255) {
        TSet         not_set    = TSet_not(tset);
        ByteRangeSet not_ranges = solver_pp_collect_ranges(&not_set);
        if (not_ranges.len == 1
            && not_ranges.data[0].start == 10
            && not_ranges.data[0].end == 10) {
            ByteRangeSet_free(&not_ranges);
            ByteRangeSet_free(&ranges);
            return cstr_dup(".");
        }
        char          *content = solver_pp_content(&not_ranges);
        n00b_buffer_t *sb      = n00b_alloc(n00b_buffer_t);
        n00b_buffer_init(sb, .length = 0, .no_lock = true);
        buffer_push_cstr(sb, "[^");
        buffer_push_cstr(sb, content);
        buffer_push_char(sb, ']');
        ByteRangeSet_free(&not_ranges);
        ByteRangeSet_free(&ranges);
        return buffer_into_cstr(sb);
    }

    if (ranges.len == 1) {
        const ByteRange *r = ByteRangeSet_first(&ranges);
        if (r->start == r->end) {
            char *out = solver_pp_byte(r->start);
            ByteRangeSet_free(&ranges);
            return out;
        }
        char          *content = solver_pp_content(&ranges);
        n00b_buffer_t *sb      = n00b_alloc(n00b_buffer_t);
        n00b_buffer_init(sb, .length = 0, .no_lock = true);
        buffer_push_char(sb, '[');
        buffer_push_cstr(sb, content);
        buffer_push_char(sb, ']');
        ByteRangeSet_free(&ranges);
        return buffer_into_cstr(sb);
    }

    char          *content = solver_pp_content(&ranges);
    n00b_buffer_t *sb      = n00b_alloc(n00b_buffer_t);
    n00b_buffer_init(sb, .length = 0, .no_lock = true);
    buffer_push_char(sb, '[');
    buffer_push_cstr(sb, content);
    buffer_push_char(sb, ']');
    ByteRangeSet_free(&ranges);
    return buffer_into_cstr(sb);
}

// ============================================================
// impl Solver — second block from Rust
// ============================================================

[[nodiscard]] TSet
solver_full(void)
{
    return FULL;
}

[[nodiscard]] TSet
solver_empty(void)
{
    return EMPTY;
}

[[nodiscard]] TSetId
solver_or_id(Solver *self, TSetId set1, TSetId set2)
{
    return solver_get_id(self, TSet_bitor(solver_get_set(self, set1),
                                          solver_get_set(self, set2)));
}

[[nodiscard]] TSetId
solver_and_id(Solver *self, TSetId set1, TSetId set2)
{
    return solver_get_id(self, TSet_bitand(solver_get_set(self, set1),
                                           solver_get_set(self, set2)));
}

[[nodiscard]] TSetId
solver_not_id(Solver *self, TSetId set_id)
{
    return solver_get_id(self, TSet_not(solver_get_set(self, set_id)));
}

[[nodiscard]] bool
solver_is_sat_id(Solver *self, TSetId set1, TSetId set2)
{
    return !tsetid_eq(solver_and_id(self, set1, set2), TSET_ID_EMPTY);
}

[[nodiscard]] bool
solver_unsat_id(Solver *self, TSetId set1, TSetId set2)
{
    return tsetid_eq(solver_and_id(self, set1, set2), TSET_ID_EMPTY);
}

[[nodiscard]] static uint32_t
popcount_u64(uint64_t x)
{
    // Match Rust's `count_ones`.  Phase-2 may pull in a portable fallback
    // from the project's bit-utils header.
    return (uint32_t)__builtin_popcountll(x);
}

[[nodiscard]] static uint32_t
ctz_u64(uint64_t x)
{
    return (uint32_t)__builtin_ctzll(x);
}

[[nodiscard]] uint32_t
solver_byte_count(const Solver *self, TSetId set_id)
{
    TSet     tset = solver_get_set(self, set_id);
    uint32_t s    = 0;
    for (size_t i = 0; i < 4; ++i) {
        s += popcount_u64(tset.words[i]);
    }
    return s;
}

void
solver_collect_bytes(const Solver *self, TSetId set_id,
                     uint8_t **out, size_t *out_len)
{
    TSet     tset  = solver_get_set(self, set_id);
    size_t   cap   = 0;
    size_t   len   = 0;
    uint8_t *bytes = nullptr;
    for (size_t i = 0; i < 4; ++i) {
        uint64_t bits = tset.words[i];
        while (bits != 0) {
            uint32_t j = ctz_u64(bits);
            if (len == cap) {
                size_t   new_cap = cap == 0 ? 16 : cap * 2;
                uint8_t *new_buf = n00b_alloc_array_with_opts(uint8_t, new_cap,
                    &(n00b_alloc_opts_t){
                        .allocator = self->allocator,
                        .scan_kind = N00B_GC_SCAN_KIND_NONE,
                    });
                if (len > 0) {
                    memcpy(new_buf, bytes, len);
                }
                if (bytes) {
                    n00b_free(bytes);
                }
                bytes = new_buf;
                cap   = new_cap;
            }
            bytes[len++] = (uint8_t)(i * 64 + j);
            bits &= bits - 1;
        }
    }
    *out     = bytes;
    *out_len = len;
}

[[nodiscard]] bool
solver_single_byte(const Solver *self, TSetId set_id, uint8_t *out_byte)
{
    TSet     tset  = solver_get_set(self, set_id);
    uint32_t total = 0;
    for (size_t i = 0; i < 4; ++i) {
        total += popcount_u64(tset.words[i]);
    }
    if (total != 1) {
        return false;
    }
    for (size_t i = 0; i < 4; ++i) {
        if (tset.words[i] != 0) {
            *out_byte = (uint8_t)(i * 64 + (size_t)ctz_u64(tset.words[i]));
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool
solver_is_empty_id(const Solver *self, TSetId set1)
{
    (void)self;
    return tsetid_eq(set1, TSET_ID_EMPTY);
}

[[nodiscard]] bool
solver_is_full_id(const Solver *self, TSetId set1)
{
    (void)self;
    return tsetid_eq(set1, TSET_ID_FULL);
}

[[nodiscard]] bool
solver_contains_id(Solver *self, TSetId large_id, TSetId small_id)
{
    TSetId not_large = solver_not_id(self, large_id);
    return tsetid_eq(solver_and_id(self, small_id, not_large), TSET_ID_EMPTY);
}

[[nodiscard]] TSetId
solver_u8_to_set_id(Solver *self, uint8_t byte)
{
    TSet     result = TSet_splat(0);
    uint64_t nthbit = (uint64_t)1 << (byte % 64);
    // Rust match arms 0..=63 / 64..=127 / 128..=191 / 192..=255 are exhaustive
    // over u8 — equivalent to byte / 64.
    result.words[(size_t)byte / 64] = nthbit;
    return solver_get_id(self, result);
}

[[nodiscard]] TSetId
solver_range_to_set_id(Solver *self, uint8_t start, uint8_t end)
{
    TSet result = TSet_splat(0);
    // Inclusive range; guard against start > end (Rust `start..=end` is empty
    // in that case).
    if (start <= end) {
        for (unsigned int byte = start; byte <= end; ++byte) {
            uint64_t nthbit = (uint64_t)1 << ((uint8_t)byte % 64);
            result.words[(size_t)byte / 64] |= nthbit;
        }
    }
    return solver_get_id(self, result);
}

[[nodiscard]] TSet
solver_and(const TSet *set1, const TSet *set2)
{
    return TSet_bitand(*set1, *set2);
}

[[nodiscard]] bool
solver_is_sat(const TSet *set1, const TSet *set2)
{
    TSet r = TSet_bitand(*set1, *set2);
    return !TSet_eq(r, solver_empty());
}

[[nodiscard]] TSet
solver_or(const TSet *set1, const TSet *set2)
{
    return TSet_bitor(*set1, *set2);
}

[[nodiscard]] TSet
solver_not(const TSet *set)
{
    return TSet_not(*set);
}

[[nodiscard]] bool
solver_is_full(const TSet *set)
{
    return TSet_eq(*set, solver_full());
}

[[nodiscard]] bool
solver_is_empty(const TSet *set)
{
    return TSet_eq(*set, solver_empty());
}

[[nodiscard]] bool
solver_contains(const TSet *large, const TSet *small)
{
    TSet nl    = TSet_not(*large);
    TSet inter = TSet_bitand(*small, nl);
    return TSet_eq(solver_empty(), inter);
}

[[nodiscard]] TSet
solver_u8_to_set(uint8_t byte)
{
    TSet     result = TSet_splat(0);
    uint64_t nthbit = (uint64_t)1 << (byte % 64);
    result.words[(size_t)byte / 64] = nthbit;
    return result;
}

[[nodiscard]] TSet
solver_range_to_set(uint8_t start, uint8_t end)
{
    TSet result = TSet_splat(0);
    if (start <= end) {
        for (unsigned int byte = start; byte <= end; ++byte) {
            uint64_t nthbit = (uint64_t)1 << ((uint8_t)byte % 64);
            result.words[(size_t)byte / 64] |= nthbit;
        }
    }
    return result;
}

// extern const sentinels for cross-TU linking — engine/lib.h and prefix.h
// declare `extern const TSetId TSET_ID_EMPTY/FULL`; constexpr at file scope
// in solver.h is internal-linkage under C23, so we provide global symbols here.
extern const TSetId TSET_ID_EMPTY;
extern const TSetId TSET_ID_FULL;
const TSetId TSET_ID_EMPTY = { .v = 0 };
const TSetId TSET_ID_FULL  = { .v = 1 };
