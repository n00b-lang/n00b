#pragma once

#include "abi_container.h"
#include "lib/object_section.h"

typedef struct {
    const uint8_t *object;
    size_t object_bytes;
    core_object_section_t metadata;
    n00bb_abi_image_header_t header;
    n00bb_abi_directory_entry_t *directory;
    size_t directory_count;
} n00bb_abi_object_view_t;

/* The object and table bytes are borrowed; only the decoded directory is
 * owned by the view. An unsuccessful open leaves `out` unchanged. */
bool n00bb_abi_object_open(const core_target_desc_t *target,
                           const uint8_t *object, size_t object_bytes,
                           n00bb_abi_object_view_t *out);
void n00bb_abi_object_close(n00bb_abi_object_view_t *view);
const n00bb_abi_directory_entry_t *n00bb_abi_object_table(
    const n00bb_abi_object_view_t *view, uint32_t kind);
const void *n00bb_abi_object_row(const n00bb_abi_object_view_t *view,
                                  uint32_t kind, uint32_t index);
