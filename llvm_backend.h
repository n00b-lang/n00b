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

/* Produces bitcode for a phase module. Until IR translation is installed,
 * this is only a backend-linkage probe and is not a compiled n00b program. */
bool n00bb_llvm_backend_probe_bitcode(
    const n00bb_llvm_backend_t *backend, bool comptime,
    core_buffer_t *out, core_diagnostics_t *diagnostics);
