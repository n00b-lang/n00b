#pragma once

#include "compiler_tu.h"

typedef struct {
    parse_tree_t *argument;
    parse_tree_t *site;
    bool by_name;
} n00bb_call_slot_t;

typedef enum {
    N00BB_CALL_PLAN_OK,
    N00BB_CALL_PLAN_TOO_MANY,
    N00BB_CALL_PLAN_UNKNOWN_NAME,
    N00BB_CALL_PLAN_VARARGS_BY_NAME,
    N00BB_CALL_PLAN_REPEATED,
    N00BB_CALL_PLAN_MIXED_STYLES,
    N00BB_CALL_PLAN_MISSING,
} n00bb_call_plan_status_t;

typedef struct {
    parse_grammar_t *grammar;
    parse_sym_entry_t *callee;
    parse_tree_t *formals;
    th_type *signature;
    size_t offset;
    size_t parameter_count;
    size_t slot_count;
    n00bb_call_slot_t *slots;
    bool *optional;
    size_t *argument_slots;
    core_list_t(n00bb_tree_ptr_t) positional;
    core_list_t(n00bb_tree_ptr_t) keywords;
    core_list_t(n00bb_tree_ptr_t) rest;
    n00bb_call_plan_status_t status;
    parse_tree_t *problem;
    size_t problem_index;
} n00bb_call_plan_t;

bool n00bb_call_plan_init(n00bb_call_plan_t *plan, parse_grammar_t *grammar,
                          parse_sym_entry_t *callee, th_type *signature,
                          size_t offset);
void n00bb_call_plan_collect(n00bb_call_plan_t *plan, parse_tree_t *call_args);
bool n00bb_call_plan_bind(n00bb_call_plan_t *plan);
bool n00bb_plan_call(n00bb_call_plan_t *plan, parse_grammar_t *grammar,
                     parse_sym_entry_t *callee, th_type *signature,
                     parse_tree_t *call_args, size_t offset);
void n00bb_call_plan_free(n00bb_call_plan_t *plan);
bool n00bb_call_plan_optional(const n00bb_call_plan_t *plan, size_t index);
core_string_t n00bb_call_plan_parameter_name(const n00bb_call_plan_t *plan,
                                             size_t index);
core_string_t n00bb_call_plan_rest_name(const n00bb_call_plan_t *plan);
th_type *n00bb_call_plan_parameter_type(const n00bb_call_plan_t *plan,
                                        size_t index);
size_t n00bb_call_plan_argument_count(const n00bb_call_plan_t *plan);
parse_tree_t *n00bb_call_plan_argument(const n00bb_call_plan_t *plan,
                                       size_t position, size_t *index);
parse_tree_t *n00bb_call_plan_parameter_argument(const n00bb_call_plan_t *plan,
                                                 size_t index);
th_type *n00bb_call_plan_formal_type(const n00bb_call_plan_t *plan,
                                     size_t index);
void n00bb_call_plan_report(const n00bb_call_plan_t *plan,
                            const core_translation_unit_t *unit,
                            core_diagnostics_t *diagnostics,
                            parse_tree_t *site);
