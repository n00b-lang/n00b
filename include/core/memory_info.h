#pragma once

#include "n00b.h"
#include "core/alloc_mdata.h"
#include "core/mmaps.h"
#include "core/option.h"

// This API is got getting info about memory, whether or not it is
// managed.
//
// For known managed memory, alloc_structs.h contains the core accounting
// structures for managed allocations.

typedef struct n00b_memory_scan_t {
    uint64_t *cur;
    uint64_t *end;
    uint32_t  flags;
} n00b_memory_scan_t;

// Returns the length of a "likely string". A likely string currently
// is a C-style string with a null terminator, compatable with UTF-8 encoding.
extern size_t n00b_address_is_probable_cstring(void *addr, size_t *bytelen, size_t min_len);

// Sets up a scan for a range of memory, to look for pointers.  The
// initializor aligns memory, and checks the first and last page to
// see if they're readable. It returns true if initialization is
// successful, and false if not (either because it's not memory, or
// the memory is not currently readable).
//
// The cat_flags field should be an OR-ing of:
//  - n00b_static_addr
//  - n00b_managed_heap_addr
//  - n00b_unmanaged_heap_addr
//
// Passing anything else sets all 3 flags.
//
// The user is expected to ensure the pages don't go away or change
// perms in their lifetime. Note that we do not currently bother
// checking intermediate pages.
extern bool
n00b_memory_scan_init(n00b_memory_scan_t *ctx, void *s, size_t len, uint8_t cat_flags);

// The actual scanner returns nullptr when it has run out of pointers of
// the specified kind.
//
// The second argument may be null, but if provided, it will return
// one of the three above flags depending on the type of memory, or
//  n00b_data_not_addr if no more pointers are in the range.

extern void *n00b_memory_scan_next(n00b_memory_scan_t   *ctx,
                                   n00b_mmap_rec_kind_t *tinfo,
                                   n00b_mmap_perms_t    *perms);

extern n00b_mmap_perms_t n00b_check_memory_perms(void *ptr);

#if defined(N00B_MEM_INTERNAL_API)
extern void n00b_init_memperm_pipe_cache(void);
#endif

static inline bool
n00b_memory_is_readable(void *ptr)
{
    switch (n00b_check_memory_perms(ptr)) {
    case n00b_mmap_perms_ro:
    case n00b_mmap_perms_rw:
        return true;
    default:
        return false;
    }
}

extern n00b_mmap_info_t *n00b_mmap_info_lookup(const void *addr);

n00b_option_decl(n00b_allocator_t *);

extern n00b_option_t(n00b_allocator_t *) n00b_find_allocator(void *val);
// Find all global mmap entries with an exact name match.
// TODO: replace
// extern n00b_list_t      *n00b_mmaps_by_file(char *);

static inline bool
n00b_value_is_data(void *ptr)
{
    n00b_mmap_info_t *mmap = n00b_mmap_by_address(ptr);

    return (!mmap || n00b_check_memory_perms(ptr) == n00b_mmap_perms_no_access);
}

static inline bool
n00b_value_is_likely_pointer(void *ptr)
{
    return !n00b_value_is_data(ptr);
}

// We don't consider direct calls to mmap to be in-heap. They need to
// use our arena interface.
static inline bool
n00b_in_heap(void *ptr)
{
    n00b_mmap_info_t *mmap = n00b_mmap_by_address(ptr);

    if (mmap) {
        switch (mmap->kind) {
        case n00b_mmap_managed_segment:
        case n00b_mmap_sys_segment:
        case n00b_mmap_pool:
            return true;
        default:
            break;
        }
    }
    return false;
}

#if defined(N00B_USE_INTERNAL_API)
extern uint64_t n00b_gc_guard;

static inline bool
n00b_is_mem_metadata_record(n00b_alloc_info_t *h)
{
    return !(h->guard & 1);
}
#endif

#if !defined(N00B_MEM_INTERNAL_API)
extern void n00b_load_static_ranges(void);
#endif

#if defined(N00B_SHOW_ARENAS_ON_GC)
extern void n00b_debug_memory_info(bool);
#endif

#if 0 // TODO

static inline n00b_alloc_metadata_t *
n00b_to_mem_metadata_record(n00b_alloc_info_t *info)
{
    if (!info || (info->guard & 1)) {
        return nullptr;
    }

    return (n00b_alloc_metadata_t *)info;
}
#endif

// n00b_get_arena_addr_type is defined in arena.h (needs full n00b_arena_t definition)
