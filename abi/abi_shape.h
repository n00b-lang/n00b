#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "n00b-abi-v1.h"

typedef bool (*n00bb_abi_extent_reader_t)(const void *context,
    n00b_abi_range_v1 extents, uint32_t ordinal, uint64_t *out);

static inline bool
n00bb_abi_shape_valid(const n00b_abi_shape_v1 *shape,
                      n00bb_abi_extent_reader_t read_extent,
                      const void *context)
{
    if (!shape || !read_extent || !shape->rank
        || !shape->flat_rank || shape->flat_rank > shape->rank
        || shape->extents.count != shape->rank) return false;
    uint64_t total = 1;
    for (uint32_t i = 0; i < shape->rank; i++) {
        uint64_t extent = 0;
        if (!read_extent(context, shape->extents, i, &extent)) return false;
        if (i < shape->flat_rank) {
            if (extent && total > UINT64_MAX / extent) return false;
            total *= extent;
        }
        else if (extent) return false;
    }
    return total == shape->total_count;
}
