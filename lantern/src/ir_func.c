#include <string.h>

#include "ir_internal.h"

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

    return g;
}

ir_global_t *ir_global_find(ir_module_t *m, const char *name)
{
    if (!m || !name) {
        return nullptr;
    }

    for (size_t i = 0; i < m->n_globals; i++) {
        if (!strcmp(m->globals[i]->name, name)) {
            return m->globals[i];
        }
    }

    return nullptr;
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

    return f;
}

ir_func_t *ir_func_find(ir_module_t *m, const char *name)
{
    if (!m || !name) {
        return nullptr;
    }

    for (size_t i = 0; i < m->n_funcs; i++) {
        if (!strcmp(m->funcs[i]->name, name)) {
            return m->funcs[i];
        }
    }

    return nullptr;
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

    return l;
}

ir_local_t *ir_local_find(ir_func_t *f, const char *name)
{
    if (!f || !name) {
        return nullptr;
    }

    for (uint32_t i = 0; i < f->n_locals; i++) {
        if (!strcmp(f->locals[i]->name, name)) {
            return f->locals[i];
        }
    }

    return nullptr;
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

    return b;
}

ir_block_t *ir_block_find(ir_func_t *f, const char *name)
{
    if (!f || !name) {
        return nullptr;
    }

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        if (!strcmp(f->blocks[i]->name, name)) {
            return f->blocks[i];
        }
    }

    return nullptr;
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

    return ir_op_is_terminator(last->op) ? last : nullptr;
}

ir_instr_t *ir_instr_add(ir_block_t *b, ir_op_t op)
{
    if (!b || op >= IR_OP_COUNT) {
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
    if (!in || !name || !type || !ir_op_has_result(in->op)) {
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
    if (!in || !l || !ir_op_has_result(in->op)) {
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

    ir_ctx_t          *ctx  = in->block->owner->owner->ctx;
    const ir_opinfo_t *info = ir_op_info(in->op);

    if (ir_op_operand_kind(in->op, in->n_operands) != kind) {
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

bool ir_instr_push_int(ir_instr_t *in, int64_t v)
{
    ir_operand_t *o = push(in, IR_OPD_CONST);

    if (o) {
        o->lit.i = v;
    }

    return o != nullptr;
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

bool ir_instr_push_phi_arm(ir_instr_t *in, ir_block_t *b, ir_value_t *v)
{
    ir_operand_t *o = b && v ? push(in, IR_OPD_PHI_ARM) : nullptr;

    if (o) {
        o->phi.block = b;
        o->phi.value = v;
    }

    return o != nullptr;
}

bool ir_instr_push_switch_arm(ir_instr_t *in, int64_t v, ir_block_t *b)
{
    ir_operand_t *o = b ? push(in, IR_OPD_SWITCH_ARM) : nullptr;

    if (o) {
        o->arm.value = v;
        o->arm.block = b;
    }

    return o != nullptr;
}

bool ir_instr_push_mod(ir_instr_t *in, const char *mod)
{
    if (!in || !mod) {
        return false;
    }

    ir_ctx_t *ctx = in->block->owner->owner->ctx;
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
