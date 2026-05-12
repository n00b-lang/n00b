#include "text/unicode/properties.h"
#include "text/unicode/encoding.h"
#include "text/unicode/ctx.h"
#include "internal/text/unicode/raw.h"
#include "core/alloc.h"
#include "core/atomic.h"
#include "core/runtime.h"
#include "core/rt_access.h"
#include "core/thread.h"  // n00b_thread_id, transitively required by core/lock_common.h
#include "core/mutex.h"
#include "internal/text/unicode/tables.h"

// Get the per-runtime unicode subsystem context.  Eager-allocated in
// n00b_init() and registered as a GC root — accessors here can deref
// unconditionally.
static inline n00b_unicode_ctx_t *
_uctx(void)
{
    return n00b_get_runtime()->unicode_ctx;
}

#include <ctype.h>
#include <string.h>

// External tables from generated files
extern const uint16_t n00b_unicode_gc_stage1[];
extern const uint8_t  n00b_unicode_gc_stage2[];

extern const uint16_t n00b_unicode_ccc_stage1[];
extern const uint8_t  n00b_unicode_ccc_stage2[];

extern const uint16_t n00b_unicode_script_stage1[];
extern const uint8_t  n00b_unicode_script_stage2[];
extern const char    *n00b_unicode_script_names[];
extern const uint32_t n00b_unicode_script_count;

extern const uint32_t n00b_unicode_block_ranges[][2];
extern const uint16_t n00b_unicode_block_ids[];
extern const uint32_t n00b_unicode_block_count;
extern const char    *n00b_unicode_block_names[];

extern const uint16_t n00b_unicode_bidi_stage1[];
extern const uint8_t  n00b_unicode_bidi_stage2[];

extern const uint16_t n00b_unicode_eaw_stage1[];
extern const uint8_t  n00b_unicode_eaw_stage2[];

extern const uint16_t n00b_unicode_jt_stage1[];
extern const uint8_t  n00b_unicode_jt_stage2[];

extern const uint16_t n00b_unicode_props_stage1[];
extern const uint64_t n00b_unicode_props_stage2[];

extern const uint32_t n00b_unicode_script_ext_index[][2];
extern const uint32_t n00b_unicode_script_ext_index_len;
extern const uint32_t n00b_unicode_script_ext_data[];

extern const uint32_t n00b_unicode_numeric_index[][2];
extern const uint32_t n00b_unicode_numeric_index_len;
extern const uint32_t n00b_unicode_numeric_data[];

extern const uint16_t n00b_unicode_age_stage1[];
extern const uint8_t  n00b_unicode_age_stage2[];
extern const char    *n00b_unicode_age_names[];
extern const uint32_t n00b_unicode_age_count;

n00b_unicode_gc_t
n00b_unicode_general_category(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return N00B_UNICODE_GC_CN;
    return (n00b_unicode_gc_t)N00B_UNICODE_LOOKUP(n00b_unicode_gc_stage1,
                                                  n00b_unicode_gc_stage2,
                                                  cp);
}

uint8_t
n00b_unicode_combining_class(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return 0;
    return N00B_UNICODE_LOOKUP(n00b_unicode_ccc_stage1, n00b_unicode_ccc_stage2, cp);
}

n00b_unicode_script_t
n00b_unicode_script(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return 0;
    return N00B_UNICODE_LOOKUP(n00b_unicode_script_stage1, n00b_unicode_script_stage2, cp);
}

const char *
n00b_unicode_script_name(n00b_unicode_script_t s)
{
    if (s >= n00b_unicode_script_count)
        return "Unknown";
    return n00b_unicode_script_names[s];
}

n00b_unicode_block_t
n00b_unicode_block(n00b_codepoint_t cp)
{
    // Binary search in block ranges
    uint32_t lo = 0;
    uint32_t hi = n00b_unicode_block_count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (cp < n00b_unicode_block_ranges[mid][0]) {
            hi = mid;
        }
        else if (cp > n00b_unicode_block_ranges[mid][1]) {
            lo = mid + 1;
        }
        else {
            return n00b_unicode_block_ids[mid];
        }
    }
    return 0; // No_Block
}

const char *
n00b_unicode_block_name(n00b_unicode_block_t b)
{
    return n00b_unicode_block_names[b];
}

n00b_unicode_bidi_class_t
n00b_unicode_bidi_class(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return N00B_UNICODE_BIDI_L;
    return (n00b_unicode_bidi_class_t)N00B_UNICODE_LOOKUP(n00b_unicode_bidi_stage1,
                                                          n00b_unicode_bidi_stage2,
                                                          cp);
}

n00b_unicode_eaw_t
n00b_unicode_east_asian_width(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return N00B_UNICODE_EAW_N;
    return (n00b_unicode_eaw_t)N00B_UNICODE_LOOKUP(n00b_unicode_eaw_stage1,
                                                   n00b_unicode_eaw_stage2,
                                                   cp);
}

int
n00b_unicode_char_width(n00b_codepoint_t cp)
{
    // NUL
    if (cp == 0)
        return 0;

    // C0/C1 control chars
    n00b_unicode_gc_t gc = n00b_unicode_general_category(cp);
    if (gc == N00B_UNICODE_GC_CC || gc == N00B_UNICODE_GC_CF || gc == N00B_UNICODE_GC_MN
        || gc == N00B_UNICODE_GC_ME || gc == N00B_UNICODE_GC_MC) {
        // Combining marks and control characters have zero width
        // Exception: U+00AD SOFT HYPHEN is Cf but width 1
        if (cp == 0x00AD)
            return 1;
        if (gc == N00B_UNICODE_GC_MC)
            return 0; // spacing combining marks
        if (gc == N00B_UNICODE_GC_CC || gc == N00B_UNICODE_GC_CF)
            return 0;
        return 0; // Mn, Me
    }

    n00b_unicode_eaw_t eaw = n00b_unicode_east_asian_width(cp);
    if (eaw == N00B_UNICODE_EAW_W || eaw == N00B_UNICODE_EAW_F)
        return 2;

    return 1;
}

