#define N00B_MEM_INTERNAL_API
#define N00B_USE_INTERNAL_API

#include <sys/mman.h>
#include <unistd.h>

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

#ifndef N00B_DEFAULT_SCRATCH_ARENA_SIZE
#define N00B_DEFAULT_SCRATCH_ARENA_SIZE (1 << 18) // 256K
#endif

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

    if (!arena->vtable.hidden) {
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
n00b_arena_alloc(n00b_arena_t *arena, uint64_t request, void *ignore)
{
    char        *found_value;
    char        *desired_value;
    _Atomic bool already_collected = false;

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

static void
n00b_arena_delete(n00b_arena_t *arena)
{
    n00b_segment_t      *segment = n00b_atomic_load(&arena->current_segment);
    n00b_segment_t      *next;
    n00b_mmap_rec_kind_t kind  = n00b_get_arena_addr_type(arena, nullptr);
    bool                 unreg = (!arena->vtable.hidden);

    if (arena->vtable.metadata_arena) {
        n00b_arena_delete((n00b_arena_t *)arena->vtable.metadata_arena);
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
n00b_initialize_arena(n00b_arena_t *arena) _kargs
{
    uint64_t    size           = N00B_DEFAULT_SCRATCH_ARENA_SIZE;
    bool        use_gc         = true;
    bool        no_map         = false;
    bool        hidden         = false;
    bool        __system       = false;
    bool        __is_md_arena  = false;
    bool        inline_headers = true;
    char       *name           = "arena";
}
{
    n00b_atomic_store(&arena->next_alloc, nullptr);
    n00b_atomic_store(&arena->mutex, 0);

    if (__is_md_arena) {
        __system       = true;
        hidden         = true;
        no_map         = true;
        use_gc         = false;
        inline_headers = false;
    }
    *arena = (n00b_arena_t){
        .segment_end        = nullptr,
        .current_segment    = nullptr,
        .next_alloc         = nullptr,
        .alloc_count        = 0,
        .mutex              = 0,
        .collection_enabled = use_gc,
#if defined(N00B_GC_STATS)
        .collect_start_time = {},
        .collect_count      = 0,
#endif
    };
    // clang-format off
    n00b_allocator_setup(
	(n00b_allocator_t *)arena,
	(n00b_calloc_fn)n00b_arena_alloc,
	.destroy           = (n00b_allocator_destroy_fn)n00b_arena_delete,
	.name              = name,
	.inline_headers    = inline_headers,
	.external_metadata = !no_map,
	.hidden            = hidden,
	.__system          = __system,
	.__is_md_arena     = __is_md_arena);
    // clang-format on

    n00b_add_arena_segment(arena, size);
}
