// Still need to:
//
// 5. Marshal.
// 6. Custom pointer marking.

#define N00B_MEM_INTERNAL_API
#define N00B_USE_INTERNAL_API

#ifndef N00B_METADATA_START_ENTRIES
#define N00B_METADATA_START_ENTRIES 1 << 12
#endif

#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
// IWYU pragma: no_include <sys/_pthread/_pthread_key_t.h>

#include "n00b.h"
#include "core/alloc_mdata.h"
#include "core/alloc.h"
#include "core/memory_info.h"
#include "core/mmaps.h"
#include "core/align.h"
#include "core/arena.h"
#include "core/atomic.h"
#include "core/gc.h"
#include "core/thread.h"
#include "core/random.h"
#include "core/dict_untyped.h"
// #include "core/stw.h"
// TODO: put back stw
#define n00b_stop_the_world()
#define n00b_restart_the_world()
// TODO: collect
#define n00b_collect(...)

n00b_mmap_info_t *
n00b_register_arena_segment(const void   *start,
                            const void   *end,
                            n00b_arena_t *arena,
                            const char   *file)
{
    n00b_mmap_rec_kind_t kind = n00b_get_arena_addr_type(arena, (void *)start);

    return n00b_register_mmap(start, end, file, (n00b_allocator_t *)arena, 0, 0, kind, 0, true);
}

static void
n00b_add_arena_segment(n00b_arena_t *arena, uint64_t request_len)
{
    n00b_segment_t *old_segment;
    n00b_segment_t *segment;

    // Spin lock.
    while (n00b_atomic_or(&arena->mutex, 1))
        /* No body */;

    // Check to see if someone else added a segment. If so,
    // unlock and return.
    char *next = n00b_atomic_load(&arena->next_alloc);

    if (next && next + request_len < arena->segment_end) {
        n00b_atomic_store(&arena->mutex, 0);
        return;
    }

    // Give ourselves at least a page of overhead.
    uint64_t needed = request_len + n00b_page_size + sizeof(n00b_segment_t);
    uint64_t size   = 0;

    old_segment = n00b_atomic_load(&arena->current_segment);

    if (old_segment) {
        size = old_segment->size;
        if (size < needed) {
            size = needed;
        }
    }
    else {
        size = needed;
    }

    size    = n00b_page_align(size);
    segment = mmap(nullptr, size, N00B_MPROT, N00B_MFLAG, -1, 0);

    if (segment == MAP_FAILED) {
        abort(); // out of memory.
    }

    // Save this info off for GC reporting and any sanity checking.
    if (old_segment) {
        old_segment->last_addr = n00b_atomic_load(&arena->next_alloc);
    }

    segment->size = size;

    segment->next_segment = old_segment;
    arena->next_alloc     = (char *)n00b_align((uint64_t)segment->mem);
    arena->segment_end    = ((char *)segment) + size;
    segment->last_addr    = arena->segment_end;
    n00b_atomic_store(&arena->current_segment, segment);

    if (!arena->hidden) {
        n00b_register_arena_segment(segment, arena->segment_end, arena, nullptr);
    }

    // Make the lock a full thread fence so we ensure our fields are
    // fully written before people use them.
    //
    // We don't care when they become visible within this function, but we
    // sure care about everything being visible by the time we open
    // up the mutex and other people can see us.
    n00b_atomic_fence();
    atomic_store(&arena->mutex, 0);
}

static inline bool
arena_changed(n00b_arena_t *arena, char *desired_value)
{
    if (desired_value > arena->segment_end) {
        return true;
    }
    if (desired_value < (char *)n00b_atomic_load(&arena->current_segment)) {
        return true;
    }

    return false;
}

static void *
n00b_core_alloc(n00b_arena_t *arena, uint64_t *request_ptr)
{
    uint64_t request = arena->overhead + *request_ptr;

    if (arena->startup_arena) {
        n00b_thread_checkin();
    }

    char        *found_value;
    char        *desired_value;
    _Atomic bool already_collected = false;

    request      = n00b_align(request);
    *request_ptr = request;

    do {
        found_value   = n00b_atomic_load(&arena->next_alloc);
        desired_value = found_value + request;

        if (arena_changed(arena, desired_value)) {
            if (already_collected || !arena->collection_enabled) {
                n00b_add_arena_segment(arena, request);
                continue;
            }

            n00b_stop_the_world();
            if (n00b_atomic_load(&arena->next_alloc) == found_value) {
                n00b_collect(arena);
            }
            already_collected = true;
            n00b_restart_the_world();
        }
    } while (!n00b_atomic_cas(&arena->next_alloc, &found_value, desired_value));
    n00b_atomic_add(&arena->alloc_count, 1);

    return found_value;
}

static inline bool
should_set_header(n00b_arena_t *arena)
{
    return !arena->no_inline_headers;
}

