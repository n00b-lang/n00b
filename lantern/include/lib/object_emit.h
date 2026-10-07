#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/target.h"

typedef struct {
    const char *name;
    const char *segment;
    const uint8_t *bytes;
    size_t size;
    uint32_t alignment;
    /* Mach-O final protection follows `segment`, not this flag. */
    bool writable;
    bool executable;
} core_object_emit_section_t;

typedef struct {
    const char *name;
    uint32_t section_index;
    uint64_t section_offset;
    bool exported;
} core_object_emit_symbol_t;

typedef struct {
    uint32_t section_index;
    uint64_t section_offset;
    uint32_t symbol_index;
    uint64_t addend_bits;
} core_object_emit_absolute64_t;

/* Section indexes are one-based; zero denotes an undefined symbol. Symbol
 * indexes in relocations are zero-based positions in `symbols`. The returned
 * image is allocated by Lantern and released with core_free(). On failure,
 * the output arguments are unchanged. */
bool core_object_emit(const core_target_desc_t *target,
                      const core_object_emit_section_t *sections,
                      size_t section_count,
                      const core_object_emit_symbol_t *symbols,
                      size_t symbol_count,
                      const core_object_emit_absolute64_t *relocations,
                      size_t relocation_count,
                      uint8_t **out_image, size_t *out_size);
