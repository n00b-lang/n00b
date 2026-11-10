#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "private/ir/internal.h"
#include "ir/typelang.h"

struct ir_parsed_scalars_t {
    const ir_profile_t  *profile;
    uint64_t            *hashes;
    th_type            **types;
    ir_parsed_scalars_t *next;
};

static pthread_mutex_t parsed_scalars_lock = PTHREAD_MUTEX_INITIALIZER;

static ir_machine_t opaque(void)
{
    return (ir_machine_t){.class = IR_CLASS_OPAQUE, .width = 0};
}

static ir_parsed_scalars_t *parsed_scalars(ir_ctx_t *ctx,
                                           const ir_profile_t *p)
{
    pthread_mutex_lock(&parsed_scalars_lock);
    ir_parsed_scalars_t *parsed = ctx->parsed_scalars;

    while (parsed && parsed->profile != p) {
        parsed = parsed->next;
    }
    if (!parsed) {
        size_t n = p->n_scalars ? p->n_scalars : 1;

        parsed          = ir_alloc(ctx, sizeof(ir_parsed_scalars_t));
        parsed->profile = p;
        parsed->hashes  = ir_alloc(ctx, n * sizeof(uint64_t));
        parsed->types   = ir_alloc(ctx, n * sizeof(th_type *));
        for (size_t i = 0; i < p->n_scalars; i++) {
            core_string_t err = {};
            th_type      *t   = ir_typelang_parse(ctx->types,
                                                  p->scalars[i].type_text,
                                                  &err);

            core_free(err.data);
            parsed->hashes[i] = t ? th_hash(ctx->types, t) : 0;
            parsed->types[i]  = t;
        }
        parsed->next        = ctx->parsed_scalars;
        ctx->parsed_scalars = parsed;
    }
    pthread_mutex_unlock(&parsed_scalars_lock);

    return parsed;
}

