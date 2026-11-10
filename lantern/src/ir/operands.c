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
ir_operand_referenced_value(const ir_operand_t *operand)
{
    switch (operand->kind) {
    case IR_OPD_ARG:
    case IR_OPD_OUT:
        return operand->value;
    default:
        return ir_operand_value(operand);
    }
}

ir_value_t *
ir_call_callee(const ir_instr_t *call)
{
    return call && call->op == IR_OP_CALL && call->n_operands
        ? ir_operand_value(&call->operands[0]) : nullptr;
}

ir_func_t *
ir_call_function(const ir_instr_t *call)
{
    ir_value_t *callee = ir_call_callee(call);
    return callee && callee->kind == IR_VAL_FUNC ? callee->owner : nullptr;
}

uint32_t
ir_call_argument_count(const ir_instr_t *call)
{
    return call && call->n_operands ? call->n_operands - 1 : 0;
}

ir_call_arg_t
ir_call_argument(const ir_instr_t *call, uint32_t param)
{
    uint32_t operand = param + 1;
    if (!call || operand >= call->n_operands)
        return (ir_call_arg_t){.role = IR_ARG_ABSENT, .operand = operand};
    const ir_operand_t *o = &call->operands[operand];
    ir_arg_role_t role = IR_ARG_UNMAPPED;
    switch (o->kind) {
    case IR_OPD_VALUE:
    case IR_OPD_ARG:
        role = IR_ARG_VALUE;
        break;
    case IR_OPD_OUT:
        role = IR_ARG_OUT;
        break;
    case IR_OPD_FRESH:
        role = IR_ARG_FRESH;
        break;
    case IR_OPD_MISSING:
        role = IR_ARG_MISSING;
        break;
    default:
        break;
    }
    return (ir_call_arg_t){.role = role, .operand = operand,
                           .value = ir_operand_referenced_value(o)};
}

bool
ir_call_argument_writes(const ir_call_arg_t *argument)
{
    return argument->role == IR_ARG_OUT || argument->role == IR_ARG_FRESH;
}

bool
ir_call_arguments_positional(const ir_instr_t *call, uint32_t through)
{
    for (uint32_t param = 0; param <= through; param++) {
        ir_call_arg_t argument = ir_call_argument(call, param);
        if (argument.role != IR_ARG_VALUE
            && !ir_call_argument_writes(&argument))
            return false;
    }
    return true;
}

static bool
mutable_storage(const ir_value_t *value)
{
    if (value->kind == IR_VAL_LOCAL)
        return !value->local || value->local->mutability == IR_MUT_MUTABLE;
    return value->kind == IR_VAL_GLOBAL
        && (!value->global || value->global->mutability == IR_MUT_MUTABLE);
}

static const ir_value_t *
copied_from(const ir_value_t *value)
{
    const ir_instr_t *definition = value->def;
    if (!definition || definition->op != IR_OP_COPY) return nullptr;
    const ir_value_t *source = ir_value_operand(definition, 0);
    return source == value ? nullptr : source;
}

const ir_value_t *
ir_value_uncopied(const ir_value_t *value)
{
    for (uint32_t i = 0; value && i < 64; i++) {
        const ir_value_t *source = copied_from(value);
        if (!source || mutable_storage(source)) break;
        value = source;
    }
    return value;
}

bool
ir_value_stable(const ir_value_t *value)
{
    const ir_value_t *source = ir_value_uncopied(value);
    if (!source) return false;
    const ir_value_t *storage = copied_from(source);
    return !mutable_storage(storage ? storage : source);
}

bool
ir_block_belongs_to(const ir_func_t *f, const ir_block_t *block)
{
    return block && block->owner == f && block->index < f->n_blocks
        && f->blocks[block->index] == block;
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
