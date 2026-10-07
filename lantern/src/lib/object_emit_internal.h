#pragma once

#include "lib/object_emit.h"

bool core_object_emit_elf(const core_target_desc_t *target,
                          const core_object_emit_section_t *sections,
                          size_t section_count,
                          const core_object_emit_symbol_t *symbols,
                          size_t symbol_count,
                          const core_object_emit_absolute64_t *relocations,
                          size_t relocation_count,
                          uint8_t **out_image, size_t *out_size);
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
