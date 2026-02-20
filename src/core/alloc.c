#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/alloc_mdata.h"
#include "core/dict_untyped.h"
#include "core/mmaps.h"
#include "core/memory_info.h"
#include "core/stw.h"
#include "core/pool.h"
#include "core/runtime.h"

#ifndef N00B_METADATA_START_ENTRIES
#define N00B_METADATA_START_ENTRIES 1 << 12
#endif

extern uint64_t n00b_gc_guard;

static void n00b_run_and_remove_finalizers(void *ptr);

static inline void
n00b_alloc_add_inline_header(n00b_inline_hdr_t **hdrp,
                             size_t              alloc_len,
                             char               *type,
                             bool                is_array,
                             bool                no_scan,
                             bool                mem_debug,
                             bool                mem_debug_taint)
{
    n00b_inline_hdr_t *hdr = *hdrp;
    assert(alloc_len >= sizeof(n00b_inline_hdr_t));

    *hdr = (n00b_inline_hdr_t){
        .guard           = n00b_gc_guard,
        .tinfo           = type,
        .alloc_len       = alloc_len,
        .is_array        = is_array,
        .no_scan         = no_scan,
        .mem_debug       = mem_debug,
        .mem_debug_taint = mem_debug_taint,
    };

    *hdrp = ++hdr;
}

void *
_n00b_alloc_raw(size_t n, size_t sz, char *base_type, const char *location) _kargs
{
    n00b_allocator_t *allocator      = nullptr;
    void             *aparams        = nullptr; // Marking for debug, cleanup, etc.
    void             *iparams        = nullptr; // To be sent to an object instance (TODO)
    bool              no_scan        = false;
    bool              mem_debug      = false;
    bool              debug_taint    = false;
    n00b_finalizer_t  finalizer      = nullptr;
    void             *finalizer_data = nullptr;
}
{
    n00b_inline_hdr_t *hdr      = nullptr;
    n00b_oob_hdr_t    *map_item = nullptr;

    n00b_ensure_allocator(allocator);

    if (!allocator->__system) {
        n00b_thread_checkin();
    }

    if (allocator->add_inline_header) {
        sz += N00B_ALLOC_HDR_SZ;
    }

    uint64_t request = n * sz;
    void    *r;

    if (!request) {
        request = allocator->add_inline_header
                      ? sizeof(n00b_inline_hdr_t)
                      : 1;
    }

    request = n00b_align(request);

    r = (*allocator->zero_alloc)(allocator, request, aparams);

    if (allocator->add_inline_header) {
        hdr = r;

        n00b_alloc_add_inline_header((n00b_inline_hdr_t **)&r,
                                     request,
                                     base_type,
                                     n > 1,
                                     no_scan,
                                     mem_debug,
                                     debug_taint);
    }

    if (allocator->metadata_pool != nullptr) {
        map_item = n00b_alloc(n00b_oob_hdr_t,
                              .allocator = allocator->metadata_pool);

        *map_item = (n00b_oob_hdr_t){
            .user_ptr        = r,
            .tinfo           = base_type,
            .alloc_len       = request,
            .is_array        = n > 1,
            .no_scan         = no_scan,
            .mem_debug       = mem_debug,
            .mem_debug_taint = debug_taint,
            .hcur            = hdr,
            .file_name       = location,
        };

        n00b_dict_untyped_put(allocator->metadata, r, map_item);
        assert(n00b_dict_untyped_get(allocator->metadata, r, nullptr) == map_item);
    }

    assert(!(((uint64_t)r) & (N00B_ALIGN - 1)));
    // TODO: Add object info lookup, iff instance_params.

    if (finalizer) {
        n00b_runtime_t *rt = n00b_get_runtime();
        if (rt) {
            // The raw header key must match what the GC uses in ctx->memos:
            // - OOB arenas: (n00b_inline_hdr_t *)map_item (the OOB record)
            // - Inline-only: hdr (the actual inline header)
            n00b_inline_hdr_t *alloc_key = (allocator->metadata_pool != nullptr)
                                               ? (n00b_inline_hdr_t *)map_item
                                               : hdr;

            n00b_allocator_t      *sp   = (n00b_allocator_t *)&rt->system_pool;
            n00b_finalizer_info_t *info = n00b_alloc(n00b_finalizer_info_t,
                                                      .allocator = sp);
            *info = (n00b_finalizer_info_t){
                .funcptr    = finalizer,
                .alloc_info = alloc_key,
                .user_ptr   = finalizer_data,
            };
            n00b_list_push(rt->finalizers, info);
        }
    }

    return r;
}

