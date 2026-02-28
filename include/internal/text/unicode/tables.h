#pragma once
/** @file tables.h
 *  @brief Internal macros and utilities for generated Unicode table lookups.
 */

#include <stddef.h>
#include <stdint.h>
#include "text/unicode/types.h"

// Two-stage table lookup: cp -> stage1[cp >> 8] * 256 + (cp & 0xFF) -> stage2[idx]
#define N00B_UNICODE_LOOKUP(stage1, stage2, cp) \
    ((stage2)[(uint32_t)(stage1)[(cp) >> 8] * 256u + ((cp) & 0xFF)])

// Binary search in sorted sparse mapping arrays.
// Returns data pointer or nullptr if not found.
static inline const uint32_t *
n00b_unicode_sparse_lookup(const uint32_t index[][2], uint32_t index_len,
                      const uint32_t *data, n00b_codepoint_t cp)
{
    if (index_len == 0) {
        return nullptr;
    }

    uint32_t lo = 0;
    uint32_t hi = index_len;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (index[mid][0] < cp) {
            lo = mid + 1;
        }
        else if (index[mid][0] > cp) {
            hi = mid;
        }
        else {
            return &data[index[mid][1]];
        }
    }

    return nullptr;
}
