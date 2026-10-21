#ifndef IR_ABCD_H
#define IR_ABCD_H

#include "ir/interval.h"
#include "ir/ir.h"

typedef struct ir_abcd_t ir_abcd_t;

ir_abcd_t *ir_abcd_build(ir_func_t *f);
ir_abcd_t *ir_func_abcd(ir_func_t *f);
void       ir_abcd_free(ir_abcd_t *abcd);

bool ir_abcd_check_redundant(ir_abcd_t *abcd, const ir_instr_t *access);
bool ir_abcd_at_most(ir_abcd_t *abcd, const ir_value_t *value,
                     const ir_value_t *bound, ir_int_t offset,
                     const ir_instr_t *at);
bool ir_abcd_at_least(ir_abcd_t *abcd, const ir_value_t *value,
                      const ir_value_t *bound, ir_int_t offset,
                      const ir_instr_t *at);
bool ir_abcd_overflow_safe(ir_abcd_t *abcd, const ir_instr_t *arithmetic);
int  ir_abcd_compare(ir_abcd_t *abcd, const ir_instr_t *comparison);
uint32_t ir_abcd_fold(ir_func_t *f);
uint32_t ir_abcd_hoist(ir_func_t *f);

#endif
