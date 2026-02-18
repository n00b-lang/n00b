#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/option.h"
#include "core/variant.h"

// Declare types at file scope (once each).
n00b_option_decl(int);
n00b_option_decl(double);
n00b_option_decl(char *);
n00b_variant_decl(int, double, char *);
n00b_variant_decl(int, double);
n00b_variant_decl(int, char *);
n00b_variant_decl(char, int, double);
n00b_variant_decl(char *, int);

// ============================================================================
// 1. Construction and tag
// ============================================================================

static void
test_construction(void)
{
    n00b_variant_t(int, double, char *) v = n00b_variant_ctor(0, int, 42, int, double, char *);
    assert(n00b_variant_is(v, 0));
    assert(!n00b_variant_is(v, 1));
    assert(!n00b_variant_is(v, 2));

    n00b_variant_t(int, double, char *) v2
        = n00b_variant_ctor(1, double, 3.14, int, double, char *);
    assert(n00b_variant_is(v2, 1));
    assert(!n00b_variant_is(v2, 0));

    n00b_variant_t(int, double, char *) v3
        = n00b_variant_ctor(2, char *, "hello", int, double, char *);
    assert(n00b_variant_is(v3, 2));

    printf("  [PASS] construction and tag\n");
}

// ============================================================================
// 2. Type matching
// ============================================================================

static void
test_type_matching(void)
{
    n00b_variant_t(int, double) v = n00b_variant_ctor(0, int, 10, int, double);
    assert(n00b_variant_type_matches(v, int));
    assert(!n00b_variant_type_matches(v, double));

    n00b_variant_t(int, double) v2 = n00b_variant_ctor(1, double, 2.718, int, double);
    assert(n00b_variant_type_matches(v2, double));
    assert(!n00b_variant_type_matches(v2, int));

    printf("  [PASS] type matching\n");
}

// ============================================================================
// 3. Get (checked)
// ============================================================================

static void
test_get(void)
{
    n00b_variant_t(int, double, char *) v = n00b_variant_ctor(0, int, 99, int, double, char *);
    assert(n00b_variant_get(v, int) == 99);

    n00b_variant_t(int, double, char *) v2
        = n00b_variant_ctor(1, double, 1.5, int, double, char *);
    assert(n00b_variant_get(v2, double) == 1.5);

    char *msg = "world";
    n00b_variant_t(int, double, char *) v3
        = n00b_variant_ctor(2, char *, msg, int, double, char *);
    assert(n00b_variant_get(v3, char *) == msg);
    assert(strcmp(n00b_variant_get(v3, char *), "world") == 0);

    printf("  [PASS] get (checked)\n");
}

// ============================================================================
// 4. Get unchecked
// ============================================================================

static void
test_get_unchecked(void)
{
    n00b_variant_t(int, double) v = n00b_variant_ctor(0, int, 42, int, double);
    assert(n00b_variant_get_unchecked(v, int) == 42);

    n00b_variant_t(int, double) v2 = n00b_variant_ctor(1, double, 6.28, int, double);
    assert(n00b_variant_get_unchecked(v2, double) == 6.28);

    printf("  [PASS] get unchecked\n");
}

// ============================================================================
// 5. Get safe (with default)
// ============================================================================

static void
test_get_safe(void)
{
    n00b_variant_t(int, double) v = n00b_variant_ctor(0, int, 100, int, double);

    // Correct tag + type -> value
    assert(n00b_variant_get_safe(v, 0, int, -1) == 100);

    // Wrong tag -> default
    assert(n00b_variant_get_safe(v, 1, double, -9.0) == -9.0);

    // Wrong type for correct tag -> default
    assert(n00b_variant_get_safe(v, 0, double, -9.0) == -9.0);

    printf("  [PASS] get safe\n");
}

// ============================================================================
// 6. Variant as option
// ============================================================================

