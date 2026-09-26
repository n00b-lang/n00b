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

/* Range propagation accepts straight-line single-block IR (including
 * unterminated fragments) and multi-block paths made of unconditional jumps
 * ending in return. It supports signed integer widths up to 64 and unsigned
 * widths up to 63. Unsupported operations remain unconstrained and make the
 * result PARTIAL; branches, loops, and malformed multi-block paths make the
 * result UNSUPPORTED. Trapping integer add/sub relations are modeled over the
 * full supported value range. */
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
