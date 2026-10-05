#define XXH_INLINE_ALL
#include <xxhash.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "th/typehash.h"
#include "private/th/internal.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "lib/option.h"

void *th_alloc(th_ctx *ctx, size_t n)
{
    return core_arena_alloc(&ctx->arena, n);
}

char *th_strdup(th_ctx *ctx, const char *s, size_t n)
{
    char *d = th_alloc(ctx, n + 1);

    memcpy(d, s, n);
    d[n] = 0;

    return d;
}

th_ctx *th_ctx_new(void)
{
    th_ctx *ctx = core_alloc(th_ctx);

    memset(ctx, 0, sizeof(*ctx));
    core_arena_init(&ctx->arena, 0);
    ctx->type_definitions = core_list_new(th_type_definition_t);

    return ctx;
}

void th_ctx_free(th_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->promo_index) {
        core_dict_foreach (ctx->promo_index, e) {
            core_list_t(core_const_charptr_t) *edges = e->value;

            core_list_free(*edges);
            core_free(edges);
        }

        core_dict_free(ctx->promo_index);
        core_free(ctx->promo_index);
    }

    if (ctx->iface_index) {
        core_dict_free(ctx->iface_index);
        core_free(ctx->iface_index);
    }

    core_list_free(ctx->promos);
    core_list_free(ctx->ifaces);
    core_list_free(ctx->type_definitions);
    core_list_free(ctx->methods);
    core_list_free(ctx->errors);
    core_list_free(ctx->coercions);
    core_list_free(ctx->pending_environment_constraints);
    core_list_free(ctx->active_environment_constraints);
    core_arena_free(&ctx->arena);
    core_buffer_release(&ctx->scratch);
    core_free(ctx);
}

typedef struct {
    const th_type *source;
    th_type *copy;
} th_clone_entry_t;
core_list_decl(th_clone_entry_t);

static const char *
clone_string(th_ctx *destination, const char *source)
{
    return source ? th_strdup(destination, source, strlen(source)) : nullptr;
}

static th_type *clone_type_graph(th_ctx *destination, const th_type *source,
                                 core_list_t(th_clone_entry_t) *seen);

static bool
clone_constraint_graph(th_ctx *destination, const th_constraint *source,
                       th_constraint *copy,
                       core_list_t(th_clone_entry_t) *seen)
{
    *copy = *source;
    copy->scope_module = clone_string(destination, source->scope_module);
    copy->scope_package = clone_string(destination, source->scope_package);
    switch (source->kind) {
    case TH_CON_UNIFIES:
    case TH_CON_PROMOTES:
    case TH_CON_EXCLUDES:
        copy->target = clone_type_graph(destination, source->target, seen);
        return !source->target || copy->target;
    case TH_CON_ONE_OF:
        copy->one_of.types = source->one_of.n
            ? core_alloc_array(th_type *, source->one_of.n) : nullptr;
        for (size_t i = 0; i < source->one_of.n; i++) {
            copy->one_of.types[i] = clone_type_graph(destination,
                source->one_of.types[i], seen);
            if (!copy->one_of.types[i]) return false;
        }
        return true;
    case TH_CON_IMPLEMENTS:
        copy->iface = clone_string(destination, source->iface);
        return !source->iface || copy->iface;
    case TH_CON_KIND:
        return true;
    case TH_CON_HAS_FIELD:
        copy->field.name = clone_string(destination, source->field.name);
        copy->field.type = clone_type_graph(destination, source->field.type,
                                             seen);
        return (!source->field.name || copy->field.name)
            && (!source->field.type || copy->field.type);
    case TH_CON_HAS_PARAM:
        copy->param.type = clone_type_graph(destination, source->param.type,
                                            seen);
        return !source->param.type || copy->param.type;
    case TH_CON_METHOD:
        copy->method.name = clone_string(destination, source->method.name);
        copy->method.result = clone_type_graph(destination,
            source->method.result, seen);
        copy->method.call = clone_type_graph(destination,
            source->method.call, seen);
        return (!source->method.name || copy->method.name)
            && (!source->method.result || copy->method.result)
            && (!source->method.call || copy->method.call);
    }
    return false;
}

