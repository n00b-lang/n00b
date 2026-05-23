/**
 * @file gc_map.c
 * @brief Per-allocation GC scan-bitmap helpers and built-in callbacks.
 *
 * Step 1 of `~/dd/gc-bits.md`.  These routines are pure data
 * manipulation — no GC state, no allocator state.  They are wired
 * into the collector in later steps.
 */
#include "core/gc_map.h"
#include <string.h>

/// Mask of the in-range bits of the bitmap's last uint64_t.  When
/// `num_words` is a multiple of 64 the last word is fully in-range.
static inline uint64_t
last_word_mask(uint64_t num_words)
{
    uint64_t rem = num_words & 63;
    return rem ? ((UINT64_C(1) << rem) - 1) : UINT64_MAX;
}

void
n00b_gc_map_mark_all(n00b_gc_map_t *m)
{
    uint64_t nw = n00b_gc_map_word_count(m->num_words);
    if (nw == 0) {
        return;
    }
    memset(m->bitmap, 0xff, (nw - 1) * sizeof(uint64_t));
    m->bitmap[nw - 1] = last_word_mask(m->num_words);
}

void
n00b_gc_map_unmark_all(n00b_gc_map_t *m)
{
    memset(m->bitmap, 0, n00b_gc_map_word_count(m->num_words) * sizeof(uint64_t));
}

void
n00b_gc_map_mark_range(n00b_gc_map_t *m, uint64_t start, uint64_t len)
{
    assert(start + len <= m->num_words);
    if (len == 0) {
        return;
    }

    uint64_t end = start + len; // exclusive

    // Head: bits in the partial leading word, up to the next 64-boundary
    // or `end`, whichever comes first.
    uint64_t head_end = (start + 63) & ~UINT64_C(63);
    if (head_end > end) {
        head_end = end;
    }
    for (uint64_t i = start; i < head_end; ++i) {
        n00b_gc_map_mark(m, i);
    }

    if (head_end == end) {
        return;
    }

    // Middle: whole 64-bit words.
    uint64_t middle_start_word = head_end >> 6;
    uint64_t tail_start        = end & ~UINT64_C(63);
    uint64_t middle_words      = (tail_start - head_end) >> 6;
    if (middle_words) {
        memset(&m->bitmap[middle_start_word], 0xff, middle_words * sizeof(uint64_t));
    }

    // Tail: bits in the partial trailing word.
    for (uint64_t i = tail_start; i < end; ++i) {
        n00b_gc_map_mark(m, i);
    }
}

void
n00b_gc_map_unmark_range(n00b_gc_map_t *m, uint64_t start, uint64_t len)
{
    assert(start + len <= m->num_words);
    if (len == 0) {
        return;
    }

    uint64_t end = start + len;

    uint64_t head_end = (start + 63) & ~UINT64_C(63);
    if (head_end > end) {
        head_end = end;
    }
    for (uint64_t i = start; i < head_end; ++i) {
        n00b_gc_map_unmark(m, i);
    }

    if (head_end == end) {
        return;
    }

    uint64_t middle_start_word = head_end >> 6;
    uint64_t tail_start        = end & ~UINT64_C(63);
    uint64_t middle_words      = (tail_start - head_end) >> 6;
    if (middle_words) {
        memset(&m->bitmap[middle_start_word], 0, middle_words * sizeof(uint64_t));
    }

    for (uint64_t i = tail_start; i < end; ++i) {
        n00b_gc_map_unmark(m, i);
    }
}

void
n00b_gc_map_mark_stride(n00b_gc_map_t *m, uint64_t start, uint64_t stride, uint64_t count)
{
    if (count == 0) {
        return;
    }
    assert(stride > 0);
    assert(start + (count - 1) * stride < m->num_words);

    uint64_t idx = start;
    for (uint64_t i = 0; i < count; ++i) {
        n00b_gc_map_mark(m, idx);
        idx += stride;
    }
}

void
n00b_gc_map_mark_every_other(n00b_gc_map_t *m, uint64_t start_offset)
{
    if (start_offset >= m->num_words) {
        return;
    }
    uint64_t count = (m->num_words - start_offset + 1) >> 1;
    n00b_gc_map_mark_stride(m, start_offset, 2, count);
}

void
n00b_gc_map_mark_struct_field(n00b_gc_map_t *m,
                              uint64_t base,
                              uint64_t stride,
                              uint64_t offset,
                              uint64_t count)
{
    if (count == 0) {
        return;
    }
    n00b_gc_map_mark_stride(m, base + offset, stride, count);
}

void
n00b_gc_map_mark_struct_layout(n00b_gc_map_t *m, const n00b_gc_struct_layout_t *layout)
{
    if (!layout || layout->count == 0 || layout->offset_count == 0) {
        return;
    }

    assert(layout->stride > 0);
    assert(layout->offsets != nullptr);

    for (uint64_t i = 0; i < layout->count; i++) {
        uint64_t base = i * layout->stride;

        for (uint64_t j = 0; j < layout->offset_count; j++) {
            uint64_t offset = layout->offsets[j];

            assert(offset < layout->stride);
            assert(base + offset < m->num_words);
            n00b_gc_map_mark(m, base + offset);
        }
    }
}

// ---------------------------------------------------------------------------
// Built-in callbacks.
// ---------------------------------------------------------------------------

void
n00b_gc_scan_cb_all(n00b_gc_map_t *m, void *user)
{
    (void)user;
    n00b_gc_map_mark_all(m);
}

void
n00b_gc_scan_cb_none(n00b_gc_map_t *m, void *user)
{
    (void)m;
    (void)user;
    // The GC zero-initialises the bitmap before calling us; leaving it
    // alone is exactly "no pointer words".
}

void
n00b_gc_scan_cb_every_other(n00b_gc_map_t *m, void *user)
{
    (void)user;
    n00b_gc_map_mark_every_other(m, 0);
}

void
n00b_gc_scan_cb_struct_field(n00b_gc_map_t *m, void *user)
{
    n00b_gc_struct_array_t *desc = (n00b_gc_struct_array_t *)user;
    n00b_gc_map_mark_struct_field(m, 0, desc->stride, desc->offset, desc->count);
}

void
n00b_gc_scan_cb_struct_layout(n00b_gc_map_t *m, void *user)
{
    n00b_gc_map_mark_struct_layout(m, (const n00b_gc_struct_layout_t *)user);
}
