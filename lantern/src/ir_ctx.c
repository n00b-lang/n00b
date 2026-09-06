#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/typelang.h"
#include "ir/walk.h"

#include "lib/dict.h"

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

void drop_dict(void *p)
{
    ncc_dict_free(p);
    free(p);
}

void ir_ctx_on_free(ir_ctx_t *ctx, void (*fn)(void *), void *arg)
{
    ir_cleanup_t *c = ir_alloc(ctx, sizeof(ir_cleanup_t));

    if (!c) {
        return;
    }

    c->fn         = fn;
    c->arg        = arg;
    c->next       = ctx->cleanups;
    ctx->cleanups = c;
}

void ir_ctx_free(ir_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    for (ir_cleanup_t *k = ctx->cleanups; k; k = k->next) {
        k->fn(k->arg);
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

static bool drop_on_global(ir_visit_t *vi)
{
    ir_annot_invalidate(&vi->global->annots, *(ir_props_t *)vi->user);

    return true;
}

static bool drop_on_func(ir_visit_t *vi)
{
    ir_props_t p = *(ir_props_t *)vi->user;

    vi->func->props &= (ir_props_t) ~(uint16_t)p;
    ir_annot_invalidate(&vi->func->annots, p);

    if (p & IR_PROP_STRUCTURED) {
        vi->func->regions = nullptr;
    }

    return true;
}

static bool drop_on_local(ir_visit_t *vi)
{
    ir_annot_invalidate(&vi->local->annots, *(ir_props_t *)vi->user);

    return true;
}

static bool drop_on_block(ir_visit_t *vi)
{
    ir_annot_invalidate(&vi->block->annots, *(ir_props_t *)vi->user);

    return true;
}

static bool drop_on_instr(ir_visit_t *vi)
{
    ir_annot_invalidate(&vi->instr->annots, *(ir_props_t *)vi->user);

    return false;
}

void ir_module_invalidate(ir_module_t *m, ir_props_t p)
{
    if (!m) {
        return;
    }

    m->props &= (ir_props_t) ~(uint16_t)p;
    ir_annot_invalidate(&m->annots, p);

    ir_walk_t *w = ir_walk_new(&p);

    ir_walk_on_global(w, drop_on_global, nullptr);
    ir_walk_on_func(w, drop_on_func, nullptr, nullptr);
    ir_walk_on_local(w, drop_on_local, nullptr);
    ir_walk_on_block(w, drop_on_block, nullptr, nullptr);
    ir_walk_on_instr(w, drop_on_instr, nullptr, nullptr);
    ir_walk_run(w, m);
    ir_walk_free(w);
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

    if (!m->file_index) {
        ncc_dict_t *ix = calloc(1, sizeof(ncc_dict_t));

        if (!ix) {
            return -1;
        }
        ncc_dict_init(ix, ncc_hash_cstring, ncc_dict_cstr_eq);
        ir_ctx_on_free(m->ctx, drop_dict, ix);
        m->file_index = ix;
    }

    bool   found = false;
    size_t at    = (size_t)(uintptr_t)ncc_dict_get((ncc_dict_t *)
                                                       m->file_index,
                                                   path, &found);

    if (found) {
        return (int32_t)at;
    }

    m->files = ir_grow(m->ctx, m->files, &m->cap_files, m->n_files + 1,
                       sizeof(char *));
    m->files[m->n_files] = ir_strdup(m->ctx, path);
    ncc_dict_put((ncc_dict_t *)m->file_index, m->files[m->n_files],
                 m->n_files);

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
