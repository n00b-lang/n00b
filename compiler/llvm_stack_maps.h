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
    core_object_symbol_name_t name;
    uint64_t stack_bytes;
    uint64_t code_bytes;
    uint32_t section;
    uint64_t offset;
} n00bb_llvm_stack_function_t;

typedef struct {
    const n00bb_llvm_stack_function_t *function;
    uint64_t id;
    uint32_t instruction_offset;
    uint8_t kind;
    uint16_t dwarf_register;
    uint16_t size_bytes;
    int32_t frame_offset;
} n00bb_llvm_stack_location_t;

#define N00BB_LLVM_FRAME_CONSERVATIVE_ID (UINT64_MAX - 1)

typedef struct {
    bool (*function)(void *context,
                     const n00bb_llvm_stack_function_t *function);
    bool (*location)(void *context,
                     const n00bb_llvm_stack_location_t *location);
    void *context;
} n00bb_llvm_stack_visitor_t;

bool n00bb_llvm_stack_maps_visit(const void *object, size_t object_bytes,
    core_target_object_format_t format,
    const n00bb_llvm_stack_visitor_t *visitor, bool *found);
