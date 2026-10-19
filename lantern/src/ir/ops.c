#include "ir/ir.h"

#define VAR IR_VAR_ARITY

#define IR_OP(id, spelling, lo, hi, res, pat, mods, flags)                     \
    [IR_OP_##id] = {CORE_STRING_STATIC(spelling), lo, hi, IR_RES_##res,        \
                    CORE_STRING_STATIC(pat), mods, (flags)},

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

bool ir_op_lookup(core_string_t spelling, ir_op_t *out)
{
    if (!spelling.data) {
        return false;
    }

    for (uint16_t i = 0; i < IR_OP_COUNT; i++) {
        if (core_string_eq(table[i].spelling, spelling)) {
            if (out) {
                *out = (ir_op_t)i;
            }

            return true;
        }
    }

    return false;
}

core_string_t ir_op_spelling(ir_op_t op)
{
    return op < IR_OP_COUNT ? table[op].spelling : (core_string_t){};
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

core_string_t ir_opdkind_name(ir_opdkind_t k)
{
    switch (k) {
    case IR_OPD_VALUE:
        return CORE_STRING_STATIC("value");
    case IR_OPD_SPREAD:
        return CORE_STRING_STATIC("spread argument");
    case IR_OPD_TYPE:
        return CORE_STRING_STATIC("type");
    case IR_OPD_NAME:
        return CORE_STRING_STATIC("name");
    case IR_OPD_BLOCK:
        return CORE_STRING_STATIC("block");
    case IR_OPD_DEFAULT_BLOCK:
        return CORE_STRING_STATIC("default block");
    case IR_OPD_CONST:
        return CORE_STRING_STATIC("literal");
    case IR_OPD_PHI_ARM:
        return CORE_STRING_STATIC("phi arm");
    case IR_OPD_SWITCH_ARM:
        return CORE_STRING_STATIC("switch arm");
    case IR_OPD_NAMED_ARG:
        return CORE_STRING_STATIC("named argument");
    case IR_OPD_OUT:
        return CORE_STRING_STATIC("out argument");
    case IR_OPD_FRESH:
        return CORE_STRING_STATIC("fresh construction target");
    case IR_OPD_MISSING:
        return CORE_STRING_STATIC("omitted optional argument");
    default:
        return CORE_STRING_STATIC("argument");
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

    const char *p   = info->pattern.data;
    size_t      len = info->pattern.u8_bytes;

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
    core_string_t name;
} mods[] = {
    {IR_MOD_OVERFLOW, CORE_STRING_STATIC("wrap")},
    {IR_MOD_OVERFLOW, CORE_STRING_STATIC("trap")},
    {IR_MOD_OVERFLOW, CORE_STRING_STATIC("sat")},
    {IR_MOD_OVERFLOW, CORE_STRING_STATIC("undef")},
    {IR_MOD_DIVZERO, CORE_STRING_STATIC("trap")},
    {IR_MOD_DIVZERO, CORE_STRING_STATIC("undef")},
    {IR_MOD_SHIFT, CORE_STRING_STATIC("mask")},
    {IR_MOD_CMPORDER, CORE_STRING_STATIC("ordered")},
    {IR_MOD_CMPORDER, CORE_STRING_STATIC("unordered")},
    {IR_MOD_ACCESS, CORE_STRING_STATIC("volatile")},
    {IR_MOD_ACCESS, CORE_STRING_STATIC("atomic")},
    {IR_MOD_FIT, CORE_STRING_STATIC("wrap")},
    {IR_MOD_FIT, CORE_STRING_STATIC("trap")},
    {IR_MOD_FIT, CORE_STRING_STATIC("sat")},
    {IR_MOD_FIT, CORE_STRING_STATIC("undef")},
    {IR_MOD_ROUND, CORE_STRING_STATIC("to_zero")},
    {IR_MOD_ROUND, CORE_STRING_STATIC("nearest")},
    {IR_MOD_ROUND, CORE_STRING_STATIC("floor")},
    {IR_MOD_ROUND, CORE_STRING_STATIC("ceil")},
    {IR_MOD_ATOMIC_ORDER, CORE_STRING_STATIC("release")},
    {IR_MOD_ATOMIC_ORDER, CORE_STRING_STATIC("relaxed")},
    {IR_MOD_ATOMIC_ORDER, CORE_STRING_STATIC("acquire")},
};

static uintptr_t mod_groups(core_string_t name)
{
    uintptr_t groups = 0;

    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        if (core_string_eq(mods[i].name, name)) {
            groups |= (uintptr_t)mods[i].group;
        }
    }

    return groups;
}

bool ir_opinfo_accepts_mod(const ir_opinfo_t *info, core_string_t name)
{
    if (!info || !name.data) {
        return false;
    }

    return (info->mods & mod_groups(name)) != 0;
}

bool ir_op_accepts_mod(ir_op_t op, core_string_t name)
{
    return ir_opinfo_accepts_mod(ir_op_info(op), name);
}

bool ir_mod_exists(core_string_t name)
{
    return name.data && mod_groups(name) != 0;
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

bool ir_op_spelling_is_namespaced(core_string_t spelling,
                                  core_string_t profile)
{
    if (!spelling.data || !profile.data) {
        return false;
    }

    int64_t dot = core_string_find_byte(spelling, '.');

    if (dot <= 0 || (size_t)dot + 1 >= spelling.u8_bytes) {
        return false;
    }

    return core_string_eq(core_string_slice(spelling, 0, (size_t)dot),
                          profile);
}
