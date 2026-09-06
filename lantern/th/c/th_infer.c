#include <stdlib.h>
#include <string.h>

#include "th_internal.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "lib/option.h"

static th_constraint *con_same_slot(th_type *var, th_constraint c)
{
    for (size_t i = 0; i < var->var.cons.len; i++) {
        th_constraint *e = &var->var.cons.data[i];

        if (e->kind != c.kind) {
            continue;
        }

        if (c.kind == TH_CON_HAS_FIELD && !strcmp(e->field.name,
                                                  c.field.name)) {
            return e;
        }

        if (c.kind == TH_CON_HAS_PARAM && e->param.index == c.param.index) {
            return e;
        }
    }

    return nullptr;
}

bool th_var_constrain(th_ctx *ctx, th_type *var, th_constraint c)
{
    if (!var || var->kind != TH_VAR) {
        return false;
    }

    th_constraint *slot = con_same_slot(var, c);

    if (slot) {
        th_type *have = c.kind == TH_CON_HAS_FIELD ? slot->field.type
                                                   : slot->param.type;
        th_type *want = c.kind == TH_CON_HAS_FIELD ? c.field.type
                                                   : c.param.type;

        if (have && want && !th_unify(ctx, have, want)) {
            th_err(ctx, TH_ERR_CONSTRAINT,
                   c.kind == TH_CON_HAS_FIELD ? "has_field" : "has_param",
                   have, want,
                   "constraint conflicts with one already on the variable");

            return false;
        }

        return true;
    }

    ncc_list_push(var->var.cons, c);

    return true;
}

size_t th_var_constraint_count(const th_type *var)
{
    return var && var->kind == TH_VAR ? var->var.cons.len : 0;
}

bool th_var_constraint_at(const th_type *var, size_t i, th_constraint *out)
{
    if (!var || var->kind != TH_VAR || i >= var->var.cons.len) {
        return false;
    }

    if (out) {
        *out = var->var.cons.data[i];
    }

    return true;
}

void th_register_promotion(th_ctx *ctx, const char *from, const char *to)
{
    if (!ctx || !from || !to) {
        return;
    }

    th_promo p = {.from = th_strdup(ctx, from, strlen(from)),
                  .to   = th_strdup(ctx, to, strlen(to))};

    ncc_list_push(ctx->promos, p);
}

bool th_promotes_to(th_ctx *ctx, const char *from, const char *to)
{
    if (!ctx || !from || !to) {
        return false;
    }

    if (!strcmp(from, to)) {
        return true;
    }

    ncc_dict_t seen;
    ncc_list_t(ncc_const_charptr_t) queue
        = ncc_list_new(ncc_const_charptr_t);

    ncc_dict_init(&seen, ncc_hash_cstring, ncc_dict_cstr_eq);
    ncc_dict_put(&seen, from, from);
    ncc_list_push(queue, from);

    bool hit = false;

    for (size_t at = 0; !hit && at < queue.len; at++) {
        const char *cur = queue.data[at];

        for (size_t i = 0; i < ctx->promos.len; i++) {
            const th_promo *p = &ctx->promos.data[i];

            if (strcmp(p->from, cur)) {
                continue;
            }

            if (!strcmp(p->to, to)) {
                hit = true;
                break;
            }

            if (!ncc_dict_contains(&seen, (void *)p->to)) {
                ncc_dict_put(&seen, p->to, p->to);
                ncc_list_push(queue, p->to);
            }
        }
    }

    ncc_dict_free(&seen);
    ncc_list_free(queue);

    return hit;
}

static bool uni_in_place(th_ctx *ctx, th_type *a, th_type *b);

void th_register_iface(th_ctx *ctx, const char *name, th_type *methods)
{
    if (!ctx || !name || !methods) {
        return;
    }

    th_iface i = {.name    = th_strdup(ctx, name, strlen(name)),
                  .methods = methods};

    ncc_list_push(ctx->ifaces, i);
}

static const th_iface *iface_named(th_ctx *ctx, const char *name)
{
    for (size_t i = 0; i < ctx->ifaces.len; i++) {
        if (!strcmp(ctx->ifaces.data[i].name, name)) {
            return &ctx->ifaces.data[i];
        }
    }

    return nullptr;
}

