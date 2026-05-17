#define N00B_MEM_INTERNAL_API

#include "n00b.h"
#include "core/arena.h"
#include "core/runtime.h"
#include "core/mmaps.h"
#include "core/alloc_mdata.h"
#include "core/alloc.h"
#include "core/atomic.h"
#include "core/futex.h"
#include "core/align.h"
#include "core/pool.h"
#include "core/thread.h"
#include "core/runtime.h"
#include "adt/interval_tree.h"
#include "adt/variant.h"

// TODO: fix this
// #include "conduit/print.h"
#include <stdio.h>
#define n00b_fprintf fprintf

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <errno.h>

#if !defined(N00B_MMAPS_START_PAGES)
#define N00B_MMAPS_START_PAGES 16
#endif

// Convenience alias for the concrete tree/node types.
#define mmap_tree_t n00b_interval_tree_t(n00b_mmap_data_t)
#define mmap_node_t n00b_interval_node_t(n00b_mmap_data_t)

// ============================================================================
// Locking (reentrant TID-based spinlock, unchanged from treap version)
// ============================================================================

static inline void
mmap_lock(n00b_mmap_ctx_t *ctx)
{
    int64_t tid      = n00b_thread_unique_id();
    int64_t expected = -1;

    do {
        if (expected == tid) {
            break;
        }
        expected = -1;
    } while (!n00b_cas(&ctx->tid_lock, &expected, tid));
}

static inline void
mmap_unlock(n00b_mmap_ctx_t *ctx)
{
    n00b_atomic_store(&ctx->tid_lock, -1);
}

#define mmap_write_lock(ctx)   mmap_lock(ctx)
#define mmap_read_lock(ctx)    mmap_lock(ctx)
#define mmap_write_unlock(ctx) mmap_unlock(ctx)
#define mmap_read_unlock(ctx)  mmap_unlock(ctx)

// ============================================================================
// Internal helpers
// ============================================================================

static n00b_mmap_info_t *
mmaps_insert_raw(n00b_mmap_ctx_t     *ctx,
                 void                *startp,
                 uint64_t             blen,
                 n00b_mmap_rec_kind_t kind,
                 uint64_t             binary_offset)
{
    n00b_allocator_t *alloc = (n00b_allocator_t *)&ctx->pool;
    n00b_mmap_info_t *info  = n00b_alloc_with_opts(n00b_mmap_info_t, &(n00b_alloc_opts_t){.allocator = alloc});
    uint64_t          start = (uint64_t)startp;
    uint64_t          end   = start + blen;

    *info = (n00b_mmap_info_t){
        .start         = start,
        .end           = end,
        .kind          = kind,
        .binary_offset = binary_offset,
    };

    n00b_mmap_data_t data = n00b_variant_set(n00b_mmap_data_t, n00b_mmap_info_t *, info);
    auto insert_r = n00b_interval_insert(ctx->mmap_tree, start, end, data);
    assert(n00b_result_is_ok(insert_r));
    info->tree_node = n00b_result_get(insert_r);

    return info;
}

static void
n00b_mmaps_remove_base(n00b_mmap_ctx_t *ctx, n00b_mmap_info_t *info)
{
    if (info->allocator && info->allocator->hidden) {
        return;
    }

    (void)n00b_interval_delete(ctx->mmap_tree, (mmap_node_t *)info->tree_node);
    n00b_free(info);
}

static inline void
n00b_mmaps_remove(n00b_mmap_ctx_t *ctx, n00b_mmap_info_t *info)
{
    mmap_write_lock(ctx);
    n00b_mmaps_remove_base(ctx, info);
    mmap_write_unlock(ctx);
}

// ============================================================================
// Lookup
// ============================================================================

n00b_option_t(n00b_mmap_info_t *)
n00b_mmap_lookup(n00b_mmap_ctx_t *ctx, void *addr)
{
    uint64_t start = (uint64_t)addr;
    uint64_t end   = start + 1;

    if (end < start) {
        return n00b_option_none(n00b_mmap_info_t *);
    }

    mmap_read_lock(ctx);

    // Use search_any to avoid allocating a temp stack per call.
    // This is called per potential pointer during GC collection.
    auto r = n00b_interval_search_any(ctx->mmap_tree, start, end);

    mmap_read_unlock(ctx);

    if (n00b_result_is_err(r)) {
        return n00b_option_none(n00b_mmap_info_t *);
    }

    mmap_node_t *node = n00b_result_get(r);

    if (!node) {
        return n00b_option_none(n00b_mmap_info_t *);
    }

    if (n00b_variant_is_type(node->data, n00b_mmap_info_t *)) {
        return n00b_option_set(n00b_mmap_info_t *,
                                n00b_variant_get(node->data, n00b_mmap_info_t *));
    }

    return n00b_option_none(n00b_mmap_info_t *);
}

// ============================================================================
// Initialization
// ============================================================================

// In memory_info.c
extern void n00b_load_static_ranges();

