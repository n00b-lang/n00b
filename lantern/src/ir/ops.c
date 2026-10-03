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

bool ir_opinfo_has_result(const ir_opinfo_t *i)
{
    return i && (i->flags & IR_OPF_HASRESULT);
}

bool ir_opinfo_is_terminator(const ir_opinfo_t *i)
{
    return i && (i->flags & IR_OPF_TERMINATOR);
}

bool ir_op_has_result(ir_op_t op)
{
    return ir_opinfo_has_result(ir_op_info(op));
}

bool ir_op_is_terminator(ir_op_t op)
{
    return ir_opinfo_is_terminator(ir_op_info(op));
}

const char *ir_opdkind_name(ir_opdkind_t k)
{
    switch (k) {
    case IR_OPD_VALUE:
        return "value";
    case IR_OPD_SPREAD:
        return "spread argument";
    case IR_OPD_TYPE:
        return "type";
    case IR_OPD_NAME:
        return "name";
    case IR_OPD_BLOCK:
        return "block";
    case IR_OPD_DEFAULT_BLOCK:
        return "default block";
    case IR_OPD_CONST:
        return "literal";
    case IR_OPD_PHI_ARM:
        return "phi arm";
    case IR_OPD_SWITCH_ARM:
        return "switch arm";
    case IR_OPD_NAMED_ARG:
        return "named argument";
    case IR_OPD_OUT:
        return "out argument";
    case IR_OPD_FRESH:
        return "fresh construction target";
    case IR_OPD_MISSING:
        return "omitted optional argument";
    default:
        return "argument";
    }
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
    case 'd':
        return IR_OPD_DEFAULT_BLOCK;
    case 'k':
        return IR_OPD_ARG;
    case 'o':
        return IR_OPD_OUT;
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

ir_opdkind_t ir_opinfo_operand_kind(const ir_opinfo_t *info, uint32_t i)
{
    if (!info) {
        return IR_OPD_VALUE;
    }

    const char *p   = info->pattern;
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

static const struct {
    ir_modgroup_t group;
    const char   *name;
} mods[] = {
    {IR_MOD_OVERFLOW, "wrap"},     {IR_MOD_OVERFLOW, "trap"},
    {IR_MOD_OVERFLOW, "sat"},      {IR_MOD_OVERFLOW, "undef"},
    {IR_MOD_DIVZERO, "trap"},      {IR_MOD_DIVZERO, "undef"},
    {IR_MOD_SHIFT, "mask"},        {IR_MOD_CMPORDER, "ordered"},
    {IR_MOD_CMPORDER, "unordered"},{IR_MOD_ACCESS, "volatile"},
    {IR_MOD_ACCESS, "atomic"},     {IR_MOD_FIT, "wrap"},
    {IR_MOD_FIT, "trap"},          {IR_MOD_FIT, "sat"},
    {IR_MOD_FIT, "undef"},         {IR_MOD_ROUND, "to_zero"},
    {IR_MOD_ROUND, "nearest"},     {IR_MOD_ROUND, "floor"},
    {IR_MOD_ROUND, "ceil"},
};

static uintptr_t mod_groups(const char *name)
{
    uintptr_t groups = 0;

    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        if (!strcmp(mods[i].name, name)) {
            groups |= (uintptr_t)mods[i].group;
        }
    }

    return groups;
}

bool ir_opinfo_accepts_mod(const ir_opinfo_t *info, const char *name)
{
    if (!info || !name) {
        return false;
    }

    return (info->mods & mod_groups(name)) != 0;
}

bool ir_op_accepts_mod(ir_op_t op, const char *name)
{
    return ir_opinfo_accepts_mod(ir_op_info(op), name);
}

bool ir_mod_exists(const char *name)
{
    return name && mod_groups(name) != 0;
}

bool ir_opinfo_accepts_operands(const ir_opinfo_t *i, uint32_t n)
{
    if (!i) {
        return false;
    }

    return n >= i->min_operands
        && (i->max_operands == IR_VAR_ARITY || n <= i->max_operands);
}

ir_opdkind_t ir_op_operand_kind(ir_op_t op, uint32_t i)
{
    return ir_opinfo_operand_kind(ir_op_info(op), i);
}

bool ir_op_accepts_operands(ir_op_t op, uint32_t n)
{
    return ir_opinfo_accepts_operands(ir_op_info(op), n);
}

bool ir_op_is_extension(ir_op_t op)
{
    return op >= IR_OP_COUNT;
}

bool ir_op_spelling_is_namespaced(const char *spelling, const char *profile)
{
    if (!spelling || !profile) {
        return false;
    }

    const char *dot = strchr(spelling, '.');

    if (!dot || dot == spelling || !dot[1]) {
        return false;
    }

    size_t n = (size_t)(dot - spelling);

    return strlen(profile) == n && !strncmp(spelling, profile, n);
}
