#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/tuple.h"

// Declare tuple types at file scope (once each).
n00b_tuple_decl(int, double, char *);
n00b_tuple_decl(int, int);
n00b_tuple_decl(int);
n00b_tuple_decl(char *, char *);
n00b_tuple_decl(char, int, double, char *);
n00b_tuple_decl(int, double);
n00b_tuple_decl(bool, bool);
n00b_tuple_decl(int, int, int);

// ============================================================================
// 1. Basic construction and access
// ============================================================================

static void
test_construction(void)
{
    n00b_tuple_t(int, double, char *) t = n00b_tuple(42, 3.14, "hello");

    assert(n00b_tuple_get(t, 0) == 42);
    assert(n00b_tuple_get(t, 1) == 3.14);
    assert(strcmp(n00b_tuple_get(t, 2), "hello") == 0);

    printf("  [PASS] construction\n");
}

// ============================================================================
// 2. Two-element tuple
// ============================================================================

static void
test_pair(void)
{
    n00b_tuple_t(int, int) pair = n00b_tuple(10, 20);

    assert(n00b_tuple_get(pair, 0) == 10);
    assert(n00b_tuple_get(pair, 1) == 20);

    printf("  [PASS] pair\n");
}

// ============================================================================
// 3. Single-element tuple
// ============================================================================

static void
test_single(void)
{
    n00b_tuple_t(int) single = n00b_tuple(99);

    assert(n00b_tuple_get(single, 0) == 99);

    printf("  [PASS] single element\n");
}

// ============================================================================
// 4. Pointer types
// ============================================================================

static void
test_pointer_types(void)
{
    char *a = "alpha";
    char *b = "beta";

    n00b_tuple_t(char *, char *) t = n00b_tuple(a, b);

    assert(n00b_tuple_get(t, 0) == a);
    assert(n00b_tuple_get(t, 1) == b);
    assert(strcmp(n00b_tuple_get(t, 0), "alpha") == 0);
    assert(strcmp(n00b_tuple_get(t, 1), "beta") == 0);

    printf("  [PASS] pointer types\n");
}

// ============================================================================
// 5. Mixed types
// ============================================================================

static void
test_mixed_types(void)
{
    n00b_tuple_t(char, int, double, char *) t = n00b_tuple('X', 100, 2.718, "euler");

    assert(n00b_tuple_get(t, 0) == 'X');
    assert(n00b_tuple_get(t, 1) == 100);
    assert(n00b_tuple_get(t, 2) == 2.718);
    assert(strcmp(n00b_tuple_get(t, 3), "euler") == 0);

    printf("  [PASS] mixed types\n");
}

// ============================================================================
// 6. Struct field access (direct)
// ============================================================================

static void
test_field_access(void)
{
    n00b_tuple_t(int, double) t = n00b_tuple(5, 1.5);

    // Access via the generated field names directly
    assert(t.item_0 == 5);
    assert(t.item_1 == 1.5);

    // Mutation via direct field access
    t.item_0 = 50;
    t.item_1 = 15.0;
    assert(n00b_tuple_get(t, 0) == 50);
    assert(n00b_tuple_get(t, 1) == 15.0);

    printf("  [PASS] field access\n");
}

// ============================================================================
// 7. Copy semantics
// ============================================================================

static void
test_copy(void)
{
    n00b_tuple_t(int, double) t1 = n00b_tuple(1, 2.0);
    n00b_tuple_t(int, double) t2 = t1;

    // t2 is a copy
    assert(n00b_tuple_get(t2, 0) == 1);
    assert(n00b_tuple_get(t2, 1) == 2.0);

    // Mutating t2 doesn't affect t1
    t2.item_0 = 999;
    assert(n00b_tuple_get(t1, 0) == 1);
    assert(n00b_tuple_get(t2, 0) == 999);

    printf("  [PASS] copy semantics\n");
}

// ============================================================================
// 8. Sizeof
// ============================================================================

static void
test_sizeof(void)
{
    n00b_tuple_t(int, double) t = n00b_tuple(0, 0.0);

    // Struct must be at least large enough for its fields
    assert(sizeof(t) >= sizeof(int) + sizeof(double));

    printf("  [PASS] sizeof\n");
}

// ============================================================================
// 9. Bool tuple
// ============================================================================

static void
test_bool_tuple(void)
{
    n00b_tuple_t(bool, bool) t = n00b_tuple(true, false);

    assert(n00b_tuple_get(t, 0) == true);
    assert(n00b_tuple_get(t, 1) == false);

    printf("  [PASS] bool tuple\n");
}

// ============================================================================
// 10. Negative and zero values
// ============================================================================

static void
test_edge_values(void)
{
    n00b_tuple_t(int, double) t = n00b_tuple(-1, -0.0);

    assert(n00b_tuple_get(t, 0) == -1);
    // -0.0 == 0.0 in IEEE 754
    assert(n00b_tuple_get(t, 1) == 0.0);

    n00b_tuple_t(int, int, int) zeros = n00b_tuple(0, 0, 0);
    assert(n00b_tuple_get(zeros, 0) == 0);
    assert(n00b_tuple_get(zeros, 1) == 0);
    assert(n00b_tuple_get(zeros, 2) == 0);

    printf("  [PASS] edge values\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running tuple tests...\n");

    test_construction();
    test_pair();
    test_single();
    test_pointer_types();
    test_mixed_types();
    test_field_access();
    test_copy();
    test_sizeof();
    test_bool_tuple();
    test_edge_values();

    printf("All tuple tests passed.\n");
    n00b_shutdown();
    return 0;
}
