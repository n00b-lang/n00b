#include <string.h>

#include "ir_internal.h"

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

static const ir_scalar_t synth_scalars[] = {
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
};
