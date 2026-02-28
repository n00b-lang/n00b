/**
 * @file context.c
 * @brief Type-checking context implementation.
 * @ingroup typecheck
 */

#include "typecheck/context.h"
#include "typecheck/construct.h"
#include "logic/asp_rule.h"
#include "core/alloc.h"
#include "adt/list.h"
#include "core/string.h"
#include "core/vargs.h"
#include "text/strings/string_ops.h"

// ============================================================================
// Lifecycle
// ============================================================================

static void
tc_install_transitive_promotion(n00b_tc_ctx_t *ctx)
{
    // promotes(A, C) :- promotes(A, B), promotes(B, C).
    n00b_dl_sym_t a = n00b_logic_var(&ctx->logic, *r"A");
    n00b_dl_sym_t b = n00b_logic_var(&ctx->logic, *r"B");
    n00b_dl_sym_t c = n00b_logic_var(&ctx->logic, *r"C");

    n00b_dl_rule_builder_t rb;
    n00b_dl_rule_builder_init(&rb);

    // Head: promotes(A, C)
    n00b_dl_rule_builder_head(&rb, ctx->rel_promotes, 2,
                               (n00b_dl_sym_t[]){a, c});
    // Body: promotes(A, B), promotes(B, C)
    n00b_dl_rule_builder_add(&rb, ctx->rel_promotes, 2,
                              (n00b_dl_sym_t[]){a, b}, false);
    n00b_dl_rule_builder_add(&rb, ctx->rel_promotes, 2,
                              (n00b_dl_sym_t[]){b, c}, false);

    n00b_dl_rule_t rule = n00b_dl_rule_builder_finish(&rb);
    n00b_logic_add_rule(&ctx->logic, rule);
}

static void
tc_register_numeric_promotions(n00b_tc_ctx_t *ctx)
{
    // Signed widening.
    n00b_tc_register_promotion(ctx, *r"i8",  *r"i16");
    n00b_tc_register_promotion(ctx, *r"i16", *r"i32");
    n00b_tc_register_promotion(ctx, *r"i32", *r"i64");
    n00b_tc_register_promotion(ctx, *r"i32", *r"int");

    // Unsigned widening.
    n00b_tc_register_promotion(ctx, *r"u8",  *r"u16");
    n00b_tc_register_promotion(ctx, *r"u16", *r"u32");
    n00b_tc_register_promotion(ctx, *r"u32", *r"u64");

    // Unsigned → next-wider-signed (safe, no info loss).
    n00b_tc_register_promotion(ctx, *r"u8",  *r"i16");
    n00b_tc_register_promotion(ctx, *r"u16", *r"i32");
    n00b_tc_register_promotion(ctx, *r"u32", *r"i64");
    n00b_tc_register_promotion(ctx, *r"u32", *r"int");

    // Float widening.
    n00b_tc_register_promotion(ctx, *r"f32", *r"f64");

    // Integer → float (lossy but conventional).
    n00b_tc_register_promotion(ctx, *r"i32", *r"f64");
    n00b_tc_register_promotion(ctx, *r"int", *r"f64");
    n00b_tc_register_promotion(ctx, *r"i64", *r"f64");

    // int ↔ i64 aliasing (bidirectional).
    n00b_tc_register_promotion(ctx, *r"int", *r"i64");
    n00b_tc_register_promotion(ctx, *r"i64", *r"int");

    // bool → int (true=1, false=0).
    n00b_tc_register_promotion(ctx, *r"bool", *r"int");
    n00b_tc_register_promotion(ctx, *r"bool", *r"i64");
}

static void
tc_create_builtins(n00b_tc_ctx_t *ctx)
{
    ctx->t_int    = n00b_tc_prim(ctx, *r"int");
    ctx->t_i8     = n00b_tc_prim(ctx, *r"i8");
    ctx->t_i16    = n00b_tc_prim(ctx, *r"i16");
    ctx->t_i32    = n00b_tc_prim(ctx, *r"i32");
    ctx->t_i64    = n00b_tc_prim(ctx, *r"i64");
    ctx->t_u8     = n00b_tc_prim(ctx, *r"u8");
    ctx->t_u16    = n00b_tc_prim(ctx, *r"u16");
    ctx->t_u32    = n00b_tc_prim(ctx, *r"u32");
    ctx->t_u64    = n00b_tc_prim(ctx, *r"u64");
    ctx->t_f32    = n00b_tc_prim(ctx, *r"f32");
    ctx->t_f64    = n00b_tc_prim(ctx, *r"f64");
    ctx->t_bool   = n00b_tc_prim(ctx, *r"bool");
    ctx->t_string = n00b_tc_prim(ctx, *r"string");
    ctx->t_nil    = n00b_tc_prim(ctx, *r"nil");
    ctx->t_void   = n00b_tc_prim(ctx, *r"void");
}

