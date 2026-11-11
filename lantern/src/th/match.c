#include "th/typehash.h"
#include "th/environment.h"
#include "private/th/internal.h"
#include "lib/alloc.h"
#include "lib/list.h"

typedef th_type *match_var_t;
core_list_decl(match_var_t);

typedef struct {
    th_type *pattern;
    th_type *concrete;
} match_pair_t;
core_list_decl(match_pair_t);

typedef struct {
    core_list_t(match_pair_t) active;
    core_list_t(match_var_t) bound;
    bool                     bind;
    bool                     both;
} matcher_t;

static bool unbound_var(const th_type *t)
{
    return t && t->kind == TH_VAR && !t->var.ref;
}

static bool mentions(th_type *t, th_type *var, size_t depth)
{
    t = th_resolve(t);
    if (!t || depth > 4096) {
        return false;
    }
    if (t == var) {
        return true;
    }
    for (size_t i = 0; i < th_child_count(t); i++) {
        if (mentions(th_child(t, i), var, depth + 1)) {
            return true;
        }
    }

    return false;
}

static bool same_modes(const th_type *a, const th_type *b)
{
    if (a->kind != TH_FUNCTION) {
        return true;
    }
    for (size_t i = 0; i < a->fn.np; i++) {
        if (a->fn.params[i].mode != b->fn.params[i].mode) {
            return false;
        }
    }

    return !a->fn.rest || a->fn.rest->mode == b->fn.rest->mode;
}

static bool walk(matcher_t *m, th_type *pattern, th_type *concrete,
                 size_t depth)
{
    pattern  = th_resolve(pattern);
    concrete = th_resolve(concrete);
    if (!pattern || !concrete) {
        return pattern == concrete;
    }
    if (pattern == concrete) {
        return true;
    }
    if (depth > 4096) {
        return false;
    }
    if (unbound_var(pattern)) {
        if (!m->bind || mentions(concrete, pattern, 0)) {
            return false;
        }
        th_var_bind(pattern, concrete);
        core_list_push(m->bound, pattern);
        return true;
    }
    if (m->both && unbound_var(concrete)) {
        if (mentions(pattern, concrete, 0)) {
            return false;
        }
        th_var_bind(concrete, pattern);
        core_list_push(m->bound, concrete);
        return true;
    }
    if (pattern->kind == TH_VAR || concrete->kind == TH_VAR
        || !th_same_shape(pattern, concrete)
        || !same_modes(pattern, concrete)) {
        return false;
    }
    for (size_t i = 0; i < m->active.len; i++) {
        if (m->active.data[i].pattern == pattern
            && m->active.data[i].concrete == concrete) {
            return true;
        }
    }
    core_list_push(m->active, ((match_pair_t){pattern, concrete}));

    bool ok = true;

    for (size_t i = 0; ok && i < th_child_count(pattern); i++) {
        ok = walk(m, th_child(pattern, i), th_child(concrete, i), depth + 1);
    }
    m->active.len--;

    return ok;
}

static void undo(matcher_t *m)
{
    for (size_t i = m->bound.len; i > 0; i--) {
        m->bound.data[i - 1]->var.ref = nullptr;
        m->bound.data[i - 1]->revision++;
    }
    m->bound.len = 0;
}

static bool run_mode(th_type *pattern, th_type *concrete, bool bind,
                     bool both, bool keep)
{
    matcher_t m  = {.active = core_list_new(match_pair_t),
                    .bound = core_list_new(match_var_t), .bind = bind,
                    .both = both};
    bool      ok = walk(&m, pattern, concrete, 0);

    if (!ok || !keep) {
        undo(&m);
    }
    core_list_free(m.bound);
    core_list_free(m.active);

    return ok;
}

static bool run(th_type *pattern, th_type *concrete, bool bind, bool keep)
{
    return run_mode(pattern, concrete, bind, false, keep);
}

bool th_match(th_type *pattern, th_type *concrete)
{
    return run(pattern, concrete, true, true);
}

bool th_matches(th_type *pattern, th_type *concrete)
{
    return run(pattern, concrete, true, false);
}

bool th_match_each(th_type *pattern, th_type *concrete, th_binding_fn bound,
                   void *context)
{
    matcher_t m  = {.active = core_list_new(match_pair_t),
                    .bound = core_list_new(match_var_t), .bind = true};
    bool      ok = walk(&m, pattern, concrete, 0);

    for (size_t i = 0; ok && bound && i < m.bound.len; i++) {
        ok = bound(context, m.bound.data[i], th_resolve(m.bound.data[i]));
    }
    undo(&m);
    core_list_free(m.bound);
    core_list_free(m.active);

    return ok;
}

bool th_same_type(th_type *a, th_type *b)
{
    return run(a, b, false, false);
}

th_type *th_fill(th_type *placeholder, th_type *type)
{
    th_type *slot = th_resolve(placeholder);
    th_type *value = th_resolve(type);

    if (!slot) {
        return value;
    }
    if (!value || slot == value) {
        return slot;
    }
    if (unbound_var(slot)) {
        return mentions(value, slot, 0) || !th_var_bind(slot, value)
                   ? nullptr
                   : value;
    }

    return th_same_type(slot, value) ? slot : nullptr;
}

bool th_compatible(th_type *a, th_type *b)
{
    return run_mode(a, b, true, true, false);
}

th_type *th_meet(th_ctx *ctx, th_type *a, th_type *b)
{
    const th_type *sources[2] = {a, b};
    th_type       *copies[2]  = {nullptr, nullptr};

    if (!ctx || !a || !b || !th_type_clone_many(ctx, sources, copies, 2)
        || !run_mode(copies[0], copies[1], true, true, true)) {
        return nullptr;
    }

    return th_resolve(copies[0]);
}

bool th_type_widens_to(const th_type *from, const th_type *to)
{
    from = th_resolve((th_type *)from);
    to   = th_resolve((th_type *)to);
    if (!from || !to) {
        return false;
    }
    if (from == to) {
        return true;
    }
    if (from->kind != TH_BITINT || to->kind != TH_BITINT) {
        return false;
    }
    if (from->bitint.width == to->bitint.width
        && from->bitint.flavor == to->bitint.flavor) {
        return true;
    }
    if (from->bitint.flavor == TH_BITINT_BITS
        || to->bitint.flavor == TH_BITINT_BITS
        || from->bitint.width >= to->bitint.width) {
        return false;
    }
    if (from->bitint.flavor == TH_BITINT_SIGNED) {
        return to->bitint.flavor == TH_BITINT_SIGNED;
    }

    return true;
}

bool th_match_pair(th_type *a, th_type *b)
{
    return run_mode(a, b, true, true, true);
}

