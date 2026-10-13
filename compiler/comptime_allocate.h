#ifndef N00BB_COMPTIME_ALLOCATE_H
#define N00BB_COMPTIME_ALLOCATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *base;
    size_t size;
    uint32_t type_index;
    uint32_t alignment;
} n00bb_comptime_allocation_t;

void *n00b_comptime_allocate(uint32_t allocator, uint32_t type_index,
                            uint64_t byte_size, uint32_t alignment,
                            const void *instance_descriptor);
bool n00bb_comptime_allocation_containing(
    const void *address, n00bb_comptime_allocation_t *out);

#endif