static th_type *
clone_type_graph(th_ctx *destination, const th_type *source,
                core_list_t(th_clone_entry_t) *seen)
{
    if (!source) return nullptr;
    for (size_t i = 0; i < seen->len; i++)
        if (seen->data[i].source == source) return seen->data[i].copy;

    th_type *copy = th_alloc(destination, sizeof(*copy));
    if (!copy) return nullptr;
    memset(copy, 0, sizeof(*copy));
    copy->kind = source->kind;
    core_list_push(*seen, ((th_clone_entry_t){source, copy}));
    switch (source->kind) {
    case TH_NAMED:
        copy->named = source->named;
        copy->named.name = clone_string(destination, source->named.name);
        copy->named.args = source->named.nargs
            ? core_alloc_array(th_arg, source->named.nargs) : nullptr;
        for (size_t i = 0; i < source->named.nargs; i++) {
            copy->named.args[i] = source->named.args[i];
            if (source->named.args[i].kind == TH_ARG_TYPE) {
                copy->named.args[i].type = clone_type_graph(destination,
                    source->named.args[i].type, seen);
                if (!copy->named.args[i].type) return nullptr;
            }
            else if (source->named.args[i].kind == TH_ARG_STR) {
                size_t len = source->named.args[i].str.n;
                char *text = th_strdup(destination,
                    source->named.args[i].str.p, len);
                if (!text) return nullptr;
                copy->named.args[i].str.p = text;
            }
        }
        return copy->named.name ? copy : nullptr;
    case TH_BITINT:
        copy->bitint = source->bitint;
        return copy;
    case TH_INDIRECTION:
        copy->ind.attrs = source->ind.attrs;
        copy->ind.referent = clone_type_graph(destination,
            source->ind.referent, seen);
        return copy->ind.referent ? copy : nullptr;
    case TH_SEQUENCE:
        copy->seq = source->seq;
        copy->seq.elem = clone_type_graph(destination, source->seq.elem, seen);
        return copy->seq.elem ? copy : nullptr;
    case TH_RECORD:
        copy->record.n = source->record.n;
        copy->record.members = source->record.n
            ? core_alloc_array(th_member, source->record.n) : nullptr;
        for (size_t i = 0; i < source->record.n; i++) {
            copy->record.members[i].name = clone_string(destination,
                source->record.members[i].name);
            copy->record.members[i].type = clone_type_graph(destination,
                source->record.members[i].type, seen);
            if (!copy->record.members[i].name
                || !copy->record.members[i].type) return nullptr;
        }
        return copy;
    case TH_FUNCTION:
        copy->fn.ret = clone_type_graph(destination, source->fn.ret, seen);
        copy->fn.error = clone_type_graph(destination, source->fn.error, seen);
        copy->fn.np = source->fn.np;
        copy->fn.params = source->fn.np
            ? core_alloc_array(th_param, source->fn.np) : nullptr;
        for (size_t i = 0; i < source->fn.np; i++) {
            copy->fn.params[i] = source->fn.params[i];
            copy->fn.params[i].type = clone_type_graph(destination,
                source->fn.params[i].type, seen);
            if (!copy->fn.params[i].type) return nullptr;
        }
        if (source->fn.rest) {
            copy->fn.rest = th_alloc(destination, sizeof(*copy->fn.rest));
            if (!copy->fn.rest) return nullptr;
            *copy->fn.rest = *source->fn.rest;
            copy->fn.rest->type = clone_type_graph(destination,
                source->fn.rest->type, seen);
            if (!copy->fn.rest->type) return nullptr;
        }
        copy->fn.nkw = source->fn.nkw;
        copy->fn.kw = source->fn.nkw
            ? core_alloc_array(th_kwparam, source->fn.nkw) : nullptr;
        for (size_t i = 0; i < source->fn.nkw; i++) {
            copy->fn.kw[i] = source->fn.kw[i];
            copy->fn.kw[i].name = clone_string(destination,
                source->fn.kw[i].name);
            copy->fn.kw[i].type = clone_type_graph(destination,
                source->fn.kw[i].type, seen);
            if (!copy->fn.kw[i].name || !copy->fn.kw[i].type)
                return nullptr;
        }
        return copy->fn.ret ? copy : nullptr;
    case TH_BINDER:
        copy->binder.arity = source->binder.arity;
        copy->binder.body = clone_type_graph(destination,
            source->binder.body, seen);
        if (source->binder.qvars) {
            copy->binder.qvars = core_alloc_array(th_type *,
                (size_t)source->binder.arity);
            for (uint64_t i = 0; i < source->binder.arity; i++) {
                copy->binder.qvars[i] = clone_type_graph(destination,
                    source->binder.qvars[i], seen);
                if (!copy->binder.qvars[i]) return nullptr;
            }
        }
        return copy->binder.body ? copy : nullptr;
    case TH_BOUND:
        copy->bound = source->bound;
        return copy;
    case TH_SUM:
        copy->sum.comb = source->sum.comb;
        copy->sum.n = source->sum.n;
        copy->sum.arms = source->sum.n
            ? core_alloc_array(th_type *, source->sum.n) : nullptr;
        for (size_t i = 0; i < source->sum.n; i++) {
            copy->sum.arms[i] = clone_type_graph(destination,
                source->sum.arms[i], seen);
            if (!copy->sum.arms[i]) return nullptr;
        }
        return copy;
    case TH_VAR:
        copy->var.index = source->var.index;
        copy->var.has_index = source->var.has_index;
        copy->var.name = clone_string(destination, source->var.name);
        copy->var.ref = clone_type_graph(destination, source->var.ref, seen);
        copy->var.cons = core_list_new(th_constraint);
        for (size_t i = 0; i < source->var.cons.len; i++) {
            th_constraint constraint;
            if (!clone_constraint_graph(destination,
                    &source->var.cons.data[i], &constraint, seen))
                return nullptr;
            core_list_push(copy->var.cons, constraint);
        }
        return copy;
    }
    return nullptr;
}

th_type *
th_type_clone(th_ctx *destination, const th_type *source)
{
    if (!destination || !source) return nullptr;
    core_list_t(th_clone_entry_t) seen = core_list_new(th_clone_entry_t);
    th_type *copy = clone_type_graph(destination, source, &seen);
    core_list_free(seen);
    return copy;
}

