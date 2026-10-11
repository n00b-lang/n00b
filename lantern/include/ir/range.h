#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ir/diag.h"

typedef struct ir_range_analysis ir_range_analysis_t;

typedef enum {
  IR_RANGE_COMPLETE,
  IR_RANGE_PARTIAL,
  IR_RANGE_UNSATISFIABLE,
  IR_RANGE_UNSUPPORTED,
} ir_range_status_t;

/* Range propagation accepts straight-line IR and acyclic CFG paths ending in
 * return, fail, or unreachable. Branch comparisons constrain their selected
 * path, and phi values are selected by predecessor before path ranges merge.
 * The path count is bounded; loops, switches, and malformed CFGs are
 * UNSUPPORTED. It supports profile-resolved signed integer widths up to 64
 * and unsigned widths up to 63. Unsupported operations leave their result
 * unconstrained and make the result PARTIAL. Trapping integer add/sub
 * relations are modeled over the full supported value range. */
ir_range_analysis_t *ir_range_analyze_function(ir_func_t *function,
                                               ir_diags_t *diagnostics);
void ir_range_analysis_free(ir_range_analysis_t *analysis);
ir_range_status_t ir_range_analysis_status(
    const ir_range_analysis_t *analysis);
size_t ir_range_analysis_unsupported_count(
    const ir_range_analysis_t *analysis);
ir_instr_t *ir_range_analysis_failure_source(
    const ir_range_analysis_t *analysis);
bool ir_range_analysis_bounds(const ir_range_analysis_t *analysis,
                              const ir_value_t *value, int64_t *minimum,
                              int64_t *maximum);

typedef struct ir_index_proofs ir_index_proofs_t;

/* Conservative, loop-aware bounds proofs over promoted SSA. The module must
 * have IR_PROP_SSA; a false result retains the normal runtime check. */
ir_index_proofs_t *ir_index_proofs_new(ir_func_t *function);
bool ir_index_proven(const ir_index_proofs_t *proofs,
                    const ir_instr_t *index);
void ir_index_proofs_free(ir_index_proofs_t *proofs);

/* A checked inner subtraction bounds its result by its left operand.
 * This proves limit - (bounded - value) safe when bounded < limit. */
bool ir_unsigned_sub_proven_safe(const ir_instr_t *sub);
