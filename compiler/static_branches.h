#pragma once

#include "ir/ir.h"
#include "lib/list.h"

#define N00BB_EXIT_VERDICT_KEY "n00bb.exit-verdict"

th_type *n00bb_type_test_source(th_ctx *types, ir_value_t *value);
int n00bb_static_type_test(const ir_instr_t *test);
bool n00bb_fold_static_branches(ir_func_t *function,
                                core_list_t(uint32_t) *outcomes);
void n00bb_drop_unreachable_blocks(ir_func_t *function);
void n00bb_fold_exit_verdicts(ir_func_t *function);
