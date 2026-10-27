#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ir/ir.h"
#include "ir/storage_origin.h"
#include "ir/summary.h"
#include "lib/buffer.h"
#include "diag/diagnostics.h"
#include "lib/target.h"

typedef struct n00bb_llvm_backend_t n00bb_llvm_backend_t;
typedef struct n00bb_native_cache_t n00bb_native_cache_t;

typedef struct {
    uint64_t typehash;
    uint32_t index;
    bool finalizable;
    bool array_type;
} n00bb_llvm_type_index_t;

typedef struct {
    const ir_global_t *global;
    core_string_t native_name;
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

typedef struct {
    core_string_t symbol;
    const uint64_t *type_hashes;
    size_t type_count;
    ir_func_t *const *functions;
    size_t function_count;
} n00bb_llvm_index_table_t;

n00bb_llvm_backend_t *n00bb_llvm_backend_prepare(const ir_module_t *ir,
    bool debug_enabled, bool native_cpu, core_diagnostics_t *diagnostics);
void n00bb_llvm_backend_free(n00bb_llvm_backend_t *backend);
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
bool n00bb_llvm_backend_prepare_object(n00bb_llvm_backend_t *backend,
    const core_string_t *entry_initializers, size_t initializer_count,
    size_t metadata_bytes, core_diagnostics_t *diagnostics);
bool n00bb_llvm_backend_finish_object(n00bb_llvm_backend_t *backend,
    core_buffer_t *out, core_diagnostics_t *diagnostics);
typedef struct n00bb_llvm_callback_bindings_t n00bb_llvm_callback_bindings_t;
n00bb_llvm_callback_bindings_t *n00bb_llvm_callback_bindings_new(
    ir_func_t *const *functions, size_t count);
void n00bb_llvm_callback_bindings_free(
    n00bb_llvm_callback_bindings_t *bindings);
bool n00bb_llvm_backend_bind_reachable_functions(
    n00bb_llvm_backend_t *backend, ir_func_t *const *functions, size_t count,
    const n00bb_llvm_callback_bindings_t *bindings);
bool n00bb_llvm_backend_bind_storage_origin(
    n00bb_llvm_backend_t *backend, ir_storage_origin_t *origin);
bool n00bb_llvm_backend_enable_frame_maps(n00bb_llvm_backend_t *backend);
void n00bb_llvm_backend_enable_panic_trap(n00bb_llvm_backend_t *backend);
void n00bb_llvm_backend_bind_native_cache(n00bb_llvm_backend_t *backend,
    n00bb_native_cache_t *cache);

uint32_t n00bb_llvm_mutator_flags(const ir_func_t *function, uint32_t flags);
void n00bb_llvm_backend_bind_summaries(n00bb_llvm_backend_t *backend,
    ir_summaries_t *summaries);

bool n00bb_llvm_backend_write_bitcode(
    n00bb_llvm_backend_t *backend, core_buffer_t *out,
    core_diagnostics_t *diagnostics);
bool n00bb_llvm_backend_bind_index_table(n00bb_llvm_backend_t *backend,
    const n00bb_llvm_index_table_t *table);
bool n00bb_llvm_backend_emit_object(
    n00bb_llvm_backend_t *backend,
    const core_string_t *entry_initializers, size_t initializer_count,
    size_t metadata_bytes,
    core_buffer_t *out, core_diagnostics_t *diagnostics);
bool n00bb_llvm_backend_emit_linkable_bitcode(
    n00bb_llvm_backend_t *backend,
    const core_string_t *entry_initializers, size_t initializer_count,
    size_t metadata_bytes, core_buffer_t *out,
    core_diagnostics_t *diagnostics);

typedef struct {
    const void *data;
    size_t length;
    bool native_support;
} n00bb_llvm_bitcode_input_t;

char *n00bb_llvm_target_triple(core_string_t profile);
void n00bb_llvm_set_profiling(bool enabled);
bool n00bb_llvm_backend_link_bitcode(
    const n00bb_llvm_bitcode_input_t *inputs, size_t count,
    bool debug_enabled, bool native_cpu, bool emit_frame_maps,
    core_buffer_t *out, core_diagnostics_t *diagnostics);
