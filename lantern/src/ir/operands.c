#include "ir/operands.h"

#include "ir/doms.h"

const ir_value_t *
ir_value_operand(const ir_instr_t *in, uint32_t index)
{
    return in && index < in->n_operands
        && in->operands[index].kind == IR_OPD_VALUE
        ? in->operands[index].value : nullptr;
}

const ir_value_t *
ir_operand_used_value(const ir_instr_t *in, uint32_t index)
{
    const ir_operand_t *operand = &in->operands[index];
    return operand->kind == IR_OPD_ARG ? operand->value
                                       : ir_operand_value(operand);
}

const ir_value_t *
ir_value_uncopied(const ir_value_t *value)
{
    for (uint32_t i = 0; value && value->def && i < 64; i++) {
        const ir_instr_t *definition = value->def;
        if (definition->op != IR_OP_COPY) break;
        const ir_value_t *source = ir_value_operand(definition, 0);
        if (!source || source == value) break;
        value = source;
    }
    return value;
}

bool
ir_value_stable(const ir_value_t *value)
{
    const ir_value_t *source = ir_value_uncopied(value);
    if (source && source->kind == IR_VAL_GLOBAL)
        return source->global && source->global->mutability != IR_MUT_MUTABLE;
    if (value && value->kind == IR_VAL_LOCAL)
        return value->local && value->local->mutability != IR_MUT_MUTABLE;
    return value != nullptr;
}

bool
ir_block_belongs_to(const ir_func_t *f, const ir_block_t *block)
{
    return block && block->owner == f && block->index < f->n_blocks
        && f->blocks[block->index] == block;
}

bool
ir_value_has_uses(const ir_func_t *f, const ir_value_t *value)
{
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = f->blocks[b]->instrs[i];
            for (uint32_t o = 0; o < in->n_operands; o++)
                if (ir_value_operand(in, o) == value) return true;
        }
    return false;
}

void
ir_func_used_values(const ir_func_t *f, core_dict_t *out)
{
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = f->blocks[b]->instrs[i];
            for (uint32_t o = 0; o < in->n_operands; o++) {
                const ir_value_t *value = ir_value_operand(in, o);
                if (value) core_dict_put(out, value, 1);
            }
        }
}

bool
ir_op_is_comparison(ir_op_t op)
{
    return op == IR_OP_LT || op == IR_OP_LE || op == IR_OP_GT
        || op == IR_OP_GE || op == IR_OP_EQ || op == IR_OP_NE;
}

ir_op_t
ir_comparison_swapped(ir_op_t op)
{
    switch (op) {
    case IR_OP_LT: return IR_OP_GT;
    case IR_OP_LE: return IR_OP_GE;
    case IR_OP_GT: return IR_OP_LT;
    case IR_OP_GE: return IR_OP_LE;
    default:       return op;
    }
}

ir_op_t
ir_comparison_negated(ir_op_t op)
{
    switch (op) {
    case IR_OP_LT: return IR_OP_GE;
    case IR_OP_LE: return IR_OP_GT;
    case IR_OP_GT: return IR_OP_LE;
    case IR_OP_GE: return IR_OP_LT;
    case IR_OP_EQ: return IR_OP_NE;
    case IR_OP_NE: return IR_OP_EQ;
    default:       return op;
    }
}

static bool
constant_truth(const ir_value_t *value, bool *truth)
{
    value = ir_value_uncopied(value);
    const ir_instr_t *definition = value ? value->def : nullptr;
    if (!definition || definition->op != IR_OP_CONST
        || definition->n_operands != 1
        || definition->operands[0].kind != IR_OPD_CONST
        || definition->operands[0].lit.is_float) return false;
    *truth = definition->operands[0].lit.i.mag != 0;
    return true;
}

const ir_instr_t *
ir_condition_test(const ir_value_t *condition, bool *sense)
{
    for (uint32_t depth = 0; depth < 16; depth++) {
        condition = ir_value_uncopied(condition);
        const ir_instr_t *test = condition ? condition->def : nullptr;
        if (!test) return nullptr;
        bool truth = false;
        if (test->op == IR_OP_SELECT && test->n_operands == 3
            && constant_truth(ir_value_operand(test, 0), &truth)) {
            condition = ir_value_operand(test, truth ? 1 : 2);
            continue;
        }
        if (test->op != IR_OP_NOT || test->n_operands != 1) return test;
        *sense = !*sense;
        condition = ir_value_operand(test, 0);
    }
    return nullptr;
}

bool
ir_edge_condition(const ir_block_t *from, const ir_block_t *to,
                  const ir_value_t **condition, bool *sense)
{
    ir_instr_t *end = from ? ir_block_terminator((ir_block_t *)from)
                           : nullptr;
    if (!end || end->op != IR_OP_BRANCH || end->n_operands != 3)
        return false;
    ir_block_t *yes = ir_succ_of(&end->operands[1]);
    ir_block_t *no  = ir_succ_of(&end->operands[2]);
    if (yes == no || (to != yes && to != no)) return false;
    *condition = ir_value_operand(end, 0);
    *sense     = to == yes;
    return *condition != nullptr;
}
