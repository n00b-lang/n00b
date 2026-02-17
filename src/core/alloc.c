#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/alloc_mdata.h"
#include "core/dict_untyped.h"
#include "core/mmaps.h"
#include "core/memory_info.h"

#ifndef N00B_METADATA_START_ENTRIES
#define N00B_METADATA_START_ENTRIES 1 << 12
#endif

extern uint64_t n00b_gc_guard;

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
    n00b_inline_hdr_t *hdr = nullptr;
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
        request = 1;
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

    if (allocator->metadata_arena != nullptr) {
        // clang-format off
        n00b_oob_hdr_t *map_item = _n00b_alloc_raw(
	    1,
	    sizeof(n00b_oob_hdr_t),
	    nullptr,
	    nullptr,
	    .allocator = allocator->metadata_arena);
        // clang-format on

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
    n00b_allocator_opt_t alloc_opt = n00b_mem_get_allocator(ptr);
    assert(n00b_option_is_set(alloc_opt));
    n00b_allocator_t *allocator = n00b_option_get(alloc_opt);

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
    n00b_inline_hdr_t *hdr = n00b_object_header(obj);

    assert(hdr);

    *info = (n00b_finalizer_info_t){
        .funcptr    = fn,
        .alloc_info = hdr,
        .user_ptr   = obj,
    };

    n00b_list_append(arena->finalizers, info);
}

#endif

void
n00b_allocator_destroy(n00b_allocator_t *allocator)
{
    if (allocator->metadata_arena) {
        n00b_allocator_destroy(allocator->metadata_arena);
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
    n00b_mmap_info_t *mmap = n00b_mmap_by_address(addr);
    char             *p    = (char *)addr;
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
            n00b_oob_hdr_t *oob = n00b_dict_untyped_get(al->metadata, p, nullptr);
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
