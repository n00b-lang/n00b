#include <stdlib.h>
#include <string.h>

#include "private/th/internal.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "lib/option.h"

typedef struct {
    th_type  *t;
    th_type **kids;
    size_t    n;
    size_t    next;
    bool      entered;
} fvframe;

core_option_decl(fvframe);
core_list_decl(fvframe);

typedef th_type *th_occ_ptr_t;

core_list_decl(th_occ_ptr_t);

static th_type **fv_children(th_ctx *ctx, th_type *t, core_dict_t *acc,
                             size_t *out)
{
    th_type **kids = nullptr;
    size_t    n    = 0;

    *out = 0;

    switch (t->kind) {
    case TH_VAR: {
        bool found;

        core_dict_get(acc, t, &found);

        if (!found) {
            core_dict_put(acc, t, core_dict_len(acc) + 1);
        }
        break;
    }

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
        size_t     rn = t->record.n;
        th_member *m  = core_alloc_array(th_member, rn);

        memcpy(m, t->record.members, rn * sizeof(th_member));
        qsort(m, rn, sizeof(th_member), th_cmp_member);

        kids = core_alloc_array(th_type *, rn);

        for (size_t i = 0; i < rn; i++) {
            kids[n++] = m[i].type;
        }
        core_free(m);
        break;
    }

    case TH_TRAIT:
        kids = core_alloc_array(th_type *,
            t->trait.n + 2 * t->trait.nr);
        for (size_t i = 0; i < t->trait.n; i++)
            kids[n++] = t->trait.methods[i].type;
        for (size_t i = 0; i < t->trait.nr; i++) {
            kids[n++] = t->trait.requirements[i].subject;
            if (t->trait.requirements[i].target)
                kids[n++] = t->trait.requirements[i].target;
        }
        break;

    case TH_FUNCTION: {
        kids      = core_alloc_array(th_type *, t->fn.np + t->fn.nkw + 3);
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
        *out = t->sum.n;

        return th_sort_arms(ctx, t->sum.arms, t->sum.n);

    default:
        return nullptr;
    }

    *out = n;

    return kids;
}

static void fv_walk(th_ctx *ctx, th_type *root, core_dict_t *acc,
                    core_dict_t *stack)
{
    core_list_t(fvframe) st = core_list_new_cap(fvframe, 16);

    core_list_push(st, ((fvframe){.t = root}));

    while (st.len) {
        fvframe *f = &st.data[st.len - 1];

        if (!f->entered) {
            f->entered = true;
            f->t       = th_resolve(f->t);

            if (!f->t || core_dict_contains(stack, f->t)) {
                f->t = nullptr;
                core_list_pop(fvframe, st);

                continue;
            }
            core_dict_put(stack, f->t, f->t);
            f->kids = fv_children(ctx, f->t, acc, &f->n);
            f       = &st.data[st.len - 1];
        }

        if (f->next < f->n) {
            th_type *kid = f->kids[f->next++];

            core_list_push(st, ((fvframe){.t = kid}));

            continue;
        }

        core_dict_remove(stack, f->t);
        core_free(f->kids);
        core_list_pop(fvframe, st);
    }

    core_list_free(st);
}

size_t th_free_vars(th_ctx *ctx, th_type *t, th_type ***out)
{
    core_dict_t acc, stack;

    core_dict_init(&acc, nullptr, nullptr);
    core_dict_init(&stack, nullptr, nullptr);

    fv_walk(ctx, t, &acc, &stack);
    core_dict_free(&stack);

    size_t n = core_dict_len(&acc);

    if (out) {
        th_type **r = th_alloc(ctx, (n ? n : 1) * sizeof(th_type *));

        core_dict_foreach (&acc, b) {
            r[(size_t)(uintptr_t)b->value - 1] = (th_type *)b->key;
        }
        *out = r;
    }
    core_dict_free(&acc);

    return n;
}

typedef struct {
    th_type *type;
    size_t next;
} closed_frame_t;

