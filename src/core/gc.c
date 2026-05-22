/*
 * @file gc.c
 * @brief Copying/compacting garbage collector for n00b arenas.
 *
 * Ported from the original gc.nc.  The algorithm is unchanged: when
 * an arena is full, we create a "to-space", trace roots
 * (user-registered roots, runtime argv/envp, thread stacks), copy
 * live allocations, rewrite pointers, and swap segments.
 */

#define N00B_MEM_INTERNAL_API
#define N00B_USE_INTERNAL_API

#include <string.h>
#include <assert.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "n00b.h"
#include "core/gc.h"
#include "core/gc_stack.h"
#include "core/alloc_mdata.h"
#include "core/alloc.h"
#include "core/memory_info.h"
#include "core/arena.h"
#include "core/atomic.h"
#include "core/thread.h"
#include "core/align.h"
#include "core/mmaps.h"
#include "core/runtime.h"
#include "core/pool.h"
#include "adt/dict_untyped.h"

// ============================================================================
// Forward declarations
// ============================================================================

static void n00b_collect_setup(n00b_collect_t *, n00b_arena_t *);
static void n00b_scan_memory_range(n00b_collect_t *, void *, size_t);
static void n00b_process_worklist(n00b_collect_t *);
static bool n00b_visit_possible_pointer(n00b_collect_t *ctx, uint64_t **base,
                                         size_t i, bool base_checked);
static void n00b_collection_cleanup(n00b_collect_t *);
static void n00b_process_finalizers(n00b_collect_t *);
static void n00b_scan_thread_stacks(n00b_collect_t *);
static void n00b_scan_runtime(n00b_collect_t *);
static void n00b_scan_roots(n00b_collect_t *);
static void n00b_add_alloc_to_worklist(n00b_alloc_info_t  ainfo,
                                       n00b_collect_t    *ctx);
static void n00b_add_range_strided_to_worklist(void *start, uint64_t nwords,
                                                uint64_t stride, uint64_t offset,
                                                n00b_collect_t *ctx);
static void n00b_add_range_to_worklist(void *start, uint64_t nwords,
                                       n00b_collect_t *ctx);
static bool n00b_add_alloc_range_to_worklist(n00b_collect_t      *ctx,
                                             n00b_alloc_range_t  *range);

// ============================================================================
// Exact stack-map frame publication
// ============================================================================

n00b_gc_stack_policy_t
n00b_gc_stack_get_policy(void)
{
    return (n00b_gc_stack_policy_t)n00b_thread_self()->gc_stack_policy;
}

n00b_gc_stack_policy_t
n00b_gc_stack_set_policy(n00b_gc_stack_policy_t policy)
{
    assert(policy <= N00B_GC_STACK_EXACT_ONLY);

    n00b_thread_t         *thread = n00b_thread_self();
    n00b_gc_stack_policy_t old    = (n00b_gc_stack_policy_t)thread->gc_stack_policy;

    thread->gc_stack_policy = (uint32_t)policy;
    return old;
}

void
n00b_gc_stack_push(n00b_gc_stack_frame_t *frame, const n00b_gc_stack_map_t *map,
                   void **roots)
{
    assert(frame);
    assert(map);
    assert(!map->num_roots || roots);

    n00b_thread_t *thread = n00b_thread_self();

    *frame = (n00b_gc_stack_frame_t){
        .prev  = thread->gc_stack_top,
        .map   = map,
        .roots = roots,
    };
    thread->gc_stack_top = frame;
}

void
n00b_gc_stack_pop(n00b_gc_stack_frame_t *frame)
{
    assert(frame);

    n00b_thread_t *thread = n00b_thread_self();

    assert(thread->gc_stack_top == frame);
    thread->gc_stack_top = frame->prev;
    frame->prev          = nullptr;
    frame->map           = nullptr;
    frame->roots         = nullptr;
}

// ============================================================================
// Helpers
// ============================================================================

static inline uint32_t
arena_overhead(n00b_arena_t *arena)
{
    return arena->vtable.add_inline_header ? N00B_ALLOC_HDR_SZ : 0;
}

static inline n00b_inline_hdr_t *
alloc_info_raw_hdr(n00b_alloc_info_t info)
{
    return (info.kind == n00b_alloc_oob)
               ? (n00b_inline_hdr_t *)info.hdr.oob
               : info.hdr.in_line;
}

// ============================================================================
// Create destination arena (to-space)
// ============================================================================

static n00b_arena_t *
n00b_create_destination_arena(n00b_arena_t *src)
{
    uint64_t sz = n00b_arena_size(src);

    // If we were really short on memory last time, go up a power of two.
    if (src->current_segment->next_segment
        || src->alloc_count < N00B_TOO_FEW_ALLOCS) {
        sz *= 2;
    }

    // To-space never gets its own metadata pool; we reuse from-space's.
    // clang-format off
    n00b_arena_t *result = n00b_new_arena(
        .size           = sz,
        .use_gc         = true,
        .no_map         = true,
        .hidden         = true,
        .inline_headers = src->vtable.add_inline_header,
        .name           = "to-space");
    // clang-format on

    assert(result->current_segment->size > 0
           && result->current_segment->size >= sz);

    return result;
}

