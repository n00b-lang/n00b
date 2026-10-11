#include "comptime_allocate.h"
#include "builtin_os.h"
#include "n00b-abi-v1.h"

#include <stdint.h>
#include <string.h>

int main(void)
{
    uint8_t *first = n00b_comptime_allocate(N00B_ABI_ALLOCATOR_GC, 7,
                                             64, 8, NULL);
    uint8_t *second = n00b_comptime_allocate(N00B_ABI_ALLOCATOR_GC, 8,
                                              32, 16, NULL);
    n00bb_comptime_allocation_t found = {0};
    if (!first || !second
        || !n00bb_comptime_allocation_containing(first, &found)
        || found.base != first || found.size != 64
        || found.type_index != 7 || found.alignment != 8
        || !n00bb_comptime_allocation_containing(first + 63, &found)
        || found.base != first
        || n00bb_comptime_allocation_containing(first + 64, &found)
        || !n00bb_comptime_allocation_containing(second + 31, &found)
        || found.base != second || found.type_index != 8
        || found.alignment != 16
        || n00bb_comptime_allocation_containing(NULL, &found)
        || n00bb_comptime_allocation_containing(first, NULL)) return 1;

    struct {
        n00b_abi_array_descriptor_header_v1 header;
        uint64_t extent;
    } descriptor = {
        .header = {.total_count = 3, .rank = 1, .flat_rank = 1},
        .extent = 3,
    };
    uint8_t *array = n00b_comptime_allocate(N00B_ABI_ALLOCATOR_GC, 9,
                                              sizeof descriptor, 8,
                                              &descriptor);
    if (!array || memcmp(array, &descriptor, sizeof descriptor)
        || !n00bb_comptime_allocation_containing(array, &found)
        || found.base != array || found.type_index != 9) return 2;

    size_t initial_capacity = n00b_os_page_size() / sizeof(found);
    for (size_t i = 0; i <= initial_capacity; i++) {
        uint8_t *item = n00b_comptime_allocate(N00B_ABI_ALLOCATOR_GC, 10,
                                                1, 1, NULL);
        if (!item) return 3;
    }
    if (!n00bb_comptime_allocation_containing(first + 63, &found)
        || found.base != first || found.type_index != 7
        || !n00bb_comptime_allocation_containing(array, &found)
        || found.base != array || found.type_index != 9)
        return 4;
    return 0;
}
