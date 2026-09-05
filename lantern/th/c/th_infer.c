#include <stdlib.h>
#include <string.h>

#include "th_internal.h"

typedef struct {
    const void **key;
    void       **val;
    size_t       n;
    size_t       cap;
} th_map;

typedef struct {
    const void **p;
    size_t       n;
    size_t       cap;
} th_plist;

typedef struct {
    const void **a;
    const void **b;
    size_t       n;
    size_t       cap;
} th_pairs;

static bool map_get(th_map *m, const void *k, void **out)
{
    for (size_t i = 0; i < m->n; i++) {
        if (m->key[i] == k) {
            *out = m->val[i];

            return true;
        }
    }

    return false;
}

static void map_put(th_map *m, const void *k, void *v)
{
    for (size_t i = 0; i < m->n; i++) {
        if (m->key[i] == k) {
            m->val[i] = v;

            return;
        }
    }

    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        m->key = realloc(m->key, m->cap * sizeof(void *));
        m->val = realloc(m->val, m->cap * sizeof(void *));
    }
    m->key[m->n] = k;
    m->val[m->n] = v;
    m->n++;
}

static void map_free(th_map *m)
{
    free(m->key);
    free(m->val);
    m->key = nullptr;
    m->val = nullptr;
    m->n   = 0;
    m->cap = 0;
}

static bool plist_has(th_plist *l, const void *p)
{
    for (size_t i = 0; i < l->n; i++) {
        if (l->p[i] == p) {
            return true;
        }
    }

    return false;
}

static void plist_push(th_plist *l, const void *p)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->p   = realloc(l->p, l->cap * sizeof(void *));
    }
    l->p[l->n++] = p;
}

static void plist_free(th_plist *l)
{
    free(l->p);
    l->p   = nullptr;
    l->n   = 0;
    l->cap = 0;
}

static bool pairs_has(th_pairs *s, const void *a, const void *b)
{
    for (size_t i = 0; i < s->n; i++) {
        if (s->a[i] == a && s->b[i] == b) {
            return true;
        }
    }

    return false;
}

static void pairs_add(th_pairs *s, const void *a, const void *b)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->a   = realloc(s->a, s->cap * sizeof(void *));
        s->b   = realloc(s->b, s->cap * sizeof(void *));
    }
    s->a[s->n] = a;
    s->b[s->n] = b;
    s->n++;
}

static void pairs_free(th_pairs *s)
{
    free(s->a);
    free(s->b);
    s->a   = nullptr;
    s->b   = nullptr;
    s->n   = 0;
    s->cap = 0;
}

th_type *th_resolve(th_type *t)
{
    return (th_type *)th_resolve_c(t);
}

static void sort_arms(th_ctx *ctx, th_type *const *in, size_t n, th_type **out)
{
    th_buf *b = calloc(n ? n : 1, sizeof(th_buf));

    for (size_t i = 0; i < n; i++) {
        th_erase_encode(ctx, in[i], &b[i]);
        out[i] = in[i];
    }

    for (size_t i = 1; i < n; i++) {
        th_type *kt = out[i];
        th_buf   kb = b[i];
        size_t   j  = i;

        while (j > 0) {
            th_slice x = {.p = b[j - 1].p, .n = b[j - 1].len};
            th_slice y = {.p = kb.p, .n = kb.len};

            if (th_cmp_slice(&x, &y) <= 0) {
                break;
            }
            out[j] = out[j - 1];
            b[j]   = b[j - 1];
            j--;
        }
        out[j] = kt;
        b[j]   = kb;
    }

    for (size_t i = 0; i < n; i++) {
        th_buf_free(&b[i]);
    }
    free(b);
}

