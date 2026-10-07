#include <string.h>

#include "private/th/internal.h"

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

bool th_bitint_info(const th_type *t, uint64_t *width,
                    th_bitint_flavor *flavor)
{
    const th_type *n = as(t, TH_BITINT);

    if (!n) {
        return false;
    }

    if (width) {
        *width = n->bitint.width;
    }

    if (flavor) {
        *flavor = n->bitint.flavor;
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

static bool same_named_type(const th_type *left, const th_type *right)
{
    const char *left_name = nullptr, *right_name = nullptr;
    size_t left_len = 0, right_len = 0;
    return th_named_info(left, &left_name, &left_len, nullptr, nullptr)
        && th_named_info(right, &right_name, &right_len, nullptr, nullptr)
        && left_len == right_len
        && !memcmp(left_name, right_name, left_len);
}

bool th_type_define(th_ctx *ctx, th_type *type, th_type *definition)
{
    if (!ctx || !type || !definition || th_kind_of(th_resolve(type)) != TH_NAMED)
        return false;
    for (size_t i = 0; i < ctx->type_definitions.len; i++) {
        th_type_definition_t *entry = &ctx->type_definitions.data[i];
        if (!same_named_type(entry->type, type)) continue;
        if (entry->definition == definition
            || th_hash(ctx, entry->definition) == th_hash(ctx, definition))
            return true;
        return false;
    }
    core_list_push(ctx->type_definitions,
                   ((th_type_definition_t){type, definition}));
    return true;
}

th_type *th_type_definition(const th_ctx *ctx, const th_type *type)
{
    if (!ctx || !type) return nullptr;
    type = th_resolve_c(type);
    for (size_t i = 0; i < ctx->type_definitions.len; i++) {
        const th_type_definition_t *entry = &ctx->type_definitions.data[i];
        if (same_named_type(entry->type, type)) return entry->definition;
    }
    return nullptr;
}

th_type *th_record_lookup_in(const th_ctx *ctx, const th_type *type,
                             const char *name)
{
    th_type *member = th_record_lookup(type, name);
    if (member || !ctx) return member;
    th_type *definition = th_type_definition(ctx, type);
    return definition ? th_record_lookup(definition, name) : nullptr;
}

th_type *th_func_ret(const th_type *t)
{
    const th_type *n = as(t, TH_FUNCTION);

    return n ? n->fn.ret : nullptr;
}

th_type *th_func_error(const th_type *t)
{
    const th_type *n = as(t, TH_FUNCTION);
    return n ? n->fn.error : nullptr;
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

const char *th_var_name(const th_type *t)
{
    const th_type *n = as(t, TH_VAR);

    return n ? n->var.name : nullptr;
}

static bool named_is(const th_type *t, const char *name, size_t n)
{
    const th_type *v = as(t, TH_NAMED);

    return v && v->named.nargs == 0 && v->named.nlen == n
        && !memcmp(v->named.name, name, n);
}

bool th_is_erased(const th_type *t)
{
    return named_is(t, TH_ERASED, sizeof(TH_ERASED) - 1);
}

bool th_is_bottom(const th_type *t)
{
    return named_is(t, TH_BOTTOM, sizeof(TH_BOTTOM) - 1);
}
