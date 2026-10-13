#pragma once

#include <stdint.h>

typedef struct {
    const void *function;
    int32_t sentinel_offset;
    uint32_t root_count;
    uint32_t first_root;
    uint32_t reserved_zero;
} n00b_native_frame_record;

const n00b_native_frame_record *n00b_builtin_abi_native_frame_at(
    const void *fp, const void *low, const void *high);
int32_t n00b_builtin_abi_native_frame_root_offset(
    const n00b_native_frame_record *frame, uint32_t ordinal);