static void fv_walk(th_ctx *ctx, th_type *t, th_plist *acc, th_plist *stack)
{
    t = th_resolve(t);

    if (!t || plist_has(stack, t)) {
        return;
    }
    plist_push(stack, t);

    switch (t->kind) {
    case TH_VAR:
        if (!plist_has(acc, t)) {
            plist_push(acc, t);
        }
        break;

    case TH_NAMED:
        for (size_t i = 0; i < t->named.nargs; i++) {
            if (t->named.args[i].kind == TH_ARG_TYPE) {
                fv_walk(ctx, t->named.args[i].type, acc, stack);
            }
        }
        break;

    case TH_INDIRECTION:
        fv_walk(ctx, t->ind.referent, acc, stack);
        break;

    case TH_SEQUENCE:
        fv_walk(ctx, t->seq.elem, acc, stack);
        break;

    case TH_RECORD: {
        size_t     n = t->record.n;
        th_member *m = calloc(n ? n : 1, sizeof(th_member));

        memcpy(m, t->record.members, n * sizeof(th_member));
        qsort(m, n, sizeof(th_member), th_cmp_member);

        for (size_t i = 0; i < n; i++) {
            fv_walk(ctx, m[i].type, acc, stack);
        }
        free(m);
        break;
    }

    case TH_FUNCTION: {
        fv_walk(ctx, t->fn.ret, acc, stack);

        for (size_t i = 0; i < t->fn.np; i++) {
            fv_walk(ctx, t->fn.params[i].type, acc, stack);
        }

        if (t->fn.rest) {
            fv_walk(ctx, t->fn.rest->type, acc, stack);
        }

        if (t->fn.nkw) {
            th_kwparam *k = calloc(t->fn.nkw, sizeof(th_kwparam));

            memcpy(k, t->fn.kw, t->fn.nkw * sizeof(th_kwparam));
            qsort(k, t->fn.nkw, sizeof(th_kwparam), th_cmp_kw);

            for (size_t i = 0; i < t->fn.nkw; i++) {
                fv_walk(ctx, k[i].type, acc, stack);
            }
            free(k);
        }
        break;
    }

    case TH_BINDER:
        fv_walk(ctx, t->binder.body, acc, stack);
        break;

    case TH_SUM: {
        size_t    n = t->sum.n;
        th_type **a = calloc(n ? n : 1, sizeof(th_type *));

        sort_arms(ctx, t->sum.arms, n, a);

        for (size_t i = 0; i < n; i++) {
            fv_walk(ctx, a[i], acc, stack);
        }
        free(a);
        break;
    }

    default:
        break;
    }

    stack->n--;
}

size_t th_free_vars(th_ctx *ctx, th_type *t, th_type ***out)
{
    th_plist acc = {0}, stack = {0};

    fv_walk(ctx, t, &acc, &stack);
    plist_free(&stack);

    size_t n = acc.n;

    if (out) {
        th_type **r = th_alloc(ctx, (n ? n : 1) * sizeof(th_type *));

        for (size_t i = 0; i < n; i++) {
            r[i] = (th_type *)acc.p[i];
        }
        *out = r;
    }
    plist_free(&acc);

    return n;
}

bool th_is_closed(th_ctx *ctx, th_type *t)
{
    return th_free_vars(ctx, t, nullptr) == 0;
}

bool th_occurs(th_ctx *ctx, th_type *var, th_type *t)
{
    th_plist acc = {0}, stack = {0};

    fv_walk(ctx, t, &acc, &stack);
    plist_free(&stack);

    var = th_resolve(var);

    bool hit = plist_has(&acc, var);

    plist_free(&acc);

    return hit;
}

static th_type *dup_shallow(th_ctx *ctx, const th_type *t)
{
    th_type *o = th_alloc(ctx, sizeof(th_type));

    *o = *t;

    return o;
}

