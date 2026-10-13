#pragma once

#include "compiler_tu.h"

typedef enum {
    N00BB_PARAM_EFFECT_UNKNOWN,
    N00BB_PARAM_EFFECT_NO_ESCAPE,
    N00BB_PARAM_EFFECT_MAY_ESCAPE,
    N00BB_PARAM_EFFECT_REFERENT_VALUE_ESCAPE,
    N00BB_PARAM_EFFECT_BOTH_ESCAPE,
} n00bb_param_effect_t;

typedef enum {
    N00BB_RETURN_ALIAS_NONE,
    N00BB_RETURN_ALIAS_PARAMETER,
    N00BB_RETURN_ALIAS_GLOBAL,
    N00BB_RETURN_ALIAS_UNKNOWN,
    N00BB_RETURN_ALIAS_FRESH_EXTERNAL,
} n00bb_return_alias_kind_t;

typedef struct {
    const uint8_t *parameter_effects;
    uint32_t parameter_count;
    n00bb_return_alias_kind_t return_kind;
    uint32_t return_parameter;
    const char *return_global;
    size_t return_global_size;
} n00bb_function_lifetime_effects_t;

bool function_lifetime_effects_for_symbol(
    n00bb_link_analysis_t *analysis, parse_sym_entry_t *function,
    n00bb_function_lifetime_effects_t *out);
bool function_returned_parameter(parse_grammar_t *grammar,
    n00bb_link_analysis_t *analysis, parse_sym_entry_t *function,
    size_t *formal_index);
void record_link_unit_call_result_aliases(n00bb_link_analysis_t *analysis);
bool record_call_argument_escape_facts(n00bb_link_analysis_t *analysis);
th_env *build_function_lifetime_environment(n00bb_link_analysis_t *analysis);
bool simple_returned_global(parse_grammar_t *grammar,
    n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
    parse_tree_t *expression,
    core_list_t(parse_sym_entry_ptr_t) *visited,
    core_buffer_t *global_name);
parse_sym_entry_t *global_symbol_from_qualified_name(
    parse_grammar_t *grammar, n00bb_link_analysis_t *analysis,
    const char *name, size_t name_size);