// ============================================================================
// Forward allocation helpers
// ============================================================================

static __attribute__((noinline)) n00b_inline_hdr_t *
n00b_forward_mdata(n00b_collect_t    *ctx,
                   n00b_oob_hdr_t    *old_map,
                   n00b_inline_hdr_t *new_alloc)
{
    n00b_oob_hdr_t *map_item;

    char   *old_user_ptr = old_map->user_ptr;
    int64_t copy_len     = old_map->alloc_len - arena_overhead(ctx->to_space);
    char   *new_user_ptr = ((char *)new_alloc) + arena_overhead(ctx->to_space);

    assert(new_user_ptr + old_map->alloc_len < ctx->to_space->segment_end);

    // Allocate new OOB record from the shared metadata pool.
    map_item = n00b_alloc_with_opts(n00b_oob_hdr_t,
                                    &(n00b_alloc_opts_t){.allocator = ctx->from_space->vtable.metadata_pool});

    memcpy(map_item, old_map, sizeof(n00b_oob_hdr_t));

    map_item->user_ptr  = new_user_ptr;
    map_item->hcur      = new_alloc;
    map_item->file_name = old_map->file_name;

    if (ctx->to_space->vtable.add_inline_header) {
        memcpy(new_alloc, old_map, sizeof(n00b_inline_hdr_t));
        new_alloc->guard = n00b_gc_guard;
    }

    // Add to the new metadata dict (stored on to-space during collection).
    n00b_dict_untyped_put(ctx->to_space->vtable.metadata, new_user_ptr, map_item);

    memcpy(new_user_ptr, old_user_ptr, copy_len);

    old_map->hcur->moved = true;
    old_map->moved       = true;

    return (n00b_inline_hdr_t *)map_item;
}

// Must have full size headers in this case.
static __attribute__((noinline)) n00b_inline_hdr_t *
n00b_forward_inline(n00b_collect_t    *ctx,
                    n00b_inline_hdr_t *old_alloc,
                    n00b_inline_hdr_t *new_alloc)
{
    assert(!ctx->to_space->vtable.metadata);
    memcpy(new_alloc, old_alloc, old_alloc->alloc_len);

    old_alloc->moved = true;

    return new_alloc;
}

// This needs to act like we actually called n00b_core_alloc().
// But we have solo use of the allocator with no thread contention,
// so we bypass most of it.
//
// Note that we prefer working with the metadata record.  However, we
// will also update the inline alloc record if it's used.
//
// The inline alloc record must be used if the arena collects, yet
// doesn't keep metadata records. Otherwise, we assume that the first
// bytes are the inline header, unless:
//
//  1. The arena 'overhead' is less than sizeof(n00b_inline_hdr_t); or
//  2. The no_inline_headers flag is on (currently unused otherwise).
//
// The default arena keeps inline headers on, so that we can marshal
// out of it.

static inline n00b_inline_hdr_t *
n00b_forward_alloc(n00b_collect_t *ctx, n00b_inline_hdr_t *old)
{
    char              *top;
    n00b_inline_hdr_t *new;

    top = n00b_atomic_load(&ctx->to_space->next_alloc);
    new = (n00b_inline_hdr_t *)top;
    top = top + old->alloc_len;

    ctx->to_space->alloc_count++;

    n00b_atomic_store(&ctx->to_space->next_alloc, top);

    n00b_inline_hdr_t *result;
    void              *scan_start;
    [[maybe_unused]] bool no_scan;
    uint32_t           nwords;
    n00b_gc_scan_kind_t scan_kind;

    if (ctx->from_space->vtable.metadata_pool) {
        n00b_option_t(n00b_oob_hdr_t *) old_oob_opt =
            n00b_to_mem_metadata_record(old);
        assert(n00b_option_is_set(old_oob_opt));
        n00b_oob_hdr_t *old_oob = n00b_option_get(old_oob_opt);
        result     = n00b_forward_mdata(ctx, old_oob, new);
        scan_start = ((n00b_oob_hdr_t *)result)->user_ptr;
        no_scan    = old_oob->no_scan;
        scan_kind  = (n00b_gc_scan_kind_t)old_oob->scan_kind;
        nwords     = (old_oob->alloc_len - arena_overhead(ctx->from_space))
                         / sizeof(void *);
    }
    else {
        result     = n00b_forward_inline(ctx, old, new);
        scan_start = (char *)new + arena_overhead(ctx->to_space);
        no_scan    = new->no_scan;
        scan_kind  = (n00b_gc_scan_kind_t)new->scan_kind;
        nwords     = (old->alloc_len - arena_overhead(ctx->from_space))
                         / sizeof(void *);
    }

#if defined(N00B_DISABLE_NOSCAN)
    bool do_scan = true;
#else
    bool do_scan = !no_scan;
#endif

    if (do_scan) {
        if (scan_kind == N00B_GC_SCAN_KIND_EVERY_OTHER) {
            n00b_add_range_strided_to_worklist(scan_start, nwords, 2, 0, ctx);
        }
        else if (scan_kind == N00B_GC_SCAN_KIND_CALLBACK) {
            n00b_gc_scan_cb_t cb;
            void             *user;
            if (ctx->from_space->vtable.metadata_pool) {
                n00b_option_t(n00b_oob_hdr_t *) old_oob_opt =
                    n00b_to_mem_metadata_record(old);
                assert(n00b_option_is_set(old_oob_opt));
                n00b_oob_hdr_t *old_oob = n00b_option_get(old_oob_opt);
                cb   = old_oob->scan_cb;
                user = old_oob->scan_user;
            } else {
                cb   = new->scan_cb;
                user = new->scan_user;
            }
            if (cb) {
                n00b_allocator_t *wpool = (n00b_allocator_t *)&ctx->work_pool;
                uint64_t  bm_words = n00b_gc_map_word_count(nwords);
                uint64_t *bitmap   = n00b_alloc_array_with_opts(uint64_t, bm_words,
                                         &(n00b_alloc_opts_t){.allocator = wpool});
                for (uint64_t bi = 0; bi < bm_words; bi++) bitmap[bi] = 0;
                n00b_gc_map_t m = {.user_ptr = scan_start, .num_words = nwords,
                                   .bitmap = bitmap};
                cb(&m, user);
                for (uint64_t bi = 0; bi < nwords; bi++) {
                    if (n00b_gc_map_is_set(&m, bi)) {
                        n00b_add_range_strided_to_worklist(
                            (char *)scan_start + bi * sizeof(void *),
                            1, 0, 0, ctx);
                    }
                }
            }
        }
        else {
            n00b_add_range_to_worklist(scan_start, nwords, ctx);
        }
    }

    return result;
}

