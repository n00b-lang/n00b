#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/alloc_mdata.h"
#include "core/dict_untyped.h"

#ifndef N00B_METADATA_START_ENTRIES
#define N00B_METADATA_START_ENTRIES 1 << 12
#endif

extern uint64_t n00b_gc_guard;

static inline void
n00b_alloc_add_inline_header(n00b_alloc_info_t **hdrp,
                             size_t              alloc_len,
                             char               *type,
                             bool                is_array,
                             bool                no_scan,
                             bool                mem_debug,
                             bool                mem_debug_taint)
{
    n00b_alloc_info_t *hdr = *hdrp;
    assert(alloc_len >= sizeof(n00b_alloc_info_t));

    *hdr = (n00b_alloc_info_t){
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

#define n00b_thread_checkin() // TODO: port

void *
_n00b_alloc_raw(size_t n, size_t sz, char *base_type, const char *location) _kargs
{
    n00b_allocator_t *allocator   = nullptr;
    void             *aparams     = nullptr; // Marking for debug, cleanup, etc.
    void             *iparams     = nullptr; // To be sent to an object instance (TODO)
    bool              no_scan     = false;
    bool              mem_debug   = false;
    bool              debug_taint = false;
}
{
    n00b_alloc_info_t *hdr = nullptr;
    n00b_ensure_allocator(allocator);

    if (!allocator->__system) {
        n00b_thread_checkin();
    }

    if (allocator->add_inline_header) {
        sz += N00B_ALLOC_HDR_SZ;
    }
    uint64_t request = n * sz;
    if (!request) {
        request = 1;
    }

    void *r;

    request = n00b_align(request);

    r = (*allocator->zero_alloc)(allocator, request, aparams);

    if (allocator->add_inline_header) {
        hdr = r;

        n00b_alloc_add_inline_header((n00b_alloc_info_t **)&r,
                                     request,
                                     base_type,
                                     n > 1,
                                     no_scan,
                                     mem_debug,
                                     debug_taint);
    }

    if (allocator->metadata_arena != nullptr) {
        // clang-format off
        n00b_alloc_metadata_t *map_item = _n00b_alloc_raw(
	    1,
	    sizeof(n00b_alloc_metadata_t),
	    nullptr,
	    nullptr,
	    .allocator = allocator->metadata_arena);
        // clang-format on

        *map_item = (n00b_alloc_metadata_t){
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
    // DO NOT USE for custom allocators. Skips STW check.
    bool                      __system          = false;
    bool                      __is_md_arena     = false;
}
{
    n00b_allocator_t *mda = nullptr;

    if (external_metadata) {
        mda = (n00b_allocator_t *)n00b_new_arena(.__is_md_arena = true, .name = "alloc_md");
    }

    *allocator = (n00b_allocator_t){
        .zero_alloc        = alloc,
        .free              = free,
        .destroy           = destroy,
        .debug_name        = name,
        .add_inline_header = inline_headers,
        .__system          = __system,
        .__md_arena        = __is_md_arena,
        .hidden            = hidden,
        .metadata_arena    = mda,
        .metadata          = {},
    };

    if (external_metadata) {
        n00b_dict_untyped_init(allocator->metadata,
                               .start_capacity = N00B_METADATA_START_ENTRIES,
                               .allocator      = allocator->metadata_arena,
                               .hash           = n00b_hash_word,
                               .skip_obj_hash  = true);
    }
}

void
n00b_free(void *ptr)
{
    n00b_allocator_t *allocator = n00b_mem_get_allocator(ptr);
    assert(allocator);

    if (!allocator->free) {
        return;
    }

    if (allocator->add_inline_header) {
        ptr -= (N00B_ALLOC_HDR_SZ / sizeof(void *));
    }

    (*allocator->free)(allocator, ptr);
}

#if 0

void
n00b_add_finalizer(void *obj, n00b_finalizer_t fn)
{
    n00b_mmap_info_t *obj_mmap = n00b_mmap_by_address(obj);
    assert(n00b_mmap_is_managed(obj_mmap));

    n00b_arena_t *arena = (n00b_arena_t *)n00b_atomic_load(&obj_mmap->allocator);
    if (!arena->finalizers) {
        while (n00b_atomic_or(&arena->mutex, 1))
            /* No body */;
        if (!arena->finalizers) {
            // Use the TSI arena for now, since, if this is the system
            // arena we're GCing, we don't want to depend on ourselves
            // for the list.
            arena->finalizers = n00b_list_from_arena(N00B_T_REF, &n00b_tsi_arena);
        }
        atomic_store(&arena->mutex, 0);
    }

    n00b_finalizer_info_t *info = n00b_alloc(n00b_finalizer_info_t, .allocator = (void *)arena);
    n00b_alloc_info_t *hdr = n00b_get_object_header(obj);

    assert(hdr);

    *info = (n00b_finalizer_info_t){
        .funcptr    = fn,
        .alloc_info = hdr,
        .user_ptr   = obj,
    };

    n00b_list_append(arena->finalizers, info);
}

n00b_alloc_info_t *
n00b_find_user_start_address(void *user_ptr, const void *start_ptr)
{
    uint64_t *start = (uint64_t *)start_ptr;
    uint64_t *p     = (uint64_t *)n00b_align_floor((uint64_t)user_ptr, sizeof(void *));

    while (p >= start) {
        if (*p == n00b_gc_guard) {
            return (n00b_alloc_info_t *)p;
        }
        p--;
    }

    return nullptr;
}

void *
_n00b_find_alloc_info(void *addr) _kargs
{
    n00b_mmap_info_t *info            = nullptr;
    bool              scan_for_header = false;
}
{
    // We expect to be handed the user pointer in most cases. If
    // `scan_for_header` is true, then we can be anywhere in the user
    // alloc though, but only for dynamically managed GC-able memory,
    // until we better track object ranges for static objects.

    if (!addr) {
        return nullptr;
    }

    if (!info) {
        info = n00b_mmap_by_address(addr);
    }

    if (!info) {
        return nullptr;
    }

    char *p = (char *)addr;
    void *result;
    char *scan_ptr;

    switch (info->kind) {
    case n00b_mmap_static:

        // This should change to not require a pointer to the object start.
        if (n00b_check_memory_perms(p) == n00b_mmap_perms_no_access) {
            return nullptr;
        }

        p -= sizeof(n00b_static_header_t);

        if (((uint64_t)p) < info->start) {
            return nullptr;
        }
        n00b_static_header_t *sh = (n00b_static_header_t *)p;

        // STATIC magic in truly static segments, but the gc sentinel
        // for the stack.

        if (sh->static_magic != N00B_STATIC_MAGIC && sh->static_magic != n00b_gc_guard) {
            return nullptr;
        }

        return (void *)&sh->static_magic;
    case n00b_mmap_pool:

        // TODO: put back pool.
        /*
        if (scan_for_header) {
            // Returns the user pointer.
            scan_ptr = (char *)n00b_find_user_start_address(p, (void *)info->start);

            if (scan_ptr) {
                p = scan_ptr + arena->overhead;
            }
            else {
                return nullptr;
            }
        }
        p                      = p - arena->overhead;
        n00b_alloc_info_t *hdr = (n00b_alloc_info_t *)p;

        if (((uint64_t)p) < info->start || hdr->guard != n00b_gc_guard) {
            return nullptr;
        }
        */
        return p;
    case n00b_mmap_managed_segment:
    case n00b_mmap_sys_segment:

        n00b_arena_t *arena = (n00b_arena_t *)info->allocator;

        if (scan_for_header) {
            // Returns the user pointer.
            scan_ptr = (char *)n00b_find_user_start_address(p, (void *)info->start);

            if (scan_ptr) {
                p = scan_ptr + arena->overhead;
            }
            else {
                return nullptr;
            }
        }

        if (arena->metadata) {
            // This is keyed from the user pointer.
            result = n00b_dict_untyped_get(arena->metadata, p, nullptr);

            if (!result) {
                return nullptr;
            }
        }
        else {
            p                      = p - arena->overhead;
            n00b_alloc_info_t *hdr = (n00b_alloc_info_t *)p;

            if (((uint64_t)p) < info->start || hdr->guard != n00b_gc_guard) {
                return nullptr;
            }
            result = p;
        }

        return result;
    default:
        return nullptr;
    }
}
#endif
