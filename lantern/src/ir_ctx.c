#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/typelang.h"

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

size_t ir_ctx_bytes(const ir_ctx_t *ctx)
{
    size_t n = 0;

    for (const ir_chunk *c = ctx ? ctx->chunks : nullptr; c; c = c->next) {
        n += c->cap;
    }

    return n;
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

        if (p & IR_PROP_STRUCTURED) {
            f->regions = nullptr;
        }

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

void ir_module_set_profile(ir_module_t *m, const ir_profile_t *p)
{
    if (!m || !p) {
        return;
    }

    m->profile       = p;
    m->rules         = nullptr;
    m->scalar_hashes = nullptr;
    m->scalars_ready = false;
}

ir_span_t ir_module_span(const ir_module_t *m)
{
    return m ? m->span : IR_SPAN_UNKNOWN;
}

void ir_module_set_span(ir_module_t *m, ir_span_t s)
{
    if (m) {
        m->span = s;
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

static size_t type_slot(ir_module_t *m, uint64_t h)
{
    size_t mask = m->type_index_cap - 1;
    size_t i    = (size_t)h & mask;

    while (m->type_index[i]) {
        if (m->types[m->type_index[i] - 1].hash == h) {
            break;
        }
        i = (i + 1) & mask;
    }

    return i;
}

static void type_index_grow(ir_module_t *m)
{
    size_t want = m->type_index_cap ? m->type_index_cap : 64;

    while ((m->n_types + 1) * 2 > want) {
        want *= 2;
    }

    if (want == m->type_index_cap && m->type_index) {
        return;
    }

    m->type_index     = ir_alloc(m->ctx, want * sizeof(uint32_t));
    m->type_index_cap = want;

    for (size_t k = 0; k < m->n_types; k++) {
        m->type_index[type_slot(m, m->types[k].hash)] = (uint32_t)(k + 1);
    }
}

static ir_typeslot_t *type_find(ir_module_t *m, uint64_t h)
{
    if (!m->type_index_cap) {
        return nullptr;
    }

    uint32_t at = m->type_index[type_slot(m, h)];

    return at ? &m->types[at - 1] : nullptr;
}

th_type *ir_type_intern(ir_module_t *m, th_type *t)
{
    if (!m || !t) {
        return nullptr;
    }

    uint64_t       h    = ir_type_id(m, t);
    ir_typeslot_t *slot = type_find(m, h);

    if (slot) {
        return slot->type;
    }

    m->types = ir_grow(m->ctx, m->types, &m->cap_types, m->n_types + 1,
                       sizeof(ir_typeslot_t));
    m->types[m->n_types].hash = h;
    m->types[m->n_types].type = t;
    m->types[m->n_types].def  = nullptr;
    m->n_types++;
    type_index_grow(m);
    m->type_index[type_slot(m, h)] = (uint32_t)m->n_types;

    return t;
}

ir_typeslot_t *ir_type_slot(ir_module_t *m, th_type *t)
{
    if (!m || !t) {
        return nullptr;
    }
    ir_type_intern(m, t);

    return type_find(m, ir_type_id(m, t));
}

ir_typeslot_t *ir_type_slot_at(ir_module_t *m, size_t i)
{
    return m && i < m->n_types ? &m->types[i] : nullptr;
}

th_type *ir_type_at(ir_module_t *m, size_t i)
{
    return m && i < m->n_types ? m->types[i].type : nullptr;
}

bool ir_type_define(ir_module_t *m, th_type *named, th_type *def)
{
    if (!m || !named || !def || th_kind_of(named) != TH_NAMED) {
        return false;
    }

    ir_type_intern(m, named);
    ir_type_intern(m, def);

    ir_typeslot_t *slot = type_find(m, th_hash(m->ctx->types, named));

    if (!slot) {
        return false;
    }
    slot->def = def;

    return true;
}

th_type *ir_type_definition(ir_module_t *m, th_type *named)
{
    if (!m || !named || th_kind_of(named) != TH_NAMED) {
        return nullptr;
    }

    ir_typeslot_t *slot = type_find(m, th_hash(m->ctx->types, named));

    return slot ? slot->def : nullptr;
}

th_type *ir_type_parse(ir_module_t *m, const char *text)
{
    if (!m || !text) {
        return nullptr;
    }

    char    *err = nullptr;
    th_type *t   = ir_typelang_parse(m->ctx->types, text, &err);

    free(err);

    return t ? ir_type_intern(m, t) : nullptr;
}

static size_t id_slot(th_type **key, size_t cap, const th_type *t)
{
    size_t h = (size_t)((uintptr_t)t >> 4) * 11400714819323198485u;
    size_t i = h & (cap - 1);

    while (key[i] && key[i] != t) {
        i = (i + 1) & (cap - 1);
    }

    return i;
}

uint64_t ir_type_id(ir_module_t *m, th_type *t)
{
    if (!m || !t) {
        return 0;
    }

    if ((m->id_n + 1) * 2 > m->id_cap) {
        size_t    cap = m->id_cap ? m->id_cap * 2 : 64;
        th_type **key = ir_alloc(m->ctx, cap * sizeof(th_type *));
        uint64_t *val = ir_alloc(m->ctx, cap * sizeof(uint64_t));

        if (!key || !val) {
            return th_hash(m->ctx->types, t);
        }

        for (size_t i = 0; i < m->id_cap; i++) {
            if (m->id_key[i]) {
                size_t at = id_slot(key, cap, m->id_key[i]);

                key[at] = m->id_key[i];
                val[at] = m->id_val[i];
            }
        }
        m->id_key = key;
        m->id_val = val;
        m->id_cap = cap;
    }

    size_t at = id_slot(m->id_key, m->id_cap, t);

    if (!m->id_key[at]) {
        m->id_key[at] = t;
        m->id_val[at] = th_hash(m->ctx->types, t);
        m->id_n++;
    }

    return m->id_val[at];
}

bool ir_type_eq(ir_module_t *m, th_type *a, th_type *b)
{
    if (a == b) {
        return true;
    }

    if (!m || !a || !b) {
        return false;
    }

    return ir_type_id(m, a) == ir_type_id(m, b);
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
