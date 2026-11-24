#pragma once

#include "lib/alloc.h"
#include "lib/object_emit.h"

bool core_object_emit_elf(const core_target_desc_t *target,
                          const core_object_emit_section_t *sections,
                          size_t section_count,
                          const core_object_emit_symbol_t *symbols,
                          size_t symbol_count,
                          const core_object_emit_absolute64_t *relocations,
                          size_t relocation_count,
                          uint8_t **out_image, size_t *out_size);
bool core_object_emit_elf_native_section_index(size_t section_count,
                                               uint32_t input_section_index,
                                               uint32_t *native_section_index);
bool core_object_emit_macho(const core_target_desc_t *target,
                            const core_object_emit_section_t *sections,
                            size_t section_count,
                            const core_object_emit_symbol_t *symbols,
                            size_t symbol_count,
                            const core_object_emit_absolute64_t *relocations,
                            size_t relocation_count,
                            uint8_t **out_image, size_t *out_size);
bool core_object_emit_coff(const core_target_desc_t *target,
                           const core_object_emit_section_t *sections,
                           size_t section_count,
                           const core_object_emit_symbol_t *symbols,
                           size_t symbol_count,
                           const core_object_emit_absolute64_t *relocations,
                           size_t relocation_count,
                           uint8_t **out_image, size_t *out_size);

static inline uint32_t *
core_object_rel_counts(const core_object_emit_absolute64_t *relocations,
                       size_t relocation_count, size_t section_count)
{
    uint32_t *counts = core_alloc_array(uint32_t, section_count);
    for (size_t i = 0; i < relocation_count; i++)
        counts[relocations[i].section_index - 1]++;
    return counts;
}

static inline bool core_object_size_add(size_t a, size_t b, size_t *out)
{
    if (b > SIZE_MAX - a) return false;
    *out = a + b;
    return true;
}

static inline bool core_object_size_mul(size_t a, size_t b, size_t *out)
{
    if (a && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}

static inline bool core_object_size_align(size_t value, size_t alignment,
                                          size_t *out)
{
    if (!alignment || (alignment & (alignment - 1)) ||
        alignment - 1 > SIZE_MAX - value) return false;
    *out = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

static inline bool core_object_place_table(size_t *offset, size_t count,
                                           size_t width, size_t *at)
{
    size_t bytes;
    if (!core_object_size_mul(count, width, &bytes) ||
        !core_object_size_align(*offset, 4, offset) ||
        *offset > UINT32_MAX) return false;
    *at = *offset;
    return core_object_size_add(*offset, bytes, offset);
}