bool th_is_closed(th_ctx *ctx, th_type *t)
{
    (void)ctx;
    t = th_resolve(t);
    if (!t) return true;
    if (t->kind == TH_VAR) return false;
    closed_frame_t local[64];
    closed_frame_t *frames = local;
    size_t depth = 0, capacity = 64;
    frames[depth++] = (closed_frame_t){t, 0};
    th_type *seen_local[32];
    size_t seen = 0;
    core_dict_t seen_index;
    bool indexed = false;
    seen_local[seen++] = t;
    bool closed = true;
    while (closed && depth) {
        closed_frame_t *frame = &frames[depth - 1];
        if (frame->next >= th_child_count(frame->type)) {
            depth--;
            continue;
        }
        th_type *child = th_resolve(th_child(frame->type, frame->next++));
        if (!child || !th_child_count(child)) {
            closed = !child || child->kind != TH_VAR;
            continue;
        }
        bool visited = false;
        if (indexed) (void)core_dict_get(&seen_index, child, &visited);
        for (size_t i = 0; !indexed && i < seen && !visited; i++)
            visited = seen_local[i] == child;
        if (visited) continue;
        if (!indexed && seen < sizeof(seen_local) / sizeof(*seen_local))
            seen_local[seen++] = child;
        else {
            if (!indexed) {
                core_dict_init(&seen_index, nullptr, nullptr);
                for (size_t i = 0; i < seen; i++)
                    core_dict_put(&seen_index, seen_local[i], 1);
                indexed = true;
            }
            core_dict_put(&seen_index, child, 1);
        }
        if (depth == capacity) {
            closed_frame_t *grown = core_alloc_array(closed_frame_t,
                                                     capacity * 2);
            memcpy(grown, frames, depth * sizeof(*frames));
            if (frames != local) core_free(frames);
            frames = grown;
            capacity *= 2;
        }
        frames[depth++] = (closed_frame_t){child, 0};
    }
    if (frames != local) core_free(frames);
    if (indexed) core_dict_free(&seen_index);
    return closed;
}

static bool type_contains(th_type *t,
                          bool (*matches)(const th_type *, const void *),
                          const void *target)
{
    core_dict_t seen;
    core_list_t(th_occ_ptr_t) work = core_list_new_cap(th_occ_ptr_t, 16);
    bool                      hit  = false;

    core_dict_init(&seen, nullptr, nullptr);

    core_list_push(work, t);

    while (!hit && work.len) {
        th_type *cur = th_resolve(work.data[--work.len]);

        if (!cur || core_dict_contains(&seen, cur)) {
            continue;
        }

        if (matches(cur, target)) {
            hit = true;

            break;
        }

        core_dict_put(&seen, cur, cur);

        switch (cur->kind) {
        case TH_NAMED:
            for (size_t i = 0; i < cur->named.nargs; i++) {
                if (cur->named.args[i].kind == TH_ARG_TYPE) {
                    core_list_push(work, cur->named.args[i].type);
                }
            }
            break;

        case TH_INDIRECTION:
            core_list_push(work, cur->ind.referent);
            break;

        case TH_SEQUENCE:
            core_list_push(work, cur->seq.elem);
            break;

        case TH_RECORD:
            for (size_t i = 0; i < cur->record.n; i++) {
                core_list_push(work, cur->record.members[i].type);
            }
            break;

        case TH_TRAIT:
            for (size_t i = 0; i < cur->trait.n; i++)
                core_list_push(work, cur->trait.methods[i].type);
            for (size_t i = 0; i < cur->trait.nr; i++) {
                core_list_push(work, cur->trait.requirements[i].subject);
                if (cur->trait.requirements[i].target)
                    core_list_push(work, cur->trait.requirements[i].target);
            }
            break;

        case TH_FUNCTION:
            core_list_push(work, cur->fn.ret);
            if (cur->fn.error) core_list_push(work, cur->fn.error);

            for (size_t i = 0; i < cur->fn.np; i++) {
                core_list_push(work, cur->fn.params[i].type);
            }

            if (cur->fn.rest) {
                core_list_push(work, cur->fn.rest->type);
            }

            for (size_t i = 0; i < cur->fn.nkw; i++) {
                core_list_push(work, cur->fn.kw[i].type);
            }
            break;

        case TH_BINDER:
            core_list_push(work, cur->binder.body);
            break;

        case TH_SUM:
            for (size_t i = 0; i < cur->sum.n; i++) {
                core_list_push(work, cur->sum.arms[i]);
            }
            break;

        default:
            break;
        }
    }

    core_dict_free(&seen);
    core_list_free(work);

    return hit;
}