static th_type *copy_kids(th_ctx *ctx, th_type *out, const th_type *t,
                          th_type *(*go)(th_ctx *, th_type *, void *),
                          void *st)
{
    switch (t->kind) {
    case TH_NAMED:
        if (t->named.nargs) {
            out->named.args = th_alloc(ctx, t->named.nargs * sizeof(th_arg));
            memcpy(out->named.args, t->named.args,
                   t->named.nargs * sizeof(th_arg));

            for (size_t i = 0; i < t->named.nargs; i++) {
                if (out->named.args[i].kind == TH_ARG_TYPE) {
                    out->named.args[i].type = go(ctx, out->named.args[i].type,
                                                 st);
                }
            }
        }
        break;

    case TH_INDIRECTION:
        out->ind.referent = go(ctx, t->ind.referent, st);
        break;

    case TH_SEQUENCE:
        out->seq.elem = go(ctx, t->seq.elem, st);
        break;

    case TH_RECORD:
        if (t->record.n) {
            out->record.members = th_alloc(ctx,
                                           t->record.n * sizeof(th_member));
            memcpy(out->record.members, t->record.members,
                   t->record.n * sizeof(th_member));

            for (size_t i = 0; i < t->record.n; i++) {
                out->record.members[i].type = go(ctx,
                                                 out->record.members[i].type,
                                                 st);
            }
        }
        break;

    case TH_FUNCTION:
        out->fn.ret = go(ctx, t->fn.ret, st);

        if (t->fn.np) {
            out->fn.params = th_alloc(ctx, t->fn.np * sizeof(th_param));
            memcpy(out->fn.params, t->fn.params, t->fn.np * sizeof(th_param));

            for (size_t i = 0; i < t->fn.np; i++) {
                out->fn.params[i].type = go(ctx, out->fn.params[i].type, st);
            }
        }

        if (t->fn.rest) {
            out->fn.rest       = th_alloc(ctx, sizeof(th_param));
            *out->fn.rest      = *t->fn.rest;
            out->fn.rest->type = go(ctx, t->fn.rest->type, st);
        }

        if (t->fn.nkw) {
            out->fn.kw = th_alloc(ctx, t->fn.nkw * sizeof(th_kwparam));
            memcpy(out->fn.kw, t->fn.kw, t->fn.nkw * sizeof(th_kwparam));

            for (size_t i = 0; i < t->fn.nkw; i++) {
                out->fn.kw[i].type = go(ctx, out->fn.kw[i].type, st);
            }
        }
        break;

    case TH_BINDER:
        out->binder.body = go(ctx, t->binder.body, st);
        break;

    case TH_SUM:
        if (t->sum.n) {
            out->sum.arms = th_alloc(ctx, t->sum.n * sizeof(th_type *));

            for (size_t i = 0; i < t->sum.n; i++) {
                out->sum.arms[i] = go(ctx, t->sum.arms[i], st);
            }
        }
        break;

    default:
        break;
    }

    return out;
}

static th_type *clone_go(th_ctx *ctx, th_type *t, void *st)
{
    th_map *memo = st;

    if (!t) {
        return nullptr;
    }

    void *hit;

    if (map_get(memo, t, &hit)) {
        return hit;
    }

    if (t->kind == TH_VAR) {
        th_type *v = th_var(ctx);

        map_put(memo, t, v);

        return v;
    }

    th_type *out = dup_shallow(ctx, t);

    map_put(memo, t, out);

    return copy_kids(ctx, out, t, clone_go, st);
}

th_type *th_clone(th_ctx *ctx, th_type *t)
{
    th_map   memo = {0};
    th_type *r    = clone_go(ctx, t, &memo);

    map_free(&memo);

    return r;
}

static th_type *subst_go(th_ctx *ctx, th_type *t, void *st)
{
    th_map *memo = st;

    t = th_resolve(t);

    if (!t) {
        return nullptr;
    }

    void *hit;

    if (map_get(memo, t, &hit)) {
        return hit;
    }

    if (t->kind == TH_VAR) {
        return t;
    }

    th_type *out = dup_shallow(ctx, t);

    map_put(memo, t, out);

    return copy_kids(ctx, out, t, subst_go, st);
}

th_type *th_apply_subst(th_ctx *ctx, th_type *t)
{
    th_map   memo = {0};
    th_type *r    = subst_go(ctx, t, &memo);

    map_free(&memo);

    return r;
}

static bool arg_eq(const th_arg *x, const th_arg *y)
{
    if (x->kind != y->kind) {
        return false;
    }

    switch (x->kind) {
    case TH_ARG_UINT:
        return x->u == y->u;
    case TH_ARG_SINT:
        return x->s == y->s;
    case TH_ARG_STR:
        return x->str.n == y->str.n
            && !memcmp(x->str.p, y->str.p, x->str.n);
    default:
        return true;
    }
}

static bool uni(th_ctx *ctx, th_type *a, th_type *b, th_pairs *seen);

