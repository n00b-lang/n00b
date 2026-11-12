#pragma once

#include "compiler_tu.h"
#include "specialize.h"
#include "lib/link_unit.h"
#include "lib/target.h"

#define N00BB_MODULE_INIT_NAME "__n00bb_module_init"
#define N00BB_START_NAME "start"
#define N00BB_CONST_INIT_NAME "__n00bb_const_init"
#define N00BB_COMPTIME_START_NAME "comptime_start"
#define N00BB_COMPTIME_FREEZE_NATIVE "n00b_comptime_freeze"
#define N00BB_COMPTIME_TRANSITION_KEY "n00bb.comptime.transition"

static inline uint64_t
n00bb_comptime_module_key(core_string_t name)
{
    uint64_t key = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < name.u8_bytes; i++)
        key = (key ^ (uint8_t)name.data[i]) * 0x100000001b3ull;
    return key;
}

ir_func_t *n00bb_root_entry(const n00bb_link_analysis_t *analysis,
                            core_string_t name);
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
void warn_dead_type_arms(n00bb_link_analysis_t *analysis);
bool n00bb_const_initializer(const ir_func_t *function);
bool n00bb_static_declaration(parse_grammar_t *grammar,
                              n00bb_tu_state_t *state, parse_tree_t *node);
bool n00bb_runtime_functions(n00bb_link_analysis_t *analysis,
                             core_list_t(n00bb_ir_func_ptr_t) *out);
bool n00bb_boxed_signature_position(ir_module_t *module, th_type *type);
ir_module_t *n00bb_source_unit_module(void *context, core_string_t name);
ir_module_t *n00bb_linked_unit_module(n00bb_link_analysis_t *analysis,
                                      core_string_t name);
bool n00bb_output_target_profile(core_target_desc_t *target);
bool n00bb_cross_target(core_target_desc_t *target);
bool lower_requests_to_ir(parse_grammar_t *grammar, core_link_unit_t *link,
                          n00bb_link_analysis_t *analysis,
                          ir_ctx_t *ir_context,
                          core_diagnostics_t *diagnostics,
                          bool dump_ir, bool dump_control_flow,
                          bool dump_pdg, bool dump_summaries,
                          bool dump_layout);
bool n00bb_analyze_program(parse_grammar_t *grammar, core_link_unit_t *link,
                           n00bb_link_analysis_t *analysis,
                           bool dump_control_flow, bool dump_pdg,
                           bool dump_summaries);
bool n00bb_recompute_summaries(n00bb_link_analysis_t *analysis);
bool check_trait_implementations(n00bb_link_analysis_t *analysis);
bool check_callback_bounds(n00bb_link_analysis_t *analysis);
