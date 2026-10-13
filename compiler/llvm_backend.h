#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ir/ir.h"
#include "ir/storage_origin.h"
#include "lib/buffer.h"
#include "lib/diagnostic.h"
#include "abi_static_graph.h"

typedef struct n00bb_llvm_backend_t n00bb_llvm_backend_t;
typedef struct n00bb_native_cache_t n00bb_native_cache_t;

enum { N00BB_COMPTIME_RESULT_FD = 3 };

typedef enum {
    N00BB_LLVM_LITERAL_ARRAY,
    N00BB_LLVM_LITERAL_STRING,
    N00BB_LLVM_LITERAL_RELOC,
    N00BB_LLVM_LITERAL_NESTED_ARRAY,
    N00BB_LLVM_LITERAL_NESTED_RELOC,
    N00BB_LLVM_LITERAL_RAW_VALUE,
    N00BB_LLVM_LITERAL_EFFECT,
    N00BB_LLVM_LITERAL_RECORD_ARRAY,
    N00BB_LLVM_LITERAL_RECORD_RELOC,
    N00BB_LLVM_LITERAL_RECORD_FIELDS_ARRAY,
    N00BB_LLVM_LITERAL_RECORD_FIELDS_RELOC,
    N00BB_LLVM_LITERAL_GRAPH,
} n00bb_llvm_literal_kind_t;

typedef struct {
    const char *symbol;
    const char *identifier;
    ir_func_t *function;
    n00bb_llvm_literal_kind_t kind;
    uint32_t element_stride;
    uint32_t inner_stride;
    uint32_t record_size;
    uint32_t array_field_offset;
    uint32_t array_field_index;
    const uint32_t *string_offsets;
    size_t string_offset_count;
    const uint8_t *byte_mask;
    bool comptime_consumer;
    bool after_initializers;
} n00bb_llvm_array_producer_t;

typedef struct {
    const char *symbol;
    uint64_t typehash;
    size_t size;
    uint32_t alignment;
    bool allocation;
    bool immutable;
} n00bb_llvm_graph_node_t;

typedef struct {
    const char *identifier;
    const uint8_t *bytes;
    size_t byte_len;
    uint64_t count;
    n00bb_llvm_literal_kind_t kind;
    uint8_t string_encoding;
    uint32_t element_stride;
    uint32_t inner_stride;
    const uint32_t *string_offsets;
    size_t string_offset_count;
    const n00bb_llvm_graph_node_t *graph_nodes;
    size_t graph_node_count;
    const n00bb_static_graph_edge_t *graph_edges;
    size_t graph_edge_count;
    size_t graph_root_index;
} n00bb_llvm_phase_array_t;

typedef struct {
    uint64_t typehash;
    uint32_t index;
    bool finalizable;
    bool array_type;
} n00bb_llvm_type_index_t;

typedef struct {
    const ir_global_t *global;
    const char *native_name;
    uint32_t storage_index;
} n00bb_llvm_global_binding_t;

typedef struct {
    uint32_t type_index;
    ir_func_t *function;
} n00bb_llvm_finalizer_binding_t;

typedef struct {
    ir_func_t *function;
    uint32_t index;
} n00bb_llvm_function_binding_t;

n00bb_llvm_backend_t *n00bb_llvm_backend_prepare(
    const ir_module_t *comptime_ir, const ir_module_t *runtime_ir,
    bool debug_enabled, bool native_cpu, core_diagnostics_t *diagnostics);
void n00bb_llvm_backend_free(n00bb_llvm_backend_t *backend);
bool n00bb_llvm_backend_bind_phase_arrays(
    n00bb_llvm_backend_t *backend, const n00bb_llvm_phase_array_t *arrays,
    size_t count);
bool n00bb_llvm_backend_bind_type_indices(
    n00bb_llvm_backend_t *backend, const n00bb_llvm_type_index_t *types,
    size_t count);
bool n00bb_llvm_backend_bind_globals(
    n00bb_llvm_backend_t *backend,
    const n00bb_llvm_global_binding_t *globals, size_t count);
bool n00bb_llvm_backend_bind_finalizers(
    n00bb_llvm_backend_t *backend,
    const n00bb_llvm_finalizer_binding_t *finalizers, size_t count);
bool n00bb_llvm_backend_bind_function_indices(
    n00bb_llvm_backend_t *backend,
    const n00bb_llvm_function_binding_t *functions, size_t count);
bool n00bb_llvm_backend_bind_reachable_functions(
    n00bb_llvm_backend_t *backend, bool comptime,
    ir_func_t *const *functions, size_t count);
bool n00bb_llvm_backend_bind_storage_origin(
    n00bb_llvm_backend_t *backend, ir_storage_origin_t *origin);
bool n00bb_llvm_backend_enable_frame_maps(n00bb_llvm_backend_t *backend);
void n00bb_llvm_backend_bind_native_cache(n00bb_llvm_backend_t *backend,
    n00bb_native_cache_t *cache);

/* Produces verified LLVM bitcode for a phase module. */
bool n00bb_llvm_backend_write_bitcode(
    n00bb_llvm_backend_t *backend, bool comptime,
    core_buffer_t *out, core_diagnostics_t *diagnostics);
bool n00bb_llvm_backend_emit_object(
    n00bb_llvm_backend_t *backend, bool comptime,
    const char *const *entry_initializers, size_t initializer_count,
    const n00bb_llvm_array_producer_t *array_producers,
    size_t array_producer_count,
    size_t metadata_bytes,
    core_buffer_t *out, core_diagnostics_t *diagnostics);
bool n00bb_llvm_backend_emit_linkable_bitcode(
    n00bb_llvm_backend_t *backend,
    const char *const *entry_initializers, size_t initializer_count,
    size_t metadata_bytes, core_buffer_t *out,
    core_diagnostics_t *diagnostics);

typedef struct {
    const void *data;
    size_t length;
    bool native_support;
} n00bb_llvm_bitcode_input_t;

bool n00bb_llvm_backend_link_bitcode(
    const n00bb_llvm_bitcode_input_t *inputs, size_t count,
    bool debug_enabled, bool native_cpu, bool emit_frame_maps,
    core_buffer_t *out, core_diagnostics_t *diagnostics);
