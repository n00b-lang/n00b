#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"

void *ir_alloc(ir_ctx_t *ctx, size_t n)
{
    n = (n + 15u) & ~(size_t)15u;

    if (!ctx->chunks || ctx->chunks->used + n > ctx->chunks->cap) {
        size_t    cap = n > IR_CHUNK ? n : IR_CHUNK;
        ir_chunk *c   = calloc(1, sizeof(ir_chunk) + cap);

        if (!c) {
            return nullptr;
        }
        c->cap      = cap;
        c->next     = ctx->chunks;
        ctx->chunks = c;
    }

    void *r = ctx->chunks->data + ctx->chunks->used;

    ctx->chunks->used += n;

    return r;
}

char *ir_strdup(ir_ctx_t *ctx, const char *s)
{
    if (!s) {
        return nullptr;
    }

    size_t n = strlen(s);
    char  *d = ir_alloc(ctx, n + 1);

    if (d) {
        memcpy(d, s, n + 1);
    }

    return d;
}

void *ir_grow(ir_ctx_t *ctx, void *p, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) {
        return p;
    }

    size_t ncap = *cap ? *cap * 2 : 8;

    while (ncap < need) {
        ncap *= 2;
    }

    void *q = ir_alloc(ctx, ncap * elem);

    if (!q) {
        return p;
    }

    if (p && *cap) {
        memcpy(q, p, *cap * elem);
    }
    *cap = ncap;

    return q;
}

ir_ctx_t *ir_ctx_new(void)
{
    ir_ctx_t *ctx = calloc(1, sizeof(ir_ctx_t));

    if (!ctx) {
        return nullptr;
    }

    ctx->types = th_ctx_new();

    if (!ctx->types) {
        free(ctx);

        return nullptr;
    }

    return ctx;
}

void ir_ctx_free(ir_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ir_chunk *c = ctx->chunks;

    while (c) {
        ir_chunk *next = c->next;

        free(c);
        c = next;
    }
    th_ctx_free(ctx->types);
    free(ctx);
}

th_ctx *ir_ctx_types(ir_ctx_t *ctx)
{
    return ctx ? ctx->types : nullptr;
}

ir_module_t *ir_module_new(ir_ctx_t *ctx, const char *name,
                           const ir_profile_t *profile, const char *target)
{
    if (!ctx || !name || !profile) {
        return nullptr;
    }

    ir_module_t *m = ir_alloc(ctx, sizeof(ir_module_t));

    if (!m) {
        return nullptr;
    }

    m->ctx     = ctx;
    m->name    = ir_strdup(ctx, name);
    m->target  = target ? ir_strdup(ctx, target) : nullptr;
    m->profile = profile;

    return m;
}

const char *ir_module_name(const ir_module_t *m)
{
    return m ? m->name : nullptr;
}

const char *ir_module_target(const ir_module_t *m)
{
    return m ? m->target : nullptr;
}

const ir_profile_t *ir_module_profile(const ir_module_t *m)
{
    return m ? m->profile : nullptr;
}

ir_props_t ir_module_props(const ir_module_t *m)
{
    return m ? m->props : 0;
}

void ir_module_establish(ir_module_t *m, ir_props_t p)
{
    if (m) {
        m->props |= p;
    }
}

void ir_module_invalidate(ir_module_t *m, ir_props_t p)
{
    if (!m) {
        return;
    }

    m->props &= (ir_props_t) ~(uint16_t)p;
    ir_annot_invalidate(&m->annots, p);

    for (size_t i = 0; i < m->n_funcs; i++) {
        ir_func_t *f = m->funcs[i];

        f->props &= (ir_props_t) ~(uint16_t)p;
        ir_annot_invalidate(&f->annots, p);

        for (uint32_t b = 0; b < f->n_blocks; b++) {
            ir_block_t *bb = f->blocks[b];

            ir_annot_invalidate(&bb->annots, p);

            for (uint32_t k = 0; k < bb->n_instrs; k++) {
                ir_annot_invalidate(&bb->instrs[k]->annots, p);
            }
        }

        for (uint32_t l = 0; l < f->n_locals; l++) {
            ir_annot_invalidate(&f->locals[l]->annots, p);
        }
    }

    for (size_t i = 0; i < m->n_globals; i++) {
        ir_annot_invalidate(&m->globals[i]->annots, p);
    }
}

ir_annots_t *ir_module_annots(ir_module_t *m)
{
    return m ? &m->annots : nullptr;
}

const char *ir_prop_name(ir_props_t one)
{
    switch (one) {
    case IR_PROP_PARSED:
        return "parsed";
    case IR_PROP_TYPED:
        return "typed";
    case IR_PROP_SSA:
        return "ssa";
    case IR_PROP_STRUCTURED:
        return "structured";
    case IR_PROP_LAID_OUT:
        return "laid-out";
    case IR_PROP_NO_CRIT_EDGE:
        return "no-critical-edges";
    case IR_PROP_VERIFIED:
        return "verified";
    default:
        return nullptr;
    }
}

th_type *ir_type_intern(ir_module_t *m, th_type *t)
{
    if (!m || !t) {
        return nullptr;
    }

    uint64_t h = th_hash(m->ctx->types, t);

    for (size_t i = 0; i < m->n_types; i++) {
        if (m->types[i].hash == h) {
            return m->types[i].type;
        }
    }

    m->types = ir_grow(m->ctx, m->types, &m->cap_types, m->n_types + 1,
                       sizeof(ir_typeslot_t));
    m->types[m->n_types].hash = h;
    m->types[m->n_types].type = t;
    m->n_types++;

    return t;
}

th_type *ir_type_parse(ir_module_t *m, const char *text)
{
    if (!m || !text) {
        return nullptr;
    }

    th_error err = {0};
    th_type *t   = th_parse(m->ctx->types, text, &err);

    return t ? ir_type_intern(m, t) : nullptr;
}

uint64_t ir_type_id(ir_module_t *m, th_type *t)
{
    return m && t ? th_hash(m->ctx->types, t) : 0;
}

bool ir_type_eq(ir_module_t *m, th_type *a, th_type *b)
{
    if (!m || !a || !b) {
        return a == b;
    }

    return th_hash(m->ctx->types, a) == th_hash(m->ctx->types, b);
}

size_t ir_type_count(const ir_module_t *m)
{
    return m ? m->n_types : 0;
}

int32_t ir_file_add(ir_module_t *m, const char *path)
{
    if (!m || !path) {
        return -1;
    }

    for (size_t i = 0; i < m->n_files; i++) {
        if (!strcmp(m->files[i], path)) {
            return (int32_t)i;
        }
    }

    m->files = ir_grow(m->ctx, m->files, &m->cap_files, m->n_files + 1,
                       sizeof(char *));
    m->files[m->n_files] = ir_strdup(m->ctx, path);

    return (int32_t)m->n_files++;
}

const char *ir_file_path(const ir_module_t *m, int32_t id)
{
    if (!m || id < 0 || (size_t)id >= m->n_files) {
        return nullptr;
    }

    return m->files[id];
}

size_t ir_file_count(const ir_module_t *m)
{
    return m ? m->n_files : 0;
}
