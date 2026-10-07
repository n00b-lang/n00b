#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/target.h"

typedef struct {
    const uint8_t *bytes;
    size_t size;
    uint32_t native_index;
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
    uint64_t section_offset;
    uint32_t native_symbol_index;
    uint64_t addend_bits;
    bool target_is_section;
} core_object_absolute64_relocation_t;

/* Segment is required for Mach-O and ignored for ELF and COFF. All pointers
 * borrow the input image. A false result leaves `out` unchanged, whether the
 * object is invalid or the requested section is absent. */
bool core_object_section_find(const void *image, size_t image_size,
                              core_target_object_format_t format,
                              const char *segment, const char *name,
                              core_object_section_t *out);

/* Decodes an absolute 64-bit pointer relocation. `native_symbol_index` is
 * the format's symbol-table index, except for a local Mach-O relocation,
 * where it is the one-based target section ordinal. Addends retain their
 * exact two's-complement bit pattern. Unsupported relocation kinds fail. */
bool core_object_absolute64_relocation_at(
    const core_object_section_t *section,
    core_target_object_format_t format, uint32_t index,
    core_object_absolute64_relocation_t *out);

/* Resolves the native symbol index from a relocation of `section`. The name
 * borrows the object image and need not be null-terminated. Local Mach-O
 * section relocations have no symbol-table index and do not use this API. */
bool core_object_symbol_name_at(const void *image, size_t image_size,
                                core_target_object_format_t format,
                                const core_object_section_t *section,
                                uint32_t native_symbol_index,
                                core_object_symbol_name_t *out);
