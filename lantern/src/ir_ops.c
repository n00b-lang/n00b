#include <string.h>

#include "ir/ir.h"

#define VAR IR_VAR_ARITY

#define IR_OP(id, spelling, lo, hi, res, pat, mods, flags)                     \
    [IR_OP_##id] = {spelling, lo, hi, IR_RES_##res, pat, mods, (flags)},

#define HASRESULT  IR_OPF_HASRESULT
#define SIDEEFFECT IR_OPF_SIDEEFFECT
#define TERMINATOR IR_OPF_TERMINATOR
#define COMPTIME   IR_OPF_COMPTIME

static const ir_opinfo_t table[IR_OP_COUNT] = {
#include "ir/ir_ops.def"
};

#undef IR_OP
#undef HASRESULT
#undef SIDEEFFECT
#undef TERMINATOR
#undef COMPTIME
#undef VAR

const ir_opinfo_t *ir_op_info(ir_op_t op)
{
    return op < IR_OP_COUNT ? &table[op] : nullptr;
}

bool ir_op_lookup(const char *spelling, ir_op_t *out)
{
    if (!spelling) {
        return false;
    }

    for (uint16_t i = 0; i < IR_OP_COUNT; i++) {
        if (!strcmp(table[i].spelling, spelling)) {
            if (out) {
                *out = (ir_op_t)i;
            }

            return true;
        }
    }

    return false;
}

const char *ir_op_spelling(ir_op_t op)
{
    return op < IR_OP_COUNT ? table[op].spelling : nullptr;
}

bool ir_op_has_result(ir_op_t op)
{
    return op < IR_OP_COUNT && (table[op].flags & IR_OPF_HASRESULT);
}

bool ir_op_is_terminator(ir_op_t op)
{
    return op < IR_OP_COUNT && (table[op].flags & IR_OPF_TERMINATOR);
}

static ir_opdkind_t kind_of_char(char c)
{
    switch (c) {
    case 'v':
        return IR_OPD_VALUE;
    case 't':
        return IR_OPD_TYPE;
    case 'n':
        return IR_OPD_NAME;
    case 'b':
        return IR_OPD_BLOCK;
    case 'c':
        return IR_OPD_CONST;
    case 'a':
        return IR_OPD_PHI_ARM;
    case 's':
        return IR_OPD_SWITCH_ARM;
    default:
        return IR_OPD_VALUE;
    }
}

ir_opdkind_t ir_op_operand_kind(ir_op_t op, uint32_t i)
{
    if (op >= IR_OP_COUNT) {
        return IR_OPD_VALUE;
    }

    const char *p   = table[op].pattern;
    size_t      len = strlen(p);

    if (!len) {
        return IR_OPD_VALUE;
    }

    bool   varying = p[len - 1] == '*';
    size_t fixed   = varying ? len - 2 : len;

    if (i < fixed) {
        return kind_of_char(p[i]);
    }

    if (varying) {
        return kind_of_char(p[len - 2]);
    }

    return kind_of_char(p[len - 1]);
}

bool ir_op_accepts_operands(ir_op_t op, uint32_t n)
{
    if (op >= IR_OP_COUNT) {
        return false;
    }

    return n >= table[op].min_operands
        && (table[op].max_operands == IR_VAR_ARITY
            || n <= table[op].max_operands);
}
