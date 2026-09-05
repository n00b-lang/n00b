#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"

static uint32_t name_hash(const char *k)
{
    uint32_t h = 2166136261u;

    for (const unsigned char *p = (const unsigned char *)k; *p; p++) {
        h = (h ^ *p) * 16777619u;
    }

    return h;
}

typedef const char *(*name_of_fn)(const void *node);

static const char *global_name_of(const void *node)
{
    return ((const ir_global_t *)node)->name;
}

static const char *func_name_of(const void *node)
{
    return ((const ir_func_t *)node)->name;
}

static const char *block_name_of(const void *node)
{
    return ((const ir_block_t *)node)->name;
}

static const char *local_name_of(const void *node)
{
    return ((const ir_local_t *)node)->name;
}

static size_t index_slot(void **ix, size_t cap, name_of_fn name_of,
                         const char *k)
{
    size_t i = name_hash(k) & (cap - 1);

    while (ix[i] && strcmp(name_of(ix[i]), k)) {
        i = (i + 1) & (cap - 1);
    }

    return i;
}

static void index_add(ir_ctx_t *ctx, void ***ix, uint32_t *cap, uint32_t n,
                      void **list, name_of_fn name_of, void *node)
{
    size_t want = *cap ? *cap : 16;

    while ((size_t)(n + 1) * 2 > want) {
        want *= 2;
    }

    if (!*ix || want != *cap) {
        *ix  = ir_alloc(ctx, want * sizeof(void *));
        *cap = (uint32_t)want;

        for (uint32_t k = 0; k < n; k++) {
            (*ix)[index_slot(*ix, want, name_of, name_of(list[k]))] = list[k];
        }
    }
    (*ix)[index_slot(*ix, *cap, name_of, name_of(node))] = node;
}

static void *index_find(void **ix, uint32_t cap, name_of_fn name_of,
                        const char *k)
{
    return ix && cap ? ix[index_slot(ix, cap, name_of, k)] : nullptr;
}

ir_global_t *ir_global_add(ir_module_t *m, const char *name, th_type *type)
{
    if (!m || !name || !type || ir_global_find(m, name)) {
        return nullptr;
    }

    ir_global_t *g = ir_alloc(m->ctx, sizeof(ir_global_t));

    if (!g) {
        return nullptr;
    }

    g->name = ir_strdup(m->ctx, name);
    g->type = ir_type_intern(m, type);
    g->span = IR_SPAN_UNKNOWN;

    m->globals = ir_grow(m->ctx, m->globals, &m->cap_globals,
                         m->n_globals + 1, sizeof(ir_global_t *));
    m->globals[m->n_globals++] = g;
    index_add(m->ctx, &m->global_index, &m->global_index_cap,
              (uint32_t)m->n_globals - 1, (void **)m->globals, global_name_of,
              g);

    return g;
}

ir_global_t *ir_global_find(ir_module_t *m, const char *name)
{
    if (!m || !name) {
        return nullptr;
    }

    return index_find(m->global_index, m->global_index_cap, global_name_of,
                      name);
}

size_t ir_global_count(const ir_module_t *m)
{
    return m ? m->n_globals : 0;
}

ir_global_t *ir_global_at(ir_module_t *m, size_t i)
{
    return m && i < m->n_globals ? m->globals[i] : nullptr;
}

ir_func_t *ir_func_add(ir_module_t *m, const char *name, th_type *type,
                       ir_linkage_t linkage)
{
    if (!m || !name || !type || ir_func_find(m, name)) {
        return nullptr;
    }

    if (th_kind_of(type) != TH_FUNCTION) {
        return nullptr;
    }

    ir_func_t *f = ir_alloc(m->ctx, sizeof(ir_func_t));

    if (!f) {
        return nullptr;
    }

    f->name    = ir_strdup(m->ctx, name);
    f->type    = ir_type_intern(m, type);
    f->linkage = linkage;
    f->span    = IR_SPAN_UNKNOWN;
    f->owner   = m;

    m->funcs = ir_grow(m->ctx, m->funcs, &m->cap_funcs, m->n_funcs + 1,
                       sizeof(ir_func_t *));
    m->funcs[m->n_funcs++] = f;
    index_add(m->ctx, &m->func_index, &m->func_index_cap,
              (uint32_t)m->n_funcs - 1, (void **)m->funcs, func_name_of, f);

    return f;
}

ir_func_t *ir_func_find(ir_module_t *m, const char *name)
{
    if (!m || !name) {
        return nullptr;
    }

    return index_find(m->func_index, m->func_index_cap, func_name_of, name);
}

size_t ir_func_count(const ir_module_t *m)
{
    return m ? m->n_funcs : 0;
}

