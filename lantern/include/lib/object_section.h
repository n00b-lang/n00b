#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/string.h"
#include "lib/target.h"

typedef struct {
    const uint8_t *bytes;
    size_t size;
    const uint8_t *name;
    size_t name_length;
    const uint8_t *segment;
    size_t segment_length;
    uint32_t native_index;
    uint64_t virtual_address;
    uint64_t alignment;
    uint64_t flags;
    const uint8_t *relocations;
    uint32_t relocation_count;
    uint32_t relocation_entry_size;
    uint32_t machine;
    bool relocation_has_explicit_addend;
    uint32_t native_symbol_table_index;
} core_object_section_t;

typedef struct {
    const uint8_t *bytes;
    size_t length;
} core_object_symbol_name_t;

typedef struct {
    core_object_symbol_name_t name;
    uint32_t native_section_index;
    uint64_t section_offset;
    bool exported;
} core_object_symbol_t;

typedef struct {
    uint64_t section_offset;
    uint32_t native_symbol_index;
    uint64_t addend_bits;
    bool target_is_section;
} core_object_absolute64_relocation_t;

typedef struct {
    uint64_t section_offset;
    uint32_t native_symbol_index;
    uint32_t type;
    int64_t addend;
    bool target_is_section;
} core_object_relocation_t;

bool core_object_section_find(const void *image, size_t image_size,
                              core_target_object_format_t format,
                              core_string_t segment, core_string_t name,
                              core_object_section_t *out);

bool core_object_section_count(const void *image, size_t image_size,
                               core_target_object_format_t format,
                               uint32_t *out_count);
bool core_object_section_at(const void *image, size_t image_size,
                            core_target_object_format_t format,
                            uint32_t native_index,
                            core_object_section_t *out);

bool core_object_absolute64_relocation_at(
    const core_object_section_t *section,
    core_target_object_format_t format, uint32_t index,
    core_object_absolute64_relocation_t *out);

bool core_object_symbol_name_at(const void *image, size_t image_size,
                                core_target_object_format_t format,
                                const core_object_section_t *section,
                                uint32_t native_symbol_index,
                                core_object_symbol_name_t *out);

bool core_object_symbol_at(const void *image, size_t image_size,
                           core_target_object_format_t format,
                           const core_object_section_t *section,
                           uint32_t native_symbol_index,
                           core_object_symbol_t *out);

bool core_object_symbol_extent(const void *image, size_t image_size,
                               core_target_object_format_t format,
                               const core_object_section_t *section,
                               uint32_t native_symbol_index,
                               uint64_t *out_bytes);

bool core_object_relocation_at(const core_object_section_t *section,
                               core_target_object_format_t format,
                               uint32_t index,
                               core_object_relocation_t *out);

bool core_object_relocation_target(const void *image, size_t image_size,
                                   core_target_object_format_t format,
                                   const core_object_section_t *section,
                                   const core_object_relocation_t *relocation,
                                   uint32_t *out_section,
                                   uint64_t *out_offset);