static void *
n00b_arena_alloc(n00b_arena_t                   *arena,
                 uint32_t                        num_items,
                 uint32_t                        item_sz,
                 char                           *tinfo,
                 const char                     *file,
                 const n00b_arena_alloc_param_t *param)
{
    uint64_t request = num_items * item_sz;

    if (!arena->array_arena && !request) {
        // If there's a null allocation, produce more than a header.
        request = 1;
    }

    n00b_alloc_info_t     *info   = n00b_core_alloc(arena, &request);
    char                  *result = ((char *)info) + arena->overhead;
    n00b_alloc_metadata_t *map_item;

    if (should_set_header(arena)) {
        *info = (n00b_alloc_info_t){
            .guard           = n00b_gc_guard,
            .tinfo           = tinfo,
            .alloc_len       = request,
            .is_array        = num_items > 1 ? 1 : 0,
            .no_scan         = param ? param->no_scan : false,
            .mem_debug       = param ? param->mem_debug : false,
            .mem_debug_taint = param ? param->mem_debug_taint : false,
        };
    }

    if (arena->alloc_metadata != nullptr) {
        map_item = n00b_arena_alloc(arena->linked_arena,
                                    1,
                                    sizeof(n00b_alloc_metadata_t),
                                    nullptr,
                                    __FILE__,
                                    nullptr);

        *map_item = (n00b_alloc_metadata_t){
            .user_ptr        = result,
            .tinfo           = tinfo,
            .alloc_len       = request,
            .is_array        = num_items > 1 ? 1 : 0,
            .no_scan         = param ? param->no_scan : false,
            .mem_debug       = param ? param->mem_debug : false,
            .mem_debug_taint = param ? param->mem_debug_taint : false,
            .hcur            = info,
            .file_name       = (char *)file,
        };

        n00b_dict_untyped_put(arena->alloc_metadata, result, map_item);
        assert(n00b_dict_untyped_get(arena->alloc_metadata, result, nullptr) == map_item);
    }

    return result;
}

static void
n00b_arena_delete(n00b_arena_t *arena)
{
    n00b_segment_t      *segment = n00b_atomic_load(&arena->current_segment);
    n00b_segment_t      *next;
    n00b_mmap_rec_kind_t kind  = n00b_get_arena_addr_type(arena, nullptr);
    bool                 unreg = (!arena->hidden);

    if (arena->alloc_metadata) {
        n00b_arena_delete(arena->linked_arena);
    }

    while (segment) {
        next = segment->next_segment;
        if (unreg) {
            n00b_unregister_mmap(segment);
        }
        munmap(segment, segment->size);
        segment = next;
    }

    n00b_mmap_info_t *arena_map = n00b_mmap_by_address(arena);
    if (arena_map && arena_map->kind == n00b_mmap_arena) {
        n00b_munmap(arena);
        return;
    }

    switch (kind) {
    case n00b_mmap_arena:
        n00b_munmap(arena);
        break;
    case n00b_mmap_static:
        memset(arena, 0, sizeof(n00b_arena_t));
        break;
    case n00b_mmap_managed_segment:
    case n00b_mmap_sys_segment:
        munmap(arena, n00b_page_align(sizeof(n00b_arena_t)));
        break;
    default:
        // Bad (invalid or unsafe) storage location for arena header.
        abort();
    }
}

void
n00b_add_finalizer(void *obj, n00b_finalizer_t fn)
{
#if 0
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
    // TODO: Pool allocator

    n00b_finalizer_info_t *info = n00b_alloc(n00b_finalizer_info_t, .allocator = (void *)arena);
    n00b_alloc_info_t *hdr = n00b_get_object_header(obj);

    assert(hdr);

    *info = (n00b_finalizer_info_t){
        .funcptr    = fn,
        .alloc_info = hdr,
        .user_ptr   = obj,
    };

    n00b_list_append(arena->finalizers, info);
#endif
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

        if (arena->alloc_metadata) {
            // This is keyed from the user pointer.
            result = n00b_dict_untyped_get(arena->alloc_metadata, p, nullptr);

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

void
n00b_initialize_arena(n00b_arena_t *arena) _kargs
{
    uint64_t    size     = N00B_DEFAULT_SCRATCH_ARENA_SIZE;
    bool        use_gc   = true;
    bool        system   = false;
    bool        no_map   = false;
    bool        hidden   = false;
    size_t      overhead = N00B_ALLOC_HDR_SZ;
    const char *name     = nullptr;
}
{
    n00b_atomic_store(&arena->next_alloc, nullptr);
    n00b_atomic_store(&arena->mutex, 0);

    *arena = (n00b_arena_t){
	.vtable = {
	    .zero_alloc = (n00b_calloc_fn)n00b_arena_alloc,
	    .destroy    = (n00b_allocator_destroy_fn)n00b_arena_delete,
	    .free       = nullptr,
	    .debug_name = name,
	},
        .segment_end        = nullptr,
        .current_segment    = nullptr,
        .alloc_count        = 0,
        .collection_enabled = use_gc,
        .system_arena       = system,
        .overhead           = n00b_align(overhead),
        .hidden             = hidden,
        .no_inline_headers  = false,
    };

    if (overhead < sizeof(n00b_alloc_info_t)) {
        if (no_map) {
            arena->array_arena = true;
        }
        arena->no_inline_headers = true;
    }

    if (no_map) {
        arena->alloc_metadata = nullptr;
        arena->linked_arena   = nullptr;
    }
    else {
        n00b_arena_t *mda   = n00b_new_arena(.use_gc = false,
                                             .system = true,
                                             .no_map = true,
                                             .hidden = true,
                                             .name   = "alloc_md");
        mda->linked_arena   = arena;
        arena->linked_arena = mda;

        // clang-format off
	n00b_arena_alloc_param_t params = { .no_scan = true };
	arena->alloc_metadata = n00b_alloc(n00b_dict_untyped_t,
					   .allocator = (n00b_allocator_t *)mda,
					   .aparams = &params);


	n00b_dict_untyped_init(arena->alloc_metadata,
			       .start_capacity = N00B_METADATA_START_ENTRIES,
			       .allocator      = (n00b_allocator_t *)mda,
			       .hash           = n00b_hash_word,
			       .skip_obj_hash  = true);

        // clang-format on
    }

#if defined(N00B_GC_STATS)
    arena->collect_count = 0;
#endif

    n00b_add_arena_segment(arena, size);
}
