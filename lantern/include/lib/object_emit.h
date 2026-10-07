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
    bool is_thread_local;
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

/* Map an emitter input section ordinal to the native section index written
 * into the object. A caller can use this while constructing metadata that
 * refers to sections in the same object. */
bool core_object_emit_native_section_index(
    const core_target_desc_t *target, size_t section_count,
    uint32_t input_section_index, uint32_t *native_section_index);