ir_func_t *ir_func_at(ir_module_t *m, size_t i)
{
    return m && i < m->n_funcs ? m->funcs[i] : nullptr;
}

ir_value_t *ir_param_add(ir_func_t *f, const char *name, th_type *type)
{
    if (!f || !name || !type) {
        return nullptr;
    }

    ir_ctx_t   *ctx = f->owner->ctx;
    ir_value_t *v   = ir_alloc(ctx, sizeof(ir_value_t));

    if (!v) {
        return nullptr;
    }

    v->kind  = IR_VAL_PARAM;
    v->name  = ir_strdup(ctx, name);
    v->type  = ir_type_intern(f->owner, type);
    v->owner = f;
    v->index = f->n_params;

    size_t cap = f->cap_params;

    f->params     = ir_grow(ctx, f->params, &cap, f->n_params + 1u,
                            sizeof(ir_value_t *));
    f->cap_params = (uint32_t)cap;
    f->params[f->n_params++] = v;

    return v;
}

ir_local_t *ir_local_add(ir_func_t *f, const char *name, th_type *type)
{
    if (!f || !name || !type || ir_local_find(f, name)) {
        return nullptr;
    }

    ir_ctx_t   *ctx = f->owner->ctx;
    ir_local_t *l   = ir_alloc(ctx, sizeof(ir_local_t));

    if (!l) {
        return nullptr;
    }

    l->name  = ir_strdup(ctx, name);
    l->type  = ir_type_intern(f->owner, type);
    l->span  = IR_SPAN_UNKNOWN;
    l->index = f->n_locals;

    size_t cap = f->cap_locals;

    f->locals     = ir_grow(ctx, f->locals, &cap, f->n_locals + 1u,
                            sizeof(ir_local_t *));
    f->cap_locals = (uint32_t)cap;
    f->locals[f->n_locals++] = l;
    index_add(ctx, &f->local_index, &f->local_index_cap, f->n_locals - 1,
              (void **)f->locals, local_name_of, l);

    return l;
}

ir_local_t *ir_local_find(ir_func_t *f, const char *name)
{
    if (!f || !name) {
        return nullptr;
    }

    return index_find(f->local_index, f->local_index_cap, local_name_of, name);
}

ir_block_t *ir_block_add(ir_func_t *f, const char *name)
{
    if (!f || !name || ir_block_find(f, name)) {
        return nullptr;
    }

    ir_ctx_t   *ctx = f->owner->ctx;
    ir_block_t *b   = ir_alloc(ctx, sizeof(ir_block_t));

    if (!b) {
        return nullptr;
    }

    b->name  = ir_strdup(ctx, name);
    b->span  = IR_SPAN_UNKNOWN;
    b->owner = f;
    b->index = f->n_blocks;

    size_t cap = f->cap_blocks;

    f->blocks     = ir_grow(ctx, f->blocks, &cap, f->n_blocks + 1u,
                            sizeof(ir_block_t *));
    f->cap_blocks = (uint32_t)cap;
    f->blocks[f->n_blocks++] = b;
    index_add(ctx, &f->block_index, &f->block_index_cap, f->n_blocks - 1,
              (void **)f->blocks, block_name_of, b);

    return b;
}

ir_block_t *ir_block_find(ir_func_t *f, const char *name)
{
    if (!f || !name) {
        return nullptr;
    }

    return index_find(f->block_index, f->block_index_cap, block_name_of, name);
}

ir_block_t *ir_block_entry(ir_func_t *f)
{
    return f && f->n_blocks ? f->blocks[0] : nullptr;
}

ir_instr_t *ir_block_terminator(ir_block_t *b)
{
    if (!b || !b->n_instrs) {
        return nullptr;
    }

    ir_instr_t *last = b->instrs[b->n_instrs - 1];

    return ir_opinfo_is_terminator(
               ir_module_op_info(b->owner->owner, last->op))
               ? last
               : nullptr;
}

ir_value_t *ir_operand_value(const ir_operand_t *od)
{
    switch (od->kind) {
    case IR_OPD_VALUE:
        return od->value;
    case IR_OPD_NAMED_ARG:
        return od->named.value;
    case IR_OPD_PHI_ARM:
        return od->phi.value;
    default:
        return nullptr;
    }
}

ir_local_t *ir_operand_local(const ir_operand_t *od)
{
    ir_value_t *v = ir_operand_value(od);

    return v && v->kind == IR_VAL_LOCAL ? v->local : nullptr;
}

bool ir_local_read_earlier(const ir_instr_t *in, uint32_t o, ir_local_t *l)
{
    for (uint32_t i = 0; i < o; i++) {
        if (ir_operand_local(&in->operands[i]) == l) {
            return true;
        }
    }

    return false;
}

