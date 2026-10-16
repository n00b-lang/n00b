#pragma once

#include "compiler_tu.h"
#include "lib/link_unit.h"

#define N00BB_MODULE_INIT_NAME "__n00bb_module_init"
#define N00BB_LITERAL_INIT_NAME "__n00bb_literal_init"
#define N00BB_SCRIPT_BODY_NAME "__n00bb_script_body"
#define N00BB_CAPTURE_MODULE "sys.internal.marshal"
#define N00BB_CAPTURE_FUNCTION "capture_consts"
#define N00BB_START_NAME "start"
#define N00BB_COMPTIME_START_NAME "comptime_start"

ir_func_t *n00bb_root_entry(const n00bb_link_analysis_t *analysis,
                            const char *name);
ir_func_t *n00bb_baked_function(const n00bb_link_analysis_t *analysis,
                                const char *module, const char *name);
bool n00bb_dump_ir(const n00bb_link_analysis_t *analysis);

bool n00bb_atomic_native_support(ir_module_t *module, th_type *atomic_type,
                                  const core_target_desc_t *target);
bool initialize_ir_modules(ir_ctx_t *ir_context, parse_grammar_t *grammar,
                           core_link_unit_t *link,
                           n00bb_link_analysis_t *analysis,
                           uint32_t producer_stage);
bool validate_module_declarations(parse_grammar_t *grammar,
                                  core_translation_unit_t *unit,
                                  ir_ctx_t *ir_context);
bool build_reachable_functions(
    n00bb_link_analysis_t *analysis,
    core_list_t(core_translation_unit_ptr_t) *initializer_order,
    core_diagnostics_t *diagnostics);
bool bind_link_declarations_to_ir(parse_grammar_t *grammar,
                                  core_link_unit_t *link,
                                  n00bb_link_analysis_t *analysis,
                                  uint32_t producer_stage);
bool lower_requests_to_ir(parse_grammar_t *grammar, core_link_unit_t *link,
                          n00bb_link_analysis_t *analysis,
                          ir_ctx_t *ir_context,
                          core_diagnostics_t *diagnostics,
                          bool dump_ir, bool dump_control_flow,
                          bool dump_pdg, bool dump_layout);
