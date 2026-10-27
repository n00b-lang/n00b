#pragma once

#include <stdint.h>

typedef struct {
    const void *function;
    uint32_t code_bytes;
    uint32_t root_count;
    uint32_t first_root;
    int32_t record_offset;
    uint32_t flags;
    uint32_t reserved_zero;
} n00b_native_frame_record;

typedef struct {
    int32_t offset;
    uint32_t bytes;
} n00b_native_frame_root;

uint32_t n00b_builtin_abi_native_frame_count(void);
const n00b_native_frame_record *n00b_builtin_abi_native_frame(uint32_t index);
const n00b_native_frame_root *n00b_builtin_abi_native_frame_root(
    const n00b_native_frame_record *frame, uint32_t ordinal);
