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
} core_object_section_t;

/* Segment is required for Mach-O and ignored for ELF and COFF. All pointers
 * borrow the input image. A false result leaves `out` unchanged, whether the
 * object is invalid or the requested section is absent. */
bool core_object_section_find(const void *image, size_t image_size,
                              core_target_object_format_t format,
                              const char *segment, const char *name,
                              core_object_section_t *out);
