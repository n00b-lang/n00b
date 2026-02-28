/**
 * @file construct.c
 * @brief Type constructor implementations for the type inference engine.
 * @ingroup typecheck
 */

#include "typecheck/construct.h"
#include "typecheck/context.h"
#include "core/alloc.h"
#include "core/string.h"
#include "core/vargs.h"

#include <stdio.h>

// ============================================================================
// Helpers
// ============================================================================

static n00b_tc_type_t *
tc_alloc_type(n00b_tc_ctx_t *ctx)
{
    n00b_tc_type_t *t = n00b_alloc(n00b_tc_type_t);
    t->forward        = nullptr;
    n00b_tc_ctx_register(ctx, t);
    return t;
}

// ============================================================================
// n00b_tc_var — named type variable
// ============================================================================

n00b_tc_type_t *
n00b_tc_var(n00b_tc_ctx_t *ctx, n00b_string_t *name)
{
    n00b_tc_type_t *t = tc_alloc_type(ctx);

    n00b_tc_var_t var = {
        .id           = ctx->next_var_id++,
        .given_name   = n00b_option_set(n00b_string_t *, name),
        .display_name = name,
        .constraints  = nullptr,
    };

    _n00b_variant_set_ptr(&t->kind, n00b_tc_var_t, var);
    return t;
}

// ============================================================================
// n00b_tc_fresh_var — anonymous type variable
// ============================================================================

n00b_tc_type_t *
n00b_tc_fresh_var(n00b_tc_ctx_t *ctx)
{
    uint32_t id = ctx->next_var_id++;

    // Generate display name like "t_42".
    char buf[32];
    snprintf(buf, sizeof(buf), "t_%u", id);
    n00b_string_t *display = n00b_string_from_cstr(buf);

    n00b_tc_type_t *t = tc_alloc_type(ctx);

    n00b_tc_var_t var = {
        .id           = id,
        .given_name   = n00b_option_none(n00b_string_t *),
        .display_name = display,
        .constraints  = nullptr,
    };

    _n00b_variant_set_ptr(&t->kind, n00b_tc_var_t, var);
    return t;
}

// ============================================================================
// n00b_tc_prim — primitive type
// ============================================================================

n00b_tc_type_t *
n00b_tc_prim(n00b_tc_ctx_t *ctx, n00b_string_t *name)
{
    n00b_tc_type_t *t = tc_alloc_type(ctx);

    n00b_tc_prim_t prim = {
        .name = name,
    };

    _n00b_variant_set_ptr(&t->kind, n00b_tc_prim_t, prim);
    return t;
}

// ============================================================================
// n00b_tc_param — parameterized type
// ============================================================================

n00b_tc_type_t *
n00b_tc_param(n00b_tc_ctx_t *ctx, n00b_string_t *name, n00b_tc_type_t *+)
{
    n00b_tc_type_t *t = tc_alloc_type(ctx);

    n00b_list_t(n00b_tc_type_t *) params = n00b_list_new_private(n00b_tc_type_t *);

    unsigned int count = n00b_remaining_vargs(vargs);
    for (unsigned int i = 0; i < count; i++) {
        n00b_tc_type_t *p = (n00b_tc_type_t *)n00b_vargs_next(vargs);
        n00b_list_push(params, p);
    }

    n00b_tc_param_t param = {
        .name   = name,
        .params = n00b_alloc(n00b_list_t(n00b_tc_type_t *)),
    };
    *param.params = params;

    _n00b_variant_set_ptr(&t->kind, n00b_tc_param_t, param);
    return t;
}

// ============================================================================
// n00b_tc_fn — function type
// ============================================================================

n00b_tc_type_t *
n00b_tc_fn(n00b_tc_ctx_t *ctx, n00b_tc_type_t *+)
    _kargs {
        n00b_tc_type_t *returns  = nullptr;
        n00b_tc_type_t *variadic = nullptr;
        n00b_tc_type_t *kwonly   = nullptr;
    }
{
    (void)returns;
    (void)variadic;
    (void)kwonly;

    n00b_tc_type_t *t = tc_alloc_type(ctx);

    // Collect positional params from typed vargs.
    n00b_list_t(n00b_tc_type_t *) positional = n00b_list_new_private(n00b_tc_type_t *);

    unsigned int count = n00b_remaining_vargs(vargs);
    for (unsigned int i = 0; i < count; i++) {
        n00b_tc_type_t *p = (n00b_tc_type_t *)n00b_vargs_next(vargs);
        n00b_list_push(positional, p);
    }

    n00b_tc_fn_t fn = {
        .positional  = n00b_alloc(n00b_list_t(n00b_tc_type_t *)),
        .vargs_type  = kargs->variadic,
        .kargs_type  = kargs->kwonly,
        .return_type = kargs->returns,
    };
    *fn.positional = positional;

    _n00b_variant_set_ptr(&t->kind, n00b_tc_fn_t, fn);
    return t;
}