void
n00b_mmaps_initialize(n00b_mmap_ctx_t *ctx)
{
    *ctx = (n00b_mmap_ctx_t){ .tid_lock = -1 };

    n00b_pool_init(&ctx->pool, .__system = true, .hidden = true, .name = "mmaps");

    n00b_allocator_t  *alloc = (n00b_allocator_t *)&ctx->pool;
    n00b_alloc_opts_t  aopts = { .allocator = alloc };

    ctx->mmap_tree = n00b_alloc_with_opts(mmap_tree_t, &aopts);
    n00b_interval_tree_init(ctx->mmap_tree, alloc);

    n00b_load_static_ranges();
}

// ============================================================================
// Sub-range tracking
// ============================================================================

void
n00b_mmap_delete_ranges(n00b_mmap_ctx_t *ctx, uint64_t start, uint64_t end)
{
    n00b_allocator_t *alloc = (n00b_allocator_t *)&ctx->pool;
    n00b_stack_t(void *) hits = n00b_stack_new(void *, alloc);

    (void)n00b_interval_search(ctx->mmap_tree, start, end, &hits);

    while (n00b_stack_len(hits) > 0) {
        mmap_node_t *node = n00b_option_get(n00b_stack_pop(void *, hits));

        // Only delete sub-range entries, leave mmap records alone.
        if (n00b_variant_is_type(node->data, n00b_alloc_range_t *)) {
            n00b_alloc_range_t *range = n00b_variant_get(node->data, n00b_alloc_range_t *);
            (void)n00b_interval_delete(ctx->mmap_tree, node);
            n00b_free(range);
        }
    }
}

void
n00b_mmap_register_range(void *startp, void *endp, n00b_mmap_rec_kind_t kind) _kargs
{
    n00b_allocator_t    *allocator = nullptr;
    const char          *file      = nullptr;
    n00b_gc_scan_kind_t  scan_kind = N00B_GC_SCAN_KIND_DEFAULT;
    n00b_gc_scan_cb_t    scan_cb   = nullptr;
    void                *scan_user = nullptr;
}
{
    n00b_runtime_t  *rt  = n00b_get_runtime();
    n00b_mmap_ctx_t *ctx = n00b_global_mem_map(rt);

    n00b_allocator_t   *alloc = (n00b_allocator_t *)&ctx->pool;
    n00b_alloc_range_t *range = n00b_alloc_with_opts(n00b_alloc_range_t,
                                                      &(n00b_alloc_opts_t){.allocator = alloc});
    uint64_t start = (uint64_t)startp;
    uint64_t end   = (uint64_t)endp;

    *range = (n00b_alloc_range_t){
        .start     = startp,
        .len       = (uint32_t)(end - start),
        .scan_kind = scan_kind,
        .scan_cb   = scan_cb,
        .scan_user = scan_user,
    };

    n00b_mmap_data_t data = n00b_variant_set(n00b_mmap_data_t, n00b_alloc_range_t *, range);

    mmap_write_lock(ctx);
    auto range_r = n00b_interval_insert(ctx->mmap_tree, start, end, data);
    assert(n00b_result_is_ok(range_r));
    mmap_write_unlock(ctx);
}

// ============================================================================
// Registration / unregistration
// ============================================================================

// clang-format off
n00b_option_t(n00b_mmap_info_t *)
n00b_mmap_register(void *startp, void *endp, n00b_mmap_rec_kind_t kind) _kargs
{
    n00b_runtime_t   *runtime           = n00b_get_runtime();
    const char       *file              = nullptr;
    n00b_allocator_t *allocator         = nullptr;
    uint64_t          binary_offset     = 0;
    intptr_t          slide             = 0;
    uint64_t          order_id          = 0;
    bool              definitely_unique = true;
}
// clang-format on
{
    if (allocator && allocator->hidden) {
        return n00b_option_none(n00b_mmap_info_t *);
    }

    n00b_mmap_info_t *result;
    n00b_mmap_ctx_t  *ctx   = n00b_global_mem_map(runtime);
    uint64_t          start = (uint64_t)startp;
    uint64_t          end   = (uint64_t)endp;
    uint64_t          blen  = end - start;

    assert(ctx);
    assert(end > start);

    mmap_write_lock(ctx);

    if (!definitely_unique) {
        mmap_node_t *existing = n00b_result_get(
            n00b_interval_search_any(ctx->mmap_tree, start, start + 1));
        if (existing && n00b_variant_is_type(existing->data, n00b_mmap_info_t *)) {
            result = n00b_variant_get(existing->data, n00b_mmap_info_t *);
            assert(allocator == result->allocator || !allocator);
            mmap_write_unlock(ctx);
            return n00b_option_set(n00b_mmap_info_t *, result);
        }
    }

    result            = mmaps_insert_raw(ctx, startp, blen, kind, binary_offset);
    result->allocator = allocator;
    result->file      = file;
    result->slide     = slide;
    result->order_id  = order_id;

    mmap_write_unlock(ctx);

    return n00b_option_set(n00b_mmap_info_t *, result);
}