n00b_tc_ctx_t *
n00b_tc_ctx_new(void)
{
    n00b_tc_ctx_t *ctx = n00b_alloc(n00b_tc_ctx_t);

    // Type ownership.
    n00b_list_t(n00b_tc_type_t *) all = n00b_list_new_private(n00b_tc_type_t *);
    ctx->all_types  = n00b_alloc(n00b_list_t(n00b_tc_type_t *));
    *ctx->all_types = all;
    ctx->next_var_id = 0;

    // Datalog engine (embedded).
    n00b_logic_init(&ctx->logic);
    ctx->rel_implements  = n00b_logic_relation(&ctx->logic, *r"implements", 2);
    ctx->rel_promotes    = n00b_logic_relation(&ctx->logic, *r"promotes", 2);
    ctx->rel_iface_param = n00b_logic_relation(&ctx->logic, *r"iface_param", 3);

    tc_install_transitive_promotion(ctx);

    // Interface / implementation lists.
    n00b_list_t(n00b_tc_iface_t) ifaces = n00b_list_new_private(n00b_tc_iface_t);
    ctx->interfaces  = n00b_alloc(n00b_list_t(n00b_tc_iface_t));
    *ctx->interfaces = ifaces;

    n00b_list_t(n00b_tc_impl_t) impls = n00b_list_new_private(n00b_tc_impl_t);
    ctx->implementations  = n00b_alloc(n00b_list_t(n00b_tc_impl_t));
    *ctx->implementations = impls;

    // Error / coercion lists.
    n00b_list_t(n00b_tc_error_t) errs = n00b_list_new_private(n00b_tc_error_t);
    ctx->errors  = n00b_alloc(n00b_list_t(n00b_tc_error_t));
    *ctx->errors = errs;

    n00b_list_t(n00b_tc_coercion_t) coercions = n00b_list_new_private(n00b_tc_coercion_t);
    ctx->coercions  = n00b_alloc(n00b_list_t(n00b_tc_coercion_t));
    *ctx->coercions = coercions;

    ctx->logic_dirty = false;

    // Built-in primitive types.
    tc_create_builtins(ctx);

    // Numeric promotion graph.
    tc_register_numeric_promotions(ctx);

    return ctx;
}

void
n00b_tc_ctx_free(n00b_tc_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    n00b_logic_free(&ctx->logic);
    n00b_free(ctx);
}

// ============================================================================
// Type registration
// ============================================================================

void
n00b_tc_ctx_register(n00b_tc_ctx_t *ctx, n00b_tc_type_t *type)
{
    n00b_list_push(*ctx->all_types, type);
}

// ============================================================================
// Interface + implementation registration
// ============================================================================

void
n00b_tc_register_iface(n00b_tc_ctx_t *ctx, n00b_string_t name,
                         n00b_tc_iface_param_t +)
{
    // Build the params list.
    n00b_list_t(n00b_tc_iface_param_t) params =
        n00b_list_new_private(n00b_tc_iface_param_t);
    n00b_list_t(n00b_tc_iface_param_t) *params_ptr =
        n00b_alloc(n00b_list_t(n00b_tc_iface_param_t));

    unsigned int count = n00b_remaining_vargs(vargs);
    for (unsigned int i = 0; i < count; i++) {
        n00b_tc_iface_param_t *p = (n00b_tc_iface_param_t *)n00b_vargs_next(vargs);
        n00b_list_push(params, *p);

        // Also record in Datalog: iface_param(iface, param_name, type_id).
        // We use the param name as the third column for now.
        n00b_dl_sym_t iface_sym = n00b_logic_const(&ctx->logic, name);
        n00b_dl_sym_t param_sym = n00b_logic_const(&ctx->logic, p->name);
        // Use the param index as an integer symbol for the type_id column.
        n00b_dl_sym_t idx_sym   = n00b_logic_int(&ctx->logic, (int64_t)i);
        n00b_logic_add_fact(&ctx->logic, ctx->rel_iface_param, 3,
                             (n00b_dl_sym_t[]){iface_sym, param_sym, idx_sym});
    }

    *params_ptr = params;

    n00b_tc_iface_t iface = {
        .name   = name,
        .params = params_ptr,
    };

    n00b_list_push(*ctx->interfaces, iface);
    ctx->logic_dirty = true;
}

