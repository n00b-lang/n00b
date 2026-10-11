#include "builtin_os.h"
#include "n00b-abi-v1.h"

#include <stdint.h>

void *
n00b_comptime_allocate(uint32_t allocator, uint32_t type_index,
                        uint64_t byte_size, uint32_t alignment,
                        const void *instance_descriptor)
{
    (void)instance_descriptor;
    if (allocator != N00B_ABI_ALLOCATOR_GC || !type_index
        || byte_size > SIZE_MAX) return nullptr;
    return n00b_os_map_zeroed(byte_size ? (size_t)byte_size : 1,
                              alignment);
}
