#pragma once

#include <stdbool.h>

#include "ir/ir.h"
#include "lib/buffer.h"
#include "lib/diagnostic.h"

typedef struct n00bb_llvm_backend_t n00bb_llvm_backend_t;

n00bb_llvm_backend_t *n00bb_llvm_backend_prepare(
    const ir_module_t *comptime_ir, const ir_module_t *runtime_ir,
    bool debug_enabled, core_diagnostics_t *diagnostics);
void n00bb_llvm_backend_free(n00bb_llvm_backend_t *backend);

/* Produces verified LLVM bitcode for a phase module. */
bool n00bb_llvm_backend_write_bitcode(
    n00bb_llvm_backend_t *backend, bool comptime,
    core_buffer_t *out, core_diagnostics_t *diagnostics);
bool n00bb_llvm_backend_emit_object(
    n00bb_llvm_backend_t *backend, bool comptime,
    const char *const *entry_initializers, size_t initializer_count,
    size_t metadata_bytes,
    core_buffer_t *out, core_diagnostics_t *diagnostics);