static bool satisfies(th_ctx *ctx, th_type *type, th_type *methods)
{
    size_t n = th_record_len(methods);

    for (size_t i = 0; i < n; i++) {
        const th_member *m = th_record_member(methods, i);
        th_type         *have;

        if (!m) {
            return false;
        }

        have = th_record_lookup(type, m->name);

        if (!have || !th_unify(ctx, have, m->type)) {
            return false;
        }
    }

    return true;
}

bool th_implements(th_ctx *ctx, th_type *type, const char *iface)
{
    if (!ctx || !type || !iface) {
        return false;
    }

    const th_iface *i = iface_named(ctx, iface);

    return i && satisfies(ctx, type, i->methods);
}

size_t th_infer_ifaces(th_ctx *ctx, th_type *type, const char **out,
                       size_t max)
{
    if (!ctx || !type) {
        return 0;
    }

    size_t n = 0;

    for (size_t i = 0; i < ctx->ifaces.len; i++) {
        const th_iface *f = &ctx->ifaces.data[i];

        if (!satisfies(ctx, type, f->methods)) {
            continue;
        }

        if (out && n < max) {
            out[n] = f->name;
        }
        n++;
    }

    return n;
}

static bool nullary_name(const th_type *t, const char **name);

th_type *th_coerce(th_ctx *ctx, th_type *from, th_coerce_policy policy)
{
    th_type *bottom = th_name(ctx, TH_BOTTOM);

    if (!ctx || !from || !policy.target) {
        return bottom;
    }

    if (th_unify(ctx, from, policy.target)) {
        return policy.target;
    }

    if (policy.allow & TH_COERCE_DEREF) {
        th_type *inner = th_ind_referent(from);

        if (inner) {
            th_coerce_policy inner_policy = policy;
            th_type         *r            = th_coerce(ctx, inner,
                                                      inner_policy);

            if (!th_is_bottom(r)) {
                return r;
            }
        }
    }

    if (policy.allow & TH_COERCE_PROMOTE) {
        const char *a, *b;

        if (nullary_name(from, &a) && nullary_name(policy.target, &b)
            && th_promotes_to(ctx, a, b)) {
            return policy.target;
        }
    }

    return bottom;
}

static bool nullary_name(const th_type *t, const char **name)
{
    t = th_resolve_c(t);

    if (!t || t->kind != TH_NAMED || t->named.nargs) {
        return false;
    }

    *name = t->named.name;

    return true;
}

