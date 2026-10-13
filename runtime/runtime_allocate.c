#include "n00b-abi-v1.h"

extern void *n00b_gc_allocate(uint32_t type_index, uint64_t byte_size,
                              uint32_t alignment,
                              const void *instance_descriptor);
void *n00b_allocate(uint32_t allocator, uint32_t type_index,
                    uint64_t byte_size, uint32_t alignment,
                    const void *instance_descriptor)
{
    if (!type_index) __builtin_trap();
    if (allocator == N00B_ABI_ALLOCATOR_GC) {
        void *result = n00b_gc_allocate(type_index, byte_size, alignment,
                                        instance_descriptor);
        if (!result) __builtin_trap();
        return result;
    }
    __builtin_trap();
}