void th_ctx_set_normalizer(th_ctx *ctx, th_normalizer fn, void *state)
{
    ctx->norm       = fn;
    ctx->norm_state = state;
}

const char *th_last_error(const th_ctx *ctx)
{
    return ctx->err;
}

th_kind th_kind_of(const th_type *t)
{
    return t->kind;
}

bool th_is_scheme(const th_type *t)
{
    return t->kind == TH_BINDER;
}

static void u8(core_buffer_t *b, uint8_t v)
{
    core_buffer_append(b, (const char *)&v, 1);
}

static void u64(core_buffer_t *b, uint64_t v)
{
    char t[8];

    for (int i = 0; i < 8; i++) {
        t[i] = (char)(v >> (56 - 8 * i));
    }

    core_buffer_append(b, t, 8);
}

static void str(core_buffer_t *b, const char *s, size_t n)
{
    u64(b, n);
    core_buffer_append(b, s, n);
}

static bool is_start(unsigned char c)
{
    return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || c >= 0x80;
}

static bool is_cont(unsigned char c)
{
    return is_start(c) || (c >= '0' && c <= '9');
}

static bool valid_name(const char *s, size_t n)
{
    if (!n) {
        return false;
    }

    size_t seg = 0;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '.') {
            if (!seg) {
                return false;
            }
            seg = 0;
            continue;
        }
        if (seg == 0 ? !is_start(c) : !is_cont(c)) {
            return false;
        }
        seg++;
    }

    return seg != 0;
}

static bool name(th_ctx *ctx, core_buffer_t *b, const char *s, size_t n)
{
    if (!valid_name(s, n)) {
        th_err(ctx, TH_ERR_BAD_NAME, nullptr, nullptr, nullptr,
               "not a valid type name");

        return false;
    }

    if (ctx->norm) {
        const char *o;
        size_t      on;

        if (!ctx->norm(ctx->norm_state, s, n, &o, &on)) {
            th_err(ctx, TH_ERR_BAD_NAME, nullptr, nullptr, nullptr,
                   "name normalization failed");

            return false;
        }

        str(b, o, on);

        return true;
    }

    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)s[i] >= 0x80) {
            th_err(ctx, TH_ERR_BAD_NAME, nullptr, nullptr, nullptr,
                   "non-ASCII name requires an NFC normalizer; "
                   "call th_ctx_set_normalizer");

            return false;
        }
    }

    str(b, s, n);

    return true;
}

typedef const th_type *th_type_cptr_t;

core_option_decl(th_type_cptr_t);
core_list_decl(th_type_cptr_t);

typedef struct {
    core_dict_t                 at;
    core_list_t(th_type_cptr_t) stack;
} th_path;

static void pm_init(th_path *p)
{
    core_dict_init(&p->at, nullptr, nullptr);
    p->stack = core_list_new(th_type_cptr_t);
}

static size_t pm_depth(const th_path *p)
{
    return p->stack.len;
}

static size_t pm_find(const th_path *p, const th_type *t)
{
    bool   found;
    size_t at = (size_t)(uintptr_t)core_dict_get((core_dict_t *)&p->at, t,
                                               &found);

    return found ? at - 1 : SIZE_MAX;
}

static void pm_push(th_path *p, const th_type *t)
{
    core_dict_put(&p->at, t, p->stack.len + 1);
    core_list_push(p->stack, t);
}

static void pm_pop(th_path *p)
{
    const th_type *t = core_option_get(core_list_pop(th_type_cptr_t, p->stack));

    core_dict_remove(&p->at, t);
}

static void pm_free(th_path *p)
{
    core_dict_free(&p->at);
    core_list_free(p->stack);
}

typedef struct {
    th_path  path;
    bool     erase;
    th_ctx  *ctx;
    bool     ok;
} th_enc;

static void enc_node(th_enc *e, core_buffer_t *b, const th_type *t);

int th_cmp_slice(const void *a, const void *b)
{
    const th_slice *x = a, *y = b;
    size_t          m = x->n < y->n ? x->n : y->n;
    int             c = memcmp(x->p, y->p, m);

    if (c) {
        return c;
    }

    return x->n < y->n ? -1 : x->n > y->n ? 1 : 0;
}

typedef struct {
    core_buffer_t   pre;
    const th_type *child;
} estep;

core_option_decl(estep);
core_list_decl(estep);

typedef struct {
    const th_type    *t;
    core_buffer_t     *out;
    core_list_t(estep) steps;
    size_t            ninline;
    size_t            at;
    core_buffer_t     *side;
    core_buffer_t      post;
    bool              entered;
    bool              pushed;
} eframe;

core_option_decl(eframe);
core_list_decl(eframe);

static estep *step_add(core_list_t(estep) *v, const th_type *child)
{
    core_list_push(*v, ((estep){.child = child}));

    return &v->data[v->len - 1];
}