// ============================================================================
// Pointer translation
// ============================================================================

// Compute the user-data base address from a raw header, using the
// arena configuration (has_oob) rather than bit-testing.
static inline char *
n00b_user_data_base(n00b_inline_hdr_t *hdr, bool has_oob)
{
    if (has_oob) {
        return ((n00b_oob_hdr_t *)hdr)->user_ptr;
    }
    return ((char *)hdr) + sizeof(n00b_inline_hdr_t);
}

static void *
n00b_translate_pointer(n00b_collect_t    *ctx,
                       n00b_inline_hdr_t *old_alloc,
                       uint64_t         **arr,
                       n00b_inline_hdr_t *fw_loc,
                       uint32_t           ix)
{
    uint64_t *old_ptr = n00b_atomic_load((_Atomic(uint64_t *) *)(arr + ix));
    bool      has_oob = ctx->from_space->vtable.metadata_pool != nullptr;

    assert(old_ptr);

    char *old_base = n00b_user_data_base(old_alloc, has_oob);
    char *new_base = n00b_user_data_base(fw_loc, has_oob);

    ptrdiff_t offset = (char *)old_ptr - old_base;

    assert(offset >= 0);
    if (offset > old_alloc->alloc_len) {
        return arr[ix];
    }

    assert(fw_loc);
    assert(fw_loc != old_alloc);

    return (void *)(new_base + offset);
}

// ============================================================================
// Memo operations
// ============================================================================

static inline bool
n00b_is_first_visit(n00b_collect_t     *ctx,
                    n00b_inline_hdr_t  *h,
                    n00b_inline_hdr_t **fw)
{
    bool result;

    *fw = n00b_dict_untyped_get(&ctx->memos, h, &result);

    if (!result) {
        assert(!h->moved);
    }

    return !result;
}

static inline void
n00b_register_visit(n00b_collect_t    *ctx,
                    n00b_inline_hdr_t *h,
                    n00b_inline_hdr_t *fw)
{
    n00b_dict_untyped_add(&ctx->memos, h, fw);
}

// ============================================================================
// Worklist operations
// ============================================================================

static void
n00b_add_range_to_worklist(void *start, uint64_t nwords, n00b_collect_t *ctx)
{
    n00b_gc_wl_item_t *entry;
    entry = n00b_alloc_with_opts(n00b_gc_wl_item_t,
                                 &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)&ctx->work_pool});
    entry->start     = start;
    entry->num_words = nwords;
    entry->stride    = 0;  // 0 == legacy scan-every-word
    entry->offset    = 0;
    n00b_list_push(ctx->worklist, entry);
}

static void
n00b_add_scan_range_to_worklist(n00b_collect_t       *ctx,
                                void                 *start,
                                uint64_t              nwords,
                                n00b_gc_scan_kind_t   scan_kind,
                                n00b_gc_scan_cb_t     scan_cb,
                                void                 *scan_user)
{
    if (!nwords || scan_kind == N00B_GC_SCAN_KIND_NONE) {
        return;
    }

    if (scan_kind == N00B_GC_SCAN_KIND_EVERY_OTHER) {
        n00b_add_range_strided_to_worklist(start, nwords, 2, 0, ctx);
        return;
    }

    if (scan_kind == N00B_GC_SCAN_KIND_CALLBACK && scan_cb) {
        n00b_allocator_t *wpool = (n00b_allocator_t *)&ctx->work_pool;
        uint64_t          bm_words = n00b_gc_map_word_count(nwords);
        uint64_t         *bitmap   = n00b_alloc_array_with_opts(
            uint64_t,
            bm_words,
            &(n00b_alloc_opts_t){.allocator = wpool});

        for (uint64_t bi = 0; bi < bm_words; bi++) {
            bitmap[bi] = 0;
        }

        n00b_gc_map_t m = {.user_ptr = start,
                           .num_words = nwords,
                           .bitmap    = bitmap};
        scan_cb(&m, scan_user);

        for (uint64_t bi = 0; bi < nwords; bi++) {
            if (n00b_gc_map_is_set(&m, bi)) {
                n00b_add_range_strided_to_worklist((char *)start
                                                       + bi * sizeof(void *),
                                                   1,
                                                   0,
                                                   0,
                                                   ctx);
            }
        }
        return;
    }

    n00b_add_range_to_worklist(start, nwords, ctx);
}