static void
test_variant_as(void)
{
    n00b_variant_t(int, double) v = n00b_variant_ctor(0, int, 77, int, double);

    // Matching tag + type -> Some
    n00b_option_t(int) opt = n00b_variant_as(v, 0, int);
    assert(n00b_option_is_set(opt));
    assert(n00b_option_get(opt) == 77);

    // Wrong tag -> None
    n00b_option_t(double) opt2 = n00b_variant_as(v, 1, double);
    assert(!n00b_option_is_set(opt2));

    // Correct tag, wrong type -> None
    n00b_option_t(double) opt3 = n00b_variant_as(v, 0, double);
    assert(!n00b_option_is_set(opt3));

    printf("  [PASS] variant as option\n");
}

// ============================================================================
// 7. Set (mutation)
// ============================================================================

static void
test_set(void)
{
    n00b_variant_t(int, double) v = n00b_variant_ctor(0, int, 10, int, double);
    assert(n00b_variant_get(v, int) == 10);

    // Mutate to hold a double
    n00b_variant_set(&v, 1, double, 3.14, int, double);
    assert(n00b_variant_is(v, 1));
    assert(n00b_variant_type_matches(v, double));
    assert(n00b_variant_get(v, double) == 3.14);

    // Mutate back to int
    n00b_variant_set(&v, 0, int, 999, int, double);
    assert(n00b_variant_is(v, 0));
    assert(n00b_variant_get(v, int) == 999);

    printf("  [PASS] set (mutation)\n");
}

// ============================================================================
// 8. Two-type variant (int, char *)
// ============================================================================

static void
test_pointer_variant(void)
{
    char *hello = "hello";
    char *world = "world";

    n00b_variant_t(int, char *) v = n00b_variant_ctor(1, char *, hello, int, char *);
    assert(n00b_variant_is(v, 1));
    assert(n00b_variant_get(v, char *) == hello);

    n00b_variant_set(&v, 0, int, 42, int, char *);
    assert(n00b_variant_is(v, 0));
    assert(n00b_variant_get(v, int) == 42);

    n00b_variant_set(&v, 1, char *, world, int, char *);
    assert(n00b_variant_get(v, char *) == world);
    assert(strcmp(n00b_variant_get(v, char *), "world") == 0);

    printf("  [PASS] pointer variant\n");
}

// ============================================================================
// 9. Data buffer sizing (variant stores largest type)
// ============================================================================

static void
test_data_sizing(void)
{
    // The variant's data buffer should be at least as large as the largest type.
    n00b_variant_t(char, int, double) v
        = n00b_variant_ctor(2, double, 1.23, char, int, double);
    assert(sizeof(v._data) >= sizeof(double));
    assert(n00b_variant_get(v, double) == 1.23);

    // Store a char in the same variant type
    n00b_variant_t(char, int, double) v2
        = n00b_variant_ctor(0, char, 'A', char, int, double);
    assert(n00b_variant_get(v2, char) == 'A');

    printf("  [PASS] data sizing\n");
}

// ============================================================================
// 10. Multiple independent variants
// ============================================================================

static void
test_independent_variants(void)
{
    // Two different variant types coexisting
    n00b_variant_t(int, double) v1 = n00b_variant_ctor(0, int, 10, int, double);
    n00b_variant_t(char *, int) v2 = n00b_variant_ctor(0, char *, "test", char *, int);

    assert(n00b_variant_get(v1, int) == 10);
    assert(strcmp(n00b_variant_get(v2, char *), "test") == 0);

    // Mutating one doesn't affect the other
    n00b_variant_set(&v1, 1, double, 5.0, int, double);
    assert(strcmp(n00b_variant_get(v2, char *), "test") == 0);

    printf("  [PASS] independent variants\n");
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

    test_construction();
    test_type_matching();
    test_get();
    test_get_unchecked();
    test_get_safe();
    test_variant_as();
    test_set();
    test_pointer_variant();
    test_data_sizing();
    test_independent_variants();

    printf("All variant tests passed.\n");
    return 0;
}
