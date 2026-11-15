#ifndef IR_ANALYZE_H
#define IR_ANALYZE_H

#include "ir/diag.h"
#include "ir/ir.h"
#include "ir/summary.h"

typedef void (*ir_dead_store_fn)(const ir_instr_t *store, void *context);
// `at` reads `path` (the local's name, then the fields leading to what
// isn't set, such as `r.b`) on a path where it has no value.
typedef void (*ir_unset_read_fn)(void *context, const ir_func_t *f,
                                 const ir_local_t *local, const ir_instr_t *at,
                                 core_string_t path);

bool     ir_analyze(ir_module_t *m, ir_diags_t *d);
void     ir_dead_stores(ir_func_t *f, ir_dead_store_fn report, void *context);
// Reports the first read of each address-taken local that may not be set.
// A call handed the local's address reads it unless the callee's summary
// says it never reads what the parameter held, and sets what the callee
// must write.
void     ir_unset_locals(ir_summaries_t *summaries, ir_func_t *f,
                         ir_unset_read_fn report, void *context);
uint32_t ir_unused_params(const ir_func_t *f, uint32_t *out,
                          uint32_t capacity);
ir_span_t ir_func_report_span(const ir_func_t *f);
bool     ir_report_unbounded_recursion(ir_summaries_t *summaries,
                                       ir_func_t *f, ir_diags_t *d);

#endif
