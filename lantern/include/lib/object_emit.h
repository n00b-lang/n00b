#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/string.h"
#include "lib/target.h"

typedef struct {
    core_string_t name;
    core_string_t segment;
    const uint8_t *bytes;
    size_t size;
    uint32_t alignment;
    bool writable;
    bool executable;
    bool is_thread_local;
    bool is_thread_local_descriptor;
} core_object_emit_section_t;

typedef struct {
    core_string_t name;
    uint32_t section_index;
    uint64_t section_offset;
    bool exported;
    uint64_t size;
} core_object_emit_symbol_t;

typedef struct {
    uint32_t section_index;
    uint64_t section_offset;
    uint32_t symbol_index;
    uint64_t addend_bits;
    bool symbol_relocation;
} core_object_emit_absolute64_t;

bool core_object_emit(const core_target_desc_t *target,
                      const core_object_emit_section_t *sections,
                      size_t section_count,
                      const core_object_emit_symbol_t *symbols,
                      size_t symbol_count,
                      const core_object_emit_absolute64_t *relocations,
                      size_t relocation_count,
                      uint8_t **out_image, size_t *out_size);

bool core_object_emit_native_section_index(
    const core_target_desc_t *target, size_t section_count,
    uint32_t input_section_index, uint32_t *native_section_index);