n00b_unicode_jt_t
n00b_unicode_joining_type(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return N00B_UNICODE_JT_U;
    return (n00b_unicode_jt_t)N00B_UNICODE_LOOKUP(n00b_unicode_jt_stage1,
                                                  n00b_unicode_jt_stage2,
                                                  cp);
}

bool
n00b_unicode_has_property(n00b_codepoint_t cp, n00b_unicode_property_t prop)
{
    if (cp >= 0x110000)
        return false;
    uint64_t bits
        = N00B_UNICODE_LOOKUP(n00b_unicode_props_stage1, n00b_unicode_props_stage2, cp);
    return (bits >> prop) & 1;
}

int
n00b_unicode_script_extensions(n00b_codepoint_t       cp,
                               n00b_unicode_script_t *scripts,
                               int                    max_scripts)
{
    if (cp >= 0x110000 || max_scripts <= 0)
        return 0;

    const uint32_t *entry = n00b_unicode_sparse_lookup(n00b_unicode_script_ext_index,
                                                       n00b_unicode_script_ext_index_len,
                                                       n00b_unicode_script_ext_data,
                                                       cp);

    if (entry) {
        uint32_t count = entry[0];
        int      n     = (int)count < max_scripts ? (int)count : max_scripts;
        for (int i = 0; i < n; i++) {
            scripts[i] = (n00b_unicode_script_t)entry[1 + i];
        }
        return n;
    }

    // Fallback: singleton from Script property
    scripts[0] = n00b_unicode_script(cp);
    return 1;
}

n00b_unicode_numeric_type_t
n00b_unicode_numeric_type(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return N00B_UNICODE_NUMERIC_NONE;
    const uint32_t *entry = n00b_unicode_sparse_lookup(n00b_unicode_numeric_index,
                                                       n00b_unicode_numeric_index_len,
                                                       n00b_unicode_numeric_data,
                                                       cp);
    if (!entry)
        return N00B_UNICODE_NUMERIC_NONE;
    // entry[0] = count (3), entry[1] = type, entry[2] = numerator, entry[3] =
    // denominator
    return (n00b_unicode_numeric_type_t)entry[1];
}

n00b_unicode_numeric_value_t
n00b_unicode_numeric_value(n00b_codepoint_t cp)
{
    n00b_unicode_numeric_value_t result = {N00B_UNICODE_NUMERIC_NONE, 0, 1};
    if (cp >= 0x110000)
        return result;

    const uint32_t *entry = n00b_unicode_sparse_lookup(n00b_unicode_numeric_index,
                                                       n00b_unicode_numeric_index_len,
                                                       n00b_unicode_numeric_data,
                                                       cp);
    if (!entry)
        return result;

    result.type        = (n00b_unicode_numeric_type_t)entry[1];
    result.numerator   = (int32_t)entry[2];
    result.denominator = (int32_t)entry[3];
    return result;
}

n00b_option_t(int32_t)
n00b_unicode_digit_value(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return n00b_option_none(int32_t);
    const uint32_t *entry = n00b_unicode_sparse_lookup(n00b_unicode_numeric_index,
                                                       n00b_unicode_numeric_index_len,
                                                       n00b_unicode_numeric_data,
                                                       cp);
    if (!entry)
        return n00b_option_none(int32_t);
    uint32_t type = entry[1];
    if (type != 1 && type != 2)
        return n00b_option_none(int32_t); // Only Decimal and Digit
    int32_t val = (int32_t)entry[2];
    if (val < 0 || val > 9)
        return n00b_option_none(int32_t);
    return n00b_option_set(int32_t, val);
}

int32_t
n00b_unicode_display_width_raw(const char *data, int64_t len)
{
    int32_t  width = 0;
    uint32_t pos   = 0;

    while (pos < (uint32_t)len) {
        int32_t cp = n00b_unicode_utf8_decode(data, (uint32_t)len, &pos);
        if (cp < 0)
            break;
        width += n00b_unicode_char_width((n00b_codepoint_t)cp);
    }

    return width;
}

int32_t
n00b_unicode_display_width(n00b_string_t *s)
{
    return n00b_unicode_display_width_raw(s->data, s->u8_bytes);
}

// ===========================================================================
// Age (per-codepoint lookup + name table accessors)
// ===========================================================================

n00b_unicode_age_t
n00b_unicode_age(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return 0;
    return (n00b_unicode_age_t)N00B_UNICODE_LOOKUP(n00b_unicode_age_stage1,
                                                   n00b_unicode_age_stage2,
                                                   cp);
}

const char *
n00b_unicode_age_name(n00b_unicode_age_t age)
{
    if ((uint32_t)age >= n00b_unicode_age_count)
        return "Unassigned";
    return n00b_unicode_age_names[age];
}

// ===========================================================================
// Range enumeration: turn per-codepoint property tables into sorted, merged
// range arrays.
// ===========================================================================
//
// Strategy: do ONE sweep over 0..0x10FFFF per category-class on first query,
// emitting (cp, value) into per-value range buckets.  Each bucket is then
// stored as a contiguous slice of a single backing array, keyed by enum
// value.  The "category class" granularity is chosen so we don't waste
// memory filling 47 binary-property arrays when the caller only asked for
// "Alphabetic".  In practice the parser tends to touch all of GC and a
// handful of properties.
//
// Thread safety: a coarse global mutex around the per-class init.  After
// the init flag is set, the data is read-only and lock-free (publication
// via release-store of the flag, paired with acquire-load on the read
// path; the mutex itself contains the necessary release/acquire fences).
//
// Memory: every entry is two uint32_t's (8 bytes).  For Unicode 16, GC has
// ~3700 ranges total across 30 values, scripts ~3000 across ~163, blocks
// 1:1 with ranges (~360), Bidi ~700.  Binary properties together are
// larger; we only fill on demand per property class.
// ===========================================================================

