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

    // Canonical idiom: heap-allocate the list shell and struct-copy
    // the fully-populated lvalue in BEFORE the heap shell becomes
    // reachable through param.params, so the GC never sees a
    // zero-scan-info heap blob.
    n00b_list_t(n00b_tc_type_t *) *params_ptr =
        n00b_alloc(n00b_list_t(n00b_tc_type_t *));
    *params_ptr = params;

    n00b_tc_param_t param = {
        .name   = name,
        .params = params_ptr,
    };

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

    // Canonical idiom: heap-allocate + struct-copy BEFORE the heap
    // shell is reachable through fn.positional.
    n00b_list_t(n00b_tc_type_t *) *positional_ptr =
        n00b_alloc(n00b_list_t(n00b_tc_type_t *));
    *positional_ptr = positional;

    n00b_tc_fn_t fn = {
        .positional  = positional_ptr,
        .vargs_type  = kargs->variadic,
        .kargs_type  = kargs->kwonly,
        .return_type = kargs->returns,
    };

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

    // Canonical idiom: heap-allocate + struct-copy BEFORE the heap
    // shell is reachable through sum.variants.
    n00b_list_t(n00b_tc_type_t *) *variants_ptr =
        n00b_alloc(n00b_list_t(n00b_tc_type_t *));
    *variants_ptr = variants;

    n00b_tc_sum_t sum = {
        .variants = variants_ptr,
    };

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

    // Canonical idiom: heap-allocate + struct-copy each list BEFORE
    // the heap shell becomes reachable through rec.
    n00b_list_t(n00b_string_t *) *field_names_ptr =
        n00b_alloc(n00b_list_t(n00b_string_t *));
    *field_names_ptr = field_names;

    n00b_list_t(n00b_tc_type_t *) *field_types_ptr =
        n00b_alloc(n00b_list_t(n00b_tc_type_t *));
    *field_types_ptr = field_types;

    n00b_list_t(bool) *field_has_default_ptr = nullptr;
    if (any_default) {
        field_has_default_ptr = n00b_alloc(n00b_list_t(bool));
        *field_has_default_ptr = field_has_default;
    }

    n00b_tc_record_t rec = {
        .name              = name,
        .type_params       = nullptr,
        .field_names       = field_names_ptr,
        .field_types       = field_types_ptr,
        .field_has_default = field_has_default_ptr,
        .open              = kargs->open,
        .ordered           = kargs->ordered,
    };

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

    // Canonical idiom: heap-allocate + struct-copy BEFORE the heap
    // shell becomes reachable through tup.elements.
    n00b_list_t(n00b_tc_type_t *) *elements_ptr =
        n00b_alloc(n00b_list_t(n00b_tc_type_t *));
    *elements_ptr = elements;

    n00b_tc_tuple_t tup = {
        .elements = elements_ptr,
        .min_len  = (uint16_t)n00b_list_len(elements),
        .open     = kargs->open,
    };

    _n00b_variant_set_ptr(&t->kind, n00b_tc_tuple_t, tup);
    return t;
}

// ============================================================================
// Type scheme instantiation
// ============================================================================

// Map from original Var pointer to fresh Var, for preserving internal sharing.
#define INST_MAP_CAP 32

typedef struct {
    n00b_tc_type_t *orig;
    n00b_tc_type_t *fresh;
} inst_map_entry_t;

typedef struct {
    n00b_tc_ctx_t    *ctx;
    inst_map_entry_t  entries[INST_MAP_CAP];
    int32_t           count;
} inst_ctx_t;

static n00b_tc_type_t *
inst_lookup(inst_ctx_t *ic, n00b_tc_type_t *orig)
{
    for (int32_t i = 0; i < ic->count; i++) {
        if (ic->entries[i].orig == orig) {
            return ic->entries[i].fresh;
        }
    }

    return NULL;
}

static void
inst_insert(inst_ctx_t *ic, n00b_tc_type_t *orig, n00b_tc_type_t *fresh)
{
    if (ic->count < INST_MAP_CAP) {
        ic->entries[ic->count++] = (inst_map_entry_t){orig, fresh};
    }
}

static n00b_tc_type_t *inst_rec(inst_ctx_t *ic, n00b_tc_type_t *t);

static n00b_list_t(n00b_tc_type_t *) *
inst_list(inst_ctx_t *ic, n00b_list_t(n00b_tc_type_t *) *src)
{
    if (!src) {
        return NULL;
    }

    size_t n = n00b_list_len(*src);

    // Canonical idiom: populate a fully scan-info-threaded lvalue
    // first, then struct-copy into the heap-allocated return shell.
    n00b_list_t(n00b_tc_type_t *) lst = n00b_list_new_private(n00b_tc_type_t *);

    for (size_t i = 0; i < n; i++) {
        n00b_tc_type_t *elem = n00b_list_get(*src, i);
        n00b_list_push(lst, inst_rec(ic, elem));
    }

    n00b_list_t(n00b_tc_type_t *) *dst = n00b_alloc(n00b_list_t(n00b_tc_type_t *));
    *dst = lst;
    return dst;
}

static n00b_tc_type_t *
inst_rec(inst_ctx_t *ic, n00b_tc_type_t *t)
{
    if (!t) {
        return NULL;
    }

    // Follow union-find chain.
    while (t->forward) {
        t = t->forward;
    }

    // Var: map to fresh var (preserving sharing).
    if (n00b_variant_is_type(t->kind, n00b_tc_var_t)) {
        n00b_tc_type_t *existing = inst_lookup(ic, t);

        if (existing) {
            return existing;
        }

        n00b_tc_type_t *fresh = n00b_tc_fresh_var(ic->ctx);
        inst_insert(ic, t, fresh);
        return fresh;
    }

    // Prim: no internal type vars, return as-is.
    if (n00b_variant_is_type(t->kind, n00b_tc_prim_t)) {
        return t;
    }

    // Fn: instantiate params, return, vargs, kargs.
    if (n00b_variant_is_type(t->kind, n00b_tc_fn_t)) {
        n00b_tc_fn_t fn = n00b_variant_get(t->kind, n00b_tc_fn_t);

        n00b_tc_type_t *nt  = n00b_alloc(n00b_tc_type_t);
        n00b_tc_fn_t    nfn = {
            .positional  = inst_list(ic, fn.positional),
            .vargs_type  = inst_rec(ic, fn.vargs_type),
            .kargs_type  = inst_rec(ic, fn.kargs_type),
            .return_type = inst_rec(ic, fn.return_type),
        };
        _n00b_variant_set_ptr(&nt->kind, n00b_tc_fn_t, nfn);
        return nt;
    }

    // Param: instantiate type parameters.
    if (n00b_variant_is_type(t->kind, n00b_tc_param_t)) {
        n00b_tc_param_t p = n00b_variant_get(t->kind, n00b_tc_param_t);

        n00b_tc_type_t  *nt = n00b_alloc(n00b_tc_type_t);
        n00b_tc_param_t  np = {
            .name   = p.name,
            .params = inst_list(ic, p.params),
        };
        _n00b_variant_set_ptr(&nt->kind, n00b_tc_param_t, np);
        return nt;
    }

    // Record, Tuple, Sum: return as-is for now (no polymorphic records yet).
    return t;
}

n00b_tc_type_t *
n00b_tc_instantiate(n00b_tc_ctx_t *ctx, n00b_tc_type_t *t)
{
    if (!ctx || !t) {
        return t;
    }

    inst_ctx_t ic = {.ctx = ctx, .count = 0};
    return inst_rec(&ic, t);
}
