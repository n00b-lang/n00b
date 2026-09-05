#include <string.h>

#include "th_internal.h"

static const th_type *as(const th_type *t, th_kind k)
{
    t = th_resolve_c(t);

    return t && t->kind == k ? t : nullptr;
}

bool th_named_info(const th_type *t, const char **name, size_t *nlen,
                   bool *is_variant, size_t *nargs)
{
    const th_type *n = as(t, TH_NAMED);

    if (!n) {
        return false;
    }

    if (name) {
        *name = n->named.name;
    }

    if (nlen) {
        *nlen = n->named.nlen;
    }

    if (is_variant) {
        *is_variant = n->named.is_variant;
    }

    if (nargs) {
        *nargs = n->named.nargs;
    }

    return true;
}

size_t th_named_nargs(const th_type *t)
{
    const th_type *n = as(t, TH_NAMED);

    return n ? n->named.nargs : 0;
}

const th_arg *th_named_arg(const th_type *t, size_t i)
{
    const th_type *n = as(t, TH_NAMED);

    return n && i < n->named.nargs ? &n->named.args[i] : nullptr;
}

bool th_bitint_info(const th_type *t, uint64_t *width, bool *is_signed)
{
    const th_type *n = as(t, TH_BITINT);

    if (!n) {
        return false;
    }

    if (width) {
        *width = n->bitint.width;
    }

    if (is_signed) {
        *is_signed = n->bitint.is_signed;
    }

    return true;
}

th_type *th_ind_referent(const th_type *t)
{
    const th_type *n = as(t, TH_INDIRECTION);

    return n ? n->ind.referent : nullptr;
}

bool th_ind_attrs(const th_type *t, uint8_t *attrs)
{
    const th_type *n = as(t, TH_INDIRECTION);

    if (!n) {
        return false;
    }

    if (attrs) {
        *attrs = n->ind.attrs;
    }

    return true;
}

th_type *th_seq_elem(const th_type *t)
{
    const th_type *n = as(t, TH_SEQUENCE);

    return n ? n->seq.elem : nullptr;
}

bool th_seq_extent(const th_type *t, uint64_t *extent)
{
    const th_type *n = as(t, TH_SEQUENCE);

    if (!n || !n->seq.has_extent) {
        return false;
    }

    if (extent) {
        *extent = n->seq.extent;
    }

    return true;
}

size_t th_record_len(const th_type *t)
{
    const th_type *n = as(t, TH_RECORD);

    return n ? n->record.n : 0;
}

const th_member *th_record_member(const th_type *t, size_t i)
{
    const th_type *n = as(t, TH_RECORD);

    return n && i < n->record.n ? &n->record.members[i] : nullptr;
}

th_type *th_record_lookup(const th_type *t, const char *name)
{
    const th_type *n = as(t, TH_RECORD);

    if (!n || !name) {
        return nullptr;
    }

    for (size_t i = 0; i < n->record.n; i++) {
        if (!strcmp(n->record.members[i].name, name)) {
            return n->record.members[i].type;
        }
    }

    return nullptr;
}

th_type *th_func_ret(const th_type *t)
{
    const th_type *n = as(t, TH_FUNCTION);

    return n ? n->fn.ret : nullptr;
}

size_t th_func_nparams(const th_type *t)
{
    const th_type *n = as(t, TH_FUNCTION);

    return n ? n->fn.np : 0;
}

const th_param *th_func_param(const th_type *t, size_t i)
{
    const th_type *n = as(t, TH_FUNCTION);

    return n && i < n->fn.np ? &n->fn.params[i] : nullptr;
}

const th_param *th_func_rest(const th_type *t)
{
    const th_type *n = as(t, TH_FUNCTION);

    return n ? n->fn.rest : nullptr;
}

size_t th_func_nkw(const th_type *t)
{
    const th_type *n = as(t, TH_FUNCTION);

    return n ? n->fn.nkw : 0;
}

const th_kwparam *th_func_kw(const th_type *t, size_t i)
{
    const th_type *n = as(t, TH_FUNCTION);

    return n && i < n->fn.nkw ? &n->fn.kw[i] : nullptr;
}

const th_kwparam *th_func_kw_lookup(const th_type *t, const char *name)
{
    const th_type *n = as(t, TH_FUNCTION);

    if (!n || !name) {
        return nullptr;
    }

    for (size_t i = 0; i < n->fn.nkw; i++) {
        if (!strcmp(n->fn.kw[i].name, name)) {
            return &n->fn.kw[i];
        }
    }

    return nullptr;
}

bool th_binder_arity(const th_type *t, uint64_t *arity)
{
    const th_type *n = as(t, TH_BINDER);

    if (!n) {
        return false;
    }

    if (arity) {
        *arity = n->binder.arity;
    }

    return true;
}

th_type *th_binder_body(const th_type *t)
{
    const th_type *n = as(t, TH_BINDER);

    return n ? n->binder.body : nullptr;
}

bool th_bound_info(const th_type *t, uint64_t *distance, uint64_t *position)
{
    const th_type *n = as(t, TH_BOUND);

    if (!n) {
        return false;
    }

    if (distance) {
        *distance = n->bound.distance;
    }

    if (position) {
        *position = n->bound.position;
    }

    return true;
}

bool th_sum_comb(const th_type *t, th_comb *comb)
{
    const th_type *n = as(t, TH_SUM);

    if (!n) {
        return false;
    }

    if (comb) {
        *comb = n->sum.comb;
    }

    return true;
}

size_t th_sum_len(const th_type *t)
{
    const th_type *n = as(t, TH_SUM);

    return n ? n->sum.n : 0;
}

th_type *th_sum_arm(const th_type *t, size_t i)
{
    const th_type *n = as(t, TH_SUM);

    return n && i < n->sum.n ? n->sum.arms[i] : nullptr;
}

bool th_var_index(const th_type *t, uint64_t *index)
{
    const th_type *n = as(t, TH_VAR);

    if (!n || !n->var.has_index) {
        return false;
    }

    if (index) {
        *index = n->var.index;
    }

    return true;
}
