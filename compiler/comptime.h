#pragma once

#include <stdbool.h>

#include "abi_storage.h"
#include "compiler_tu.h"
#include "specialize.h"

bool n00bb_comptime_needed(core_translation_unit_t *unit);
bool n00bb_comptime_seeds(core_translation_unit_t *unit, ir_global_t *global);
bool n00bb_comptime_run(n00bb_link_analysis_t *analysis,
                        core_translation_unit_t *unit, const char *program,
                        bool entry, const char *frozen,
                        core_diagnostics_t *diagnostics,
                        core_list_t(core_translation_unit_ptr_t) *baked,
                        bool *exited);
void n00bb_comptime_prune(n00bb_link_analysis_t *analysis,
                          core_translation_unit_t *const *units,
                          size_t count);
void n00bb_comptime_drop_declarations(n00bb_link_analysis_t *analysis);
const n00bb_abi_graph_source_t *
n00bb_comptime_graph(core_translation_unit_t *unit);
void n00bb_comptime_function_roots(n00bb_link_analysis_t *analysis,
                                   core_list_t(n00bb_ir_func_ptr_t) *roots);
