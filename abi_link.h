#pragma once

#include "abi_container.h"
#include "n00b-abi-v1.h"
#include "lib/link_unit.h"
#include "lib/object_section.h"

typedef struct {
    const uint8_t *bytes;
    size_t length;
} n00bb_abi_string_view_t;

typedef struct {
    const uint8_t *object;
    size_t object_bytes;
    core_object_section_t metadata;
    core_target_object_format_t format;
    n00bb_abi_image_header_t header;
    n00bb_abi_directory_entry_t *directory;
    size_t directory_count;
} n00bb_abi_object_view_t;

typedef struct {
    core_string_t module;
    const uint8_t *object;
    size_t object_bytes;
} n00bb_abi_object_input_t;

typedef struct {
    n00bb_abi_object_view_t *views;
    size_t count;
} n00bb_abi_selected_objects_t;

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
/* Reads an inline or pool-backed metadata string without applying native
 * relocations. The returned bytes borrow the object; failure leaves out
 * unchanged. */
bool n00bb_abi_object_string(const n00bb_abi_object_view_t *view,
                             const n00b_abi_string_v1 *value,
                             n00bb_abi_string_view_t *out);

/* Selects exactly one object for each TU in the finalized link graph.
 * Unrelated candidates are ignored; selected objects are checked against
 * their module, import requirements, and recursive-import fragments. Object
 * bytes are borrowed and must outlive the selection. */
bool n00bb_abi_select_objects(core_link_unit_t *link,
                              const core_target_desc_t *target,
                              const n00bb_abi_object_input_t *candidates,
                              size_t candidate_count,
                              n00bb_abi_selected_objects_t *out);
void n00bb_abi_selected_objects_release(n00bb_abi_selected_objects_t *selected);