// Range slice type is defined in text/unicode/ctx.h as
// `n00b_unicode_range_slice_t`.  Alias the canonical name here to keep
// the file-local references readable; the bundle and per-subsystem
// caches live on `n00b_runtime_t::unicode_ctx`.
typedef n00b_unicode_range_slice_t range_slice_t;

// ---- lazy mutex init ------------------------------------------------------
//
// n00b_mutex_t requires an explicit init call (no static initializer is
// provided).  We elect one initializer per mutex via an atomic CAS on a
// state machine: 0 = needs init, 1 = mid-init, 2 = ready.  Late arrivals
// busy-wait on state==2; the wait is bounded by a single sys_mutex_init
// call (constant time).

static inline void
ensure_mutex_inited(n00b_mutex_t *mutex, _Atomic int *state)
{
    int snapshot = n00b_atomic_load(state);
    if (snapshot == 2)
        return;
    int expected = 0;
    if (n00b_atomic_cas(state, &expected, 1)) {
        n00b_mutex_init(mutex);
        n00b_atomic_store(state, 2);
        return;
    }
    while (n00b_atomic_load(state) != 2) {
        // Brief spin until the elected initializer publishes.
    }
}

// ---- shared init helpers --------------------------------------------------

// Append `cp` onto `*buf`, merging if contiguous with the last entry.  The
// underlying buffer grows geometrically.  cp 0xD800..0xDFFF (the surrogate
// hole) is excluded from output because regex-syntax-style codepoint
// vectors do not include surrogates in property classes.
static inline void
append_cp(n00b_codepoint_pair_t **buf, size_t *len, size_t *cap,
          n00b_codepoint_t cp)
{
    // Skip surrogates entirely.
    if (cp >= 0xD800u && cp <= 0xDFFFu)
        return;
    if (*len > 0) {
        n00b_codepoint_pair_t *last = &(*buf)[*len - 1];
        if (cp == last->hi + 1) {
            last->hi = cp;
            return;
        }
        // Bridge surrogate hole.
        if (cp == 0xE000u && last->hi == 0xD7FFu) {
            last->hi = 0xE000u;
            return;
        }
    }
    if (*len == *cap) {
        size_t newcap = *cap == 0 ? 16 : *cap * 2;
        n00b_codepoint_pair_t *newbuf
            = n00b_alloc_array(n00b_codepoint_pair_t, newcap);
        if (*buf && *len > 0) {
            memcpy(newbuf, *buf, *len * sizeof(n00b_codepoint_pair_t));
        }
        if (*buf)
            n00b_free(*buf);
        *buf = newbuf;
        *cap = newcap;
    }
    (*buf)[*len] = (n00b_codepoint_pair_t){.lo = cp, .hi = cp};
    (*len)++;
}

// Compact a per-value bucket array of dynamic vectors into a single
// backing array, populating an index-by-value `slices[]` table.
// `vecs[v]` holds the merged ranges for value v (with len/cap as
// standalone vectors).  We concatenate them into one big array so the
// public API can hand out (ranges, len) pairs by simple pointer
// arithmetic.
static void
compact_into_slice_table(n00b_codepoint_pair_t **vecs,
                         size_t                 *vec_lens,
                         size_t                 *vec_caps,
                         range_slice_t          *slices,
                         size_t                  n_values)
{
    size_t total = 0;
    for (size_t v = 0; v < n_values; v++)
        total += vec_lens[v];

    n00b_codepoint_pair_t *backing = nullptr;
    if (total > 0)
        backing = n00b_alloc_array(n00b_codepoint_pair_t, total);

    size_t off = 0;
    for (size_t v = 0; v < n_values; v++) {
        if (vec_lens[v] > 0) {
            memcpy(backing + off,
                   vecs[v],
                   vec_lens[v] * sizeof(n00b_codepoint_pair_t));
        }
        slices[v].ranges = backing + off;
        slices[v].len    = vec_lens[v];
        off += vec_lens[v];
        if (vecs[v])
            n00b_free(vecs[v]);
        vecs[v]     = nullptr;
        vec_caps[v] = 0;
    }
    (void)backing; // backing pointer is owned by slices[0..n_values-1]
}

// ===========================================================================
// General_Category ranges
// ===========================================================================

#define GC_N_VALUES N00B_UNICODE_GC_N_VALUES

static void
gc_init_locked(void)
{
    n00b_unicode_ctx_t    *ctx                  = _uctx();
    n00b_codepoint_pair_t *vecs[GC_N_VALUES]    = {nullptr};
    size_t                 lens[GC_N_VALUES]    = {0};
    size_t                 caps[GC_N_VALUES]    = {0};
    for (n00b_codepoint_t cp = 0; cp <= 0x10FFFFu; cp++) {
        n00b_unicode_gc_t gc = n00b_unicode_general_category(cp);
        append_cp(&vecs[gc], &lens[gc], &caps[gc], cp);
    }
    compact_into_slice_table(vecs, lens, caps, ctx->gc_slices, GC_N_VALUES);
    // Stash the backing pointer too: gc_slices[0..N-1].ranges all point
    // into one allocation owned by slot 0.  Keeping a copy makes the
    // ownership explicit and gives the GC a clean base pointer to mark.
    ctx->gc_backing = ctx->gc_slices[0].ranges;
}

static void
gc_ensure_init(void)
{
    n00b_unicode_ctx_t *ctx = _uctx();
    if (n00b_atomic_load(&ctx->gc_inited))
        return;
    ensure_mutex_inited(&ctx->gc_mutex, &ctx->gc_mutex_state);
    n00b_mutex_lock(&ctx->gc_mutex);
    if (!n00b_atomic_load(&ctx->gc_inited)) {
        gc_init_locked();
        n00b_atomic_store(&ctx->gc_inited, true);
    }
    n00b_mutex_unlock(&ctx->gc_mutex);
}

