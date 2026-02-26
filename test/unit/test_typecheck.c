#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/variant.h"
#include "core/option.h"
#include "core/list.h"
#include "core/string.h"
#include "strings/string_ops.h"
#include "typecheck/types.h"
#include "typecheck/construct.h"
#include "typecheck/context.h"

// ============================================================================
// Phase 1: Type Representation + Constructors (updated for ctx)
// ============================================================================

// --- 1. Primitive construction ---

static void
test_construct_primitives(void)
{
    n00b_tc_ctx_t *ctx = n00b_tc_ctx_new();

    n00b_tc_type_t *t_int    = n00b_tc_prim(ctx, *r"int");
    n00b_tc_type_t *t_bool   = n00b_tc_prim(ctx, *r"bool");
    n00b_tc_type_t *t_string = n00b_tc_prim(ctx, *r"string");
    n00b_tc_type_t *t_nil    = n00b_tc_prim(ctx, *r"nil");

    // Verify each is a Prim kind.
    assert(n00b_variant_is_type(t_int->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(t_bool->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(t_string->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(t_nil->kind, n00b_tc_prim_t));

    // Not other kinds.
    assert(!n00b_variant_is_type(t_int->kind, n00b_tc_var_t));
    assert(!n00b_variant_is_type(t_int->kind, n00b_tc_fn_t));

    // Verify names.
    auto int_prim = n00b_variant_get(t_int->kind, n00b_tc_prim_t);
    assert(n00b_unicode_str_eq(int_prim.name, *r"int"));

    auto bool_prim = n00b_variant_get(t_bool->kind, n00b_tc_prim_t);
    assert(n00b_unicode_str_eq(bool_prim.name, *r"bool"));

    auto nil_prim = n00b_variant_get(t_nil->kind, n00b_tc_prim_t);
    assert(n00b_unicode_str_eq(nil_prim.name, *r"nil"));

    // forward pointer should be null (root).
    assert(t_int->forward == nullptr);

    n00b_tc_ctx_free(ctx);
    printf("  [PASS] construct_primitives\n");
}

// --- 2. Parameterized type construction ---

static void
test_construct_param(void)
{
    n00b_tc_ctx_t *ctx = n00b_tc_ctx_new();

    n00b_tc_type_t *t_int = n00b_tc_prim(ctx, *r"int");
    n00b_tc_type_t *t_str = n00b_tc_prim(ctx, *r"string");

    // list[int]
    n00b_tc_type_t *list_int = n00b_tc_param(ctx, *r"list", t_int);
    assert(n00b_variant_is_type(list_int->kind, n00b_tc_param_t));

    auto p = n00b_variant_get(list_int->kind, n00b_tc_param_t);
    assert(n00b_unicode_str_eq(p.name, *r"list"));
    assert(n00b_list_len(*p.params) == 1);
    assert(n00b_list_get(*p.params, 0) == t_int);

    // dict[string, int]
    n00b_tc_type_t *dict_si = n00b_tc_param(ctx, *r"dict", t_str, t_int);
    assert(n00b_variant_is_type(dict_si->kind, n00b_tc_param_t));

    auto d = n00b_variant_get(dict_si->kind, n00b_tc_param_t);
    assert(n00b_unicode_str_eq(d.name, *r"dict"));
    assert(n00b_list_len(*d.params) == 2);
    assert(n00b_list_get(*d.params, 0) == t_str);
    assert(n00b_list_get(*d.params, 1) == t_int);

    n00b_tc_ctx_free(ctx);
    printf("  [PASS] construct_param\n");
}

// --- 3. Function type construction ---

static void
test_construct_fn(void)
{
    n00b_tc_ctx_t *ctx = n00b_tc_ctx_new();

    n00b_tc_type_t *t_int  = n00b_tc_prim(ctx, *r"int");
    n00b_tc_type_t *t_str  = n00b_tc_prim(ctx, *r"string");
    n00b_tc_type_t *t_bool = n00b_tc_prim(ctx, *r"bool");

    // (int, string) -> bool
    n00b_tc_type_t *fn1 = n00b_tc_fn(ctx, t_int, t_str,
                                       kw_func(n00b_tc_fn, .returns = t_bool));
    assert(n00b_variant_is_type(fn1->kind, n00b_tc_fn_t));

    auto f = n00b_variant_get(fn1->kind, n00b_tc_fn_t);
    assert(n00b_list_len(*f.positional) == 2);
    assert(n00b_list_get(*f.positional, 0) == t_int);
    assert(n00b_list_get(*f.positional, 1) == t_str);
    assert(f.return_type == t_bool);
    assert(f.vargs_type == nullptr);
    assert(f.kargs_type == nullptr);

    // (int, *int, **kargs) -> bool
    n00b_tc_type_t *kargs_rec = n00b_tc_record(ctx, *r"",
        n00b_tc_field(*r"timeout", t_int, kw_func(n00b_tc_field, .has_default = true)),
        kw_func(n00b_tc_record, .ordered = false));

    n00b_tc_type_t *fn2 = n00b_tc_fn(ctx, t_int,
                                       kw_func(n00b_tc_fn,
                                               .returns  = t_bool,
                                               .variadic = t_int,
                                               .kwonly   = kargs_rec));
    assert(n00b_variant_is_type(fn2->kind, n00b_tc_fn_t));

    auto f2 = n00b_variant_get(fn2->kind, n00b_tc_fn_t);
    assert(n00b_list_len(*f2.positional) == 1);
    assert(f2.vargs_type == t_int);
    assert(f2.kargs_type == kargs_rec);
    assert(f2.return_type == t_bool);

    n00b_tc_ctx_free(ctx);
    printf("  [PASS] construct_fn\n");
}

// --- 4. Ordered record construction ---

static void
test_construct_record_ordered(void)
{
    n00b_tc_ctx_t *ctx = n00b_tc_ctx_new();

    n00b_tc_type_t *t_int = n00b_tc_prim(ctx, *r"int");

    // Point{x: int, y: int}
    n00b_tc_type_t *point = n00b_tc_record(ctx, *r"Point",
        n00b_tc_field(*r"x", t_int),
        n00b_tc_field(*r"y", t_int),
        kw_func(n00b_tc_record));

    assert(n00b_variant_is_type(point->kind, n00b_tc_record_t));

    auto rec = n00b_variant_get(point->kind, n00b_tc_record_t);
    assert(n00b_unicode_str_eq(rec.name, *r"Point"));
    assert(rec.ordered == true);
    assert(rec.open == false);
    assert(n00b_list_len(*rec.field_names) == 2);
    assert(n00b_list_len(*rec.field_types) == 2);

    n00b_string_t name0 = n00b_list_get(*rec.field_names, 0);
    assert(n00b_unicode_str_eq(name0, *r"x"));

    n00b_string_t name1 = n00b_list_get(*rec.field_names, 1);
    assert(n00b_unicode_str_eq(name1, *r"y"));

    assert(n00b_list_get(*rec.field_types, 0) == t_int);
    assert(n00b_list_get(*rec.field_types, 1) == t_int);

    // No defaults on this record.
    assert(rec.field_has_default == nullptr);

    n00b_tc_ctx_free(ctx);
    printf("  [PASS] construct_record_ordered\n");
}

// --- 5. Unordered record (kargs) construction ---

static void
test_construct_record_unordered(void)
{
    n00b_tc_ctx_t *ctx = n00b_tc_ctx_new();

    n00b_tc_type_t *t_int  = n00b_tc_prim(ctx, *r"int");
    n00b_tc_type_t *t_bool = n00b_tc_prim(ctx, *r"bool");

    // {.timeout: int, .verbose: bool} with timeout having a default
    n00b_tc_type_t *kargs = n00b_tc_record(ctx, *r"",
        n00b_tc_field(*r"timeout", t_int, .has_default = true),
        n00b_tc_field(*r"verbose", t_bool),
        kw_func(n00b_tc_record, .ordered = false));

    assert(n00b_variant_is_type(kargs->kind, n00b_tc_record_t));

    auto rec = n00b_variant_get(kargs->kind, n00b_tc_record_t);
    assert(rec.ordered == false);
    assert(rec.open == false);
    assert(n00b_list_len(*rec.field_names) == 2);

    // Check defaults list exists because at least one field has a default.
    assert(rec.field_has_default != nullptr);
    assert(n00b_list_get(*rec.field_has_default, 0) == true);  // timeout
    assert(n00b_list_get(*rec.field_has_default, 1) == false); // verbose

    n00b_tc_ctx_free(ctx);
    printf("  [PASS] construct_record_unordered\n");
}

// --- 6. Sum type construction ---

static void
test_construct_sum(void)
{
    n00b_tc_ctx_t *ctx = n00b_tc_ctx_new();

    n00b_tc_type_t *t_int = n00b_tc_prim(ctx, *r"int");
    n00b_tc_type_t *t_str = n00b_tc_prim(ctx, *r"string");
    n00b_tc_type_t *t_nil = n00b_tc_prim(ctx, *r"nil");

    // int | string | nil
    n00b_tc_type_t *sum = n00b_tc_sum(ctx, t_int, t_str, t_nil);
    assert(n00b_variant_is_type(sum->kind, n00b_tc_sum_t));

    auto s = n00b_variant_get(sum->kind, n00b_tc_sum_t);
    assert(n00b_list_len(*s.variants) == 3);
    assert(n00b_list_get(*s.variants, 0) == t_int);
    assert(n00b_list_get(*s.variants, 1) == t_str);
    assert(n00b_list_get(*s.variants, 2) == t_nil);

    n00b_tc_ctx_free(ctx);
    printf("  [PASS] construct_sum\n");
}

// --- 7. Tuple type construction ---

static void
test_construct_tuple(void)
{
    n00b_tc_ctx_t *ctx = n00b_tc_ctx_new();

    n00b_tc_type_t *t_int = n00b_tc_prim(ctx, *r"int");
    n00b_tc_type_t *t_str = n00b_tc_prim(ctx, *r"string");

    // (int, string) — closed
    n00b_tc_type_t *tup = n00b_tc_tuple(ctx, t_int, t_str,
                                          kw_func(n00b_tc_tuple));
    assert(n00b_variant_is_type(tup->kind, n00b_tc_tuple_t));

    auto tu = n00b_variant_get(tup->kind, n00b_tc_tuple_t);
    assert(n00b_list_len(*tu.elements) == 2);
    assert(tu.min_len == 2);
    assert(tu.open == false);
    assert(n00b_list_get(*tu.elements, 0) == t_int);
    assert(n00b_list_get(*tu.elements, 1) == t_str);

    // (int, string, ...) — open
    n00b_tc_type_t *open_tup = n00b_tc_tuple(ctx, t_int, t_str,
                                               kw_func(n00b_tc_tuple, .open = true));
    assert(n00b_variant_is_type(open_tup->kind, n00b_tc_tuple_t));

    auto ot = n00b_variant_get(open_tup->kind, n00b_tc_tuple_t);
    assert(n00b_list_len(*ot.elements) == 2);
    assert(ot.min_len == 2);
    assert(ot.open == true);

    n00b_tc_ctx_free(ctx);
    printf("  [PASS] construct_tuple\n");
}

// --- 8. Type variable construction ---

static void
test_construct_var(void)
{
    n00b_tc_ctx_t *ctx = n00b_tc_ctx_new();

    // Named variable.
    n00b_tc_type_t *tv = n00b_tc_var(ctx, *r"T");
    assert(n00b_variant_is_type(tv->kind, n00b_tc_var_t));

    auto v = n00b_variant_get(tv->kind, n00b_tc_var_t);
    assert(n00b_option_is_set(v.given_name));
    assert(n00b_unicode_str_eq(n00b_option_get(v.given_name), *r"T"));
    assert(n00b_unicode_str_eq(v.display_name, *r"T"));
    assert(v.constraints == nullptr);

    // Anonymous variable.
    n00b_tc_type_t *anon = n00b_tc_fresh_var(ctx);
    assert(n00b_variant_is_type(anon->kind, n00b_tc_var_t));

    auto av = n00b_variant_get(anon->kind, n00b_tc_var_t);
    assert(!n00b_option_is_set(av.given_name));
    // display_name should be auto-generated like "t_N".
    assert(av.display_name.data != nullptr);
    assert(av.display_name.u8_bytes > 0);

    // Two anonymous vars should have different IDs.
    n00b_tc_type_t *anon2 = n00b_tc_fresh_var(ctx);
    auto av2 = n00b_variant_get(anon2->kind, n00b_tc_var_t);
    assert(av.id != av2.id);

    n00b_tc_ctx_free(ctx);
    printf("  [PASS] construct_var\n");
}

// ============================================================================
// Phase 2: Context + Datalog Integration
// ============================================================================

// --- 9. Context lifecycle ---

static void
test_context_lifecycle(void)
{
    n00b_tc_ctx_t *ctx = n00b_tc_ctx_new();

    // All 16 built-in primitives should be Prim kind.
    assert(n00b_variant_is_type(ctx->t_int->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(ctx->t_i8->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(ctx->t_i16->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(ctx->t_i32->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(ctx->t_i64->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(ctx->t_u8->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(ctx->t_u16->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(ctx->t_u32->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(ctx->t_u64->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(ctx->t_f32->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(ctx->t_f64->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(ctx->t_bool->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(ctx->t_string->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(ctx->t_nil->kind, n00b_tc_prim_t));
    assert(n00b_variant_is_type(ctx->t_void->kind, n00b_tc_prim_t));

    // Verify t_int has the right name.
    auto ip = n00b_variant_get(ctx->t_int->kind, n00b_tc_prim_t);
    assert(n00b_unicode_str_eq(ip.name, *r"int"));

    // all_types should have at least 16 entries (the builtins).
    // (15 builtins: int, i8..u64, f32, f64, bool, string, nil, void)
    assert(n00b_list_len(*ctx->all_types) >= 15);

    // next_var_id should start at 0 (no vars created yet by builtins).
    assert(ctx->next_var_id == 0);

    // Creating a fresh var should increment next_var_id.
    n00b_tc_type_t *fv = n00b_tc_fresh_var(ctx);
    (void)fv;
    assert(ctx->next_var_id == 1);

    n00b_tc_fresh_var(ctx);
    assert(ctx->next_var_id == 2);

    n00b_tc_ctx_free(ctx);
    printf("  [PASS] context_lifecycle\n");
}

// --- 10. Register promotion + transitive query ---

static void
test_register_promotion(void)
{
    n00b_tc_ctx_t *ctx = n00b_tc_ctx_new();

    // Register i32 -> i64, i64 -> f64.
    n00b_tc_register_promotion(ctx, *r"i32", *r"i64");
    n00b_tc_register_promotion(ctx, *r"i64", *r"f64");

    // Direct promotion: i32 -> i64.
    assert(n00b_tc_promotes_to(ctx, *r"i32", *r"i64") == true);

    // Direct promotion: i64 -> f64.
    assert(n00b_tc_promotes_to(ctx, *r"i64", *r"f64") == true);

    // Transitive: i32 -> f64 (via i64).
    assert(n00b_tc_promotes_to(ctx, *r"i32", *r"f64") == true);

    // No reverse.
    assert(n00b_tc_promotes_to(ctx, *r"i64", *r"i32") == false);
    assert(n00b_tc_promotes_to(ctx, *r"f64", *r"i32") == false);
    assert(n00b_tc_promotes_to(ctx, *r"f64", *r"i64") == false);

    n00b_tc_ctx_free(ctx);
    printf("  [PASS] register_promotion\n");
}

// --- 11. Register interface + query implements ---

static void
test_register_interface(void)
{
    n00b_tc_ctx_t *ctx = n00b_tc_ctx_new();

    // Define Indexable with key/value params.
    n00b_tc_type_t *key_var   = n00b_tc_fresh_var(ctx);
    n00b_tc_type_t *value_var = n00b_tc_fresh_var(ctx);

    n00b_tc_iface_param_t kp = { .name = *r"key",   .type = key_var };
    n00b_tc_iface_param_t vp = { .name = *r"value", .type = value_var };

    n00b_tc_register_iface(ctx, *r"Indexable", kp, vp);

    // Register dict as implementing Indexable with string key, int value.
    n00b_tc_register_impl(ctx, *r"dict", *r"Indexable",
                            ctx->t_string, ctx->t_int);

    // Query: dict implements Indexable? -> true.
    assert(n00b_tc_implements(ctx, *r"dict", *r"Indexable") == true);

    // Query: list implements Indexable? -> false (not registered).
    assert(n00b_tc_implements(ctx, *r"list", *r"Indexable") == false);

    n00b_tc_ctx_free(ctx);
    printf("  [PASS] register_interface\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running typecheck tests...\n");

    test_construct_primitives();
    test_construct_param();
    test_construct_fn();
    test_construct_record_ordered();
    test_construct_record_unordered();
    test_construct_sum();
    test_construct_tuple();
    test_construct_var();
    test_context_lifecycle();
    test_register_promotion();
    test_register_interface();

    printf("All typecheck tests passed.\n");
    n00b_shutdown();
    return 0;
}