void
n00b_allocator_setup(n00b_allocator_t *allocator, n00b_calloc_fn alloc) _kargs
{
    n00b_free_fn              free              = nullptr;
    n00b_allocator_destroy_fn destroy           = nullptr;
    const char               *name              = nullptr;
    bool                      inline_headers    = true;
    bool                      external_metadata = true;
    // RISKY for custom allocators. Hides from GC.
    bool                      hidden            = false;
    // DO NOT USE for custom allocators. Skips mmaps.
    bool                      __nomap           = false;
    // DO NOT USE for custom allocators. Skips STW check.
    bool                      __system          = false;
    bool                      __is_md_pool      = false;
}
{
    n00b_allocator_t *md_pool = nullptr;

    if (external_metadata) {
        md_pool = (n00b_allocator_t *)n00b_new_arena(.no_map         = true,
                                                      .__system       = true,
                                                      .hidden         = true,
                                                      .use_gc         = false,
                                                      .inline_headers = false,
                                                      .name           = "md_pool");
    }

    n00b_dict_untyped_t *md = nullptr;

    if (external_metadata) {
        md = n00b_alloc(n00b_dict_untyped_t, .allocator = md_pool);
    }

    *allocator = (n00b_allocator_t){
        .zero_alloc        = alloc,
        .free              = free,
        .destroy           = destroy,
        .debug_name        = name,
        .add_inline_header = inline_headers,
        .__system          = __system,
        .__md_pool         = __is_md_pool,
        .hidden            = hidden,
        .metadata_pool     = md_pool,
        .metadata          = md,
    };

    if (external_metadata) {
        n00b_dict_untyped_init(allocator->metadata,
                               .start_capacity = N00B_METADATA_START_ENTRIES,
                               .allocator      = allocator->metadata_pool,
                               .hash           = n00b_hash_word,
                               .skip_obj_hash  = true);
    }
}

void
n00b_free(void *ptr)
{
    n00b_run_and_remove_finalizers(ptr);

    n00b_allocator_opt_t alloc_opt = n00b_mem_get_allocator(ptr);

    if (!n00b_option_is_set(alloc_opt)) {
        return;
    }

    n00b_allocator_t *allocator = n00b_option_get(alloc_opt);

    if (!allocator->free) {
        return;
    }

    if (allocator->add_inline_header) {
        ptr = (char *)ptr - N00B_ALLOC_HDR_SZ;
    }

    (*allocator->free)(allocator, ptr);
}

void
n00b_add_finalizer(void *obj, n00b_finalizer_t fn, void *user_data)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    assert(rt);

    // Use the same raw header key that the GC stores in its memos dict.
    // For OOB arenas, this is the n00b_oob_hdr_t* cast to n00b_inline_hdr_t*.
    // For inline-only arenas, it's the actual inline header.
    n00b_alloc_info_t ainfo = n00b_find_alloc_info(obj);
    assert(ainfo.kind == n00b_alloc_oob || ainfo.kind == n00b_alloc_inline);

    n00b_inline_hdr_t *hdr = (ainfo.kind == n00b_alloc_oob)
                                  ? (n00b_inline_hdr_t *)ainfo.hdr.oob
                                  : ainfo.hdr.in_line;

    n00b_allocator_t      *sp   = (n00b_allocator_t *)&rt->system_pool;
    n00b_finalizer_info_t *info = n00b_alloc(n00b_finalizer_info_t, .allocator = sp);

    *info = (n00b_finalizer_info_t){
        .funcptr    = fn,
        .alloc_info = hdr,
        .user_ptr   = user_data,
    };

    n00b_list_push(rt->finalizers, info);
}

static void
n00b_run_and_remove_finalizers(void *ptr)
{
    if (!n00b_option_is_set(n00b_default_runtime)) {
        return;
    }

    n00b_runtime_t *rt = n00b_get_runtime();
    if (!rt || !rt->finalizers.data) {
        return;
    }

    // Use the same raw header lookup as n00b_add_finalizer.
    n00b_alloc_info_t ainfo = n00b_find_alloc_info(ptr);
    if (ainfo.kind != n00b_alloc_oob && ainfo.kind != n00b_alloc_inline) {
        return;
    }

    n00b_inline_hdr_t *hdr = (ainfo.kind == n00b_alloc_oob)
                                  ? (n00b_inline_hdr_t *)ainfo.hdr.oob
                                  : ainfo.hdr.in_line;

    // Walk backwards for safe removal via n00b_list_delete.
    size_t len = n00b_list_len(rt->finalizers);

    for (size_t i = len; i > 0; i--) {
        n00b_finalizer_info_t *entry = n00b_list_get(rt->finalizers, i - 1);

        if (entry->alloc_info == hdr) {
            entry->funcptr(entry->user_ptr);
            (void)n00b_list_delete(rt->finalizers, i - 1);
            n00b_free(entry);
        }
    }
}

