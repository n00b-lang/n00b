#ifndef IR_ANALYZE_H
#define IR_ANALYZE_H

#include "ir/diag.h"
#include "ir/ir.h"
#include "ir/summary.h"

typedef void (*ir_dead_store_fn)(const ir_instr_t *store, void *context);

bool     ir_analyze(ir_module_t *m, ir_diags_t *d);
void     ir_dead_stores(ir_func_t *f, ir_dead_store_fn report, void *context);
uint32_t ir_unused_params(const ir_func_t *f, uint32_t *out,
                          uint32_t capacity);
ir_span_t ir_func_report_span(const ir_func_t *f);
bool     ir_report_unbounded_recursion(ir_summaries_t *summaries,
                                       ir_func_t *f, ir_diags_t *d);

#endif
