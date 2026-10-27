#include <stdatomic.h>

#include "n00b-abi-v1.h"
#include "runtime_panic.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

extern void *n00b_gc_allocate(uint32_t type_index, uint64_t byte_size,
                              uint32_t alignment,
                              const void *instance_descriptor);

static uint64_t
physical_memory_bytes(void)
{
    static _Atomic uint64_t cached;
    uint64_t bytes = atomic_load_explicit(&cached, memory_order_relaxed);
#if !defined(_WIN32)
    if (!bytes) {
        long pages = sysconf(_SC_PHYS_PAGES);
        long page_size = sysconf(_SC_PAGESIZE);
        bytes = pages > 0 && page_size > 0
            ? (uint64_t)pages * (uint64_t)page_size : UINT64_MAX;
        atomic_store_explicit(&cached, bytes, memory_order_relaxed);
    }
#endif
    return bytes ? bytes : UINT64_MAX;
}

void *n00b_allocate(uint32_t allocator, uint32_t type_index,
                    uint64_t byte_size, uint32_t alignment,
                    const void *instance_descriptor)
{
    if (!type_index)
        n00bb_runtime_fail(N00BB_FAILURE_INVARIANT,
                           N00BB_INVARIANT_UNTYPED_ALLOCATION);
    if (allocator != N00B_ABI_ALLOCATOR_GC)
        n00bb_runtime_fail(N00BB_FAILURE_INVARIANT,
                           N00BB_INVARIANT_UNKNOWN_ALLOCATOR);
    if (byte_size > physical_memory_bytes())
        n00bb_runtime_fail(N00BB_FAILURE_OUT_OF_MEMORY, byte_size);
    void *result = n00b_gc_allocate(type_index, byte_size, alignment,
                                    instance_descriptor);
    if (!result) n00bb_runtime_fail(N00BB_FAILURE_OUT_OF_MEMORY, byte_size);
    return result;
}
