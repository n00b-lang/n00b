#include "th/typehash.h"
#include "private/th/internal.h"

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

th_type *th_var_ref(const th_type *var)
{
    return var && var->kind == TH_VAR ? var->var.ref : nullptr;
}

uint64_t th_var_revision(const th_type *var)
{
    return var && var->kind == TH_VAR ? var->revision : 0;
}

bool th_var_bind(th_type *var, th_type *to)
{
    if (!var || !to || var->kind != TH_VAR || var->var.ref || var == to) {
        return false;
    }
    var->var.ref = to;
    var->revision++;

    return true;
}

bool th_var_rebind(th_type *var, th_type *to)
{
    if (!var || !to || var->kind != TH_VAR || var == to) {
        return false;
    }
    var->var.ref = to;
    var->revision++;

    return true;
}

static bool same_args(const th_type *a, const th_type *b)
{
    if (!core_string_eq(a->named.name, b->named.name)
        || a->named.is_variant != b->named.is_variant
        || a->named.nargs != b->named.nargs) {
        return false;
    }
    for (size_t i = 0; i < a->named.nargs; i++) {
        const th_arg *x = &a->named.args[i];
        const th_arg *y = &b->named.args[i];

        if (x->kind != y->kind) {
            return false;
        }
        switch (x->kind) {
        case TH_ARG_TYPE:
            break;
        case TH_ARG_UINT:
            if (x->u != y->u) {
                return false;
            }
            break;
        case TH_ARG_SINT:
            if (x->s != y->s) {
                return false;
            }
            break;
        case TH_ARG_STR:
            if (!core_string_eq(x->str, y->str)) {
                return false;
            }
            break;
        }
    }

    return true;
}

static bool same_function(const th_type *a, const th_type *b)
{
    if (a->fn.np != b->fn.np || !a->fn.error != !b->fn.error
        || !a->fn.rest != !b->fn.rest || a->fn.nkw != b->fn.nkw) {
        return false;
    }
    for (size_t i = 0; i < a->fn.nkw; i++) {
        if (!core_string_eq(a->fn.kw[i].name, b->fn.kw[i].name)) {
            return false;
        }
    }

    return true;
}

static bool same_members(const th_type *a, const th_type *b)
{
    if (a->record.n != b->record.n) {
        return false;
    }
    for (size_t i = 0; i < a->record.n; i++) {
        if (!core_string_eq(a->record.members[i].name,
                            b->record.members[i].name)) {
            return false;
        }
    }

    return true;
}

bool th_same_shape(const th_type *a, const th_type *b)
{
    if (!a || !b || a->kind != b->kind) {
        return false;
    }

    switch (a->kind) {
    case TH_NAMED:
        return same_args(a, b);
    case TH_BITINT:
        return a->bitint.width == b->bitint.width
            && a->bitint.flavor == b->bitint.flavor;
    case TH_INDIRECTION:
        return a->ind.attrs == b->ind.attrs;
    case TH_SEQUENCE:
        return a->seq.has_extent == b->seq.has_extent
            && (!a->seq.has_extent || a->seq.extent == b->seq.extent);
    case TH_RECORD:
        return same_members(a, b);
    case TH_FUNCTION:
        return same_function(a, b);
    case TH_BINDER:
        return a->binder.arity == b->binder.arity;
    case TH_BOUND:
        return a->bound.distance == b->bound.distance
            && a->bound.position == b->bound.position;
    case TH_SUM:
        return a->sum.comb == b->sum.comb && a->sum.n == b->sum.n;
    case TH_TRAIT:
        return core_string_eq(a->trait.name, b->trait.name);
    case TH_VAR:
        return a == b;
    }

    return false;
}
