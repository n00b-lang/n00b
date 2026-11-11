#ifndef IR_OPERANDS_H
#define IR_OPERANDS_H

#include "ir/ir.h"
#include "lib/dict.h"

const ir_value_t *ir_value_operand(const ir_instr_t *in, uint32_t index);
const ir_value_t *ir_operand_used_value(const ir_instr_t *in, uint32_t index);
const ir_value_t *ir_operand_referenced_value(const ir_operand_t *operand);

typedef enum : uint8_t {
    IR_ARG_ABSENT,
    IR_ARG_VALUE,
    IR_ARG_OUT,
    IR_ARG_FRESH,
    IR_ARG_MISSING,
    IR_ARG_UNMAPPED,
} ir_arg_role_t;

typedef struct {
    ir_arg_role_t     role;
    uint32_t          operand;
    const ir_value_t *value;
} ir_call_arg_t;

ir_value_t   *ir_call_callee(const ir_instr_t *call);
ir_func_t    *ir_call_function(const ir_instr_t *call);
uint32_t      ir_call_argument_count(const ir_instr_t *call);
ir_call_arg_t ir_call_argument(const ir_instr_t *call, uint32_t param);
bool          ir_call_argument_writes(const ir_call_arg_t *argument);
bool          ir_call_arguments_positional(const ir_instr_t *call,
                                           uint32_t through);
const ir_value_t *ir_value_uncopied(const ir_value_t *value);
bool              ir_value_stable(const ir_value_t *value);
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
