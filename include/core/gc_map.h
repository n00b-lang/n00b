/**
 * @file gc_map.h
 * @brief Per-allocation GC scan-bitmap API.
 *
 * The GC's conservative scanner treats every 8-byte-aligned word of an
 * allocation as a candidate heap pointer.  That is wrong for backing
 * stores that mix pointers and scalars (e.g. a `uint32_t[]` whose
 * adjacent pairs happen to form a pointer-shaped 8-byte value): the
 * collector forwards the bogus "pointer" and rewrites the slot,
 * corrupting unrelated scalar data.
 *
 * This header restores a per-allocation precision mechanism.  An
 * allocation declares its scan shape (`n00b_gc_scan_kind_t`); the
 * `CALLBACK` shape invokes a user-supplied function that fills an
 * `n00b_gc_map_t` bitmap — one bit per word — to tell the collector
 * which words are pointers.  The collector forwards only those words.
 *
 * Step 1 of `~/dd/gc-bits.md` introduces just the bitmap data type,
 * its helpers, and a small set of built-in callbacks.  GC integration
 * is wired up in later steps.
 */
#pragma once

#include "n00b.h"

/**
 * @brief How the GC should scan an allocation's contents.
 *
 * `DEFAULT` falls back to the legacy `no_scan` bit.  Any explicit value
 * overrides `no_scan`.  `CALLBACK` invokes the allocation's registered
 * `n00b_gc_scan_cb_t` to fill the bitmap; the other values are fixed
 * patterns that the collector can apply without an indirect call.
 */
enum n00b_gc_scan_kind_t : uint8_t {
    N00B_GC_SCAN_KIND_DEFAULT     = 0, // honour no_scan, else scan every word
    N00B_GC_SCAN_KIND_NONE        = 1, // never scan (overrides no_scan=false)
    N00B_GC_SCAN_KIND_ALL         = 2, // scan every word (overrides no_scan=true)
    N00B_GC_SCAN_KIND_EVERY_OTHER = 3, // scan words 0, 2, 4, ...
    N00B_GC_SCAN_KIND_CALLBACK    = 4, // invoke scan_cb to fill the bitmap
};

/**
 * @brief View the GC hands to a scan callback.
 *
 * The collector zero-initialises @ref bitmap before the call and the
 * callback sets the bits whose corresponding words contain pointers.
 * `num_words` is the user-data length in 8-byte words; the bitmap is
 * `n00b_gc_map_word_count(num_words)` uint64_ts long.
 */
struct n00b_gc_map_t {
    void     *user_ptr;
    uint64_t  num_words;
    uint64_t *bitmap;
};

/**
 * @brief Descriptor for `n00b_gc_scan_cb_struct_field`.
 *
 * Passed via the allocation's `scan_user` slot when the built-in
 * struct-field callback is installed.  Represents an array of `count`
 * elements, each `stride` words wide, where the pointer field sits at
 * word `offset` within the element.
 */
typedef struct {
    uint64_t stride;
    uint64_t offset;
    uint64_t count;
} n00b_gc_struct_array_t;

/** @brief Number of uint64_t words needed to hold a bitmap for
 *  @p num_words allocation words. */
static inline uint64_t
n00b_gc_map_word_count(uint64_t num_words)
{
    return (num_words + 63) >> 6;
}

// Single-bit ops; macros so they inline cleanly in the GC's hot path.
#define n00b_gc_map_mark(m, i)   ((m)->bitmap[(i) >> 6] |=  (UINT64_C(1) << ((i) & 63)))
#define n00b_gc_map_unmark(m, i) ((m)->bitmap[(i) >> 6] &= ~(UINT64_C(1) << ((i) & 63)))
#define n00b_gc_map_is_set(m, i) (((m)->bitmap[(i) >> 6] >> ((i) & 63)) & UINT64_C(1))

extern void n00b_gc_map_mark_all(n00b_gc_map_t *m);
extern void n00b_gc_map_unmark_all(n00b_gc_map_t *m);
extern void n00b_gc_map_mark_range(n00b_gc_map_t *m, uint64_t start, uint64_t len);
extern void n00b_gc_map_unmark_range(n00b_gc_map_t *m, uint64_t start, uint64_t len);
extern void n00b_gc_map_mark_stride(n00b_gc_map_t *m, uint64_t start, uint64_t stride, uint64_t count);
extern void n00b_gc_map_mark_every_other(n00b_gc_map_t *m, uint64_t start_offset);
extern void n00b_gc_map_mark_struct_field(n00b_gc_map_t *m, uint64_t base, uint64_t stride, uint64_t offset, uint64_t count);

/// Built-in scan callbacks.  Prefer the matching fixed `scan_kind`
/// enum value when it is sufficient — it lets the GC skip the
/// indirect call.  These exist for the cases where a callback slot
/// is already wired and the natural shape happens to match.
extern void n00b_gc_scan_cb_all(n00b_gc_map_t *m, void *user);
extern void n00b_gc_scan_cb_none(n00b_gc_map_t *m, void *user);
extern void n00b_gc_scan_cb_every_other(n00b_gc_map_t *m, void *user);
/// Caller passes user = &(n00b_gc_struct_array_t){ stride, offset, count }.
extern void n00b_gc_scan_cb_struct_field(n00b_gc_map_t *m, void *user);
