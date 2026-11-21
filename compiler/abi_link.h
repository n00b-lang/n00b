#pragma once

#include "abi_container.h"
#include "abi_emit_strings.h"
#include "n00b-abi-v1.h"
#include "lib/dict.h"
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
    core_object_absolute64_relocation_t *relocations;
    uint32_t relocation_count;
    bool relocations_valid;
    core_dict_t symbol_names;
    core_dict_t function_roots;
    n00bb_abi_string_view_t module_name;
    bool names_indexed;
} n00bb_abi_object_view_t;

typedef core_list_t(uint32_t) n00bb_abi_rows_t;

typedef struct {
    core_string_t module;
    const uint8_t *object;
    size_t object_bytes;
} n00bb_abi_object_input_t;

typedef struct {
    n00bb_abi_object_view_t *views;
    size_t count;
} n00bb_abi_selected_objects_t;

bool n00bb_abi_object_open(const core_target_desc_t *target,
                           const uint8_t *object, size_t object_bytes,
                           n00bb_abi_object_view_t *out);
void n00bb_abi_object_close(n00bb_abi_object_view_t *view);
bool n00bb_abi_object_index_names(n00bb_abi_object_view_t *view);
bool n00bb_abi_object_module_name(n00bb_abi_object_view_t *view,
                                  n00bb_abi_string_view_t *out);
const n00bb_abi_rows_t *n00bb_abi_symbols_named(
    const n00bb_abi_object_view_t *view, n00bb_abi_string_view_t name);
const n00bb_abi_rows_t *n00bb_abi_functions_rooted(
    const n00bb_abi_object_view_t *view, core_string_t root);
const n00bb_abi_directory_entry_t *n00bb_abi_object_table(
    const n00bb_abi_object_view_t *view, uint32_t kind);
const void *n00bb_abi_object_row(const n00bb_abi_object_view_t *view,
                                  uint32_t kind, uint32_t index);
bool n00bb_abi_object_string(const n00bb_abi_object_view_t *view,
                             const n00b_abi_string_v1 *value,
                             n00bb_abi_string_view_t *out);

bool n00bb_abi_select_objects(core_link_unit_t *link,
                              const core_target_desc_t *target,
                              const n00bb_abi_object_input_t *candidates,
                              size_t candidate_count,
                              n00bb_abi_selected_objects_t *out);
void n00bb_abi_selected_objects_release(n00bb_abi_selected_objects_t *selected);

typedef struct n00bb_abi_index_plan_t n00bb_abi_index_plan_t;

typedef struct {
    size_t input;
    uint32_t table_kind;
    uint32_t row_index;
    uint32_t owner_type_index;
    uint32_t owner_row_index;
    uint32_t type_kind;
    uint32_t type_flags;
    uint64_t typehash;
    size_t prior_input;
    uint32_t prior_row_index;
    uint32_t constraint_count;
    uint32_t prior_constraint_count;
    bool imported_type_variable;
    bool prior_imported_type_variable;
    n00bb_abi_string_view_t name;
    n00bb_abi_string_view_t prior_name;
    core_string_t reason;
} n00bb_abi_index_plan_failure_t;
n00bb_abi_index_plan_t *n00bb_abi_index_plan_build_detailed(
    const n00bb_abi_selected_objects_t *selected,
    n00bb_abi_index_plan_failure_t *failure);
void n00bb_abi_index_plan_free(n00bb_abi_index_plan_t *plan);
bool n00bb_abi_index_plan_count(const n00bb_abi_index_plan_t *plan,
                                uint32_t kind, uint32_t *out_count);
bool n00bb_abi_index_plan_map(const n00bb_abi_index_plan_t *plan,
                              size_t input_index, uint32_t kind,
                              uint32_t local_index, uint32_t *out_index);
const core_index_remap_t *n00bb_abi_index_plan_remap(
    const n00bb_abi_index_plan_t *plan, size_t input_index, uint32_t kind);

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

bool n00bb_abi_repool_row_strings(
    const n00bb_abi_object_view_t *view, uint32_t kind,
    uint32_t local_index, uint32_t final_index,
    void *out_row, size_t out_bytes, n00bb_abi_name_pool_t *pool);

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
bool n00bb_abi_rebuild_specialization_links(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_index_plan_t *plan, n00bb_abi_merged_rows_t *merged);
const n00bb_abi_merged_table_t *n00bb_abi_merged_table(
    const n00bb_abi_merged_rows_t *merged, uint32_t kind);
const void *n00bb_abi_merged_row(const n00bb_abi_merged_rows_t *merged,
                                 uint32_t kind, uint32_t index);
void n00bb_abi_merged_rows_release(n00bb_abi_merged_rows_t *merged);

typedef struct {
    size_t input;
    uint32_t local_section_index;
    uint32_t section;
    uint32_t final_section_index;
    uint64_t base_offset;
    uint64_t size;
} n00bb_abi_native_section_map_t;

typedef struct {
    core_object_emit_section_t *sections;
    uint32_t *final_indices;
    size_t count;
    n00bb_abi_native_section_map_t *maps;
    size_t map_count;
} n00bb_abi_native_sections_t;

typedef struct {
    const n00bb_abi_selected_objects_t *selected;
    n00bb_abi_name_pool_t *pool;
    uint32_t final_section_index;
} n00bb_abi_string_storage_rebase_t;

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

bool n00bb_abi_collect_native_relocations(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_native_sections_t *sections,
    n00bb_abi_native_relocations_t *out);
void n00bb_abi_native_relocations_release(
    n00bb_abi_native_relocations_t *relocations);

typedef struct {
    uint32_t table_kind;
    uint32_t row_index;
    size_t input;
    uint32_t local_section;
    core_string_t reason;
} n00bb_abi_native_rebase_failure_t;

bool n00bb_abi_rebase_native_references(
    n00bb_abi_merged_rows_t *merged,
    const n00bb_abi_native_section_map_t *maps, size_t map_count,
    const n00bb_abi_string_storage_rebase_t *string_storage,
    n00bb_abi_native_rebase_failure_t *failure);

bool n00bb_abi_add_native_bindings(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_merged_rows_t *merged,
    const n00bb_abi_native_sections_t *sections,
    n00bb_abi_native_relocations_t *symbols);

bool n00bb_abi_consolidate_native_strings(
    const core_target_desc_t *target,
    n00bb_abi_name_pool_t *pool,
    n00bb_abi_native_sections_t *sections,
    n00bb_abi_native_relocations_t *native);

typedef bool (*n00bb_abi_link_semantics_fn)(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_index_plan_t *plan,
    n00bb_abi_merged_rows_t *merged, void *context);
typedef bool (*n00bb_abi_link_validation_fn)(
    const uint8_t *metadata, size_t metadata_bytes, void *context);

bool n00bb_abi_link_objects_with_semantics(
    core_link_unit_t *link, const core_target_desc_t *target,
    const n00bb_abi_object_input_t *candidates, size_t candidate_count,
    bool keep_summaries,
    n00bb_abi_link_semantics_fn semantics, void *context,
    n00bb_abi_link_validation_fn validate, void *validation_context,
    uint8_t **out_object, size_t *out_object_bytes,
    core_string_t *failure_stage,
    n00bb_abi_index_plan_failure_t *plan_failure,
    n00bb_abi_native_rebase_failure_t *rebase_failure);