static bool enc_plan(th_enc *e, eframe *f)
{
    const th_type *t = f->t;
    estep         *s = nullptr;

    f->steps   = core_list_new(estep);
    f->ninline = 0;

    s = step_add(&f->steps, nullptr);

    u8(&s->pre, (uint8_t)(t->kind + 1));

    switch (t->kind) {
    case TH_NAMED:
        u8(&s->pre, t->named.is_variant ? 1 : 0);

        if (!name(e->ctx, &s->pre, t->named.name, t->named.nlen)) {
            return false;
        }

        u64(&s->pre, t->named.nargs);

        for (size_t i = 0; i < t->named.nargs; i++) {
            const th_arg *a  = &t->named.args[i];
            bool          ty = a->kind == TH_ARG_TYPE;

            s = step_add(&f->steps, ty ? a->type : nullptr);

            switch (a->kind) {
            case TH_ARG_TYPE:
                u8(&s->pre, 0);
                break;
            case TH_ARG_UINT:
                u8(&s->pre, 1);
                u64(&s->pre, a->u);
                break;
            case TH_ARG_SINT:
                u8(&s->pre, 2);
                u64(&s->pre, (uint64_t)a->s);
                break;
            case TH_ARG_STR:
                u8(&s->pre, 3);
                str(&s->pre, a->str.p, a->str.n);
                break;
            }
        }
        break;

    case TH_BITINT:
        u64(&s->pre, t->bitint.width);
        u8(&s->pre, t->bitint.is_signed ? 1 : 0);
        break;

    case TH_INDIRECTION:
        s->child = t->ind.referent;
        u8(&s->pre, t->ind.attrs);
        break;

    case TH_SEQUENCE:
        s->child = t->seq.elem;

        if (t->seq.has_extent) {
            u8(&s->pre, 1);
            u64(&s->pre, t->seq.extent);
        }
        else {
            u8(&s->pre, 0);
        }
        break;

    case TH_RECORD:
        f->ninline = f->steps.len;

        for (size_t i = 0; i < t->record.n; i++) {
            const th_member *m = &t->record.members[i];

            s = step_add(&f->steps, m->type);

            if (!name(e->ctx, &s->pre, m->name, strlen(m->name))) {
                return false;
            }
        }

        u64(&f->post, t->record.n);
        break;

    case TH_FUNCTION: {
        uint8_t flags = (uint8_t)((t->fn.rest ? 1 : 0) |
                                  (t->fn.nkw ? 2 : 0) |
                                  (t->fn.error ? 4 : 0));

        s->child = t->fn.ret;
        u8(&s->pre, flags);

        s = step_add(&f->steps, t->fn.np ? t->fn.params[0].type : nullptr);

        u64(&s->pre, t->fn.np);

        if (t->fn.np) {
            u8(&s->pre, t->fn.params[0].mode);
        }

        for (size_t i = 1; i < t->fn.np; i++) {
            s = step_add(&f->steps, t->fn.params[i].type);
            u8(&s->pre, t->fn.params[i].mode);
        }

        if (t->fn.rest) {
            s = step_add(&f->steps, t->fn.rest->type);
            u8(&s->pre, t->fn.rest->mode);
        }

        f->ninline = f->steps.len;

        for (size_t i = 0; i < t->fn.nkw; i++) {
            const th_kwparam *k = &t->fn.kw[i];

            s = step_add(&f->steps, k->type);

            if (!name(e->ctx, &s->pre, k->name, strlen(k->name))) {
                return false;
            }

            u8(&s->pre, k->mode);
        }

        if (t->fn.nkw) {
            u64(&f->post, t->fn.nkw);
        }
        if (t->fn.error) {
            step_add(&f->steps, t->fn.error);
        }
        break;
    }

    case TH_BINDER:
        s->child = t->binder.body;
        u64(&s->pre, t->binder.arity);
        break;

    case TH_BOUND:
        u64(&s->pre, t->bound.distance);
        u64(&s->pre, t->bound.position);
        break;

    case TH_VAR:
        if (e->erase) {
            u64(&s->pre, 0);
        }
        else if (!t->var.has_index) {
            th_err(e->ctx, TH_ERR_FREE_VAR, nullptr, nullptr, (th_type *)t,
                   "free variable has no index; normalize first");

            return false;
        }
        else {
            u64(&s->pre, t->var.index);
        }
        break;

    case TH_SUM:
        f->ninline = f->steps.len;

        for (size_t i = 0; i < t->sum.n; i++) {
            step_add(&f->steps, t->sum.arms[i]);
        }

        u8(&f->post, t->sum.comb);
        u64(&f->post, t->sum.n);
        break;

    default:
        break;
    }

    if (!f->ninline) {
        f->ninline = f->steps.len;
    }

    size_t nside = f->steps.len - f->ninline;

    if (nside) {
        f->side = core_alloc_array(core_buffer_t, nside);
    }

    return true;
}

static void emit(core_buffer_t *to, const core_buffer_t *from)
{
    core_buffer_append(to, from->data, from->byte_len);
}

static void enc_splice(eframe *f)
{
    size_t n = f->steps.len - f->ninline;

    emit(f->out, &f->post);

    if (!n) {
        return;
    }

    th_slice *sl = core_alloc_array(th_slice, n);

    for (size_t i = 0; i < n; i++) {
        sl[i].p = (uint8_t *)f->side[i].data;
        sl[i].n = f->side[i].byte_len;
    }

    qsort(sl, n, sizeof(th_slice), th_cmp_slice);

    for (size_t i = 0; i < n; i++) {
        core_buffer_append(f->out, (const char *)sl[i].p, sl[i].n);
    }

    core_free(sl);
}

