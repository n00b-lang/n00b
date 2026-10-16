#pragma once

#include "abi_decl_index.h"
#include "abi_emit_strings.h"
#include "abi_function_rows.h"
#include "abi_type_index.h"
#include "abi_type_rows.h"
#include "n00b-abi-v1.h"
#include "lib/buffer.h"

typedef struct {
    size_t node;
    size_t offset;
    uint32_t table;
    uint32_t kind;
    uint64_t typehash;
    const char *type_name;
    const char *module;
    const char *function;
    th_type *function_type;
} n00bb_abi_graph_slot_t;

typedef struct {
    void *context;
    size_t node_count;
    size_t edge_count;
    size_t slot_count;
    bool (*root)(void *context, const char *identifier, size_t *index);
    bool (*node)(void *context, size_t index, const uint8_t **bytes,
                 size_t *size, uint32_t *alignment, uint64_t *typehash);
    bool (*edge)(void *context, size_t index, size_t *source,
                 size_t *source_offset, size_t *target,
                 size_t *target_offset);
    uint32_t (*flags)(void *context, size_t index);
    const char *(*type_name)(void *context, size_t index);
    bool (*slot)(void *context, size_t index, n00bb_abi_graph_slot_t *out);
} n00bb_abi_graph_source_t;

typedef struct {
    uint32_t source_kind;
    uint64_t byte_offset;
    uint32_t target_kind;
    uint64_t target_offset;
} n00bb_abi_storage_relocation_t;

typedef struct {
    uint32_t source_kind;
    uint64_t byte_offset;
    uint32_t table_kind;
    uint32_t patch_kind;
    uint32_t local_index;
} n00bb_abi_storage_index_patch_t;

enum {
    N00BB_ABI_RELOC_AGGREGATE = N00B_ABI_STORAGE_TLS + 1,
    N00BB_ABI_RELOC_STRING_POOL,
};

typedef struct {
    n00b_abi_storage_v1 *rows;
    size_t row_count;
    uint32_t *stream_indexes;
    size_t stream_count;
    n00b_abi_range_v1 static_range;
    n00b_abi_range_v1 tls_range;
    core_buffer_t read_only;
    core_buffer_t writable;
    core_buffer_t tls;
    core_buffer_t aggregates;
    uint32_t read_only_alignment;
    uint32_t writable_alignment;
    uint32_t tls_alignment;
    uint32_t aggregate_alignment;
    n00b_abi_shape_v1 *shapes;
    size_t shape_count;
    uint64_t *extents;
    size_t extent_count;
    n00bb_abi_storage_relocation_t *relocations;
    size_t relocation_count;
    n00bb_abi_storage_index_patch_t *index_patches;
    size_t index_patch_count;
    n00b_abi_baked_node_v1 *baked_nodes;
    size_t baked_node_count;
} n00bb_abi_storage_rows_t;

typedef struct {
    char **paths;
    size_t count;
    uint32_t first_requirement;
} n00bb_abi_baked_functions_t;

bool n00bb_abi_graph_types_collect(
    n00bb_abi_type_index_t *index,
    const n00bb_abi_decl_index_t *declarations, ir_module_t *runtime,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    const n00bb_abi_graph_source_t *graphs);
bool n00bb_abi_graph_function_requirements(
    n00bb_abi_type_index_t *index,
    const n00bb_abi_type_identity_rows_t *types,
    const n00bb_abi_decl_index_t *declarations, ir_module_t *runtime,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    const n00bb_abi_graph_source_t *graphs, n00bb_abi_name_pool_t *pool,
    n00b_abi_requirement_v1 **requirements, size_t *requirement_count,
    n00bb_abi_baked_functions_t *out);
void n00bb_abi_baked_functions_release(n00bb_abi_baked_functions_t *baked);
bool n00bb_abi_storage_rows_build(
    n00bb_abi_type_index_t *index,
    const n00bb_abi_decl_index_t *declarations,
    const n00bb_abi_type_identity_rows_t *types,
    const n00bb_abi_function_rows_t *functions,
    const n00bb_abi_baked_functions_t *baked_functions,
    ir_module_t *runtime, n00bb_abi_name_pool_t *pool,
    uint32_t stream_base,
    n00bb_abi_declaration_lookup_fn lookup, void *lookup_context,
    const n00bb_abi_graph_source_t *graphs,
    n00bb_abi_storage_rows_t *out);
void n00bb_abi_storage_rows_release(n00bb_abi_storage_rows_t *rows);
