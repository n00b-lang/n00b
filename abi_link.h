#pragma once

#include "abi_container.h"
#include "abi_emit_strings.h"
#include "n00b-abi-v1.h"
#include "lib/indexed_table.h"
#include "lib/link_unit.h"
#include "lib/object_emit.h"
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

typedef struct n00bb_abi_index_plan_t n00bb_abi_index_plan_t;

/* Assigns one final index to each pre-remap identity and retains a checked
 * local-to-final map for every input table. Rows whose identity depends on
 * remapped references are kept distinct until the remap/merge stage. */
n00bb_abi_index_plan_t *n00bb_abi_index_plan_build(
    const n00bb_abi_selected_objects_t *selected);
void n00bb_abi_index_plan_free(n00bb_abi_index_plan_t *plan);
bool n00bb_abi_index_plan_count(const n00bb_abi_index_plan_t *plan,
                                uint32_t kind, uint32_t *out_count);
bool n00bb_abi_index_plan_map(const n00bb_abi_index_plan_t *plan,
                              size_t input_index, uint32_t kind,
                              uint32_t local_index, uint32_t *out_index);
const core_index_remap_t *n00bb_abi_index_plan_remap(
    const n00bb_abi_index_plan_t *plan, size_t input_index, uint32_t kind);

/* `values` is the merged U32 stream; `kinds` is a same-length zeroed build
 * buffer that detects conflicting interpretations of a source slot. Neither
 * buffer is owned by the remapper. On failure, discard the output buffers. */
typedef struct {
    uint32_t *values;
    uint32_t *kinds;
    size_t count;
} n00bb_abi_u32_stream_build_t;

bool n00bb_abi_remap_row(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_index_plan_t *plan, size_t input_index,
    uint32_t kind, uint32_t local_index, void *out, size_t out_bytes,
    n00bb_abi_u32_stream_build_t *stream);

/* Pre-pooling manifest rows. String values still borrow the selected input
 * object and must be relocated into the final shared string pool. */
typedef struct {
    n00b_abi_package_v1 *packages;
    uint32_t package_count;
    n00b_abi_scc_v1 *sccs;
    uint32_t scc_count;
    uint32_t *u32_stream;
    uint32_t *u32_kinds;
    uint32_t u32_count;
} n00bb_abi_manifest_rows_t;

bool n00bb_abi_rebuild_manifests(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_index_plan_t *plan,
    n00bb_abi_manifest_rows_t *out);
void n00bb_abi_manifest_rows_release(n00bb_abi_manifest_rows_t *rows);

/* Re-encodes every string-shaped column from an input row into a copied
 * final row, retaining its binary/ASCII/UTF-8 encoding. Long values gain
 * relocations addressed by final table kind and row index. */
bool n00bb_abi_repool_row_strings(
    const n00bb_abi_object_view_t *view, uint32_t kind,
    uint32_t local_index, uint32_t final_index,
    void *out_row, size_t out_bytes, n00bb_abi_name_pool_t *pool);

/* A staged, unpublishable table universe. Duplicate identity rows still
 * require structural compatibility checks, and native sections/index patches
 * have not yet been linked. All row and pool bytes are owned by this value. */
typedef struct {
    size_t input;
    uint32_t local;
    bool present;
} n00bb_abi_row_origin_t;

typedef struct {
    uint32_t kind;
    uint32_t record_count;
    uint32_t record_bytes;
    uint8_t *records;
    n00bb_abi_row_origin_t *origins;
} n00bb_abi_merged_table_t;

typedef struct {
    n00bb_abi_merged_table_t *tables;
    size_t table_count;
    n00bb_abi_name_pool_t *strings;
} n00bb_abi_merged_rows_t;

bool n00bb_abi_merge_rows(const n00bb_abi_selected_objects_t *selected,
                          const n00bb_abi_index_plan_t *plan,
                          n00bb_abi_merged_rows_t *out);
const n00bb_abi_merged_table_t *n00bb_abi_merged_table(
    const n00bb_abi_merged_rows_t *merged, uint32_t kind);
const void *n00bb_abi_merged_row(const n00bb_abi_merged_rows_t *merged,
                                 uint32_t kind, uint32_t index);
void n00bb_abi_merged_rows_release(n00bb_abi_merged_rows_t *merged);

typedef struct {
    size_t input;
    uint32_t local_section_index;
    uint32_t final_section_index;
    uint64_t base_offset;
} n00bb_abi_native_section_map_t;

typedef struct {
    core_object_emit_section_t *sections;
    n00bb_abi_native_section_map_t *maps;
    size_t count;
} n00bb_abi_native_sections_t;

/* Owns copies of selected, file-backed native sections other than each TU's
 * metadata container. Index maps name those sections in a final object whose
 * first section is the rebuilt metadata container. Pointer relocations and
 * exported symbols are transferred by the final link step, not here. */
bool n00bb_abi_collect_native_sections(
    const n00bb_abi_selected_objects_t *selected,
    const core_target_desc_t *target,
    n00bb_abi_native_sections_t *out);
void n00bb_abi_native_sections_release(n00bb_abi_native_sections_t *sections);

typedef struct {
    core_object_emit_symbol_t *symbols;
    size_t symbol_count;
    core_object_emit_absolute64_t *relocations;
    size_t relocation_count;
} n00bb_abi_native_relocations_t;

/* Transfers supported native absolute-64 relocations from collected sections.
 * Defined targets are relocated through their input's section map; undefined
 * targets retain their native name. Metadata relocations are rebuilt from the
 * merged table pool separately. Failure leaves `out` unchanged. */
bool n00bb_abi_collect_native_relocations(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_native_sections_t *sections,
    n00bb_abi_native_relocations_t *out);
void n00bb_abi_native_relocations_release(
    n00bb_abi_native_relocations_t *relocations);

/* Rebases native-section references after the final sections are placed.
 * `base_offset` is the input section's byte offset within its final section.
 * Every non-reserved STORAGE and NATIVE_BINDING row must have exactly one
 * matching map. A failure leaves all merged rows unchanged. */
bool n00bb_abi_rebase_native_references(
    n00bb_abi_merged_rows_t *merged,
    const n00bb_abi_native_section_map_t *maps, size_t map_count);

/* Adds merged native bindings after section rebasing. An existing undefined
 * reference with the same native name becomes defined; a conflicting defined
 * name fails. On failure `symbols` is unchanged. */
bool n00bb_abi_add_native_bindings(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_merged_rows_t *merged,
    const n00bb_abi_native_sections_t *sections,
    n00bb_abi_native_relocations_t *symbols);

/* Patches copied native section bytes using each input's local-to-final
 * table maps. Inputs remain unchanged. On failure the copied sections must
 * be discarded; no output object has been published. */
bool n00bb_abi_apply_link_patches(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_index_plan_t *plan,
    const n00bb_abi_merged_rows_t *merged,
    n00bb_abi_native_sections_t *sections);

/* Consolidates selected TU objects into one relocatable object. Link-only
 * requirements are removed only after resolution; unsupported patch and
 * dispatch records fail without changing the output. The object is owned
 * by the caller and is not an executable. */
bool n00bb_abi_link_selected_objects(
    const n00bb_abi_selected_objects_t *selected,
    const core_target_desc_t *target,
    uint8_t **out_object, size_t *out_object_bytes);