static bool con_check(th_ctx *ctx, const th_type *var, th_type *bound_to)
{
    th_type *bt = th_resolve(bound_to);

    if (bt && bt->kind == TH_VAR) {
        for (size_t i = 0; i < var->var.cons.len; i++) {
            if (!th_var_constrain(ctx, bt, var->var.cons.data[i])) {
                return false;
            }
        }

        return true;
    }

    for (size_t i = 0; i < var->var.cons.len; i++) {
        const th_constraint *c = &var->var.cons.data[i];

        switch (c->kind) {
        case TH_CON_UNIFIES:
            if (!uni_in_place(ctx, bound_to, c->target)) {
                th_err(ctx, TH_ERR_CONSTRAINT, "unifies", c->target,
                       bound_to, "constraint unifies failed");

                return false;
            }
            break;

        case TH_CON_ONE_OF: {
            bool matched = false;

            for (size_t j = 0; !matched && j < c->one_of.n; j++) {
                const th_type *cand = th_resolve_c(c->one_of.types[j]);
                const th_type *bt   = th_resolve_c(bound_to);
                const char    *a, *b;

                if (nullary_name(bt, &a) && nullary_name(cand, &b)) {
                    matched = !strcmp(a, b);
                }
                else {
                    matched = bt == cand;
                }
            }

            if (!matched) {
                th_err(ctx, TH_ERR_CONSTRAINT, "one_of", nullptr, bound_to,
                       "constraint one_of matched no alternative");

                return false;
            }
            break;
        }

        case TH_CON_PROMOTES: {
            const char *a, *b;

            if (!nullary_name(bound_to, &a) || !nullary_name(c->target, &b)) {
                th_err(ctx, TH_ERR_CONSTRAINT, "promotes", c->target,
                       bound_to,
                       "constraint promotes needs nullary named types");

                return false;
            }

            if (!th_promotes_to(ctx, a, b)) {
                th_err(ctx, TH_ERR_CONSTRAINT, "promotes", c->target,
                       bound_to,
                       "constraint promotes has no promotion path");

                return false;
            }
            break;
        }

        case TH_CON_EXCLUDES: {
            const th_type *ex = th_resolve_c(c->target);
            const th_type *bt = th_resolve_c(bound_to);
            const char    *a, *b;
            bool           same;

            if (nullary_name(bt, &a) && nullary_name(ex, &b)) {
                same = !strcmp(a, b);
            }
            else {
                same = bt == ex;
            }

            if (same) {
                th_err(ctx, TH_ERR_CONSTRAINT, "excludes", c->target,
                       bound_to,
                       "constraint excludes matched the excluded type");

                return false;
            }
            break;
        }

        case TH_CON_HAS_FIELD: {
            th_type *m = th_record_lookup(bound_to, c->field.name);

            if (!m) {
                th_err(ctx, TH_ERR_NO_SUCH_FIELD, "has_field",
                       c->field.type, bound_to,
                       "constraint has_field: no such field");

                return false;
            }

            if (c->field.type && !uni_in_place(ctx, m, c->field.type)) {
                th_err(ctx, TH_ERR_CONSTRAINT, "has_field", c->field.type,
                       m, "constraint has_field: field type does not unify");

                return false;
            }
            break;
        }

        case TH_CON_HAS_PARAM: {
            size_t at = (size_t)c->param.index;

            if (at >= th_named_nargs(bound_to)) {
                th_err(ctx, TH_ERR_PARAM_MISMATCH, "has_param",
                       c->param.type, bound_to,
                       "constraint has_param: no parameter at that index");

                return false;
            }

            const th_arg *a = th_named_arg(bound_to, at);

            if (!a || a->kind != TH_ARG_TYPE) {
                th_err(ctx, TH_ERR_PARAM_MISMATCH, "has_param",
                       c->param.type, bound_to,
                       "constraint has_param: parameter is not a type");

                return false;
            }

            if (c->param.type && !uni_in_place(ctx, a->type, c->param.type)) {
                th_err(ctx, TH_ERR_CONSTRAINT, "has_param", c->param.type,
                       a->type,
                       "constraint has_param: parameter does not unify");

                return false;
            }
            break;
        }

        case TH_CON_IMPLEMENTS:
            if (!th_implements(ctx, bound_to, c->iface)) {
                th_err(ctx, TH_ERR_CONSTRAINT, c->iface, nullptr, bound_to,
                       "constraint implements: interface not satisfied");

                return false;
            }
            break;
        }
    }

    return true;
}

th_type *th_resolve(th_type *t)
{
    while (t && t->kind == TH_VAR && t->var.ref) {
        th_type *next = t->var.ref;

        if (next->kind == TH_VAR && next->var.ref) {
            t->var.ref = next->var.ref;
        }

        t = t->var.ref;
    }

    return t;
}

typedef struct {
    th_type  *t;
    th_type **kids;
    size_t    n;
    size_t    next;
    bool      entered;
} fvframe;

ncc_option_decl(fvframe);
ncc_list_decl(fvframe);

