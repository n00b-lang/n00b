#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "adt/option.h"
#include "adt/variant.h"

// Variant type aliases used by the tests.
typedef n00b_variant_t(int, double, char *) variant_idc_t;
typedef n00b_variant_t(int, double) variant_id_t;
typedef n00b_variant_t(int, char *) variant_ic_t;
typedef n00b_variant_t(char, int, double) variant_cid_t;
typedef n00b_variant_t(char *, int) variant_ci_t;

// ============================================================================
// 1. Construction via n00b_variant_set (by value)
// ============================================================================

static void
test_set_by_value(void)
{
    variant_idc_t v;

    v = n00b_variant_set(typeof(v), int, 42);

    assert(n00b_variant_is_set(v));
    assert(n00b_variant_is_type(v, int));
    assert(!n00b_variant_is_type(v, double));
    assert(!n00b_variant_is_type(v, char *));
    assert(n00b_variant_get(v, int) == 42);

    printf("  [PASS] set by value\n");
}

// ============================================================================
// 2. Construction via n00b_variant_set (by pointer)
// ============================================================================

static void
test_set_by_pointer(void)
{
    variant_idc_t v = n00b_variant_empty(variant_idc_t);

    assert(!n00b_variant_is_set(v));

    _n00b_variant_set_ptr(&v, double, 3.14);

    assert(n00b_variant_is_set(v));
    assert(n00b_variant_is_type(v, double));
    assert(!n00b_variant_is_type(v, int));
    assert(n00b_variant_get(v, double) == 3.14);

    printf("  [PASS] set by pointer\n");
}

// ============================================================================
// 3. Empty variant
// ============================================================================

static void
test_empty(void)
{
    variant_id_t v = n00b_variant_empty(variant_id_t);

    assert(!n00b_variant_is_set(v));
    assert(!n00b_variant_is_type(v, int));
    assert(!n00b_variant_is_type(v, double));

    printf("  [PASS] empty variant\n");
}

// ============================================================================
// 4. Get (checked)
// ============================================================================

static void
test_get(void)
{
    variant_idc_t vi = n00b_variant_set(typeof(variant_idc_t), int, 99);
    assert(n00b_variant_get(vi, int) == 99);

    variant_idc_t vd = n00b_variant_set(typeof(variant_idc_t), double, 1.5);
    assert(n00b_variant_get(vd, double) == 1.5);

    char         *msg = "world";
    variant_idc_t vs  = n00b_variant_set(typeof(variant_idc_t), char *, msg);
    assert(n00b_variant_get(vs, char *) == msg);
    assert(strcmp(n00b_variant_get(vs, char *), "world") == 0);

    printf("  [PASS] get (checked)\n");
}

// ============================================================================
// 5. Get or else (with default)
// ============================================================================

static void
test_get_or_else(void)
{
    variant_id_t v = n00b_variant_set(typeof(variant_id_t), int, 100);

    // Correct type -> value
    assert(n00b_variant_get_or_else(v, int, -1) == 100);

    // Wrong type -> default
    assert(n00b_variant_get_or_else(v, double, -9.0) == -9.0);

    // Empty variant -> default
    variant_id_t empty = n00b_variant_empty(variant_id_t);
    assert(n00b_variant_get_or_else(empty, int, -1) == -1);
    assert(n00b_variant_get_or_else(empty, double, -9.0) == -9.0);

    printf("  [PASS] get or else\n");
}

// ============================================================================
// 6. Mutation (set changes active type)
// ============================================================================

static void
test_mutation(void)
{
    variant_id_t v = n00b_variant_set(typeof(variant_id_t), int, 10);
    assert(n00b_variant_get(v, int) == 10);

    // Mutate to hold a double
    v = n00b_variant_set(typeof(v), double, 3.14);
    assert(n00b_variant_is_type(v, double));
    assert(!n00b_variant_is_type(v, int));
    assert(n00b_variant_get(v, double) == 3.14);

    // Mutate back to int
    v = n00b_variant_set(typeof(v), int, 999);
    assert(n00b_variant_is_type(v, int));
    assert(n00b_variant_get(v, int) == 999);

    printf("  [PASS] mutation\n");
}

// ============================================================================
// 7. Pointer types in variant
// ============================================================================

static void
test_pointer_variant(void)
{
    char *hello = "hello";
    char *world = "world";

    variant_ic_t v = n00b_variant_set(typeof(n00b_variant_empty(variant_ic_t)), char *, hello);
    assert(n00b_variant_is_type(v, char *));
    assert(n00b_variant_get(v, char *) == hello);

    v = n00b_variant_set(typeof(v), int, 42);
    assert(n00b_variant_is_type(v, int));
    assert(n00b_variant_get(v, int) == 42);

    v = n00b_variant_set(typeof(v), char *, world);
    assert(n00b_variant_get(v, char *) == world);
    assert(strcmp(n00b_variant_get(v, char *), "world") == 0);

    printf("  [PASS] pointer variant\n");
}

// ============================================================================
// 8. Data sizing (union is large enough for all alternatives)
// ============================================================================

static void
test_data_sizing(void)
{
    variant_cid_t v = n00b_variant_set(variant_cid_t, double, 1.23);
    assert(sizeof(v.value) >= sizeof(double));
    assert(n00b_variant_get(v, double) == 1.23);

    // Store a char in the same variant type
    variant_cid_t v2 = n00b_variant_set(variant_cid_t, char, 'A');
    assert(n00b_variant_get(v2, char) == 'A');

    printf("  [PASS] data sizing\n");
}

// ============================================================================
// 9. Multiple independent variants
// ============================================================================

static void
test_independent_variants(void)
{
    variant_id_t v1 = n00b_variant_set(typeof(n00b_variant_empty(variant_id_t)), int, 10);
    variant_ci_t v2
        = n00b_variant_set(typeof(n00b_variant_empty(variant_ci_t)), char *, "test");

    assert(n00b_variant_get(v1, int) == 10);
    assert(strcmp(n00b_variant_get(v2, char *), "test") == 0);

    // Mutating one doesn't affect the other
    v1 = n00b_variant_set(typeof(v1), double, 5.0);
    assert(strcmp(n00b_variant_get(v2, char *), "test") == 0);

    printf("  [PASS] independent variants\n");
}

// ============================================================================
// 10. Get or else via pointer
// ============================================================================

static void
test_get_or_else_ptr(void)
{
    variant_id_t v = n00b_variant_set(typeof(n00b_variant_empty(variant_id_t)), int, 77);

    // Same as by value — macro always takes address internally
    assert(n00b_variant_get_or_else(v, int, -1) == 77);

    // Wrong type -> default
    assert(n00b_variant_get_or_else(v, double, -9.0) == -9.0);

    printf("  [PASS] get or else (via second variable)\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running variant tests...\n");

    test_set_by_value();
    test_set_by_pointer();
    test_empty();
    test_get();
    test_get_or_else();
    test_mutation();
    test_pointer_variant();
    test_data_sizing();
    test_independent_variants();
    test_get_or_else_ptr();

    printf("All variant tests passed.\n");
    n00b_shutdown();
    return 0;
}