ir_instr_t *ir_instr_add(ir_block_t *b, ir_op_t op)
{
    if (!b || !ir_module_op_info(b->owner->owner, op)) {
        return nullptr;
    }

    ir_ctx_t   *ctx = b->owner->owner->ctx;
    ir_instr_t *in  = ir_alloc(ctx, sizeof(ir_instr_t));

    if (!in) {
        return nullptr;
    }

    in->op    = op;
    in->span  = IR_SPAN_UNKNOWN;
    in->block = b;
    in->index = b->n_instrs;

    size_t cap = b->cap_instrs;

    b->instrs     = ir_grow(ctx, b->instrs, &cap, b->n_instrs + 1u,
                            sizeof(ir_instr_t *));
    b->cap_instrs = (uint32_t)cap;
    b->instrs[b->n_instrs++] = in;

    return in;
}

bool ir_instr_set_dest(ir_instr_t *in, const char *name, th_type *type)
{
    if (!in || !name || !type
        || !ir_opinfo_has_result(
               ir_module_op_info(in->block->owner->owner, in->op))) {
        return false;
    }

    ir_func_t  *f   = in->block->owner;
    ir_ctx_t   *ctx = f->owner->ctx;
    ir_value_t *v   = ir_alloc(ctx, sizeof(ir_value_t));

    if (!v) {
        return false;
    }

    v->kind        = IR_VAL_INSTR;
    v->name        = ir_strdup(ctx, name);
    v->type        = ir_type_intern(f->owner, type);
    v->def         = in;
    v->owner       = f;
    in->dest_kind  = IR_DEST_VALUE;
    in->dest       = v;
    in->dest_local = nullptr;

    return true;
}

bool ir_instr_set_dest_local(ir_instr_t *in, ir_local_t *l)
{
    if (!in || !l
        || !ir_opinfo_has_result(
               ir_module_op_info(in->block->owner->owner, in->op))) {
        return false;
    }

    in->dest_kind  = IR_DEST_LOCAL;
    in->dest_local = l;
    in->dest       = nullptr;

    return true;
}

static ir_operand_t *push(ir_instr_t *in, ir_opdkind_t kind)
{
    if (!in) {
        return nullptr;
    }

    ir_module_t       *mod  = in->block->owner->owner;
    ir_ctx_t          *ctx  = mod->ctx;
    const ir_opinfo_t *info = ir_module_op_info(mod, in->op);

    ir_opdkind_t want = ir_opinfo_operand_kind(info, in->n_operands);

    if (want != kind
        && !(want == IR_OPD_ARG
             && (kind == IR_OPD_VALUE || kind == IR_OPD_NAMED_ARG))) {
        return nullptr;
    }

    if (info->max_operands != IR_VAR_ARITY
        && in->n_operands + 1u > info->max_operands) {
        return nullptr;
    }

    size_t        cap   = in->cap_operands;
    ir_operand_t *grown = ir_grow(ctx, in->operands, &cap, in->n_operands + 1u,
                                  sizeof(ir_operand_t));

    if (!grown) {
        return nullptr;
    }
    in->operands     = grown;
    in->cap_operands = (uint32_t)cap;

    ir_operand_t *o = &in->operands[in->n_operands++];

    memset(o, 0, sizeof(*o));
    o->kind = kind;

    return o;
}

bool ir_instr_push_value(ir_instr_t *in, ir_value_t *v)
{
    ir_operand_t *o = v ? push(in, IR_OPD_VALUE) : nullptr;

    if (o) {
        o->value = v;
    }

    return o != nullptr;
}

bool ir_instr_push_named(ir_instr_t *in, const char *name, ir_value_t *v)
{
    ir_operand_t *o = name && v ? push(in, IR_OPD_NAMED_ARG) : nullptr;

    if (o) {
        o->named.name  = ir_strdup(in->block->owner->owner->ctx, name);
        o->named.value = v;
    }

    return o != nullptr;
}

bool ir_instr_push_local(ir_instr_t *in, ir_local_t *l)
{
    if (!in || !l) {
        return false;
    }

    ir_ctx_t   *ctx = in->block->owner->owner->ctx;
    ir_value_t *v   = ir_alloc(ctx, sizeof(ir_value_t));

    if (!v) {
        return false;
    }

    v->kind  = IR_VAL_LOCAL;
    v->name  = l->name;
    v->type  = l->type;
    v->local = l;
    v->owner = in->block->owner;

    return ir_instr_push_value(in, v);
}

bool ir_instr_push_type(ir_instr_t *in, th_type *t)
{
    ir_operand_t *o = t ? push(in, IR_OPD_TYPE) : nullptr;

    if (o) {
        o->type = ir_type_intern(in->block->owner->owner, t);
    }

    return o != nullptr;
}