void
n00b_allocator_destroy(n00b_allocator_t *allocator)
{
    if (allocator->metadata_pool) {
        n00b_allocator_destroy(allocator->metadata_pool);
        // Only free the pool struct if it was heap-allocated (i.e.,
        // it's a real pool, not an arena whose struct lives in mmap).
        if (allocator->metadata_pool->__md_pool) {
            free(allocator->metadata_pool);
        }
        allocator->metadata_pool = nullptr;
    }

    (*allocator->destroy)(allocator);
}

#define find_sentinal(p, s) _find_sentinal(((uint64_t)p), ((uint64_t *)s))

static inline char *
_find_sentinal(uint64_t p_num, uint64_t *start)
{
    uint64_t *p = (uint64_t *)n00b_align_floor(p_num, sizeof(void *));

    while (p >= start) {
        if (*p == n00b_gc_guard) {
            return (char *)p;
        }
        p--;
    }

    return nullptr;
}

// Returns the most authoritative header, unless prefer_inline is set,
// in which case it'll return the inline header.
//
// If you need to know if it's an out-of-heap pointer so you can cast it,
// then provide is_metadata_header.
//
// Returns nullptr if not found. This obviously should all change to
// return a variant, once I revisit variants.

void
_n00b_find_alloc_info(void *addr, n00b_alloc_info_t *result) _kargs
{
    bool scan_for_header = false;
}
{
    auto              mmap_opt = n00b_mmap_by_address(addr);
    char             *p        = (char *)addr;

    if (!n00b_option_is_set(mmap_opt)) {
        *result = (n00b_alloc_info_t){.kind = n00b_alloc_none};
        return;
    }

    n00b_mmap_info_t *mmap = n00b_option_get(mmap_opt);
    n00b_allocator_t *al   = mmap->allocator;
    switch (mmap->kind) {
    case n00b_mmap_static:
        if (n00b_check_memory_perms(p) == n00b_mmap_perms_no_access) {
            break;
        }

        p -= sizeof(n00b_static_header_t);
        if (((uint64_t)p) < mmap->start) {
            *result = (n00b_alloc_info_t){.kind = n00b_alloc_none};
            break;
        }
        n00b_static_header_t *sh = (n00b_static_header_t *)p;

        // STATIC magic in truly static segments, but the gc sentinel
        // for the stack.

        if (sh->static_magic != N00B_STATIC_MAGIC && sh->static_magic != n00b_gc_guard) {
            break;
        }

        *result = (n00b_alloc_info_t){
            .kind        = n00b_alloc_inline,
            .hdr.in_line = (n00b_inline_hdr_t *)&sh->static_magic,
        };
        return;
    case n00b_mmap_pool:
    case n00b_mmap_managed_segment:
    case n00b_mmap_sys_segment:

        if (al->add_inline_header) {
            p -= N00B_ALLOC_HDR_SZ;
        }

        if (scan_for_header && al->add_inline_header) {
            char *scan_ptr = find_sentinal(p, (char *)mmap->start);

            if (!scan_ptr) {
                break;
            }
        }

        if (al->metadata) {
            // Metadata dict is keyed by user pointer (addr), not by the
            // adjusted header pointer (p).
            n00b_oob_hdr_t *oob = n00b_dict_untyped_get(al->metadata, addr, nullptr);
            if (!oob) {
                *result = (n00b_alloc_info_t){.kind = n00b_alloc_err};
                return;
            }
            *result = (n00b_alloc_info_t){
                .kind    = n00b_alloc_oob,
                .hdr.oob = oob,
            };
            return;
        }

        n00b_inline_hdr_t *hdr = (n00b_inline_hdr_t *)p;

        if (((uint64_t)p) < mmap->start || hdr->guard != n00b_gc_guard) {
            *result = (n00b_alloc_info_t){.kind = n00b_alloc_err};
            return;
        }
        *result = (n00b_alloc_info_t){
            .kind        = n00b_alloc_inline,
            .hdr.in_line = hdr,
        };
        return;

    default:
        break;
    }
    *result = (n00b_alloc_info_t){.kind = n00b_alloc_none};
    return;
}

n00b_inline_hdr_opt_t
n00b_object_header(void *p)
{
    n00b_alloc_info_t info = n00b_find_alloc_info(p);

    if (!n00b_alloc_info_is_oob(info)) {
        return n00b_alloc_info_inline(info);
    }

    n00b_oob_hdr_t *oob = info.hdr.oob;
    if (!oob->hcur) {
        return n00b_option_none(n00b_inline_hdr_t *);
    }
    return n00b_option_set(n00b_inline_hdr_t *, oob->hcur);
}