static bool
n00b_add_alloc_range_to_worklist(n00b_collect_t *ctx, n00b_alloc_range_t *range)
{
    bool found = false;

    n00b_dict_untyped_get(&ctx->memos, range, &found);
    if (found) {
        return false;
    }

    n00b_dict_untyped_add(&ctx->memos, range, range);
    n00b_add_scan_range_to_worklist(ctx,
                                    range->start,
                                    range->len / sizeof(void *),
                                    range->scan_kind,
                                    range->scan_cb,
                                    range->scan_user);
    return true;
}

static void
n00b_add_range_strided_to_worklist(void *start, uint64_t nwords,
                                   uint64_t stride, uint64_t offset,
                                   n00b_collect_t *ctx)
{
    n00b_gc_wl_item_t *entry;
    entry = n00b_alloc_with_opts(n00b_gc_wl_item_t,
                                 &(n00b_alloc_opts_t){.allocator = (n00b_allocator_t *)&ctx->work_pool});
    entry->start     = start;
    entry->num_words = nwords;
    entry->stride    = stride;
    entry->offset    = offset;
    n00b_list_push(ctx->worklist, entry);
}

static void
n00b_add_alloc_to_worklist(n00b_alloc_info_t ainfo, n00b_collect_t *ctx)
{
    void              *start;
    uint32_t           n;
    n00b_gc_scan_kind_t kind;

    if (ainfo.kind == n00b_alloc_oob) {
        n00b_oob_hdr_t *oob = ainfo.hdr.oob;
#if !defined(N00B_DISABLE_PTR_WORDS)
        n = oob->ptr_words;
        if (!n)
#endif
        {
            n = (oob->alloc_len - arena_overhead(ctx->from_space))
                    / sizeof(void *);
        }
        start = oob->user_ptr;
        kind  = (n00b_gc_scan_kind_t)oob->scan_kind;
    }
    else {
        n00b_inline_hdr_t *hdr = ainfo.hdr.in_line;
#if !defined(N00B_DISABLE_PTR_WORDS)
        n = hdr->ptr_words;
        if (!n)
#endif
        {
            n = (hdr->alloc_len - arena_overhead(ctx->from_space))
                    / sizeof(void *);
        }
        start = (char *)hdr + arena_overhead(ctx->from_space);
        kind  = (n00b_gc_scan_kind_t)hdr->scan_kind;
    }

    if (kind == N00B_GC_SCAN_KIND_EVERY_OTHER) {
        n00b_add_range_strided_to_worklist(start, n, 2, 0, ctx);
        return;
    }

    if (kind == N00B_GC_SCAN_KIND_CALLBACK) {
        n00b_gc_scan_cb_t cb;
        void             *user;
        if (ainfo.kind == n00b_alloc_oob) {
            cb   = ainfo.hdr.oob->scan_cb;
            user = ainfo.hdr.oob->scan_user;
        } else {
            cb   = ainfo.hdr.in_line->scan_cb;
            user = ainfo.hdr.in_line->scan_user;
        }
        if (cb) {
            n00b_allocator_t *wpool = (n00b_allocator_t *)&ctx->work_pool;
            uint64_t  bm_words = n00b_gc_map_word_count(n);
            uint64_t *bitmap   = n00b_alloc_array_with_opts(uint64_t, bm_words,
                                     &(n00b_alloc_opts_t){.allocator = wpool});
            for (uint64_t bi = 0; bi < bm_words; bi++) bitmap[bi] = 0;
            n00b_gc_map_t m = {.user_ptr = start, .num_words = n,
                               .bitmap = bitmap};
            cb(&m, user);
            /* Visit only marked words.  Add each as its own length-1
             * worklist entry to reuse the existing scan infrastructure. */
            for (uint64_t bi = 0; bi < n; bi++) {
                if (n00b_gc_map_is_set(&m, bi)) {
                    n00b_add_range_strided_to_worklist((char *)start
                                                            + bi * sizeof(void *),
                                                       1, 0, 0, ctx);
                }
            }
            return;
        }
    }

    /* ALL / DEFAULT fall through to a plain scan over every word. */
    n00b_add_range_to_worklist(start, n, ctx);
}

static void
n00b_process_worklist(n00b_collect_t *ctx)
{
    n00b_gc_wl_item_t *item;

    while (n00b_list_len(ctx->worklist) > 0) {
        item = n00b_option_get(n00b_list_pop_front(n00b_gc_wl_item_t *, ctx->worklist));
        if (item->stride == 0) {
            n00b_scan_memory_range(ctx, item->start, item->num_words);
        } else {
            /* Strided scan: visit slots at indices offset, offset+stride,
             * offset+2*stride, ... while in [0, num_words). */
            uint64_t **base = (uint64_t **)item->start;
            for (uint64_t i = item->offset;
                 i < item->num_words;
                 i += item->stride) {
                n00b_visit_possible_pointer(ctx, base, i, false);
            }
        }
        n00b_free(item);
    }
}

