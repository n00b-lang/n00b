#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ir/ir.h"
#include "lib/buffer.h"
#include "lib/diagnostic.h"

typedef struct n00bb_llvm_backend_t n00bb_llvm_backend_t;

enum { N00BB_COMPTIME_RESULT_FD = 3 };

typedef struct {
    const char *symbol;
    uint32_t element_stride;
} n00bb_llvm_array_producer_t;

typedef struct {
    const char *identifier;
    const uint8_t *bytes;
    uint64_t count;
    uint32_t element_stride;
} n00bb_llvm_phase_array_t;

typedef struct {
    uint64_t typehash;
    uint32_t index;
} n00bb_llvm_type_index_t;

n00bb_llvm_backend_t *n00bb_llvm_backend_prepare(
    const ir_module_t *comptime_ir, const ir_module_t *runtime_ir,
    bool debug_enabled, core_diagnostics_t *diagnostics);
void n00bb_llvm_backend_free(n00bb_llvm_backend_t *backend);
bool n00bb_llvm_backend_bind_phase_arrays(
    n00bb_llvm_backend_t *backend, const n00bb_llvm_phase_array_t *arrays,
    size_t count);
bool n00bb_llvm_backend_bind_type_indices(
    n00bb_llvm_backend_t *backend, const n00bb_llvm_type_index_t *types,
    size_t count);

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