static th_type **fv_children(th_ctx *ctx, th_type *t, ncc_dict_t *acc,
                             size_t *out)
{
    th_type **kids = nullptr;
    size_t    n    = 0;

    *out = 0;

    switch (t->kind) {
    case TH_VAR: {
        bool found;

        ncc_dict_get(acc, t, &found);

        if (!found) {
            ncc_dict_put(acc, t, ncc_dict_len(acc) + 1);
        }

        return nullptr;
    }

    case TH_NAMED:
        kids = ncc_alloc_array(th_type *, t->named.nargs);

        for (size_t i = 0; i < t->named.nargs; i++) {
            if (t->named.args[i].kind == TH_ARG_TYPE) {
                kids[n++] = t->named.args[i].type;
            }
        }
        break;

    case TH_INDIRECTION:
        kids      = ncc_alloc_array(th_type *, 1);
        kids[n++] = t->ind.referent;
        break;

    case TH_SEQUENCE:
        kids      = ncc_alloc_array(th_type *, 1);
        kids[n++] = t->seq.elem;
        break;

    case TH_RECORD: {
        size_t     rn = t->record.n;
        th_member *m  = ncc_alloc_array(th_member, rn);

        memcpy(m, t->record.members, rn * sizeof(th_member));
        qsort(m, rn, sizeof(th_member), th_cmp_member);

        kids = ncc_alloc_array(th_type *, rn);

        for (size_t i = 0; i < rn; i++) {
            kids[n++] = m[i].type;
        }
        ncc_free(m);
        break;
    }

    case TH_FUNCTION: {
        kids      = ncc_alloc_array(th_type *, t->fn.np + t->fn.nkw + 2);
        kids[n++] = t->fn.ret;

        for (size_t i = 0; i < t->fn.np; i++) {
            kids[n++] = t->fn.params[i].type;
        }

        if (t->fn.rest) {
            kids[n++] = t->fn.rest->type;
        }

        if (t->fn.nkw) {
            th_kwparam *k = ncc_alloc_array(th_kwparam, t->fn.nkw);

            memcpy(k, t->fn.kw, t->fn.nkw * sizeof(th_kwparam));
            qsort(k, t->fn.nkw, sizeof(th_kwparam), th_cmp_kw);

            for (size_t i = 0; i < t->fn.nkw; i++) {
                kids[n++] = k[i].type;
            }
            ncc_free(k);
        }
        break;
    }

    case TH_BINDER:
        kids      = ncc_alloc_array(th_type *, 1);
        kids[n++] = t->binder.body;
        break;

    case TH_SUM:
        *out = t->sum.n;

        return th_sort_arms(ctx, t->sum.arms, t->sum.n);

    default:
        return nullptr;
    }

    *out = n;

    return kids;
}

static void fv_walk(th_ctx *ctx, th_type *root, ncc_dict_t *acc,
                    ncc_dict_t *stack)
{
    ncc_list_t(fvframe) st = ncc_list_new_cap(fvframe, 16);

    ncc_list_push(st, ((fvframe){.t = root}));

    while (st.len) {
        fvframe *f = &st.data[st.len - 1];

        if (!f->entered) {
            f->entered = true;
            f->t       = th_resolve(f->t);

            if (!f->t || ncc_dict_contains(stack, f->t)) {
                f->t = nullptr;
                ncc_list_pop(fvframe, st);

                continue;
            }
            ncc_dict_put(stack, f->t, f->t);
            f->kids = fv_children(ctx, f->t, acc, &f->n);
            f       = &st.data[st.len - 1];
        }

        if (f->next < f->n) {
            th_type *kid = f->kids[f->next++];

            ncc_list_push(st, ((fvframe){.t = kid}));

            continue;
        }

        ncc_dict_remove(stack, f->t);
        ncc_free(f->kids);
        ncc_list_pop(fvframe, st);
    }

    ncc_list_free(st);
}

size_t th_free_vars(th_ctx *ctx, th_type *t, th_type ***out)
{
    ncc_dict_t acc, stack;

    ncc_dict_init(&acc, nullptr, nullptr);
    ncc_dict_init(&stack, nullptr, nullptr);

    fv_walk(ctx, t, &acc, &stack);
    ncc_dict_free(&stack);

    size_t n = ncc_dict_len(&acc);

    if (out) {
        th_type **r = th_alloc(ctx, (n ? n : 1) * sizeof(th_type *));

        ncc_dict_foreach (&acc, b) {
            r[(size_t)(uintptr_t)b->value - 1] = (th_type *)b->key;
        }
        *out = r;
    }
    ncc_dict_free(&acc);

    return n;
}

bool th_is_closed(th_ctx *ctx, th_type *t)
{
    return th_free_vars(ctx, t, nullptr) == 0;
}

bool th_occurs(th_ctx *ctx, th_type *var, th_type *t)
{
    ncc_dict_t acc, stack;

    ncc_dict_init(&acc, nullptr, nullptr);
    ncc_dict_init(&stack, nullptr, nullptr);

    fv_walk(ctx, t, &acc, &stack);
    ncc_dict_free(&stack);

    var = th_resolve(var);

    bool hit = ncc_dict_contains(&acc, var);

    ncc_dict_free(&acc);

    return hit;
}

static th_type *dup_shallow(th_ctx *ctx, const th_type *t)
{
    th_type *o = th_alloc(ctx, sizeof(th_type));

    *o = *t;

    return o;
}

typedef struct {
    th_type **slot;
    th_type  *src;
    uint64_t  depth;
} rjob;

ncc_option_decl(rjob);
ncc_list_decl(rjob);