void
n00b_tc_register_impl(n00b_tc_ctx_t *ctx,
                        n00b_string_t type_name,
                        n00b_string_t iface_name,
                        n00b_tc_type_t *+)
{
    // Collect bindings.
    n00b_list_t(n00b_tc_type_t *) bindings = n00b_list_new_private(n00b_tc_type_t *);
    n00b_list_t(n00b_tc_type_t *) *bindings_ptr =
        n00b_alloc(n00b_list_t(n00b_tc_type_t *));

    unsigned int count = n00b_remaining_vargs(vargs);
    for (unsigned int i = 0; i < count; i++) {
        n00b_tc_type_t *b = (n00b_tc_type_t *)n00b_vargs_next(vargs);
        n00b_list_push(bindings, b);
    }

    *bindings_ptr = bindings;

    n00b_tc_impl_t impl = {
        .type_name  = type_name,
        .iface_name = iface_name,
        .bindings   = bindings_ptr,
    };

    n00b_list_push(*ctx->implementations, impl);

    // Assert implements(type_name, iface_name) in Datalog.
    n00b_dl_sym_t tsym = n00b_logic_const(&ctx->logic, type_name);
    n00b_dl_sym_t isym = n00b_logic_const(&ctx->logic, iface_name);
    n00b_logic_add_fact(&ctx->logic, ctx->rel_implements, 2,
                         (n00b_dl_sym_t[]){tsym, isym});

    ctx->logic_dirty = true;
}

void
n00b_tc_register_promotion(n00b_tc_ctx_t *ctx,
                             n00b_string_t from_name,
                             n00b_string_t to_name)
{
    n00b_dl_sym_t from_sym = n00b_logic_const(&ctx->logic, from_name);
    n00b_dl_sym_t to_sym   = n00b_logic_const(&ctx->logic, to_name);

    n00b_logic_add_fact(&ctx->logic, ctx->rel_promotes, 2,
                         (n00b_dl_sym_t[]){from_sym, to_sym});

    ctx->logic_dirty = true;
}

// ============================================================================
// Query helpers
// ============================================================================

static void
tc_ensure_datalog(n00b_tc_ctx_t *ctx)
{
    if (ctx->logic_dirty) {
        n00b_logic_run_datalog(&ctx->logic);
        ctx->logic_dirty = false;
    }
}

typedef struct {
    n00b_logic_t  *logic;
    n00b_dl_sym_t  want_a;
    n00b_dl_sym_t  want_b;
    bool           found;
} tc_pair_query_t;

static bool
tc_pair_match_cb(const n00b_dl_sym_t *tuple, int32_t arity, void *user)
{
    (void)arity;
    tc_pair_query_t *q = (tc_pair_query_t *)user;
    if (tuple[0] == q->want_a && tuple[1] == q->want_b) {
        q->found = true;
        return false; // stop iteration
    }
    return true;
}

bool
n00b_tc_implements(n00b_tc_ctx_t *ctx,
                     n00b_string_t type_name,
                     n00b_string_t iface_name)
{
    tc_ensure_datalog(ctx);

    tc_pair_query_t q = {
        .logic  = &ctx->logic,
        .want_a = n00b_logic_const(&ctx->logic, type_name),
        .want_b = n00b_logic_const(&ctx->logic, iface_name),
        .found  = false,
    };

    n00b_logic_query(&ctx->logic, ctx->rel_implements, tc_pair_match_cb, &q);
    return q.found;
}

bool
n00b_tc_promotes_to(n00b_tc_ctx_t *ctx,
                      n00b_string_t from_name,
                      n00b_string_t to_name)
{
    tc_ensure_datalog(ctx);

    tc_pair_query_t q = {
        .logic  = &ctx->logic,
        .want_a = n00b_logic_const(&ctx->logic, from_name),
        .want_b = n00b_logic_const(&ctx->logic, to_name),
        .found  = false,
    };

    n00b_logic_query(&ctx->logic, ctx->rel_promotes, tc_pair_match_cb, &q);
    return q.found;
}

// ============================================================================
// Primitive lookup
// ============================================================================

n00b_tc_type_t *
n00b_tc_lookup_prim(n00b_tc_ctx_t *ctx, n00b_string_t name)
{
    if (!ctx || !name.data) {
        return nullptr;
    }

    struct { n00b_string_t name; n00b_tc_type_t *type; } builtins[] = {
        { *r"int",    ctx->t_int    },
        { *r"i8",     ctx->t_i8     },
        { *r"i16",    ctx->t_i16    },
        { *r"i32",    ctx->t_i32    },
        { *r"i64",    ctx->t_i64    },
        { *r"u8",     ctx->t_u8     },
        { *r"u16",    ctx->t_u16    },
        { *r"u32",    ctx->t_u32    },
        { *r"u64",    ctx->t_u64    },
        { *r"f32",    ctx->t_f32    },
        { *r"f64",    ctx->t_f64    },
        { *r"bool",   ctx->t_bool   },
        { *r"string", ctx->t_string },
        { *r"nil",    ctx->t_nil    },
        { *r"void",   ctx->t_void   },
    };

    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if (n00b_unicode_str_eq(name, builtins[i].name)) {
            return builtins[i].type;
        }
    }

    return nullptr;
}

