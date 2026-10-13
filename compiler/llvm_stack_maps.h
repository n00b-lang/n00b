#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/object_section.h"

typedef enum {
    N00BB_LLVM_STACKMAP_REGISTER = 1,
    N00BB_LLVM_STACKMAP_DIRECT = 2,
} n00bb_llvm_stackmap_kind_t;

typedef struct {
    core_object_symbol_name_t function;
    uint64_t id;
    uint64_t stack_bytes;
    uint32_t instruction_offset;
    uint8_t kind;
    uint16_t dwarf_register;
    uint16_t size_bytes;
    int32_t frame_offset;
} n00bb_llvm_stack_location_t;

#define N00BB_LLVM_FRAME_SENTINEL_ID UINT64_MAX
#define N00BB_LLVM_FRAME_CONSERVATIVE_ID (UINT64_MAX - 1)

typedef bool (*n00bb_llvm_stack_location_fn)(void *context,
    const n00bb_llvm_stack_location_t *location);

bool n00bb_llvm_stack_maps_visit(const void *object, size_t object_bytes,
    core_target_object_format_t format, n00bb_llvm_stack_location_fn visit,
    void *context, bool *found);