static bool is_target(const th_type *candidate, const void *target)
{
    return candidate == target;
}

static bool is_poison_type(const th_type *candidate, const void *target)
{
    (void)target;
    return th_is_poison(candidate);
}

bool th_occurs(th_ctx *ctx, th_type *var, th_type *t)
{
    (void)ctx;
    return type_contains(t, is_target, th_resolve(var));
}

bool th_mentions_poison(th_type *t)
{
    return t && type_contains(t, is_poison_type, nullptr);
}

static th_type *dup_shallow(th_ctx *ctx, const th_type *t)
{
    th_type *o = th_alloc(ctx, sizeof(th_type));

    *o = *t;
    o->ground = 0;
    o->hashed = false;

    return o;
}

typedef struct {
    th_type **slot;
    th_type  *src;
    uint64_t  depth;
} rjob;

core_option_decl(rjob);
core_list_decl(rjob);

typedef bool (*subst_fn)(th_ctx *ctx, th_type *t, uint64_t depth,
                         core_dict_t *memo, void *st, th_type **out);

static void rpush(core_list_t(rjob) *w, th_type **slot, th_type *src,
                  uint64_t depth)
{
    core_list_push(*w, ((rjob){.slot = slot, .src = src, .depth = depth}));
}

static void plan_kids(th_ctx *ctx, th_type *out, const th_type *t,
                      uint64_t depth, core_list_t(rjob) *w)
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

    case TH_TRAIT:
        if (t->trait.n) {
            out->trait.methods = th_alloc(ctx,
                t->trait.n * sizeof(th_member));
            memcpy(out->trait.methods, t->trait.methods,
                t->trait.n * sizeof(th_member));
            for (size_t i = 0; i < t->trait.n; i++)
                rpush(w, &out->trait.methods[i].type,
                    t->trait.methods[i].type, depth);
        }
        if (t->trait.nr) {
            out->trait.requirements = th_alloc(ctx,
                t->trait.nr * sizeof(th_trait_requirement));
            memcpy(out->trait.requirements, t->trait.requirements,
                t->trait.nr * sizeof(th_trait_requirement));
            for (size_t i = 0; i < t->trait.nr; i++) {
                rpush(w, &out->trait.requirements[i].subject,
                    t->trait.requirements[i].subject, depth);
                if (t->trait.requirements[i].target)
                    rpush(w, &out->trait.requirements[i].target,
                        t->trait.requirements[i].target, depth);
            }
        }
        break;

    case TH_FUNCTION:
        rpush(w, &out->fn.ret, t->fn.ret, depth);
        if (t->fn.error) rpush(w, &out->fn.error, t->fn.error, depth);

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

static th_type *rebuild(th_ctx *ctx, th_type *root, core_dict_t *memo,
                        bool resolve, subst_fn subst, void *st)
{
    th_type          *out_root = nullptr;
    core_list_t(rjob)  w        = core_list_new(rjob);

    rpush(&w, &out_root, root, 0);

    while (w.len) {
        rjob     j = core_option_get(core_list_pop(rjob, w));
        th_type *t = resolve ? th_resolve(j.src) : j.src;

        if (!t) {
            *j.slot = nullptr;

            continue;
        }

        bool  found;
        void *hit = core_dict_get(memo, t, &found);

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

        core_dict_put(memo, t, out);
        *j.slot = out;

        plan_kids(ctx, out, t, j.depth, &w);
    }

    core_list_free(w);

    return out_root;
}

static bool subst_subst(th_ctx *ctx, th_type *t, uint64_t depth,
                        core_dict_t *memo, void *st, th_type **out)
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
    core_dict_t memo;

    core_dict_init(&memo, nullptr, nullptr);

    th_type *r = rebuild(ctx, t, &memo, true, subst_subst, nullptr);

    core_dict_free(&memo);

    return r;
}

typedef struct {
    th_type *a;
    th_type *b;
    bool     boundary;
} tpair;

core_option_decl(tpair);
core_list_decl(tpair);