static void ready_scalars(ir_module_t *m)
{
    if (m->scalars_ready) {
        return;
    }

    const ir_profile_t  *p      = m->profile;
    ir_parsed_scalars_t *parsed = parsed_scalars(m->ctx, p);
    size_t               n      = p->n_scalars ? p->n_scalars : 1;

    m->scalar_hashes = parsed->hashes;
    m->scalar_types  = ir_alloc(m->ctx, n * sizeof(th_type *));

    for (size_t i = 0; i < p->n_scalars; i++) {
        m->scalar_types[i] = parsed->types[i]
            ? ir_type_intern(m, parsed->types[i]) : nullptr;
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

static const th_builtin_type_t IR_CONSTRUCTORS[] = {
    {CORE_STRING_STATIC("string"), TH_BUILTIN_STRING, 0, nullptr},
    {CORE_STRING_STATIC("atomic"), TH_BUILTIN_ATOMIC, 1, nullptr},
    {CORE_STRING_STATIC("any"), TH_BUILTIN_ANY, 0, nullptr},
    {CORE_STRING_STATIC("array"), TH_BUILTIN_ARRAY, 1, nullptr},
    {CORE_STRING_STATIC("list"), TH_BUILTIN_LIST, 1, nullptr},
    {CORE_STRING_STATIC("set"), TH_BUILTIN_SET, 1, nullptr},
    {CORE_STRING_STATIC("dict"), TH_BUILTIN_DICT, 2, nullptr},
};

void ir_profile_install_builtins(ir_ctx_t *ctx, const ir_profile_t *p)
{
    if (!ctx || !p || th_ctx_has_builtin_types(ctx->types)) return;

    size_t constructors = sizeof(IR_CONSTRUCTORS) / sizeof(IR_CONSTRUCTORS[0]);
    size_t count = p->n_scalars + constructors;
    th_builtin_type_t *table = ir_alloc(ctx, count * sizeof(*table));
    for (size_t i = 0; i < p->n_scalars; i++)
        table[i] = (th_builtin_type_t){
            .name   = p->scalars[i].type_text,
            .kind   = TH_BUILTIN_SCALAR,
            .arity  = 0,
            .scalar = &p->scalars[i],
        };
    for (size_t i = 0; i < constructors; i++)
        table[p->n_scalars + i] = IR_CONSTRUCTORS[i];
    th_ctx_set_builtin_types(ctx->types, table, count);
}

static bool scalar_in_profile(const ir_profile_t *p, const ir_scalar_t *s)
{
    return p && s >= p->scalars && s < p->scalars + p->n_scalars;
}

static const ir_scalar_t *profile_scalar_of(ir_module_t *m, th_type *t)
{
    const th_builtin_type_t *builtin = th_builtin_type(m->ctx->types, t);
    const ir_scalar_t *scalar = builtin ? builtin->scalar : nullptr;

    return scalar_in_profile(m->profile, scalar) ? scalar
                                                 : find_scalar(m, t);
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

core_string_t ir_module_op_spelling(ir_module_t *m, ir_op_t op)
{
    const ir_opinfo_t *info = ir_module_op_info(m, op);

    return info ? info->spelling : (core_string_t){};
}

bool ir_module_op_lookup(ir_module_t *m, core_string_t spelling,
                         ir_op_t *out)
{
    if (ir_op_lookup(spelling, out)) {
        return true;
    }

    if (!m || !m->profile || !spelling.data) {
        return false;
    }

    if (!m->ext_op_index) {
        for (size_t i = 0; i < m->profile->n_ext_ops; i++) {
            ir_index_add(m->ctx, &m->ext_op_index,
                         m->profile->ext_ops[i].spelling,
                         (void *)(uintptr_t)(i + 1));
        }

        if (!m->ext_op_index) {
            return false;
        }
    }

    uintptr_t slot = (uintptr_t)ir_index_find(m->ext_op_index, spelling);

    if (!slot) {
        return false;
    }

    if (out) {
        *out = (ir_op_t)(IR_OP_COUNT + (slot - 1));
    }

    return true;
}

ir_machine_t ir_machine_of(ir_module_t *m, th_type *t)
{
    if (!m || !t) {
        return opaque();
    }

    t = th_resolve(t);

    switch (th_kind_of(t)) {
    case TH_BITINT: {
        uint64_t w  = 0;
        th_bitint_flavor flavor = TH_BITINT_UNSIGNED;

        if (!th_bitint_info(t, &w, &flavor)) {
            return opaque();
        }

        return (ir_machine_t){
            .class = flavor == TH_BITINT_SIGNED
                ? IR_CLASS_SINT : IR_CLASS_UINT,
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
        const ir_scalar_t *s = profile_scalar_of(m, t);

        if (!s) {
            return opaque();
        }

        return (ir_machine_t){
            .class = s->class,
            .width = s->width,
            .role  = s->role,
        };
    }

    default:
        return opaque();
    }
}

th_type *ir_profile_scalar(ir_module_t *m, ir_class_t c, uint32_t width)
{
    if (!m || !m->profile) {
        return nullptr;
    }

    const ir_profile_t *p = m->profile;

    ready_scalars(m);

    for (size_t i = 0; i < p->n_scalars; i++) {
        if (p->scalars[i].class == c
            && (width == 0 || p->scalars[i].width == width)) {
            return m->scalar_types[i];
        }
    }

    return nullptr;
}

th_type *ir_profile_role(ir_module_t *m, ir_role_t role)
{
    if (!m || !m->profile || role == IR_ROLE_NONE) {
        return nullptr;
    }

    ready_scalars(m);

    for (size_t i = 0; i < m->profile->n_scalars; i++) {
        if (m->profile->scalars[i].role == role) {
            return m->scalar_types[i];
        }
    }

    return nullptr;
}

bool ir_profile_has(ir_module_t *m, th_type *t)
{
    if (!m || !t) {
        return false;
    }

    t = th_resolve(t);
    if (th_kind_of(t) != TH_NAMED) {
        return false;
    }

    return find_scalar(m, t) != nullptr;
}

static const ir_scalar_t bce_scalars[] = {
    {CORE_STRING_STATIC("i8"), IR_CLASS_SINT, 8, IR_ROLE_NONE},
    {CORE_STRING_STATIC("i16"), IR_CLASS_SINT, 16, IR_ROLE_NONE},
    {CORE_STRING_STATIC("i32"), IR_CLASS_SINT, 32, IR_ROLE_NONE},
    {CORE_STRING_STATIC("i64"), IR_CLASS_SINT, 64, IR_ROLE_NONE},
    {CORE_STRING_STATIC("i128"), IR_CLASS_SINT, 128, IR_ROLE_NONE},
    {CORE_STRING_STATIC("u8"), IR_CLASS_UINT, 8, IR_ROLE_NONE},
    {CORE_STRING_STATIC("u16"), IR_CLASS_UINT, 16, IR_ROLE_NONE},
    {CORE_STRING_STATIC("u32"), IR_CLASS_UINT, 32, IR_ROLE_NONE},
    {CORE_STRING_STATIC("u64"), IR_CLASS_UINT, 64, IR_ROLE_NONE},
    {CORE_STRING_STATIC("u128"), IR_CLASS_UINT, 128, IR_ROLE_NONE},
    {CORE_STRING_STATIC("f32"), IR_CLASS_FLOAT, 32, IR_ROLE_NONE},
    {CORE_STRING_STATIC("f64"), IR_CLASS_FLOAT, 64, IR_ROLE_NONE},
    {CORE_STRING_STATIC("bool"), IR_CLASS_BOOL, 1, IR_ROLE_NONE},
    {CORE_STRING_STATIC("char"), IR_CLASS_SINT, 8, IR_ROLE_CHAR},
    {CORE_STRING_STATIC("void"), IR_CLASS_VOID, 0, IR_ROLE_NONE},
};

const ir_profile_t ir_profile_bce = {
    .name      = CORE_STRING_STATIC("bce"),
    .n_scalars = sizeof(bce_scalars) / sizeof(bce_scalars[0]),
    .scalars   = bce_scalars,
};

static const ir_opinfo_t synth_ext_ops[] = {
    {CORE_STRING_STATIC("synth.concat"), 2, 2, IR_RES_ANNOTATED,
     CORE_STRING_STATIC("vv"), 0, IR_OPF_HASRESULT, {0, -1, -1, -1, false}},
    {CORE_STRING_STATIC("synth.trace"), 0, IR_VAR_ARITY, IR_RES_NONE,
     CORE_STRING_STATIC("v*"), 0, IR_OPF_SIDEEFFECT, {0, -1, -1, -1, false}},
};

static const ir_scalar_t synth_scalars[] = {
    {CORE_STRING_STATIC("str"), IR_CLASS_OPAQUE, 0, IR_ROLE_NONE},
    {CORE_STRING_STATIC("int"), IR_CLASS_SINT, 64, IR_ROLE_NONE},
    {CORE_STRING_STATIC("word"), IR_CLASS_UINT, 32, IR_ROLE_NONE},
    {CORE_STRING_STATIC("byte"), IR_CLASS_UINT, 8, IR_ROLE_NONE},
    {CORE_STRING_STATIC("real"), IR_CLASS_FLOAT, 64, IR_ROLE_NONE},
    {CORE_STRING_STATIC("flag"), IR_CLASS_BOOL, 1, IR_ROLE_NONE},
    {CORE_STRING_STATIC("unit"), IR_CLASS_VOID, 0, IR_ROLE_NONE},
};

const ir_profile_t ir_profile_synth = {
    .name      = CORE_STRING_STATIC("synth"),
    .n_scalars = sizeof(synth_scalars) / sizeof(synth_scalars[0]),
    .scalars   = synth_scalars,
    .n_ext_ops = sizeof(synth_ext_ops) / sizeof(synth_ext_ops[0]),
    .ext_ops   = synth_ext_ops,
};