static bool uni_named(th_ctx *ctx, th_type *a, th_type *b, th_pairs *seen)
{
    if (a->named.nlen != b->named.nlen
        || memcmp(a->named.name, b->named.name, a->named.nlen)
        || a->named.is_variant != b->named.is_variant
        || a->named.nargs != b->named.nargs) {
        return false;
    }

    for (size_t i = 0; i < a->named.nargs; i++) {
        const th_arg *x = &a->named.args[i], *y = &b->named.args[i];

        if (x->kind != y->kind) {
            return false;
        }

        if (x->kind == TH_ARG_TYPE) {
            if (!uni(ctx, x->type, y->type, seen)) {
                return false;
            }
        }
        else if (!arg_eq(x, y)) {
            return false;
        }
    }

    return true;
}

static bool uni_record(th_ctx *ctx, th_type *a, th_type *b, th_pairs *seen)
{
    if (a->record.n != b->record.n) {
        return false;
    }

    for (size_t i = 0; i < a->record.n; i++) {
        th_type *m = th_record_lookup(b, a->record.members[i].name);

        if (!m || !uni(ctx, a->record.members[i].type, m, seen)) {
            return false;
        }
    }

    return true;
}

static bool uni_func(th_ctx *ctx, th_type *a, th_type *b, th_pairs *seen)
{
    if (!a->fn.rest != !b->fn.rest || a->fn.np != b->fn.np
        || a->fn.nkw != b->fn.nkw) {
        return false;
    }

    if (!uni(ctx, a->fn.ret, b->fn.ret, seen)) {
        return false;
    }

    for (size_t i = 0; i < a->fn.np; i++) {
        if (a->fn.params[i].mode != b->fn.params[i].mode
            || !uni(ctx, a->fn.params[i].type, b->fn.params[i].type, seen)) {
            return false;
        }
    }

    if (a->fn.rest) {
        if (a->fn.rest->mode != b->fn.rest->mode
            || !uni(ctx, a->fn.rest->type, b->fn.rest->type, seen)) {
            return false;
        }
    }

    for (size_t i = 0; i < a->fn.nkw; i++) {
        const th_kwparam *k = th_func_kw_lookup(b, a->fn.kw[i].name);

        if (!k || k->mode != a->fn.kw[i].mode
            || !uni(ctx, a->fn.kw[i].type, k->type, seen)) {
            return false;
        }
    }

    return true;
}

static bool uni_sum(th_ctx *ctx, th_type *a, th_type *b, th_pairs *seen)
{
    if (a->sum.comb != b->sum.comb || a->sum.n != b->sum.n) {
        return false;
    }

    if (th_is_closed(ctx, a) && th_is_closed(ctx, b)) {
        return th_hash(ctx, a) == th_hash(ctx, b);
    }

    size_t    n  = a->sum.n;
    th_type **xa = calloc(n ? n : 1, sizeof(th_type *));
    th_type **xb = calloc(n ? n : 1, sizeof(th_type *));

    sort_arms(ctx, a->sum.arms, n, xa);
    sort_arms(ctx, b->sum.arms, n, xb);

    bool ok = true;

    for (size_t i = 0; ok && i < n; i++) {
        ok = uni(ctx, xa[i], xb[i], seen);
    }
    free(xa);
    free(xb);

    return ok;
}

static bool uni(th_ctx *ctx, th_type *a, th_type *b, th_pairs *seen)
{
    a = th_resolve(a);
    b = th_resolve(b);

    if (a == b) {
        return true;
    }

    if (!a || !b) {
        return false;
    }

    if (pairs_has(seen, a, b)) {
        return true;
    }
    pairs_add(seen, a, b);

    if (a->kind == TH_VAR) {
        if (th_occurs(ctx, a, b)) {
            return false;
        }
        a->var.ref = b;

        return true;
    }

    if (b->kind == TH_VAR) {
        return uni(ctx, b, a, seen);
    }

    if (a->kind != b->kind) {
        return false;
    }

    switch (a->kind) {
    case TH_NAMED:
        return uni_named(ctx, a, b, seen);

    case TH_BITINT:
        return a->bitint.width == b->bitint.width
            && a->bitint.is_signed == b->bitint.is_signed;

    case TH_INDIRECTION:
        return a->ind.attrs == b->ind.attrs
            && uni(ctx, a->ind.referent, b->ind.referent, seen);

    case TH_SEQUENCE:
        if (a->seq.has_extent != b->seq.has_extent) {
            return false;
        }

        if (a->seq.has_extent && a->seq.extent != b->seq.extent) {
            return false;
        }

        return uni(ctx, a->seq.elem, b->seq.elem, seen);

    case TH_RECORD:
        return uni_record(ctx, a, b, seen);

    case TH_FUNCTION:
        return uni_func(ctx, a, b, seen);

    case TH_BINDER:
        return a->binder.arity == b->binder.arity
            && uni(ctx, a->binder.body, b->binder.body, seen);

    case TH_BOUND:
        return a->bound.distance == b->bound.distance
            && a->bound.position == b->bound.position;

    case TH_SUM:
        return uni_sum(ctx, a, b, seen);

    default:
        return false;
    }
}