typedef struct {
    core_dict_t memo;
    th_type **fresh;
    th_type **qvars;
    uint64_t  arity;
    uint64_t  depth;
} th_inst;

static bool inst_subst(th_ctx *ctx, th_type *t, uint64_t depth, core_dict_t *memo,
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
    th_inst  s = {.arity = n, .depth = 0, .qvars = t->binder.qvars};

    core_dict_init(&s.memo, nullptr, nullptr);
    s.fresh = core_alloc_array(th_type *, n);

    for (uint64_t i = 0; i < n; i++) {
        s.fresh[i] = th_var(ctx);

        th_type *q = t->binder.qvars ? t->binder.qvars[i] : nullptr;

        if (q) {
            s.fresh[i]->var.name = q->var.name;
        }
    }

    th_type *r = rebuild(ctx, t->binder.body, &s.memo, true, inst_subst, &s);

    core_dict_free(&s.memo);
    core_free(s.fresh);
    th_normalize(r);

    return r;
}

typedef struct {
    core_dict_t memo;
    core_dict_t index;
    th_type  **vars;
    size_t     nvars;
    uint64_t   depth;
} th_gen;

static bool gen_subst(th_ctx *ctx, th_type *t, uint64_t depth, core_dict_t *memo,
                      void *st, th_type **out)
{
    th_gen *s = st;

    (void)memo;

    if (t->kind != TH_VAR) {
        return false;
    }

    bool   found;
    size_t at = (size_t)(uintptr_t)core_dict_get(&s->index, t, &found);

    *out = found ? th_bound(ctx, depth + 1, at - 1) : t;

    return true;
}

static th_type *generalize_vars(th_ctx *ctx, th_type *t, th_type **vars,
                                size_t nvars)
{
    if (!nvars) {
        th_normalize(t);
        return t;
    }

    th_gen s = {.vars = vars, .nvars = nvars};
    core_dict_init(&s.memo, nullptr, nullptr);
    core_dict_init(&s.index, nullptr, nullptr);

    for (size_t i = 0; i < s.nvars; i++) {
        core_dict_put(&s.index, s.vars[i], i + 1);
    }

    th_type *body = rebuild(ctx, t, &s.memo, true, gen_subst, &s);

    core_dict_free(&s.memo);
    core_dict_free(&s.index);

    th_type *r = th_binder(ctx, s.nvars, body);
    r->binder.qvars = s.vars;
    th_normalize(r);
    return r;
}

th_type *th_generalize(th_ctx *ctx, th_type *t)
{
    th_type **vars = nullptr;
    size_t found = th_free_vars(ctx, t, &vars);
    size_t nvars = found;
    return generalize_vars(ctx, t, vars, nvars);
}

th_type *th_generalize_excluding(th_ctx *ctx, th_type *t,
                                 th_type *const *environment,
                                 size_t environment_count)
{
    if (!environment || !environment_count)
        return th_generalize(ctx, t);

    core_dict_t fixed;
    core_dict_init(&fixed, nullptr, nullptr);
    for (size_t i = 0; i < environment_count; i++) {
        th_type **vars = nullptr;
        size_t nvars = th_free_vars(ctx, environment[i], &vars);
        for (size_t j = 0; j < nvars; j++)
            core_dict_put(&fixed, vars[j], 1);
    }

    th_type **candidates = nullptr;
    size_t count = th_free_vars(ctx, t, &candidates);
    th_type **generalizable = th_alloc(ctx,
        (count ? count : 1) * sizeof(th_type *));
    size_t n_generalizable = 0;
    for (size_t i = 0; i < count; i++) {
        bool is_fixed = false;
        core_dict_get(&fixed, candidates[i], &is_fixed);
        if (!is_fixed)
            generalizable[n_generalizable++] = candidates[i];
    }
    core_dict_free(&fixed);
    return generalize_vars(ctx, t, generalizable, n_generalizable);
}

bool th_implements(th_ctx *ctx, th_type *type, core_string_t iface)
{
    th_type *resolved = type ? th_resolve(type) : nullptr;

    if (!ctx || !resolved || !iface.data) {
        return false;
    }
    if (resolved->kind == TH_VAR) {
        return false;
    }

    return th_ctx_environment(ctx) && th_env_implements(ctx, type, iface);
}

