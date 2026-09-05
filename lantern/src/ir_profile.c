#include <string.h>

#include "ir_internal.h"
#include "ir/rule.h"

static ir_machine_t opaque(void)
{
    return (ir_machine_t){.class = IR_CLASS_OPAQUE, .width = 0};
}

static void ready_scalars(ir_module_t *m)
{
    if (m->scalars_ready) {
        return;
    }

    const ir_profile_t *p = m->profile;

    m->scalar_hashes = ir_alloc(m->ctx,
                                (p->n_scalars ? p->n_scalars : 1)
                                    * sizeof(uint64_t));

    for (size_t i = 0; i < p->n_scalars; i++) {
        th_error err = {0};
        th_type *t   = th_parse(m->ctx->types, p->scalars[i].type_text, &err);

        m->scalar_hashes[i] = t ? th_hash(m->ctx->types, t) : 0;
    }
    m->scalars_ready = true;
}

static const ir_scalar_t *find_scalar(ir_module_t *m, th_type *t)
{
    ready_scalars(m);

    const ir_profile_t *p = m->profile;
    uint64_t            h = th_hash(m->ctx->types, t);

    for (size_t i = 0; i < p->n_scalars; i++) {
        if (m->scalar_hashes[i] == h) {
            return &p->scalars[i];
        }
    }

    return nullptr;
}

const ir_opinfo_t *ir_module_op_info(ir_module_t *m, ir_op_t op)
{
    if (!ir_op_is_extension(op)) {
        return ir_op_info(op);
    }

    if (!m || !m->profile) {
        return nullptr;
    }

    size_t i = (size_t)op - IR_OP_COUNT;

    return i < m->profile->n_ext_ops ? &m->profile->ext_ops[i] : nullptr;
}

const char *ir_module_op_spelling(ir_module_t *m, ir_op_t op)
{
    const ir_opinfo_t *info = ir_module_op_info(m, op);

    return info ? info->spelling : nullptr;
}

bool ir_module_op_lookup(ir_module_t *m, const char *spelling, ir_op_t *out)
{
    if (ir_op_lookup(spelling, out)) {
        return true;
    }

    if (!m || !m->profile || !spelling) {
        return false;
    }

    for (size_t i = 0; i < m->profile->n_ext_ops; i++) {
        if (!strcmp(m->profile->ext_ops[i].spelling, spelling)) {
            if (out) {
                *out = (ir_op_t)(IR_OP_COUNT + i);
            }

            return true;
        }
    }

    return false;
}

ir_machine_t ir_machine_of(ir_module_t *m, th_type *t)
{
    if (!m || !t) {
        return opaque();
    }

    switch (th_kind_of(t)) {
    case TH_BITINT: {
        uint64_t w  = 0;
        bool     sg = false;

        if (!th_bitint_info(t, &w, &sg)) {
            return opaque();
        }

        return (ir_machine_t){
            .class = sg ? IR_CLASS_SINT : IR_CLASS_UINT,
            .width = (uint32_t)w,
        };
    }

    case TH_INDIRECTION:
    case TH_FUNCTION:
        return (ir_machine_t){.class = IR_CLASS_PTR, .width = 0};

    case TH_SEQUENCE:
    case TH_RECORD:
    case TH_SUM:
        return (ir_machine_t){.class = IR_CLASS_AGGREGATE, .width = 0};

    case TH_BINDER:
    case TH_BOUND:
    case TH_VAR:
        return opaque();

    case TH_NAMED: {
        const ir_scalar_t *s = find_scalar(m, t);

        if (!s) {
            return opaque();
        }

        return (ir_machine_t){.class = s->class, .width = s->width};
    }

    default:
        return opaque();
    }
}

bool ir_profile_has(ir_module_t *m, th_type *t)
{
    if (!m || !t || th_kind_of(t) != TH_NAMED) {
        return false;
    }

    return find_scalar(m, t) != nullptr;
}