static void enc_frame_free(eframe *f)
{
    size_t nsteps = f->steps.len;

    for (size_t i = 0; i < nsteps; i++) {
        core_buffer_release(&f->steps.data[i].pre);
    }
    core_list_free(f->steps);

    for (size_t i = f->ninline; i < nsteps; i++) {
        core_buffer_release(&f->side[i - f->ninline]);
    }
    core_free(f->side);
    core_buffer_release(&f->post);
}

static void enc_node(th_enc *e, core_buffer_t *b, const th_type *t)
{
    if (!e->ok) {
        return;
    }

    core_list_t(eframe) st = core_list_new_cap(eframe, 16);

    core_list_push(st, ((eframe){.t = t, .out = b}));

    while (st.len && e->ok) {
        eframe *f = &st.data[st.len - 1];

        if (!f->entered) {
            f->entered = true;
            f->t       = th_resolve_c(f->t);

            if (!f->t) {
                u8(f->out, 0xFF);
                core_list_pop(eframe, st);

                continue;
            }

            size_t back = pm_find(&e->path, f->t);

            if (back != SIZE_MAX) {
                u8(f->out, 0xFE);
                u64(f->out, pm_depth(&e->path) - back);
                core_list_pop(eframe, st);

                continue;
            }

            pm_push(&e->path, f->t);
            f->pushed = true;

            if (!enc_plan(e, f)) {
                e->ok = false;

                break;
            }
            f = &st.data[st.len - 1];
        }

        if (f->at < f->steps.len) {
            size_t        i  = f->at++;
            core_buffer_t *to = i < f->ninline ? f->out
                                              : &f->side[i - f->ninline];

            emit(to, &f->steps.data[i].pre);

            const th_type *child = f->steps.data[i].child;

            if (!child) {
                continue;
            }

            core_list_push(st, ((eframe){.t = child, .out = to}));

            continue;
        }

        enc_splice(f);

        if (f->pushed) {
            pm_pop(&e->path);
        }
        enc_frame_free(f);
        core_list_pop(eframe, st);
    }

    while (st.len) {
        eframe done = core_option_get(core_list_pop(eframe, st));

        if (done.entered) {
            enc_frame_free(&done);
        }
    }
    core_list_free(st);
}

typedef struct {
    core_dict_t seen;
    th_path    path;
    th_ctx    *ctx;
} th_norm;

static void norm_walk(th_norm *s, th_type *t);

void th_erase_encode(th_ctx *ctx, const th_type *t, core_buffer_t *out)
{
    th_enc e = {.ctx = ctx, .erase = true, .ok = true};

    pm_init(&e.path);
    enc_node(&e, out, t);
    pm_free(&e.path);
}

void th_full_encode(th_ctx *ctx, const th_type *t, core_buffer_t *out)
{
    th_enc e = {.ctx = ctx, .ok = true};

    pm_init(&e.path);
    enc_node(&e, out, t);
    pm_free(&e.path);
}

typedef struct {
    th_slice key;
    th_type *arm;
} th_armkey;

static int cmp_armkey(const void *a, const void *b)
{
    const th_armkey *x = a, *y = b;

    return th_cmp_slice(&x->key, &y->key);
}

th_type **th_sort_arms(th_ctx *ctx, th_type *const *arms, size_t n)
{
    th_armkey    *ak = core_alloc_array(th_armkey, n);
    core_buffer_t *tb = core_alloc_array(core_buffer_t, n);
    th_type     **ar = core_alloc_array(th_type *, n);

    for (size_t i = 0; i < n; i++) {
        th_erase_encode(ctx, arms[i], &tb[i]);
        ak[i].key.p = (uint8_t *)tb[i].data;
        ak[i].key.n = tb[i].byte_len;
        ak[i].arm   = arms[i];
    }

    qsort(ak, n, sizeof(th_armkey), cmp_armkey);

    for (size_t i = 0; i < n; i++) {
        ar[i] = ak[i].arm;
        core_buffer_release(&tb[i]);
    }
    core_free(tb);
    core_free(ak);

    return ar;
}

static th_type **norm_children_sum(th_norm *s, th_type *t, size_t *out)
{
    *out = t->sum.n;

    return th_sort_arms(s->ctx, t->sum.arms, t->sum.n);
}

int th_cmp_member(const void *a, const void *b)
{
    const th_member *x = a, *y = b;
    size_t           xn = strlen(x->name), yn = strlen(y->name);

    if (xn != yn) {
        return xn < yn ? -1 : 1;
    }

    return memcmp(x->name, y->name, xn);
}

int th_cmp_kw(const void *a, const void *b)
{
    const th_kwparam *x = a, *y = b;
    size_t            xn = strlen(x->name), yn = strlen(y->name);

    if (xn != yn) {
        return xn < yn ? -1 : 1;
    }

    return memcmp(x->name, y->name, xn);
}

typedef struct {
    th_type  *t;
    th_type **kids;
    size_t    n;
    size_t    next;
    bool      entered;
} th_nframe;