void
n00b_unicode_general_category_ranges(n00b_unicode_gc_t gc,
                                     const n00b_codepoint_pair_t **out_ranges,
                                     size_t                       *out_len)
{
    gc_ensure_init();
    if ((unsigned)gc >= GC_N_VALUES) {
        *out_ranges = nullptr;
        *out_len    = 0;
        return;
    }
    n00b_unicode_ctx_t *ctx = _uctx();
    *out_ranges = ctx->gc_slices[gc].ranges;
    *out_len    = ctx->gc_slices[gc].len;
}

// ===========================================================================
// Script ranges
// ===========================================================================

static void
script_init_locked(void)
{
    n00b_unicode_ctx_t *ctx      = _uctx();
    uint32_t            n_values = n00b_unicode_script_count;
    n00b_codepoint_pair_t **vecs
        = n00b_alloc_array(n00b_codepoint_pair_t *, n_values);
    size_t *lens = n00b_alloc_array(size_t, n_values);
    size_t *caps = n00b_alloc_array(size_t, n_values);
    for (uint32_t i = 0; i < n_values; i++) {
        vecs[i] = nullptr;
        lens[i] = 0;
        caps[i] = 0;
    }
    for (n00b_codepoint_t cp = 0; cp <= 0x10FFFFu; cp++) {
        n00b_unicode_script_t s = n00b_unicode_script(cp);
        if (s < n_values)
            append_cp(&vecs[s], &lens[s], &caps[s], cp);
    }
    ctx->script_slices = n00b_alloc_array(range_slice_t, n_values);
    compact_into_slice_table(vecs, lens, caps, ctx->script_slices, n_values);
    ctx->script_backing = (n_values > 0) ? ctx->script_slices[0].ranges : nullptr;
    n00b_free(vecs);
    n00b_free(lens);
    n00b_free(caps);
}

static void
script_ensure_init(void)
{
    n00b_unicode_ctx_t *ctx = _uctx();
    if (n00b_atomic_load(&ctx->script_inited))
        return;
    ensure_mutex_inited(&ctx->script_mutex, &ctx->script_mutex_state);
    n00b_mutex_lock(&ctx->script_mutex);
    if (!n00b_atomic_load(&ctx->script_inited)) {
        script_init_locked();
        n00b_atomic_store(&ctx->script_inited, true);
    }
    n00b_mutex_unlock(&ctx->script_mutex);
}

void
n00b_unicode_script_ranges(n00b_unicode_script_t sc,
                           const n00b_codepoint_pair_t **out_ranges,
                           size_t                       *out_len)
{
    script_ensure_init();
    if ((uint32_t)sc >= n00b_unicode_script_count) {
        *out_ranges = nullptr;
        *out_len    = 0;
        return;
    }
    n00b_unicode_ctx_t *ctx = _uctx();
    *out_ranges = ctx->script_slices[sc].ranges;
    *out_len    = ctx->script_slices[sc].len;
}

// ===========================================================================
// Script_Extensions ranges
// ===========================================================================

static void
scx_init_locked(void)
{
    n00b_unicode_ctx_t *ctx      = _uctx();
    uint32_t            n_values = n00b_unicode_script_count;
    n00b_codepoint_pair_t **vecs
        = n00b_alloc_array(n00b_codepoint_pair_t *, n_values);
    size_t *lens = n00b_alloc_array(size_t, n_values);
    size_t *caps = n00b_alloc_array(size_t, n_values);
    for (uint32_t i = 0; i < n_values; i++) {
        vecs[i] = nullptr;
        lens[i] = 0;
        caps[i] = 0;
    }

    // Each codepoint may belong to several scripts; ask for up to 16
    // (UTS #24 promises bounded set sizes — observed maximum is well
    // under 16).
    enum { MAX_SCX = 16 };
    n00b_unicode_script_t scs[MAX_SCX];

    for (n00b_codepoint_t cp = 0; cp <= 0x10FFFFu; cp++) {
        int n = n00b_unicode_script_extensions(cp, scs, MAX_SCX);
        for (int i = 0; i < n; i++) {
            n00b_unicode_script_t s = scs[i];
            if (s < n_values)
                append_cp(&vecs[s], &lens[s], &caps[s], cp);
        }
    }
    ctx->scx_slices = n00b_alloc_array(range_slice_t, n_values);
    compact_into_slice_table(vecs, lens, caps, ctx->scx_slices, n_values);
    ctx->scx_backing = (n_values > 0) ? ctx->scx_slices[0].ranges : nullptr;
    n00b_free(vecs);
    n00b_free(lens);
    n00b_free(caps);
}

static void
scx_ensure_init(void)
{
    n00b_unicode_ctx_t *ctx = _uctx();
    if (n00b_atomic_load(&ctx->scx_inited))
        return;
    ensure_mutex_inited(&ctx->scx_mutex, &ctx->scx_mutex_state);
    n00b_mutex_lock(&ctx->scx_mutex);
    if (!n00b_atomic_load(&ctx->scx_inited)) {
        scx_init_locked();
        n00b_atomic_store(&ctx->scx_inited, true);
    }
    n00b_mutex_unlock(&ctx->scx_mutex);
}

void
n00b_unicode_script_extensions_ranges(n00b_unicode_script_t sc,
                                      const n00b_codepoint_pair_t **out_ranges,
                                      size_t                       *out_len)
{
    scx_ensure_init();
    if ((uint32_t)sc >= n00b_unicode_script_count) {
        *out_ranges = nullptr;
        *out_len    = 0;
        return;
    }
    n00b_unicode_ctx_t *ctx = _uctx();
    *out_ranges = ctx->scx_slices[sc].ranges;
    *out_len    = ctx->scx_slices[sc].len;
}

