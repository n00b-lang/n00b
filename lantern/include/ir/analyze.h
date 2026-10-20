#ifndef IR_ANALYZE_H
#define IR_ANALYZE_H

#include "ir/diag.h"
#include "ir/ir.h"

bool     ir_analyze(ir_module_t *m, ir_diags_t *d);
uint32_t ir_dead_stores(ir_func_t *f, const ir_instr_t **out,
                        uint32_t capacity);

#endif