typedef bool (*subst_fn)(th_ctx *ctx, th_type *t, uint64_t depth,
                         ncc_dict_t *memo, void *st, th_type **out);

static void rpush(ncc_list_t(rjob) *w, th_type **slot, th_type *src,
                  uint64_t depth)
{
    ncc_list_push(*w, ((rjob){.slot = slot, .src = src, .depth = depth}));
}

static void plan_kids(th_ctx *ctx, th_type *out, const th_type *t,
                      uint64_t depth, ncc_list_t(rjob) *w)
{
    switch (t->kind) {
    case TH_NAMED:
        if (t->named.nargs) {
            out->named.args = th_alloc(ctx, t->named.nargs * sizeof(th_arg));
            memcpy(out->named.args, t->named.args,
                   t->named.nargs * sizeof(th_arg));

            for (size_t i = 0; i < t->named.nargs; i++) {
                if (out->named.args[i].kind == TH_ARG_TYPE) {
                    rpush(w, &out->named.args[i].type,
                          out->named.args[i].type, depth);
                }
            }
        }
        break;

    case TH_INDIRECTION:
        rpush(w, &out->ind.referent, t->ind.referent, depth);
        break;

    case TH_SEQUENCE:
        rpush(w, &out->seq.elem, t->seq.elem, depth);
        break;

    case TH_RECORD:
        if (t->record.n) {
            out->record.members = th_alloc(ctx,
                                           t->record.n * sizeof(th_member));
            memcpy(out->record.members, t->record.members,
                   t->record.n * sizeof(th_member));

            for (size_t i = 0; i < t->record.n; i++) {
                rpush(w, &out->record.members[i].type,
                      out->record.members[i].type, depth);
            }
        }
        break;

    case TH_FUNCTION:
        rpush(w, &out->fn.ret, t->fn.ret, depth);

        if (t->fn.np) {
            out->fn.params = th_alloc(ctx, t->fn.np * sizeof(th_param));
            memcpy(out->fn.params, t->fn.params, t->fn.np * sizeof(th_param));

            for (size_t i = 0; i < t->fn.np; i++) {
                rpush(w, &out->fn.params[i].type, out->fn.params[i].type,
                      depth);
            }
        }

        if (t->fn.rest) {
            out->fn.rest  = th_alloc(ctx, sizeof(th_param));
            *out->fn.rest = *t->fn.rest;
            rpush(w, &out->fn.rest->type, t->fn.rest->type, depth);
        }

        if (t->fn.nkw) {
            out->fn.kw = th_alloc(ctx, t->fn.nkw * sizeof(th_kwparam));
            memcpy(out->fn.kw, t->fn.kw, t->fn.nkw * sizeof(th_kwparam));

            for (size_t i = 0; i < t->fn.nkw; i++) {
                rpush(w, &out->fn.kw[i].type, out->fn.kw[i].type, depth);
            }
        }
        break;

    case TH_BINDER:
        rpush(w, &out->binder.body, t->binder.body, depth + 1);
        break;

    case TH_SUM:
        if (t->sum.n) {
            out->sum.arms = th_alloc(ctx, t->sum.n * sizeof(th_type *));

            for (size_t i = 0; i < t->sum.n; i++) {
                out->sum.arms[i] = t->sum.arms[i];
                rpush(w, &out->sum.arms[i], t->sum.arms[i], depth);
            }
        }
        break;

    default:
        break;
    }
}

static th_type *rebuild(th_ctx *ctx, th_type *root, ncc_dict_t *memo,
                        bool resolve, subst_fn subst, void *st)
{
    th_type          *out_root = nullptr;
    ncc_list_t(rjob)  w        = ncc_list_new(rjob);

    rpush(&w, &out_root, root, 0);

    while (w.len) {
        rjob     j = ncc_option_get(ncc_list_pop(rjob, w));
        th_type *t = resolve ? th_resolve(j.src) : j.src;

        if (!t) {
            *j.slot = nullptr;

            continue;
        }

        bool  found;
        void *hit = ncc_dict_get(memo, t, &found);

        if (found) {
            *j.slot = hit;

            continue;
        }

        th_type *fixed = nullptr;

        if (subst(ctx, t, j.depth, memo, st, &fixed)) {
            *j.slot = fixed;

            continue;
        }

        th_type *out = dup_shallow(ctx, t);

        ncc_dict_put(memo, t, out);
        *j.slot = out;

        plan_kids(ctx, out, t, j.depth, &w);
    }

    ncc_list_free(w);

    return out_root;
}