// ===========================================================================
// Block ranges
// ===========================================================================
//
// Blocks are already stored as contiguous ranges in the generated table —
// the API just exposes a pointer pair.  We allocate per-block slices that
// each hold exactly one entry (block ranges never split).  No_Block (id 0)
// is the union of all gaps; we reuse the single-sweep approach for that
// degenerate case.

static void
block_init_locked(void)
{
    n00b_unicode_ctx_t *ctx = _uctx();
    // Number of distinct block ids: range entries hold ids in [1..count];
    // id 0 is the implicit "No_Block" gap fill.  Total slot count = count+1.
    uint32_t            n_values = n00b_unicode_block_count + 1;

    n00b_codepoint_pair_t **vecs
        = n00b_alloc_array(n00b_codepoint_pair_t *, n_values);
    size_t *lens = n00b_alloc_array(size_t, n_values);
    size_t *caps = n00b_alloc_array(size_t, n_values);
    for (uint32_t i = 0; i < n_values; i++) {
        vecs[i] = nullptr;
        lens[i] = 0;
        caps[i] = 0;
    }

    // Sweep — block lookup is binary-search per cp, but doing a linear
    // scan through the range table is cheaper.  Iterate ranges, append
    // wholesale.
    for (uint32_t i = 0; i < n00b_unicode_block_count; i++) {
        uint32_t lo = n00b_unicode_block_ranges[i][0];
        uint32_t hi = n00b_unicode_block_ranges[i][1];
        uint16_t id = n00b_unicode_block_ids[i];
        if (id >= n_values)
            continue;
        // Append directly; the single named-block ranges are non-
        // contiguous by definition (different ids).
        for (n00b_codepoint_t cp = lo; cp <= hi; cp++)
            append_cp(&vecs[id], &lens[id], &caps[id], cp);
    }
    // Compute No_Block (id 0): everything not covered above.
    for (n00b_codepoint_t cp = 0; cp <= 0x10FFFFu; cp++) {
        n00b_unicode_block_t b = n00b_unicode_block(cp);
        if (b == 0)
            append_cp(&vecs[0], &lens[0], &caps[0], cp);
    }

    ctx->block_slices = n00b_alloc_array(range_slice_t, n_values);
    compact_into_slice_table(vecs, lens, caps, ctx->block_slices, n_values);
    ctx->block_backing = (n_values > 0) ? ctx->block_slices[0].ranges : nullptr;
    n00b_free(vecs);
    n00b_free(lens);
    n00b_free(caps);
}

static void
block_ensure_init(void)
{
    n00b_unicode_ctx_t *ctx = _uctx();
    if (n00b_atomic_load(&ctx->block_inited))
        return;
    ensure_mutex_inited(&ctx->block_mutex, &ctx->block_mutex_state);
    n00b_mutex_lock(&ctx->block_mutex);
    if (!n00b_atomic_load(&ctx->block_inited)) {
        block_init_locked();
        n00b_atomic_store(&ctx->block_inited, true);
    }
    n00b_mutex_unlock(&ctx->block_mutex);
}

void
n00b_unicode_block_ranges_for(n00b_unicode_block_t bl,
                              const n00b_codepoint_pair_t **out_ranges,
                              size_t                       *out_len)
{
    block_ensure_init();
    if ((uint32_t)bl > n00b_unicode_block_count) {
        *out_ranges = nullptr;
        *out_len    = 0;
        return;
    }
    n00b_unicode_ctx_t *ctx = _uctx();
    *out_ranges = ctx->block_slices[bl].ranges;
    *out_len    = ctx->block_slices[bl].len;
}

// ===========================================================================
// Bidi_Class ranges
// ===========================================================================

#define BIDI_N_VALUES N00B_UNICODE_BIDI_N_VALUES

static void
bidi_init_locked(void)
{
    n00b_unicode_ctx_t    *ctx                  = _uctx();
    n00b_codepoint_pair_t *vecs[BIDI_N_VALUES] = {nullptr};
    size_t                 lens[BIDI_N_VALUES] = {0};
    size_t                 caps[BIDI_N_VALUES] = {0};
    for (n00b_codepoint_t cp = 0; cp <= 0x10FFFFu; cp++) {
        n00b_unicode_bidi_class_t b = n00b_unicode_bidi_class(cp);
        if (b < BIDI_N_VALUES)
            append_cp(&vecs[b], &lens[b], &caps[b], cp);
    }
    compact_into_slice_table(vecs, lens, caps, ctx->bidi_slices, BIDI_N_VALUES);
    ctx->bidi_backing = ctx->bidi_slices[0].ranges;
}

static void
bidi_ensure_init(void)
{
    n00b_unicode_ctx_t *ctx = _uctx();
    if (n00b_atomic_load(&ctx->bidi_inited))
        return;
    ensure_mutex_inited(&ctx->bidi_mutex, &ctx->bidi_mutex_state);
    n00b_mutex_lock(&ctx->bidi_mutex);
    if (!n00b_atomic_load(&ctx->bidi_inited)) {
        bidi_init_locked();
        n00b_atomic_store(&ctx->bidi_inited, true);
    }
    n00b_mutex_unlock(&ctx->bidi_mutex);
}

void
n00b_unicode_bidi_class_ranges(n00b_unicode_bidi_class_t bc,
                               const n00b_codepoint_pair_t **out_ranges,
                               size_t                       *out_len)
{
    bidi_ensure_init();
    if ((unsigned)bc >= BIDI_N_VALUES) {
        *out_ranges = nullptr;
        *out_len    = 0;
        return;
    }
    n00b_unicode_ctx_t *ctx = _uctx();
    *out_ranges = ctx->bidi_slices[bc].ranges;
    *out_len    = ctx->bidi_slices[bc].len;
}

