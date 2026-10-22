#ifndef IR_OPERANDS_H
#define IR_OPERANDS_H

#include "ir/ir.h"
#include "lib/dict.h"

const ir_value_t *ir_value_operand(const ir_instr_t *in, uint32_t index);
const ir_value_t *ir_operand_used_value(const ir_instr_t *in, uint32_t index);
const ir_value_t *ir_value_uncopied(const ir_value_t *value);
bool              ir_block_belongs_to(const ir_func_t *f,
                                      const ir_block_t *block);
bool              ir_value_has_uses(const ir_func_t *f,
                                    const ir_value_t *value);
void              ir_func_used_values(const ir_func_t *f, core_dict_t *out);

bool              ir_op_is_comparison(ir_op_t op);
ir_op_t           ir_comparison_swapped(ir_op_t op);
ir_op_t           ir_comparison_negated(ir_op_t op);
const ir_instr_t *ir_condition_test(const ir_value_t *condition, bool *sense);
bool              ir_edge_condition(const ir_block_t *from,
                                    const ir_block_t *to,
                                    const ir_value_t **condition,
                                    bool *sense);

#endif