// ============================================================================
// Pointer visitor
// ============================================================================

// This examines a memory cell, rewriting it, if it contains a
// from-space pointer.
//
// If the memory cell contains a pointer into any managed heap space
// at all, we queue a scan for that pointer's allocation, UNLESS
// one of the following things is true:
//
// 1. We have already queued a scan (n00b_is_first_visit will be false)
// 2. `no_scan` is set on the alloc.
//
// For in-heap first visits, n00b_forward_alloc() will always copy,
// and only check the alloc's `no_scan` field.

static inline bool
n00b_visit_possible_pointer(n00b_collect_t *ctx,
                            uint64_t      **base,
                            size_t          i,
                            bool            base_checked)
{
    // Returns 'true' if we find a pointer, so that custom marking functions
    // can more easily be data-dependent.

    if (!base_checked) {
        auto mmap_opt = n00b_mmap_by_address(base);

        if (!n00b_option_is_set(mmap_opt)) {
            n00b_mmap_perms_t perms = n00b_check_memory_perms(base);
            if (perms == n00b_mmap_perms_no_access) {
                return false;
            }
        }
    }

    n00b_inline_hdr_t *fw_hdr;
    n00b_inline_hdr_t *old_hdr;
    uint64_t          *word = base[i];

    auto mmap_opt = n00b_mmap_by_address((void *)word);

    if (!n00b_option_is_set(mmap_opt)) {
        return false;
    }

    n00b_mmap_info_t *mmap = n00b_option_get(mmap_opt);

    if (!n00b_mmap_is_gc_scannable(mmap)) {
        return false;
    }

    switch (mmap->kind) {
    case n00b_mmap_managed_segment:
    case n00b_mmap_sys_segment:
    case n00b_mmap_static:
    case n00b_mmap_pool:
    case n00b_mmap_internal:
        break;
    case n00b_mmap_stack:
        return false; // We will scan this separately.
    case n00b_mmap_zero_page:
    case n00b_mmap_api_mmap:
    case n00b_mmap_arena:
        return false;
    default:
        // This means we have a pointer into internal memory, in GC'd
        // space, which we should be avoiding.
        //
        // Or, there could be corruption, etc.
        abort();
    }

    auto ainfo = n00b_find_alloc_info(word);

    if (ainfo.kind != n00b_alloc_oob && ainfo.kind != n00b_alloc_inline) {
        if (mmap->kind == n00b_mmap_static) {
            auto range_opt = n00b_mmap_range_by_address((void *)word);

            if (n00b_option_is_set(range_opt)) {
                n00b_add_alloc_range_to_worklist(ctx, n00b_option_get(range_opt));
            }
            return false;
        }

        ainfo = n00b_find_alloc_info(word, .scan_for_header = true);

        if (ainfo.kind != n00b_alloc_oob && ainfo.kind != n00b_alloc_inline) {
            auto range_opt = n00b_mmap_range_by_address((void *)word);

            if (n00b_option_is_set(range_opt)) {
                n00b_add_alloc_range_to_worklist(ctx, n00b_option_get(range_opt));
            }

            return false;
        }
    }

    old_hdr = alloc_info_raw_hdr(ainfo);

    bool in_from_space = mmap->allocator == (n00b_allocator_t *)ctx->from_space;

    if (n00b_is_first_visit(ctx, old_hdr, &fw_hdr)) {
        if (in_from_space) {
            fw_hdr = n00b_forward_alloc(ctx, old_hdr);
            assert(fw_hdr);
        }
        else {
            fw_hdr = nullptr;
#if !defined(N00B_DISABLE_NOSCAN)
            bool no_scan = (ainfo.kind == n00b_alloc_oob)
                               ? ainfo.hdr.oob->no_scan
                               : ainfo.hdr.in_line->no_scan;
            if (!no_scan)
#endif
            {
                n00b_add_alloc_to_worklist(ainfo, ctx);
            }
        }
        // This has to happen after we create the fw_hdr object if
        // it's in the heap we're collecting.
        n00b_register_visit(ctx, old_hdr, fw_hdr);
    }
    else {
        if (in_from_space) {
            assert(fw_hdr);
        }
    }

    if (in_from_space) {
        uint64_t *v = n00b_translate_pointer(ctx, old_hdr, base, fw_hdr, i);

        if (v) {
            base[i] = v;
            return true;
        }
    }

    return false;
}

// ============================================================================
// Memory range scanning
// ============================================================================