// ===========================================================================
// Binary property ranges
// ===========================================================================
//
// Binary props are sparse — most codepoints have very few of them set.  We
// still do a single sweep across all properties on first call to *any*
// property accessor, because the test for "is bit b set" is essentially
// free once we have the per-cp bitmask.

#define PROP_N_VALUES N00B_UNICODE_PROP_N_VALUES

static void
prop_init_locked(void)
{
    n00b_unicode_ctx_t    *ctx                  = _uctx();
    n00b_codepoint_pair_t *vecs[PROP_N_VALUES] = {nullptr};
    size_t                 lens[PROP_N_VALUES] = {0};
    size_t                 caps[PROP_N_VALUES] = {0};
    for (n00b_codepoint_t cp = 0; cp <= 0x10FFFFu; cp++) {
        for (int p = 0; p < PROP_N_VALUES; p++) {
            if (n00b_unicode_has_property(cp, (n00b_unicode_property_t)p))
                append_cp(&vecs[p], &lens[p], &caps[p], cp);
        }
    }
    compact_into_slice_table(vecs, lens, caps, ctx->prop_slices, PROP_N_VALUES);
    ctx->prop_backing = ctx->prop_slices[0].ranges;
}

static void
prop_ensure_init(void)
{
    n00b_unicode_ctx_t *ctx = _uctx();
    if (n00b_atomic_load(&ctx->prop_inited))
        return;
    ensure_mutex_inited(&ctx->prop_mutex, &ctx->prop_mutex_state);
    n00b_mutex_lock(&ctx->prop_mutex);
    if (!n00b_atomic_load(&ctx->prop_inited)) {
        prop_init_locked();
        n00b_atomic_store(&ctx->prop_inited, true);
    }
    n00b_mutex_unlock(&ctx->prop_mutex);
}

void
n00b_unicode_property_ranges(n00b_unicode_property_t prop,
                             const n00b_codepoint_pair_t **out_ranges,
                             size_t                       *out_len)
{
    prop_ensure_init();
    if ((unsigned)prop >= PROP_N_VALUES) {
        *out_ranges = nullptr;
        *out_len    = 0;
        return;
    }
    n00b_unicode_ctx_t *ctx = _uctx();
    *out_ranges = ctx->prop_slices[prop].ranges;
    *out_len    = ctx->prop_slices[prop].len;
}

// ===========================================================================
// Derived (composite) GC ranges
// ===========================================================================
//
// A few helpers to merge sorted+disjoint ranges: this is a k-way merge
// over already-canonical inputs so the result remains canonical.  Plain
// in-place growth — derived sets are computed once per name and cached.

typedef struct {
    const char       *name;       // canonical (e.g. "Letter")
    n00b_unicode_gc_t members[8]; // up to 8 base GCs
    int               n_members;
} derived_gc_def_t;

static const derived_gc_def_t derived_gc_defs[] = {
    // Long names per UAX #44 PropertyValueAliases.
    {"Letter",
     {N00B_UNICODE_GC_LU, N00B_UNICODE_GC_LL, N00B_UNICODE_GC_LT,
      N00B_UNICODE_GC_LM, N00B_UNICODE_GC_LO},
     5},
    {"Cased_Letter",
     {N00B_UNICODE_GC_LU, N00B_UNICODE_GC_LL, N00B_UNICODE_GC_LT},
     3},
    {"Mark",
     {N00B_UNICODE_GC_MN, N00B_UNICODE_GC_MC, N00B_UNICODE_GC_ME},
     3},
    {"Number",
     {N00B_UNICODE_GC_ND, N00B_UNICODE_GC_NL, N00B_UNICODE_GC_NO},
     3},
    {"Punctuation",
     {N00B_UNICODE_GC_PC, N00B_UNICODE_GC_PD, N00B_UNICODE_GC_PS,
      N00B_UNICODE_GC_PE, N00B_UNICODE_GC_PI, N00B_UNICODE_GC_PF,
      N00B_UNICODE_GC_PO},
     7},
    {"Symbol",
     {N00B_UNICODE_GC_SM, N00B_UNICODE_GC_SC, N00B_UNICODE_GC_SK,
      N00B_UNICODE_GC_SO},
     4},
    {"Separator",
     {N00B_UNICODE_GC_ZS, N00B_UNICODE_GC_ZL, N00B_UNICODE_GC_ZP},
     3},
    {"Other",
     {N00B_UNICODE_GC_CC, N00B_UNICODE_GC_CF, N00B_UNICODE_GC_CS,
      N00B_UNICODE_GC_CO, N00B_UNICODE_GC_CN},
     5},
    // One-letter aliases per UAX #44 (Unicode-1.1 short names): L / M / N /
    // P / S / Z / C.  Same members as their long-named entries above;
    // duplicate definitions keep the cache lookup straightforward.
    {"L",
     {N00B_UNICODE_GC_LU, N00B_UNICODE_GC_LL, N00B_UNICODE_GC_LT,
      N00B_UNICODE_GC_LM, N00B_UNICODE_GC_LO},
     5},
    {"M",
     {N00B_UNICODE_GC_MN, N00B_UNICODE_GC_MC, N00B_UNICODE_GC_ME},
     3},
    {"N",
     {N00B_UNICODE_GC_ND, N00B_UNICODE_GC_NL, N00B_UNICODE_GC_NO},
     3},
    {"P",
     {N00B_UNICODE_GC_PC, N00B_UNICODE_GC_PD, N00B_UNICODE_GC_PS,
      N00B_UNICODE_GC_PE, N00B_UNICODE_GC_PI, N00B_UNICODE_GC_PF,
      N00B_UNICODE_GC_PO},
     7},
    {"S",
     {N00B_UNICODE_GC_SM, N00B_UNICODE_GC_SC, N00B_UNICODE_GC_SK,
      N00B_UNICODE_GC_SO},
     4},
    {"Z",
     {N00B_UNICODE_GC_ZS, N00B_UNICODE_GC_ZL, N00B_UNICODE_GC_ZP},
     3},
    {"C",
     {N00B_UNICODE_GC_CC, N00B_UNICODE_GC_CF, N00B_UNICODE_GC_CS,
      N00B_UNICODE_GC_CO, N00B_UNICODE_GC_CN},
     5},
    // Sentinel: `\p{Any}` covers the full codepoint range INCLUDING
    // surrogates (U+D800..U+DFFF).  Detected by n_members == 0 below; the
    // unioning path is bypassed and a synthetic single-range table is
    // returned.  Members[] is unused.
    {"Any", {0}, 0},
};
static const size_t derived_gc_count
    = sizeof(derived_gc_defs) / sizeof(derived_gc_defs[0]);