static bool subst_subst(th_ctx *ctx, th_type *t, uint64_t depth,
                        ncc_dict_t *memo, void *st, th_type **out)
{
    (void)ctx;
    (void)depth;
    (void)memo;
    (void)st;

    if (t->kind != TH_VAR) {
        return false;
    }
    *out = t;

    return true;
}

th_type *th_apply_subst(th_ctx *ctx, th_type *t)
{
    ncc_dict_t memo;

    ncc_dict_init(&memo, nullptr, nullptr);

    th_type *r = rebuild(ctx, t, &memo, true, subst_subst, nullptr);

    ncc_dict_free(&memo);

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

typedef struct {
    th_type *a;
    th_type *b;
} tpair;

ncc_option_decl(tpair);
ncc_list_decl(tpair);

static void upush(ncc_list_t(tpair) *w, th_type *a, th_type *b)
{
    ncc_list_push(*w, ((tpair){.a = a, .b = b}));
}

static bool uni_named(th_ctx *ctx, th_type *a, th_type *b,
                      ncc_list_t(tpair) *w)
{
    if (a->named.nlen != b->named.nlen
        || memcmp(a->named.name, b->named.name, a->named.nlen)
        || a->named.is_variant != b->named.is_variant) {
        th_err(ctx, TH_ERR_UNIFY, nullptr, b, a,
               "these are different named types");

        return false;
    }

    if (a->named.nargs != b->named.nargs) {
        th_err(ctx, TH_ERR_ARITY_MISMATCH, nullptr, b, a,
               "the two applications have different argument counts");

        return false;
    }

    for (size_t i = a->named.nargs; i-- > 0;) {
        const th_arg *x = &a->named.args[i], *y = &b->named.args[i];

        if (x->kind != y->kind) {
            th_err(ctx, TH_ERR_ARITY_MISMATCH, nullptr, b, a,
                   "an argument differs in kind");

            return false;
        }

        if (x->kind == TH_ARG_TYPE) {
            upush(w, x->type, y->type);
        }
        else if (!arg_eq(x, y)) {
            th_err(ctx, TH_ERR_UNIFY, nullptr, b, a,
                   "a non-type argument differs");

            return false;
        }
    }

    return true;
}

static bool uni_record(th_ctx *ctx, th_type *a, th_type *b,
                       ncc_list_t(tpair) *w)
{
    if (a->record.n != b->record.n) {
        th_err(ctx, TH_ERR_ARITY_MISMATCH, nullptr, b, a,
               "the two records have different member counts");

        return false;
    }

    for (size_t i = a->record.n; i-- > 0;) {
        th_type *m = th_record_lookup(b, a->record.members[i].name);

        if (!m) {
            th_err(ctx, TH_ERR_NO_SUCH_FIELD, a->record.members[i].name,
                   b, a, "the other record has no such member");

            return false;
        }

        upush(w, a->record.members[i].type, m);
    }

    return true;
}

static bool uni_func(th_ctx *ctx, th_type *a, th_type *b,
                     ncc_list_t(tpair) *w)
{
    if (!a->fn.rest != !b->fn.rest || a->fn.np != b->fn.np
        || a->fn.nkw != b->fn.nkw) {
        th_err(ctx, TH_ERR_ARITY_MISMATCH, nullptr, b, a,
               "the two signatures differ in arity");

        return false;
    }

    for (size_t i = a->fn.nkw; i-- > 0;) {
        const th_kwparam *k = th_func_kw_lookup(b, a->fn.kw[i].name);

        if (!k || k->mode != a->fn.kw[i].mode) {
            th_err(ctx, TH_ERR_MISSING_KEYWORD, a->fn.kw[i].name, b, a,
                   "the other signature lacks that keyword parameter");

            return false;
        }

        upush(w, a->fn.kw[i].type, k->type);
    }

    if (a->fn.rest) {
        if (a->fn.rest->mode != b->fn.rest->mode) {
            th_err(ctx, TH_ERR_UNIFY, nullptr, b, a,
                   "the rest parameters differ in mode");

            return false;
        }

        upush(w, a->fn.rest->type, b->fn.rest->type);
    }

    for (size_t i = a->fn.np; i-- > 0;) {
        if (a->fn.params[i].mode != b->fn.params[i].mode) {
            th_err(ctx, TH_ERR_UNIFY, nullptr, b, a,
                   "a parameter differs in mode");

            return false;
        }

        upush(w, a->fn.params[i].type, b->fn.params[i].type);
    }

    upush(w, a->fn.ret, b->fn.ret);

    return true;
}

static bool uni_sum(th_ctx *ctx, th_type *a, th_type *b, ncc_list_t(tpair) *w)
{
    if (a->sum.comb != b->sum.comb || a->sum.n != b->sum.n) {
        th_err(ctx, TH_ERR_ARITY_MISMATCH, nullptr, b, a,
               "the two composites differ in shape");

        return false;
    }

    if (th_is_closed(ctx, a) && th_is_closed(ctx, b)) {
        return th_hash(ctx, a) == th_hash(ctx, b);
    }

    size_t    n  = a->sum.n;
    th_type **xa = th_sort_arms(ctx, a->sum.arms, n);
    th_type **xb = th_sort_arms(ctx, b->sum.arms, n);

    for (size_t i = n; i-- > 0;) {
        upush(w, xa[i], xb[i]);
    }
    ncc_free(xa);
    ncc_free(xb);

    return true;
}

static bool uni_step(th_ctx *ctx, th_type *a, th_type *b, ncc_list_t(tpair) *w)
{
    switch (a->kind) {
    case TH_NAMED:
        return uni_named(ctx, a, b, w);

    case TH_BITINT:
        return a->bitint.width == b->bitint.width
            && a->bitint.is_signed == b->bitint.is_signed;

    case TH_INDIRECTION:
        if (a->ind.attrs != b->ind.attrs) {
            return false;
        }
        upush(w, a->ind.referent, b->ind.referent);

        return true;

    case TH_SEQUENCE:
        if (a->seq.has_extent != b->seq.has_extent) {
            return false;
        }

        if (a->seq.has_extent && a->seq.extent != b->seq.extent) {
            return false;
        }

        upush(w, a->seq.elem, b->seq.elem);

        return true;

    case TH_RECORD:
        return uni_record(ctx, a, b, w);

    case TH_FUNCTION:
        return uni_func(ctx, a, b, w);

    case TH_BINDER:
        if (a->binder.arity != b->binder.arity) {
            return false;
        }
        upush(w, a->binder.body, b->binder.body);

        return true;

    case TH_BOUND:
        return a->bound.distance == b->bound.distance
            && a->bound.position == b->bound.position;

    case TH_SUM:
        return uni_sum(ctx, a, b, w);

    default:
        return false;
    }
}

static bool uni(th_ctx *ctx, th_type *a0, th_type *b0, ncc_dict_t *seen)
{
    ncc_list_t(tpair) w  = ncc_list_new(tpair);
    bool              ok = true;

    upush(&w, a0, b0);

    while (ok && w.len) {
        tpair    p = ncc_option_get(ncc_list_pop(tpair, w));
        th_type *a = th_resolve(p.a);
        th_type *b = th_resolve(p.b);

        if (a == b) {
            continue;
        }

        if (!a || !b) {
            ok = false;
            break;
        }

        bool        found;
        ncc_dict_t *rhs = ncc_dict_get(seen, a, &found);

        if (!found) {
            rhs = ncc_alloc(ncc_dict_t);

            ncc_dict_init(rhs, nullptr, nullptr);
            ncc_dict_put(seen, a, rhs);
        }

        if (ncc_dict_contains(rhs, b)) {
            continue;
        }
        ncc_dict_put(rhs, b, b);

        if (a->kind != TH_VAR && b->kind == TH_VAR) {
            th_type *t = a;

            a = b;
            b = t;
        }

        if (a->kind == TH_VAR) {
            if (th_occurs(ctx, a, b)) {
                th_err(ctx, TH_ERR_OCCURS, nullptr, a, b,
                       "the variable occurs in the type it would be bound to");
                ok = false;
                break;
            }

            if (!con_check(ctx, a, b)) {
                ok = false;
                break;
            }
            a->var.ref = b;

            continue;
        }

        if (a->kind != b->kind) {
            th_err(ctx, TH_ERR_UNIFY, nullptr, b, a,
                   "the two types are of different kinds");
            ok = false;
            break;
        }
        ok = uni_step(ctx, a, b, &w);
    }

    ncc_list_free(w);

    return ok;
}

static bool uni_in_place(th_ctx *ctx, th_type *a, th_type *b)
{
    ncc_dict_t seen;

    ncc_dict_init(&seen, nullptr, nullptr);

    bool ok = uni(ctx, a, b, &seen);

    ncc_dict_foreach (&seen, e) {
        ncc_dict_free((ncc_dict_t *)e->value);
        ncc_free(e->value);
    }
    ncc_dict_free(&seen);

    return ok;
}

th_type *th_unify(th_ctx *ctx, th_type *a, th_type *b)
{
    th_type *ia     = th_instantiate(ctx, th_generalize(ctx, a));
    th_type *ib     = th_instantiate(ctx, th_generalize(ctx, b));
    size_t   before = th_error_count(ctx);

    if (!uni_in_place(ctx, ia, ib)) {
        if (th_error_count(ctx) == before) {
            th_err(ctx, TH_ERR_UNIFY, nullptr, ib, ia,
                   "types do not unify");
        }

        return nullptr;
    }

    th_type *r = th_apply_subst(ctx, ia);

    th_normalize(r);

    return r;
}

typedef struct {
    ncc_dict_t memo;
    th_type **fresh;
    uint64_t  arity;
    uint64_t  depth;
} th_inst;

static bool inst_subst(th_ctx *ctx, th_type *t, uint64_t depth, ncc_dict_t *memo,
                       void *st, th_type **out)
{
    th_inst *s = st;

    (void)ctx;
    (void)memo;

    if (t->kind == TH_BOUND) {
        *out = t->bound.distance == depth + 1 && t->bound.position < s->arity
                 ? s->fresh[t->bound.position]
                 : t;

        return true;
    }

    if (t->kind == TH_VAR) {
        *out = t;

        return true;
    }

    return false;
}

th_type *th_instantiate(th_ctx *ctx, th_type *t)
{
    t = th_resolve(t);

    if (!t || t->kind != TH_BINDER) {
        return t;
    }

    uint64_t n = t->binder.arity;
    th_inst  s = {.arity = n, .depth = 0};

    ncc_dict_init(&s.memo, nullptr, nullptr);
    s.fresh = ncc_alloc_array(th_type *, n);

    for (uint64_t i = 0; i < n; i++) {
        s.fresh[i] = th_var(ctx);

        th_type *q = t->binder.qvars ? t->binder.qvars[i] : nullptr;

        if (q) {
            s.fresh[i]->var.name = q->var.name;

            for (size_t j = 0; j < q->var.cons.len; j++) {
                ncc_list_push(s.fresh[i]->var.cons, q->var.cons.data[j]);
            }
        }
    }

    th_type *r = rebuild(ctx, t->binder.body, &s.memo, true, inst_subst, &s);

    ncc_dict_free(&s.memo);
    ncc_free(s.fresh);
    th_normalize(r);

    return r;
}

typedef struct {
    ncc_dict_t memo;
    ncc_dict_t index;
    th_type  **vars;
    size_t     nvars;
    uint64_t   depth;
} th_gen;

static bool gen_subst(th_ctx *ctx, th_type *t, uint64_t depth, ncc_dict_t *memo,
                      void *st, th_type **out)
{
    th_gen *s = st;

    (void)memo;

    if (t->kind != TH_VAR) {
        return false;
    }

    bool   found;
    size_t at = (size_t)(uintptr_t)ncc_dict_get(&s->index, t, &found);

    *out = found ? th_bound(ctx, depth + 1, at - 1) : t;

    return true;
}

th_type *th_generalize(th_ctx *ctx, th_type *t)
{
    th_gen s = {0};

    s.nvars = th_free_vars(ctx, t, &s.vars);

    if (!s.nvars) {
        th_normalize(t);

        return t;
    }

    ncc_dict_init(&s.memo, nullptr, nullptr);
    ncc_dict_init(&s.index, nullptr, nullptr);

    for (size_t i = 0; i < s.nvars; i++) {
        ncc_dict_put(&s.index, s.vars[i], i + 1);
    }

    th_type *body = rebuild(ctx, t, &s.memo, true, gen_subst, &s);

    ncc_dict_free(&s.memo);
    ncc_dict_free(&s.index);

    th_type *r = th_binder(ctx, s.nvars, body);

    r->binder.qvars = s.vars;

    th_normalize(r);

    return r;
}
