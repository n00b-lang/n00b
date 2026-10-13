#pragma once

#include "compiler_tu.h"
#include "lib/link_unit.h"

bool n00bb_atomic_native_support(ir_module_t *module, th_type *atomic_type,
                                  const core_target_desc_t *target);
bool initialize_ir_modules(ir_ctx_t *ir_context, parse_grammar_t *grammar,
                           core_link_unit_t *link,
                           n00bb_link_analysis_t *analysis,
                           uint32_t producer_stage);
bool validate_module_declarations(parse_grammar_t *grammar,
                                  core_translation_unit_t *unit,
                                  ir_ctx_t *ir_context);
bool build_phase_reachable_functions(
    n00bb_link_analysis_t *analysis,
    core_list_t(core_translation_unit_ptr_t) *initializer_order,
    bool comptime, core_diagnostics_t *diagnostics);
bool bind_link_declarations_to_ir(parse_grammar_t *grammar,
                                  core_link_unit_t *link,
                                  n00bb_link_analysis_t *analysis,
                                  uint32_t producer_stage);
bool lower_requests_to_ir(parse_grammar_t *grammar, core_link_unit_t *link,
                          n00bb_link_analysis_t *analysis,
                          ir_ctx_t *ir_context,
                          core_diagnostics_t *diagnostics,
                          bool dump_ir, bool dump_control_flow,
                          bool dump_pdg, bool dump_layout,
                          bool interface_closure_only);