// Loose match per UAX #44 LM3 — case-insensitive, ignore underscores /
// spaces / hyphens.
static int
loose_eq_local(const char *a, const char *b)
{
    while (*a && *b) {
        while (*a == ' ' || *a == '_' || *a == '-')
            a++;
        while (*b == ' ' || *b == '_' || *b == '-')
            b++;
        if (!*a || !*b)
            break;
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (tolower(ca) != tolower(cb))
            return 0;
    }
    while (*a == ' ' || *a == '_' || *a == '-')
        a++;
    while (*b == ' ' || *b == '_' || *b == '-')
        b++;
    return *a == 0 && *b == 0;
}

// Cache + per-entry cached flags + mutex now live on the unicode ctx
// (`derived_gc_cache[]`, `derived_gc_cached[]`, `derived_gc_mutex`).
// `derived_gc_defs[]` itself is `static const` (string-literal `.name`
// fields are read-only static data — no GC tracking needed).
static_assert(sizeof(derived_gc_defs) / sizeof(derived_gc_defs[0])
                  <= N00B_UNICODE_DERIVED_GC_MAX,
              "derived_gc_defs grew past N00B_UNICODE_DERIVED_GC_MAX; "
              "bump the macro in include/text/unicode/ctx.h");

static int
range_cmp_lo_hi(const void *pa, const void *pb)
{
    const n00b_codepoint_pair_t *ra = (const n00b_codepoint_pair_t *)pa;
    const n00b_codepoint_pair_t *rb = (const n00b_codepoint_pair_t *)pb;
    if (ra->lo != rb->lo)
        return ra->lo < rb->lo ? -1 : 1;
    if (ra->hi != rb->hi)
        return ra->hi < rb->hi ? -1 : 1;
    return 0;
}

// Merge sorted+canonical inputs into one sorted+canonical output.  Inputs
// are disjoint *between* themselves (they are different GCs of the same
// cp), so the merge is a simple multi-way ascending walk.
static void
build_union(const derived_gc_def_t *def, range_slice_t *out)
{
    gc_ensure_init();
    n00b_unicode_ctx_t *ctx = _uctx();

    // Gather all candidate ranges into one buffer.
    size_t total_in = 0;
    for (int i = 0; i < def->n_members; i++)
        total_in += ctx->gc_slices[def->members[i]].len;

    if (total_in == 0) {
        out->ranges = nullptr;
        out->len    = 0;
        return;
    }

    n00b_codepoint_pair_t *all = n00b_alloc_array(n00b_codepoint_pair_t, total_in);
    size_t                 off = 0;
    for (int i = 0; i < def->n_members; i++) {
        size_t l = ctx->gc_slices[def->members[i]].len;
        memcpy(all + off,
               ctx->gc_slices[def->members[i]].ranges,
               l * sizeof(n00b_codepoint_pair_t));
        off += l;
    }

    // Sort by lo (ranges from different GCs are disjoint with respect to
    // each codepoint, so a stable sort yields a canonical order ready for
    // merging).
    qsort(all, total_in, sizeof(n00b_codepoint_pair_t), range_cmp_lo_hi);

    // Merge contiguous / overlapping ranges in place.
    size_t w = 0;
    for (size_t r = 0; r < total_in; r++) {
        if (w == 0) {
            all[w++] = all[r];
            continue;
        }
        n00b_codepoint_pair_t *last = &all[w - 1];
        if (all[r].lo <= last->hi + 1) {
            // Bridge surrogate hole if needed.
            if (all[r].hi > last->hi)
                last->hi = all[r].hi;
        }
        else if (last->hi == 0xD7FFu && all[r].lo == 0xE000u) {
            // Surrogate-hole bridge.
            last->hi = all[r].hi;
        }
        else {
            all[w++] = all[r];
        }
    }
    // Trim to actual size.
    n00b_codepoint_pair_t *trimmed = n00b_alloc_array(n00b_codepoint_pair_t, w);
    if (w > 0)
        memcpy(trimmed, all, w * sizeof(n00b_codepoint_pair_t));
    n00b_free(all);
    out->ranges = trimmed;
    out->len    = w;
}

// Build the synthetic `\p{Any}` table: a single range covering every
// codepoint 0..0x10FFFF, surrogates included.  Stored in the same
// derived-GC cache slot the matching `derived_gc_defs[]` entry indexes.
static void
build_any(n00b_unicode_range_slice_t *out)
{
    n00b_codepoint_pair_t *r = n00b_alloc_array(n00b_codepoint_pair_t, 1);
    r[0].lo     = 0;
    r[0].hi     = 0x10FFFFu;
    out->ranges = r;
    out->len    = 1;
}

