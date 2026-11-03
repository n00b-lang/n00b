#ifndef N00BB_COMPILER_MARKS_H
#define N00BB_COMPILER_MARKS_H

#include "ir/summary.h"
#include "parse/tree.h"
#include "lib/list.h"
#include "specialize.h"

typedef struct {
    ir_func_t *function;
    uint32_t   param;
    uint64_t   marks;
} n00bb_mark_rule_t;
core_list_decl(n00bb_mark_rule_t);

typedef struct {
    core_string_t type;
    core_string_t field;
    uint64_t      marks;
} n00bb_mark_carrier_t;
core_list_decl(n00bb_mark_carrier_t);

typedef struct {
    const ir_instr_t *call;
    ir_func_t        *caller;
    ir_func_t        *callee;
    uint32_t          param;
    uint64_t          marks;
    const ir_value_t *argument;
    const ir_instr_t *origin;
    const ir_value_t *carrier;
    bool              by_type;
    const ir_instr_t *field;
    const ir_instr_t *via;
    const ir_instr_t *sink;
} n00bb_mark_violation_t;

#define N00BB_MARK_QUERY "n00bb_mark_query#"

typedef struct {
    parse_tree_t  *node;
    ir_instr_t    *call;
    core_string_t  mark;
    uint64_t       marks;
    bool           answer;
} n00bb_mark_question_t;
core_list_decl(n00bb_mark_question_t);

typedef void (*n00bb_mark_report_fn)(void *context,
                                     const n00bb_mark_violation_t *violation);

typedef struct {
    core_list_t(n00bb_ir_func_ptr_t) functions;
    core_list_t(n00bb_mark_rule_t)   sources;
    core_list_t(n00bb_mark_rule_t)   clears;
    core_list_t(n00bb_mark_rule_t)   rejects;
    core_list_t(n00bb_mark_rule_t)   seeds;
    core_list_t(n00bb_mark_carrier_t) carriers;
    core_list_t(n00bb_mark_question_t) *questions;
    ir_summaries_t                  *summaries;
    n00bb_mark_report_fn             report;
    void                            *context;
} n00bb_marks_t;

void n00bb_check_marks(n00bb_marks_t *marks);

#endif