bool ir_instr_push_name(ir_instr_t *in, const char *n)
{
    ir_operand_t *o = n ? push(in, IR_OPD_NAME) : nullptr;

    if (o) {
        o->name = ir_strdup(in->block->owner->owner->ctx, n);
    }

    return o != nullptr;
}

bool ir_instr_push_block(ir_instr_t *in, ir_block_t *b)
{
    ir_operand_t *o = b ? push(in, IR_OPD_BLOCK) : nullptr;

    if (o) {
        o->block = b;
    }

    return o != nullptr;
}

bool ir_instr_push_default(ir_instr_t *in, ir_block_t *b)
{
    ir_operand_t *o = b ? push(in, IR_OPD_DEFAULT_BLOCK) : nullptr;

    if (o) {
        o->block = b;
    }

    return o != nullptr;
}

ir_lit_t ir_lit_of(ir_int_t v)
{
    if (v < 0) {
        return (ir_lit_t){.mag = (ir_uint_t)0 - (ir_uint_t)v, .neg = true};
    }

    return (ir_lit_t){.mag = (ir_uint_t)v};
}

ir_lit_t ir_lit_hex(ir_uint_t v)
{
    return (ir_lit_t){.mag = v, .hex = true};
}

ir_int_t ir_lit_signed(ir_lit_t v)
{
    return v.neg ? -(ir_int_t)v.mag : (ir_int_t)v.mag;
}

bool ir_lit_eq(ir_lit_t a, ir_lit_t b)
{
    if (!a.mag && !b.mag) {
        return true;
    }

    return a.mag == b.mag && a.neg == b.neg;
}

bool ir_instr_push_lit(ir_instr_t *in, ir_lit_t v)
{
    ir_operand_t *o = push(in, IR_OPD_CONST);

    if (o) {
        o->lit.i = v;
    }

    return o != nullptr;
}

bool ir_instr_push_int(ir_instr_t *in, ir_int_t v)
{
    return ir_instr_push_lit(in, ir_lit_of(v));
}

bool ir_instr_push_uint(ir_instr_t *in, ir_uint_t v)
{
    return ir_instr_push_lit(in, (ir_lit_t){.mag = v});
}

bool ir_instr_push_float(ir_instr_t *in, double v)
{
    ir_operand_t *o = push(in, IR_OPD_CONST);

    if (o) {
        o->lit.f        = v;
        o->lit.is_float = true;
    }

    return o != nullptr;
}

bool ir_instr_push_float_text(ir_instr_t *in, const char *text)
{
    if (!text) {
        return false;
    }

    ir_operand_t *o = push(in, IR_OPD_CONST);

    if (o) {
        o->lit.f        = strtod(text, nullptr);
        o->lit.text     = ir_strdup(in->block->owner->owner->ctx, text);
        o->lit.is_float = true;
    }

    return o != nullptr;
}

bool ir_instr_push_phi_arm(ir_instr_t *in, ir_block_t *b, ir_value_t *v)
{
    ir_operand_t *o = b && v ? push(in, IR_OPD_PHI_ARM) : nullptr;

    if (o) {
        o->phi.block = b;
        o->phi.value = v;
    }

    return o != nullptr;
}

bool ir_instr_push_switch_case(ir_instr_t *in, ir_lit_t v, ir_block_t *b)
{
    ir_operand_t *o = b ? push(in, IR_OPD_SWITCH_ARM) : nullptr;

    if (o) {
        o->arm.value = v;
        o->arm.block = b;
    }

    return o != nullptr;
}

bool ir_instr_push_switch_arm(ir_instr_t *in, ir_int_t v, ir_block_t *b)
{
    return ir_instr_push_switch_case(in, ir_lit_of(v), b);
}

bool ir_instr_push_mod(ir_instr_t *in, const char *mod)
{
    if (!in || !mod) {
        return false;
    }

    ir_module_t *m = in->block->owner->owner;

    if (!ir_opinfo_accepts_mod(ir_module_op_info(m, in->op), mod)) {
        return false;
    }

    ir_ctx_t *ctx = m->ctx;
    size_t    cap = in->cap_mods;

    in->mods = ir_grow(ctx, in->mods, &cap, in->n_mods + 1u, sizeof(char *));

    if (!in->mods) {
        return false;
    }
    in->cap_mods           = (uint32_t)cap;
    in->mods[in->n_mods++] = ir_strdup(ctx, mod);

    return true;
}

bool ir_instr_has_mod(const ir_instr_t *in, const char *mod)
{
    if (!in || !mod) {
        return false;
    }

    for (uint32_t i = 0; i < in->n_mods; i++) {
        if (!strcmp(in->mods[i], mod)) {
            return true;
        }
    }

    return false;
}