bool
n00b_unicode_gc_derived_ranges(const char *name,
                               const n00b_codepoint_pair_t **out_ranges,
                               size_t                       *out_len)
{
    if (!name)
        return false;
    n00b_unicode_ctx_t *ctx = _uctx();
    for (size_t i = 0; i < derived_gc_count; i++) {
        if (loose_eq_local(derived_gc_defs[i].name, name)) {
            // Fast path: already cached.
            if (n00b_atomic_load(&ctx->derived_gc_cached[i])) {
                *out_ranges = ctx->derived_gc_cache[i].ranges;
                *out_len    = ctx->derived_gc_cache[i].len;
                return true;
            }
            ensure_mutex_inited(&ctx->derived_gc_mutex,
                                &ctx->derived_gc_mutex_state);
            n00b_mutex_lock(&ctx->derived_gc_mutex);
            if (!n00b_atomic_load(&ctx->derived_gc_cached[i])) {
                if (derived_gc_defs[i].n_members == 0) {
                    // Sentinel entry — currently only `\p{Any}`, which is
                    // not expressible as a union of GCs because the
                    // unioning helper drops the surrogate range.
                    build_any(&ctx->derived_gc_cache[i]);
                }
                else {
                    build_union(&derived_gc_defs[i],
                                &ctx->derived_gc_cache[i]);
                }
                n00b_atomic_store(&ctx->derived_gc_cached[i], true);
            }
            n00b_mutex_unlock(&ctx->derived_gc_mutex);
            *out_ranges = ctx->derived_gc_cache[i].ranges;
            *out_len    = ctx->derived_gc_cache[i].len;
            return true;
        }
    }
    return false;
}

// ===========================================================================
// Age ranges
// ===========================================================================
//
// Per UAX #44 / regex-syntax: an Age query for "12.0" returns codepoints
// with age <= 12.0 (excluding age 0 == Unassigned).  We cache per-age-
// index.

static void
age_one_time_init_locked(void)
{
    n00b_unicode_ctx_t *ctx      = _uctx();
    uint32_t            n_values = n00b_unicode_age_count;
    ctx->age_cache  = n00b_alloc_array(range_slice_t, n_values);
    ctx->age_cached = n00b_alloc_array(_Atomic bool, n_values);
}

static void
age_one_time_init(void)
{
    n00b_unicode_ctx_t *ctx = _uctx();
    if (n00b_atomic_load(&ctx->age_init_done))
        return;
    ensure_mutex_inited(&ctx->age_mutex, &ctx->age_mutex_state);
    n00b_mutex_lock(&ctx->age_mutex);
    if (!n00b_atomic_load(&ctx->age_init_done)) {
        age_one_time_init_locked();
        n00b_atomic_store(&ctx->age_init_done, true);
    }
    n00b_mutex_unlock(&ctx->age_mutex);
}

// Build the cumulative range set for "age <= idx".  age idx 0 is
// "Unassigned" — its set is empty (regex-syntax treats Age=Unassigned as
// no codepoints).  For idx >= 1, a single sweep marks every codepoint
// whose age value is in [1..idx] inclusive.
static void
age_build(uint8_t idx, range_slice_t *out)
{
    n00b_codepoint_pair_t *buf = nullptr;
    size_t                 len = 0;
    size_t                 cap = 0;

    if (idx == 0) {
        out->ranges = nullptr;
        out->len    = 0;
        return;
    }

    for (n00b_codepoint_t cp = 0; cp <= 0x10FFFFu; cp++) {
        n00b_unicode_age_t a = n00b_unicode_age(cp);
        if (a >= 1 && a <= idx)
            append_cp(&buf, &len, &cap, cp);
    }
    if (len > 0) {
        // Trim cap.
        n00b_codepoint_pair_t *trimmed
            = n00b_alloc_array(n00b_codepoint_pair_t, len);
        memcpy(trimmed, buf, len * sizeof(n00b_codepoint_pair_t));
        n00b_free(buf);
        out->ranges = trimmed;
    }
    else {
        if (buf)
            n00b_free(buf);
        out->ranges = nullptr;
    }
    out->len = len;
}

// Map the user-supplied name (canonical "V12_0", or the dotted "12.0") to
// an index into n00b_unicode_age_names.  Entries in the table are dotted
// strings like "12.0"; we accept both spellings via loose match.
static bool
age_name_to_index(const char *name, uint8_t *out_idx)
{
    if (!name)
        return false;
    // Strip leading 'V' / 'v' if present, and translate '_' to '.'.
    char   buf[16];
    size_t len = strlen(name);
    if (len >= sizeof(buf))
        return false;
    size_t k = 0;
    size_t i = 0;
    if (len >= 2 && (name[0] == 'V' || name[0] == 'v')
        && (name[1] >= '0' && name[1] <= '9')) {
        i = 1;
    }
    for (; i < len; i++) {
        char c = name[i];
        if (c == '_')
            c = '.';
        buf[k++] = c;
    }
    buf[k] = 0;

    for (uint32_t a = 0; a < n00b_unicode_age_count; a++) {
        if (loose_eq_local(n00b_unicode_age_names[a], buf)) {
            *out_idx = (uint8_t)a;
            return true;
        }
    }
    return false;
}

bool
n00b_unicode_age_ranges(const char *name,
                        const n00b_codepoint_pair_t **out_ranges,
                        size_t                       *out_len)
{
    age_one_time_init();
    uint8_t idx;
    if (!age_name_to_index(name, &idx))
        return false;

    n00b_unicode_ctx_t *ctx = _uctx();
    if (n00b_atomic_load(&ctx->age_cached[idx])) {
        *out_ranges = ctx->age_cache[idx].ranges;
        *out_len    = ctx->age_cache[idx].len;
        return true;
    }
    n00b_mutex_lock(&ctx->age_mutex);
    if (!n00b_atomic_load(&ctx->age_cached[idx])) {
        age_build(idx, &ctx->age_cache[idx]);
        n00b_atomic_store(&ctx->age_cached[idx], true);
    }
    n00b_mutex_unlock(&ctx->age_mutex);
    *out_ranges = ctx->age_cache[idx].ranges;
    *out_len    = ctx->age_cache[idx].len;
    return true;
}
