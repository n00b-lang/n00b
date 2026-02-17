#pragma once

#include "n00b.h"
#include "core/alloc_mdata.h"
#include "core/mmaps.h"
#include "core/align.h"
#include "core/alloc.h"
#include "core/dict_untyped.h"

#define N00B_MPROT (PROT_READ | PROT_WRITE)
#define N00B_MFLAG (MAP_PRIVATE | MAP_ANON)

struct n00b_segment_t {
    uint64_t        size;
    n00b_segment_t *next_segment;
    char           *last_addr;
    _Alignas(N00B_ALIGN) char mem[];
};

struct n00b_arena_t {
    n00b_base_allocator_t     vtable;
    char                     *segment_end;
    _Atomic(char *)           next_alloc;
    _Atomic(n00b_segment_t *) current_segment;
    // TODO: add these back in.
    // n00b_list_t(n00b_finalizer_t) finalizers;
    _Atomic uint32_t          mutex;
    _Atomic uint32_t          alloc_count;
    // If collection_enabled is off, when a heap / arena runs out of
    // memory, we just tack on a new segment of at least the same size
    // as the prior one.
    //
    // If collection_enabled is on, then when there isn't enough room
    // left for an allocation, a GC pass will be triggered. This GC
    // pass will trace from the roots specified in the heap only, but
    // if there are no roots specified, it will trace through the
    // global roots.
    //
    // When a GC is triggered, scanned memory allocations are traced,
    // even if out of the heap being collected, to find all pointers
    // into the heap being collected, both to make sure all needed
    // records are moved, and to find all places where pointers into
    // the heap need to be updated.
    //
    // For heaps with `no_pointers` set, when we're tracing that heap,
    // then we do not scan individual records for pointers. If we're
    // tracing some other heap, then pointers into the `no_pointers`
    // heap cannot ever lead to the heap we're collecting, so we do
    // not need to record anything.
    //
    // For heaps with `system_arena` set, they cannot be used as GC'd
    // heaps, only static arenas. They are never traced during garbage
    // collection in any way, and are not returned by `n00b_in_heap()`
    // or similar. The purpose here is to be able to use the same
    // underlying arena code for low-level accounting, much of which
    // is used directly or indirectly by the garbage collector itself.

    uint32_t collection_enabled : 1; // GC'd heap.

#if defined(N00B_GC_STATS)
    struct timespec collect_start_time;
    uint32_t        collect_count;
#endif
};

static inline n00b_mmap_rec_kind_t
n00b_get_arena_addr_type(n00b_arena_t *arena, void *addr)
{
    if (addr && (void *)arena == addr) {
        return n00b_mmap_arena;
    }
    return arena->vtable.__system ? n00b_mmap_sys_segment : n00b_mmap_managed_segment;
}

extern void
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
};

extern void n00b_add_finalizer(void *obj, n00b_finalizer_t fn);

struct n00b_finalizer_info_t {
    n00b_finalizer_t   funcptr;
    n00b_inline_hdr_t *alloc_info;
    void              *user_ptr;
};

#define n00b_new_arena(...)                                                                    \
    ({                                                                                         \
        uint64_t      _sz    = n00b_page_align(sizeof(n00b_arena_t));                          \
        n00b_arena_t *result = n00b_mmap(_sz, .kind = n00b_mmap_arena);                        \
        n00b_initialize_arena(result __VA_OPT__(, __VA_ARGS__));                               \
        result;                                                                                \
    })

typedef struct n00b_arena_alloc_param_t {
    bool no_scan;
    bool mem_debug;
    bool mem_debug_taint;
} n00b_arena_alloc_param_t;
