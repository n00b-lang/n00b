#pragma once

#include "ir/ir.h"

#define N00BB_TYPE_TEST_OUTCOMES_KEY "n00bb.type_test_outcomes"
// The type test on what an exit block's `__exit` returned: only a `bool`
// can drop a failure.
#define N00BB_EXIT_VERDICT_KEY "n00bb.exit-verdict"

int n00bb_static_type_test(const ir_instr_t *test);
uint64_t n00bb_type_test_outcomes(const ir_instr_t *test);
bool n00bb_instantiated(const ir_func_t *function);
bool n00bb_fold_static_branches(ir_func_t *function, ir_func_t *origin,
                                ir_ctx_t *context);
void n00bb_drop_unreachable_blocks(ir_func_t *function);
// Takes the branch an exit block's drop test decides once typing knows what
// `__exit` returns, so the analyses after typing see only the paths that
// can run. Other type tests are left for the analyses to judge.
void n00bb_fold_exit_verdicts(ir_func_t *function);