th_type *th_unify(th_ctx *ctx, th_type *a, th_type *b)
{
    th_type *ca = th_clone(ctx, a);
    th_type *cb = th_clone(ctx, b);
    th_pairs seen = {0};
    bool     ok   = uni(ctx, ca, cb, &seen);

    pairs_free(&seen);

    if (!ok) {
        ctx->err = "types do not unify";

        return nullptr;
    }

    th_type *r = th_apply_subst(ctx, ca);

    th_normalize(r);

    return r;
}

typedef struct {
    th_map    memo;
    th_type **fresh;
    uint64_t  arity;
    uint64_t  depth;
} th_inst;

static th_type *inst_go(th_ctx *ctx, th_type *t, void *st)
{
    th_inst *s = st;

    t = th_resolve(t);

    if (!t) {
        return nullptr;
    }

    if (t->kind == TH_BOUND) {
        if (t->bound.distance == s->depth + 1
            && t->bound.position < s->arity) {
            return s->fresh[t->bound.position];
        }

        return t;
    }

    if (t->kind == TH_VAR) {
        return t;
    }

    void *hit;

    if (map_get(&s->memo, t, &hit)) {
        return hit;
    }

    th_type *out = dup_shallow(ctx, t);

    map_put(&s->memo, t, out);

    if (t->kind == TH_BINDER) {
        s->depth++;
        out->binder.body = inst_go(ctx, t->binder.body, st);
        s->depth--;

        return out;
    }

    return copy_kids(ctx, out, t, inst_go, st);
}

th_type *th_instantiate(th_ctx *ctx, th_type *t)
{
    t = th_resolve(t);

    if (!t || t->kind != TH_BINDER) {
        return t;
    }

    uint64_t n = t->binder.arity;
    th_inst  s = {.arity = n, .depth = 0};

    s.fresh = calloc(n ? n : 1, sizeof(th_type *));

    for (uint64_t i = 0; i < n; i++) {
        s.fresh[i] = th_var(ctx);
    }

    th_type *r = inst_go(ctx, t->binder.body, &s);

    map_free(&s.memo);
    free(s.fresh);
    th_normalize(r);

    return r;
}

typedef struct {
    th_map    memo;
    th_type **vars;
    size_t    nvars;
    uint64_t  depth;
} th_gen;

static th_type *gen_go(th_ctx *ctx, th_type *t, void *st)
{
    th_gen *s = st;

    t = th_resolve(t);

    if (!t) {
        return nullptr;
    }

    if (t->kind == TH_VAR) {
        for (size_t i = 0; i < s->nvars; i++) {
            if (s->vars[i] == t) {
                return th_bound(ctx, s->depth + 1, i);
            }
        }

        return t;
    }

    void *hit;

    if (map_get(&s->memo, t, &hit)) {
        return hit;
    }

    th_type *out = dup_shallow(ctx, t);

    map_put(&s->memo, t, out);

    if (t->kind == TH_BINDER) {
        s->depth++;
        out->binder.body = gen_go(ctx, t->binder.body, st);
        s->depth--;

        return out;
    }

    return copy_kids(ctx, out, t, gen_go, st);
}

th_type *th_generalize(th_ctx *ctx, th_type *t)
{
    th_gen s = {0};

    s.nvars = th_free_vars(ctx, t, &s.vars);

    if (!s.nvars) {
        th_normalize(t);

        return t;
    }

    th_type *body = gen_go(ctx, t, &s);

    map_free(&s.memo);

    th_type *r = th_binder(ctx, s.nvars, body);

    th_normalize(r);

    return r;
}