core_option_decl(th_nframe);
core_list_decl(th_nframe);

static th_type **norm_children(th_norm *s, th_type *t, size_t *out)
{
    th_type **kids = nullptr;
    size_t    n    = 0;

    *out = 0;

    switch (t->kind) {
    case TH_VAR:
        if (core_dict_contains(&s->seen, t)) {
            return nullptr;
        }

        t->var.index     = core_dict_len(&s->seen);
        t->var.has_index = true;

        core_dict_put(&s->seen, t, t);

        return nullptr;

    case TH_NAMED:
        kids = core_alloc_array(th_type *, t->named.nargs);

        for (size_t i = 0; i < t->named.nargs; i++) {
            if (t->named.args[i].kind == TH_ARG_TYPE) {
                kids[n++] = t->named.args[i].type;
            }
        }
        break;

    case TH_INDIRECTION:
        kids      = core_alloc_array(th_type *, 1);
        kids[n++] = t->ind.referent;
        break;

    case TH_SEQUENCE:
        kids      = core_alloc_array(th_type *, 1);
        kids[n++] = t->seq.elem;
        break;

    case TH_RECORD: {
        th_member *m = core_alloc_array(th_member, t->record.n);

        memcpy(m, t->record.members, t->record.n * sizeof(th_member));
        qsort(m, t->record.n, sizeof(th_member), th_cmp_member);

        kids = core_alloc_array(th_type *, t->record.n);

        for (size_t i = 0; i < t->record.n; i++) {
            kids[n++] = m[i].type;
        }
        core_free(m);
        break;
    }

    case TH_FUNCTION: {
        size_t want = t->fn.np + t->fn.nkw + 3;

        kids      = core_alloc_array(th_type *, want);
        kids[n++] = t->fn.ret;

        for (size_t i = 0; i < t->fn.np; i++) {
            kids[n++] = t->fn.params[i].type;
        }

        if (t->fn.rest) {
            kids[n++] = t->fn.rest->type;
        }

        if (t->fn.nkw) {
            th_kwparam *k = core_alloc_array(th_kwparam, t->fn.nkw);

            memcpy(k, t->fn.kw, t->fn.nkw * sizeof(th_kwparam));
            qsort(k, t->fn.nkw, sizeof(th_kwparam), th_cmp_kw);

            for (size_t i = 0; i < t->fn.nkw; i++) {
                kids[n++] = k[i].type;
            }
            core_free(k);
        }
        if (t->fn.error) kids[n++] = t->fn.error;
        break;
    }

    case TH_BINDER:
        kids      = core_alloc_array(th_type *, 1);
        kids[n++] = t->binder.body;
        break;

    case TH_SUM:
        return norm_children_sum(s, t, out);

    default:
        return nullptr;
    }

    *out = n;

    return kids;
}

static void norm_walk(th_norm *s, th_type *root)
{
    core_list_t(th_nframe) st = core_list_new_cap(th_nframe, 16);

    core_list_push(st, ((th_nframe){.t = root}));

    while (st.len) {
        th_nframe *f = &st.data[st.len - 1];

        if (!f->entered) {
            f->entered = true;
            f->t       = (th_type *)th_resolve_c(f->t);

            if (!f->t || pm_find(&s->path, f->t) != SIZE_MAX) {
                f->t = nullptr;
                core_list_pop(th_nframe, st);

                continue;
            }

            pm_push(&s->path, f->t);

            f->kids = norm_children(s, f->t, &f->n);
            f       = &st.data[st.len - 1];
        }

        if (f->next < f->n) {
            th_type *kid = f->kids[f->next++];

            core_list_push(st, ((th_nframe){.t = kid}));

            continue;
        }

        pm_pop(&s->path);
        core_free(f->kids);
        core_list_pop(th_nframe, st);
    }

    core_list_free(st);
}

void th_normalize(th_type *t)
{
    th_ctx  tmp = {0};
    th_norm s   = {.ctx = &tmp};

    core_dict_init(&s.seen, nullptr, nullptr);
    pm_init(&s.path);
    norm_walk(&s, t);
    core_dict_free(&s.seen);
    pm_free(&s.path);
}

bool th_encode(th_ctx *ctx, th_type *t, const uint8_t **out, size_t *len)
{
    th_normalize(t);
    ctx->scratch.byte_len = 0;

    th_enc e = {.ctx = ctx, .ok = true};

    pm_init(&e.path);
    enc_node(&e, &ctx->scratch, t);
    pm_free(&e.path);

    if (!e.ok) {
        return false;
    }
    *out = (const uint8_t *)ctx->scratch.data;
    *len = ctx->scratch.byte_len;

    return true;
}

uint64_t th_hash(th_ctx *ctx, th_type *t)
{
    const uint8_t *p;
    size_t         n;

    if (!th_encode(ctx, t, &p, &n)) {
        return 0;
    }

    uint8_t ver[4] = {(uint8_t)(TH_VERSION >> 24), (uint8_t)(TH_VERSION >> 16),
                      (uint8_t)(TH_VERSION >> 8), (uint8_t)TH_VERSION};
    XXH3_state_t st;

    XXH3_128bits_reset(&st);
    XXH3_128bits_update(&st, ver, sizeof ver);
    XXH3_128bits_update(&st, p, n);

    return XXH3_128bits_digest(&st).low64;
}