// ============================================================================
// n00b_tc_sum — sum type
// ============================================================================

n00b_tc_type_t *
n00b_tc_sum(n00b_tc_ctx_t *ctx, n00b_tc_type_t *+)
{
    n00b_tc_type_t *t = tc_alloc_type(ctx);

    n00b_list_t(n00b_tc_type_t *) variants = n00b_list_new_private(n00b_tc_type_t *);

    unsigned int count = n00b_remaining_vargs(vargs);
    for (unsigned int i = 0; i < count; i++) {
        n00b_tc_type_t *v = (n00b_tc_type_t *)n00b_vargs_next(vargs);
        n00b_list_push(variants, v);
    }

    n00b_tc_sum_t sum = {
        .variants = n00b_alloc(n00b_list_t(n00b_tc_type_t *)),
    };
    *sum.variants = variants;

    _n00b_variant_set_ptr(&t->kind, n00b_tc_sum_t, sum);
    return t;
}

// ============================================================================
// n00b_tc_field — field descriptor (returned by value)
// ============================================================================

n00b_tc_field_t
n00b_tc_field(n00b_string_t *name, n00b_tc_type_t *type)
    _kargs { bool has_default = false; }
{
    (void)has_default;

    return (n00b_tc_field_t){
        .name        = name,
        .type        = type,
        .has_default = kargs->has_default,
    };
}

// ============================================================================
// n00b_tc_record — record type
// ============================================================================

n00b_tc_type_t *
n00b_tc_record(n00b_tc_ctx_t *ctx, n00b_string_t *name, n00b_tc_field_t +)
    _kargs {
        bool ordered = true;
        bool open    = false;
    }
{
    (void)ordered;
    (void)open;

    n00b_tc_type_t *t = tc_alloc_type(ctx);

    n00b_list_t(n00b_string_t *)    field_names       = n00b_list_new_private(n00b_string_t *);
    n00b_list_t(n00b_tc_type_t *) field_types        = n00b_list_new_private(n00b_tc_type_t *);
    n00b_list_t(bool)             field_has_default   = n00b_list_new_private(bool);

    bool any_default = false;

    unsigned int count = n00b_remaining_vargs(vargs);
    for (unsigned int i = 0; i < count; i++) {
        n00b_tc_field_t *fp = (n00b_tc_field_t *)n00b_vargs_next(vargs);
        n00b_list_push(field_names, fp->name);
        n00b_list_push(field_types, fp->type);
        n00b_list_push(field_has_default, fp->has_default);
        if (fp->has_default) {
            any_default = true;
        }
    }

    n00b_tc_record_t rec = {
        .name              = name,
        .type_params       = nullptr,
        .field_names       = n00b_alloc(n00b_list_t(n00b_string_t *)),
        .field_types       = n00b_alloc(n00b_list_t(n00b_tc_type_t *)),
        .field_has_default = any_default
                                 ? n00b_alloc(n00b_list_t(bool))
                                 : nullptr,
        .open              = kargs->open,
        .ordered           = kargs->ordered,
    };

    *rec.field_names = field_names;
    *rec.field_types = field_types;
    if (any_default) {
        *rec.field_has_default = field_has_default;
    }

    _n00b_variant_set_ptr(&t->kind, n00b_tc_record_t, rec);
    return t;
}

// ============================================================================
// n00b_tc_tuple — tuple type
// ============================================================================

n00b_tc_type_t *
n00b_tc_tuple(n00b_tc_ctx_t *ctx, n00b_tc_type_t *+)
    _kargs { bool open = false; }
{
    (void)open;

    n00b_tc_type_t *t = tc_alloc_type(ctx);

    n00b_list_t(n00b_tc_type_t *) elements = n00b_list_new_private(n00b_tc_type_t *);

    unsigned int count = n00b_remaining_vargs(vargs);
    for (unsigned int i = 0; i < count; i++) {
        n00b_tc_type_t *e = (n00b_tc_type_t *)n00b_vargs_next(vargs);
        n00b_list_push(elements, e);
    }

    n00b_tc_tuple_t tup = {
        .elements = n00b_alloc(n00b_list_t(n00b_tc_type_t *)),
        .min_len  = (uint16_t)n00b_list_len(elements),
        .open     = kargs->open,
    };
    *tup.elements = elements;

    _n00b_variant_set_ptr(&t->kind, n00b_tc_tuple_t, tup);
    return t;
}