static const ir_scalar_t bce_scalars[] = {
    {"i8", IR_CLASS_SINT, 8},
    {"i16", IR_CLASS_SINT, 16},
    {"i32", IR_CLASS_SINT, 32},
    {"i64", IR_CLASS_SINT, 64},
    {"u8", IR_CLASS_UINT, 8},
    {"u16", IR_CLASS_UINT, 16},
    {"u32", IR_CLASS_UINT, 32},
    {"u64", IR_CLASS_UINT, 64},
    {"f32", IR_CLASS_FLOAT, 32},
    {"f64", IR_CLASS_FLOAT, 64},
    {"bool", IR_CLASS_BOOL, 1},
    {"char", IR_CLASS_SINT, 8},
    {"void", IR_CLASS_VOID, 0},
};

const ir_profile_t ir_profile_bce = {
    .name      = "bce",
    .n_scalars = sizeof(bce_scalars) / sizeof(bce_scalars[0]),
    .scalars   = bce_scalars,
};

static ir_rule_result_t synth_concat(ir_module_t *m, ir_instr_t *in,
                                     ir_diags_t *d, void *state)
{
    (void)d;
    (void)state;

    if (in->n_operands != 2) {
        return (ir_rule_result_t){.verdict = IR_RULE_DECLINE};
    }

    return (ir_rule_result_t){
        .verdict = IR_RULE_ACCEPT,
        .result  = ir_type_parse(m, "str"),
    };
}

static ir_rule_result_t synth_seq_len(ir_module_t *m, ir_instr_t *in,
                                      ir_diags_t *d, void *state)
{
    (void)in;
    (void)d;
    (void)state;

    return (ir_rule_result_t){
        .verdict = IR_RULE_ACCEPT,
        .result  = ir_type_parse(m, "word"),
    };
}

static ir_rule_result_t synth_list_index(ir_module_t *m, ir_instr_t *in,
                                         ir_diags_t *d, void *state)
{
    (void)d;
    (void)state;

    th_type *k = ir_instr_key_type(in);
    const th_arg *a = th_named_arg(k, 0);

    if (!a || a->kind != TH_ARG_TYPE) {
        return (ir_rule_result_t){.verdict = IR_RULE_DECLINE};
    }

    return (ir_rule_result_t){
        .verdict = IR_RULE_ACCEPT,
        .result  = ir_type_intern(m, a->type),
    };
}

static const ir_rule_t synth_rules[] = {
    {
        .name      = "synth.concat",
        .op        = IR_OP_ADD,
        .match     = IR_MATCH_EXACT,
        .type_text = "str",
        .type_rule = synth_concat,
    },
    {
        .name      = "synth.list.index",
        .op        = IR_OP_INDEX,
        .match     = IR_MATCH_CONSTRUCTOR,
        .type_text = "list",
        .type_rule = synth_list_index,
    },
    {
        .name      = "synth.seq.len",
        .op        = IR_OP_CLZ,
        .match     = IR_MATCH_KIND,
        .kind      = TH_SEQUENCE,
        .type_rule = synth_seq_len,
    },
};

static const ir_opinfo_t synth_ext_ops[] = {
    {"synth.concat", 2, 2, IR_RES_ANNOTATED, "vv", 0, IR_OPF_HASRESULT},
    {"synth.trace", 0, IR_VAR_ARITY, IR_RES_NONE, "v*", 0,
     IR_OPF_SIDEEFFECT},
};

static const ir_scalar_t synth_scalars[] = {
    {"str", IR_CLASS_OPAQUE, 0},
    {"int", IR_CLASS_SINT, 64},
    {"word", IR_CLASS_UINT, 32},
    {"byte", IR_CLASS_UINT, 8},
    {"real", IR_CLASS_FLOAT, 64},
    {"flag", IR_CLASS_BOOL, 1},
    {"unit", IR_CLASS_VOID, 0},
};

const ir_profile_t ir_profile_synth = {
    .name      = "synth",
    .n_scalars = sizeof(synth_scalars) / sizeof(synth_scalars[0]),
    .scalars   = synth_scalars,
    .n_rules   = sizeof(synth_rules) / sizeof(synth_rules[0]),
    .rules     = synth_rules,
    .n_ext_ops = sizeof(synth_ext_ops) / sizeof(synth_ext_ops[0]),
    .ext_ops   = synth_ext_ops,
};