static void
n00b_scan_memory_range(n00b_collect_t *ctx, void *start, size_t nwords)
{
    size_t     i               = nwords;
    uint64_t **base            = (uint64_t **)start;
    // Cache page readability to avoid per-word permission probes.
    size_t     page_size       = n00b_page_size;
    uintptr_t  page_mask       = ~(uintptr_t)(page_size - 1);
    uintptr_t  last_page       = 0;
    bool       last_page_ok    = false;
    bool       last_page_valid = false;

    while (i--) {
        void     *slot = (void *)(base + i);
        uintptr_t page = ((uintptr_t)slot) & page_mask;

        if (!last_page_valid || page != last_page) {
            auto slot_mmap_opt = n00b_mmap_by_address(slot);

            if (n00b_option_is_set(slot_mmap_opt)) {
                last_page_ok = true;
            }
            else {
                n00b_mmap_perms_t perms = n00b_check_memory_perms(slot);
                last_page_ok            = perms != n00b_mmap_perms_no_access;
            }

            last_page       = page;
            last_page_valid = true;
        }

        if (!last_page_ok) {
            continue;
        }

        n00b_visit_possible_pointer(ctx, base, i, true);
    }
}

// ============================================================================
// Thread stack scanning
// ============================================================================

static __attribute__((noinline)) void
n00b_scan_thread_stacks(n00b_collect_t *ctx)
{
    volatile n00b_thread_t *t;
    n00b_runtime_t         *rt = n00b_get_runtime();

    for (volatile uint32_t i = 0; i < rt->max_threads; i++) {
        t = n00b_atomic_load(&rt->threads[i].thread);

        if (!t) {
            continue;
        }

        n00b_gc_stack_policy_t stack_policy =
            (n00b_gc_stack_policy_t)t->gc_stack_policy;
        bool exact_stack_scanned = false;

        if (stack_policy != N00B_GC_STACK_CONSERVATIVE) {
            n00b_gc_stack_frame_t *frame = (n00b_gc_stack_frame_t *)t->gc_stack_top;

            if (frame) {
                exact_stack_scanned = true;
            }

            while (frame) {
                const n00b_gc_stack_map_t *map = frame->map;

                assert(map);
                assert(map->num_slots == 0 || map->slots);
                assert(map->num_roots == 0 || frame->roots);

                for (uint32_t si = 0; si < map->num_slots; si++) {
                    const n00b_gc_stack_slot_t *slot = &map->slots[si];

                    assert(slot->root_index < map->num_roots);

                    if (!slot->num_words) {
                        continue;
                    }

                    void *addr = frame->roots[slot->root_index];
                    if (addr) {
                        n00b_scan_memory_range(ctx, addr, slot->num_words);
                    }
                }

                frame = frame->prev;
            }
        }

        if (stack_policy == N00B_GC_STACK_EXACT_ONLY
            || (stack_policy == N00B_GC_STACK_EXACT_WITH_FALLBACK
                && exact_stack_scanned)) {
            goto scan_thread_state;
        }

        // Since n00b_scan_memory_range cares about aligned words, we will
        // convert the stack bounds to pointers to the type uint64_t; that
        // way we can subtract the pointers.
        //
        // Remember that the stack grows down (on machines we are
        // targeting), so the "top" will be the smaller address, and
        // the one we want to start with in the scan.

        uint64_t *top  = (uint64_t *)t->stack_top;
        uint64_t *base = (uint64_t *)t->stack_map->end;

        // Some basic sanity checking. The stack should always, always be word
        // aligned.

#ifndef _WIN32
        if (((uint64_t)top) < t->stack_map->start) {
            top = (uint64_t *)t->stack_map->start;
        }
#endif

        top  = (uint64_t *)n00b_align_ceil((uint64_t)top, 0x08);
        base = (uint64_t *)n00b_align_floor((uint64_t)base, 0x08);

        assert(base > top);

        uint64_t num_words = base - top;

        assert(num_words > 0);

        n00b_scan_memory_range(ctx, top, num_words);

scan_thread_state:
        // Scan the thread structure while we're here.
        n00b_scan_memory_range(ctx,
                               (void *)t,
                               sizeof(n00b_thread_t) / sizeof(void *));
        // Scan the thread RECORD too — it lives in `rt->threads[i]` and
        // holds pointers into the GC heap that nothing else scans:
        // `exclusive_locks` / `read_locks` (heads of per-thread lock
        // accounting chains, where each in-heap `n00b_mutex_t` /
        // `n00b_rwlock_t` lives inside a relocatable allocation — e.g.
        // an embedded field of a GC-allocated `Regex`), `lock_wait_target`,
        // and `regex_last_detail` (per-thread error string).
        //
        // Without this scan, after a collection that relocates a heap
        // lock, the chain head still points at the old (freed) address,
        // and the next `n00b_lock_acquire_accounting` walks one step
        // into freed memory.  Reproducible by running `regex_count_all`
        // in a tight loop over a multi-hundred-KB haystack — the SIMD
        // match path generates enough GC pressure to force a collection
        // mid-lock, after ~1000 iterations.
        n00b_scan_memory_range(ctx,
                               (void *)&rt->threads[i],
                               sizeof(n00b_thread_record_t)
                                   / sizeof(void *));
    }
}

// ============================================================================
// Runtime scanning (argv / envp)
// ============================================================================

static void
n00b_scan_runtime(n00b_collect_t *ctx)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    // Scan argv array (array of char *)
    if (rt->argv.data && rt->argv.len) {
        n00b_scan_memory_range(ctx, rt->argv.data,
                               rt->argv.len * sizeof(char *) / sizeof(void *));
        n00b_process_worklist(ctx);
    }

    // Scan envp array (array of char *)
    if (rt->envp.data && rt->envp.len) {
        n00b_scan_memory_range(ctx, rt->envp.data,
                               rt->envp.len * sizeof(char *) / sizeof(void *));
        n00b_process_worklist(ctx);
    }
}