uint64_t th_arm_tag(const char *n)
{
    uint64_t t = XXH3_64bits(n, strlen(n));

    return t ? t : 1;
}

const char *th_anon_name(th_ctx *ctx, const char *path, uint64_t line,
                         uint64_t col)
{
    size_t       plen = strlen(path);
    uint8_t      head[8];
    XXH3_state_t st;

    for (int i = 0; i < 8; i++) {
        head[i] = (uint8_t)(plen >> (56 - 8 * i));
    }

    XXH3_128bits_reset(&st);
    XXH3_128bits_update(&st, head, sizeof head);
    XXH3_128bits_update(&st, path, plen);

    uint64_t at[2] = {line, col};

    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 8; i++) {
            head[i] = (uint8_t)(at[k] >> (56 - 8 * i));
        }
        XXH3_128bits_update(&st, head, sizeof head);
    }

    uint64_t d = XXH3_128bits_digest(&st).low64;

    d = d ? d : 1;

    char *s = th_alloc(ctx, 32);

    snprintf(s, 32, "_anon_%016llx", (unsigned long long)d);

    return s;
}

static th_type *node(th_ctx *ctx, th_kind k)
{
    th_type *t = th_alloc(ctx, sizeof(th_type));

    t->kind = k;

    return t;
}

th_type *th_name(th_ctx *ctx, const char *n)
{
    return th_apply(ctx, n, nullptr, 0);
}

static th_type *make_named(th_ctx *ctx, const char *n, const th_arg *a,
                           size_t cnt, bool variant)
{
    th_type *t = node(ctx, TH_NAMED);

    if (!t) {
        return nullptr;
    }

    size_t ln = strlen(n);

    t->named.name       = th_strdup(ctx, n, ln);
    t->named.nlen       = ln;
    t->named.is_variant = variant;
    t->named.nargs      = cnt;

    if (cnt) {
        t->named.args = th_alloc(ctx, cnt * sizeof(th_arg));
        memcpy(t->named.args, a, cnt * sizeof(th_arg));

        for (size_t i = 0; i < cnt; i++) {
            if (t->named.args[i].kind == TH_ARG_STR) {
                t->named.args[i].str.p = th_strdup(ctx, a[i].str.p,
                                                   a[i].str.n);
            }
        }
    }

    return t;
}

th_type *th_apply(th_ctx *ctx, const char *n, const th_arg *a, size_t cnt)
{
    return make_named(ctx, n, a, cnt, false);
}

th_type *th_variant(th_ctx *ctx, const char *n, const th_arg *a, size_t cnt)
{
    return make_named(ctx, n, a, cnt, true);
}

th_type *th_bitint(th_ctx *ctx, uint64_t w, bool sg)
{
    th_type *t = node(ctx, TH_BITINT);

    t->bitint.width     = w;
    t->bitint.is_signed = sg;

    return t;
}

th_type *th_ind(th_ctx *ctx, uint8_t attrs, th_type *r)
{
    th_type *t = node(ctx, TH_INDIRECTION);

    t->ind.attrs    = attrs;
    t->ind.referent = r;

    return t;
}

bool th_indirection_attrs(const char *name, uint8_t *attrs)
{
    if (!name || !attrs) return false;
    if (!strcmp(name, "ref")) *attrs = 0;
    else if (!strcmp(name, "ptr"))
        *attrs = TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE;
    else if (!strcmp(name, "mutref")) *attrs = TH_EXCLUSIVE;
    else if (!strcmp(name, "own")) *attrs = TH_OWNING;
    else if (!strcmp(name, "gcref")) *attrs = TH_NULLABLE | TH_RESEATABLE;
    else if (!strcmp(name, "tmpref")) *attrs = TH_TEMPORARY;
    else return false;
    return true;
}

th_type *th_array(th_ctx *ctx, th_type *e)
{
    th_type *t = node(ctx, TH_SEQUENCE);

    t->seq.elem = e;

    return t;
}

th_type *th_array_n(th_ctx *ctx, th_type *e, uint64_t n)
{
    th_type *t = th_array(ctx, e);

    t->seq.has_extent = true;
    t->seq.extent     = n;

    return t;
}

th_type *th_record(th_ctx *ctx, const th_member *m, size_t n)
{
    th_type *t = node(ctx, TH_RECORD);

    t->record.n = n;

    if (n) {
        t->record.members = th_alloc(ctx, n * sizeof(th_member));

        for (size_t i = 0; i < n; i++) {
            t->record.members[i].name = th_strdup(ctx, m[i].name,
                                                  strlen(m[i].name));
            t->record.members[i].type = m[i].type;
        }
    }

    return t;
}

th_type *th_record_stub(th_ctx *ctx)
{
    return node(ctx, TH_RECORD);
}