// clang-format off
n00b_result_t(void *)
_n00b_mmap(size_t sz, char *loc) _kargs
{
    n00b_allocator_t    *allocator = nullptr;
    n00b_mmap_rec_kind_t kind      = n00b_mmap_api_mmap;
    char                *name      = nullptr;
}
// clang-format on
{
    if (name) {
        loc = name;
    }

    if (allocator) {
        sz = n00b_page_align(sz);
    }
    else {
        assert(kind == n00b_mmap_api_mmap || kind == n00b_mmap_arena);

        if (kind == n00b_mmap_arena) {
            sz = n00b_page_size;
        }
        else {
            sz = n00b_page_align(sz);
        }
    }

    auto mmap_r = n00b_check_mmap(nullptr, sz, N00B_MPROT, N00B_MFLAG, -1, 0);

    if (n00b_result_is_err(mmap_r)) {
        return n00b_result_err(void *, n00b_result_get_err(mmap_r));
    }

    void *result = n00b_result_get(mmap_r);

    (void)n00b_mmap_register(result,
                            ((char *)result) + sz,
                            kind,
                            .file      = loc,
                            .allocator = allocator);

    return n00b_result_ok(void *, result);
}

// clang-format off
n00b_result_t(int)
n00b_munmap(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
}
// clang-format on
{
    auto map_opt = n00b_mmap_by_address(addr, .runtime = runtime);

    if (!n00b_option_is_set(map_opt)) {
        return n00b_result_err(int, EINVAL);
    }

    n00b_mmap_info_t *map   = n00b_option_get(map_opt);
    void             *start = (void *)map->start;
    size_t            len   = map->end - map->start;

    n00b_mmap_ctx_t *ctx = n00b_global_mem_map(runtime);

    mmap_write_lock(ctx);
    n00b_mmap_delete_ranges(ctx, map->start, map->end);
    n00b_mmaps_remove_base(ctx, map);
    mmap_write_unlock(ctx);

#ifdef _WIN32
    VirtualFree(start, 0, MEM_RELEASE);
#else
    munmap(start, len);
#endif
    return n00b_result_ok(int, 0);
}

// clang-format off
n00b_option_t(n00b_mmap_info_t *)
n00b_mmap_by_address(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
}
// clang-format on
{
    return n00b_mmap_lookup(n00b_global_mem_map(runtime), addr);
}

n00b_allocator_opt_t
n00b_mem_get_allocator(void *addr) _kargs
{
    n00b_runtime_t *runtime = n00b_get_runtime();
}
{
    auto mmap_opt = n00b_mmap_lookup(n00b_global_mem_map(runtime), addr);

    if (!n00b_option_is_set(mmap_opt)) {
        return n00b_option_none(n00b_allocator_t *);
    }

    n00b_mmap_info_t *mmap = n00b_option_get(mmap_opt);

    if (!mmap->allocator) {
        return n00b_option_none(n00b_allocator_t *);
    }
    return n00b_option_set(n00b_allocator_t *, n00b_atomic_load(&mmap->allocator));
}

// ============================================================================
// Debug printing
// ============================================================================

void
n00b_print_mmap_tree(void)
{
    n00b_runtime_t  *rt  = n00b_get_runtime();
    n00b_mmap_ctx_t *ctx = n00b_global_mem_map(rt);

    n00b_allocator_t *alloc = (n00b_allocator_t *)&ctx->pool;
    n00b_stack_t(void *) results = n00b_stack_new(void *, alloc);

    (void)n00b_interval_search_ordered(ctx->mmap_tree, 0, UINT64_MAX, &results);

    for (size_t i = 0; i < n00b_stack_len(results); i++) {
        mmap_node_t      *node = results.data[i];
        n00b_mmap_data_t  data = node->data;

        // Skip sub-range entries in the debug dump.
        if (!n00b_variant_is_type(data, n00b_mmap_info_t *)) {
            continue;
        }

        n00b_mmap_info_t *info = n00b_variant_get(data, n00b_mmap_info_t *);
        char             *name;

        switch (info->kind) {
        case n00b_mmap_static:
            name = "Static  ";
            break;
        case n00b_mmap_arena:
            name = "Arena   ";
            break;
        case n00b_mmap_managed_segment:
            name = "Managed ";
            break;
        case n00b_mmap_sys_segment:
            name = "System  ";
            break;
        case n00b_mmap_zero_page:
            name = "No-map  ";
            break;
        case n00b_mmap_unmanaged:
            name = "mmap()  ";
            break;
        case n00b_mmap_stack:
            name = "\e[31;1;4mStack   \e[0m";
            break;
        case n00b_mmap_internal:
            name = "Internal";
            break;
        case n00b_mmap_pool:
            name = "Pool    ";
            break;
        case n00b_mmap_api_mmap:
            name = "Direct  ";
            break;
        default:
            name = "Unknown ";
            break;
        }

        n00b_fprintf(stderr,
                     "%s %p-%p %s\n",
                     name,
                     (void *)info->start,
                     (void *)info->end,
                     info->file ? info->file : "(no file)");
    }
}