// ============================================================================
// User-registered root scanning
// ============================================================================

static void
n00b_scan_roots(n00b_collect_t *ctx)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    size_t          n  = rt->gc_roots.len;

    for (size_t i = 0; i < n; i++) {
        n00b_gc_root_t *root = &rt->gc_roots.data[i];
        n00b_scan_memory_range(ctx, root->addr, root->num_words);
        n00b_process_worklist(ctx);
    }
}

// ============================================================================
// Root registration API
// ============================================================================

void
_n00b_gc_register_root(void *addr, size_t num_words)
{
    n00b_runtime_t *rt   = n00b_get_runtime();
    n00b_gc_root_t  root = {.addr = addr, .num_words = num_words};

    n00b_list_push(rt->gc_roots, root);
}

void
_n00b_gc_unregister_root(void *addr)
{
    n00b_runtime_t *rt  = n00b_get_runtime();
    size_t          len = n00b_list_len(rt->gc_roots);

    for (size_t i = 0; i < len; i++) {
        n00b_gc_root_t root = n00b_list_get(rt->gc_roots, i);

        if (root.addr == addr) {
            (void)n00b_list_delete(rt->gc_roots, i);
            return;
        }
    }
}

// ============================================================================
// Finalizer processing
// ============================================================================

static inline bool
n00b_addr_in_arena(void *addr, n00b_arena_t *arena)
{
    n00b_segment_t *seg = arena->current_segment;

    while (seg) {
        char *start = (char *)&seg->mem[0];
        char *end   = start + (seg->size - sizeof(n00b_segment_t));

        if ((char *)addr >= start && (char *)addr < end) {
            return true;
        }
        seg = seg->next_segment;
    }
    return false;
}

// Check if a finalizer entry's object was in the from_space being collected.
// For OOB arenas, the alloc_info is an OOB record (in the metadata pool,
// NOT in from_space), so we check the OOB record's user_ptr instead.
// For inline-only arenas, the alloc_info IS the inline header in from_space.
static inline bool
n00b_finalizer_in_from_space(n00b_finalizer_info_t *entry, n00b_collect_t *ctx)
{
    if (ctx->from_space->vtable.metadata_pool) {
        // OOB arena: alloc_info is n00b_oob_hdr_t*, check user_ptr.
        n00b_oob_hdr_t *oob = (n00b_oob_hdr_t *)entry->alloc_info;
        return n00b_addr_in_arena(oob->user_ptr, ctx->from_space);
    }
    // Inline-only: alloc_info is the inline header in the segment.
    return n00b_addr_in_arena(entry->alloc_info, ctx->from_space);
}

static void
n00b_process_finalizers(n00b_collect_t *ctx)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    if (!rt || !rt->finalizers.data) {
        return;
    }

    size_t len = n00b_list_len(rt->finalizers);

    for (size_t i = len; i > 0; i--) {
        n00b_finalizer_info_t *entry = n00b_list_get(rt->finalizers, i - 1);
        bool                   found;
        n00b_inline_hdr_t     *fw;

        fw = n00b_dict_untyped_get(&ctx->memos, entry->alloc_info, &found);

        if (found) {
            // Object survived — update alloc_info to the forwarded header.
            // fw is nullptr when the allocation was scanned but lives in
            // a *different* arena (not the one being collected).  In that
            // case, leave alloc_info alone — it still points to the
            // original (valid) header in that other arena.
            if (fw) {
                entry->alloc_info = fw;
            }
            // user_ptr typically points outside the collected arena
            // (e.g., a lock in system_pool), so no update needed.
        }
        else if (n00b_finalizer_in_from_space(entry, ctx)) {
            // Object is dead — run finalizer and remove entry.
            entry->funcptr(entry->user_ptr);
            (void)n00b_list_delete(rt->finalizers, i - 1);
            n00b_free(entry);
        }
        // else: object in a different arena, leave alone.
    }
}

// ============================================================================
// Collection setup
// ============================================================================

static void
n00b_collect_setup(n00b_collect_t *ctx, n00b_arena_t *from_space)
{
    ctx->from_space = from_space;
    ctx->to_space   = n00b_create_destination_arena(from_space);

    // clang-format off
    n00b_pool_init(&ctx->work_pool,
                   .__system = true,
                   .hidden   = true,
                   .name     = "gc_worklist");

    n00b_allocator_t *wa = (n00b_allocator_t *)&ctx->work_pool;

    ctx->worklist = n00b_list_new_cap(n00b_gc_wl_item_t *,
                                      N00B_GC_WL_START_SIZE, wa);

    n00b_dict_untyped_init(&ctx->memos,
                           .start_capacity = N00B_GC_WL_START_SIZE,
                           .allocator      = wa,
                           .hash           = n00b_hash_word,
                           .skip_obj_hash  = true);
    // clang-format on

    // If from-space uses OOB metadata, create a fresh dict in the
    // same metadata pool and attach it to to-space for forwarding.
    if (from_space->vtable.metadata_pool) {
        n00b_allocator_t *md_pool = from_space->vtable.metadata_pool;

        n00b_dict_untyped_t *new_md = n00b_alloc_with_opts(n00b_dict_untyped_t,
                                                           &(n00b_alloc_opts_t){.allocator = md_pool});
        n00b_dict_untyped_init(new_md,
                               .start_capacity = from_space->alloc_count * 2,
                               .allocator      = md_pool,
                               .hash           = n00b_hash_word,
                               .skip_obj_hash  = true);
        ctx->to_space->vtable.metadata = new_md;
    }

    assert(ctx->to_space->segment_end
           == ((char *)ctx->to_space->current_segment)
                  + ctx->to_space->current_segment->size);
    assert(ctx->to_space && ctx->to_space != ctx->from_space);
}