void th_record_fill(th_ctx *ctx, th_type *stub, const th_member *m, size_t n)
{
    stub->record.n       = n;
    stub->record.members = nullptr;

    if (n) {
        stub->record.members = th_alloc(ctx, n * sizeof(th_member));

        for (size_t i = 0; i < n; i++) {
            stub->record.members[i].name = th_strdup(ctx, m[i].name,
                                                     strlen(m[i].name));
            stub->record.members[i].type = m[i].type;
        }
    }
}

th_type *th_func_with_error(th_ctx *ctx, th_type *ret, th_type *error,
                            const th_param *p, size_t np,
                            const th_param *rest, const th_kwparam *kw,
                            size_t nkw)
{
    th_type *t = node(ctx, TH_FUNCTION);

    t->fn.ret = ret;
    t->fn.error = error;
    t->fn.np  = np;

    if (np) {
        t->fn.params = th_alloc(ctx, np * sizeof(th_param));
        memcpy(t->fn.params, p, np * sizeof(th_param));
    }

    if (rest) {
        t->fn.rest  = th_alloc(ctx, sizeof(th_param));
        *t->fn.rest = *rest;
    }

    t->fn.nkw = nkw;

    if (nkw) {
        t->fn.kw = th_alloc(ctx, nkw * sizeof(th_kwparam));

        for (size_t i = 0; i < nkw; i++) {
            t->fn.kw[i].name = th_strdup(ctx, kw[i].name, strlen(kw[i].name));
            t->fn.kw[i].mode = kw[i].mode;
            t->fn.kw[i].type = kw[i].type;
        }
    }

    return t;
}

th_type *th_func(th_ctx *ctx, th_type *ret, const th_param *p, size_t np,
                 const th_param *rest, const th_kwparam *kw, size_t nkw)
{
    return th_func_with_error(ctx, ret, nullptr, p, np, rest, kw, nkw);
}

th_type *th_binder(th_ctx *ctx, uint64_t arity, th_type *body)
{
    th_type *t = node(ctx, TH_BINDER);

    t->binder.arity = arity;
    t->binder.body  = body;
    t->binder.qvars = nullptr;

    return t;
}

th_type *th_bound(th_ctx *ctx, uint64_t d, uint64_t p)
{
    th_type *t = node(ctx, TH_BOUND);

    t->bound.distance = d;
    t->bound.position = p;

    return t;
}

th_type *th_var(th_ctx *ctx)
{
    th_type *t = node(ctx, TH_VAR);

    t->var.has_index = false;
    t->var.index     = ctx->next_var++;
    t->var.ref       = nullptr;
    t->var.name      = nullptr;

    return t;
}

th_type *th_var_named(th_ctx *ctx, const char *name)
{
    th_type *t = th_var(ctx);

    if (name) {
        t->var.name = th_strdup(ctx, name, strlen(name));
    }

    return t;
}

th_type *th_tuple(th_ctx *ctx, th_type *const *e, size_t n)
{
    th_arg *a = core_alloc_array(th_arg, n);

    for (size_t i = 0; i < n; i++) {
        a[i].kind = TH_ARG_TYPE;
        a[i].type = e[i];
    }

    th_type *t = th_apply(ctx, TH_TUPLE, a, n);

    core_free(a);

    return t;
}

static core_hash_value_t enc_hash(void *p)
{
    return core_buffer_hash((core_buffer_t *)p);
}

static bool enc_eq(const void *a, const void *b)
{
    const core_buffer_t *x = a, *y = b;

    return x->byte_len == y->byte_len
        && !memcmp(x->data, y->data, x->byte_len);
}

th_type *th_sum(th_ctx *ctx, th_comb comb, th_type *const *arms, size_t n)
{
    core_list_t(th_type_ptr_t) flat = core_list_new_cap(th_type_ptr_t, n);

    for (size_t i = 0; i < n; i++) {
        th_type *a = arms[i];

        if (a->kind == TH_SUM && a->sum.comb == comb) {
            for (size_t j = 0; j < a->sum.n; j++) {
                core_list_push(flat, a->sum.arms[j]);
            }
        }
        else {
            core_list_push(flat, a);
        }
    }

    size_t        fn   = flat.len;
    th_type     **uniq = core_alloc_array(th_type *, fn);
    core_buffer_t *enc  = core_alloc_array(core_buffer_t, fn);
    size_t        un   = 0;
    core_dict_t    seen;

    core_dict_init(&seen, enc_hash, enc_eq);

    for (size_t i = 0; i < fn; i++) {
        th_erase_encode(ctx, flat.data[i], &enc[un]);

        if (core_dict_contains(&seen, &enc[un])) {
            core_buffer_release(&enc[un]);

            continue;
        }
        core_dict_put(&seen, &enc[un], &enc[un]);
        uniq[un++] = flat.data[i];
    }
    core_dict_free(&seen);

    th_type *r;

    if (un == 1) {
        r = uniq[0];
    }
    else {
        r            = node(ctx, TH_SUM);
        r->sum.comb  = comb;
        r->sum.n     = un;
        r->sum.arms  = th_alloc(ctx, un * sizeof(th_type *));
        memcpy(r->sum.arms, uniq, un * sizeof(th_type *));
    }

    for (size_t i = 0; i < un; i++) {
        core_buffer_release(&enc[i]);
    }
    core_free(enc);
    core_free(uniq);
    core_list_free(flat);

    return r;
}