// ============================================================================
// Collection cleanup — swap segments and destroy temporaries
// ============================================================================

// Free all OOB values in the old metadata dict, then free the dict
// itself.  Both the OOB records and the dict internals live in the
// metadata pool, so n00b_free returns their slots to the free list.
static void
n00b_free_old_metadata(n00b_dict_untyped_t *old_dict)
{
    n00b_dict_untyped_store_t *store = n00b_atomic_load(&old_dict->store);

    if (store) {
        for (uint32_t i = 0; i <= store->last_slot; i++) {
            uint32_t flags = n00b_atomic_load(&store->buckets[i].flags);

            if (store->buckets[i].value
                && !(flags & N00B_HT_FLAG_DELETED)) {
                n00b_free(store->buckets[i].value);
            }
        }
        n00b_free(store);
    }

    n00b_free(old_dict);
}

static void
n00b_collection_cleanup(n00b_collect_t *ctx)
{
    // Swap the segment linked list so the from-space arena keeps its
    // identity but now holds the live (to-space) data.

    n00b_segment_t *new_segment  = ctx->to_space->current_segment;
    void           *old_segments = (void *)ctx->from_space->current_segment;

    ctx->from_space->current_segment = new_segment;
    ctx->from_space->next_alloc      = ctx->to_space->next_alloc;
    ctx->from_space->segment_end     = ctx->to_space->segment_end;

    // Swap the metadata dict: free old OOB records and install the
    // new dict.  The metadata pool itself is shared and stays put.
    if (ctx->from_space->vtable.metadata_pool) {
        n00b_dict_untyped_t *old_dict = ctx->from_space->vtable.metadata;
        ctx->from_space->vtable.metadata = ctx->to_space->vtable.metadata;
        ctx->to_space->vtable.metadata   = nullptr;

        if (old_dict) {
            n00b_free_old_metadata(old_dict);
        }
    }

    ctx->to_space->current_segment = old_segments;
    ctx->to_space->vtable.hidden   = false;

    n00b_register_arena_segment(new_segment,
                                ctx->from_space->segment_end,
                                ctx->from_space,
                                .file = ctx->from_space->vtable.debug_name);

    /* Scrub thread lock chains of any entries that live inside the
     * old from-space segments we're about to unmap.  Locks embedded
     * in default-arena allocations (Regex::inner_lock and friends)
     * would otherwise leave dangling heads on rt->threads[i].
     * exclusive_locks. */
    extern void n00b_lock_chains_scrub_range(uint64_t lo, uint64_t hi);
    n00b_segment_t *seg = (n00b_segment_t *)old_segments;
    while (seg) {
        uintptr_t seg_lo = (uintptr_t)seg;
        uintptr_t seg_hi = seg_lo + seg->size;
        n00b_lock_chains_scrub_range(seg_lo, seg_hi);
        seg = seg->next_segment;
    }

    n00b_allocator_destroy((n00b_allocator_t *)&ctx->work_pool);
    n00b_allocator_destroy((n00b_allocator_t *)ctx->to_space);

    n00b_atomic_fence();
}

// ============================================================================
// Entry point
// ============================================================================

// We do not want the compiler to inline this, otherwise it will quite
// likely blend the stack frame in a way we don't like w/
// n00b_collect().
static __attribute__((noinline)) void
n00b_collect_internal(n00b_arena_t *arena)
{
    n00b_collect_t ctx;
    n00b_segment_t *segment = arena->current_segment;

    segment->last_addr = n00b_atomic_load(&arena->next_alloc);

    n00b_collect_setup(&ctx, arena);
    arena->alloc_count = 0;

    n00b_scan_roots(&ctx);

    n00b_scan_runtime(&ctx);
    n00b_process_worklist(&ctx);

    n00b_scan_thread_stacks(&ctx);
    n00b_process_worklist(&ctx);
    assert(!n00b_list_len(ctx.worklist));
    n00b_process_finalizers(&ctx);

    n00b_collection_cleanup(&ctx);
}

void
n00b_collect(n00b_arena_t *arena)
{
    jmp_buf                            register_spill;
    [[maybe_unused]] volatile uint64_t top  = 0;
    volatile n00b_thread_t            *self = n00b_thread_self();

    self->stack_top = (void *)&top;

    if (!setjmp(register_spill)) {
        n00b_collect_internal(arena);
        longjmp(register_spill, 1);
    }
}
