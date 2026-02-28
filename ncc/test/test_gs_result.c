// Test _generic_struct-based n00b_result_t.
//
// Verifies that _generic_struct auto-deduplicates struct definitions
// when n00b_result_t(T) is used across multiple headers and in
// multiple contexts within a single translation unit.
//
// Compiled and run through ncc (compile_run mode).

#include <stdio.h>
#include <string.h>

// Include result header — defines n00b_result_t(T) via _generic_struct.
#include "test_gs_result.h"

// Include a "user" header that also expands n00b_result_t(int).
// Without _generic_struct, this would cause a duplicate struct error.
#include "test_gs_result_user.h"

// ============================================================================
// Basic ok/err
// ============================================================================

static void
test_result_ok(void)
{
    n00b_result_t(int) r = n00b_result_ok(int, 42);
    assert(n00b_result_is_ok(r));
    assert(!n00b_result_is_err(r));
    assert(n00b_result_get(r) == 42);
    printf("PASS: result ok\n");
}

static void
test_result_err(void)
{
    n00b_result_t(int) r = n00b_result_err(int, -1);
    assert(n00b_result_is_err(r));
    assert(!n00b_result_is_ok(r));
    assert(n00b_result_get_err(r) == -1);
    printf("PASS: result err\n");
}

// ============================================================================
// Multiple result types
// ============================================================================

static void
test_result_double(void)
{
    n00b_result_t(double) r = n00b_result_ok(double, 3.14);
    assert(n00b_result_is_ok(r));
    double v = n00b_result_get(r);
    assert(v > 3.13 && v < 3.15);
    printf("PASS: result double\n");
}

static void
test_result_pointer(void)
{
    const char *msg = "hello";
    n00b_result_t(const char *) r = n00b_result_ok(const char *, msg);
    assert(n00b_result_is_ok(r));
    assert(strcmp(n00b_result_get(r), "hello") == 0);
    printf("PASS: result pointer\n");
}

// ============================================================================
// Cross-header usage (double_result from test_gs_result_user.h)
// ============================================================================

static void
test_cross_header(void)
{
    n00b_result_t(int) r = n00b_result_ok(int, 21);
    n00b_result_t(int) r2 = double_result(r);
    assert(n00b_result_is_ok(r2));
    assert(n00b_result_get(r2) == 42);

    n00b_result_t(int) e = n00b_result_err(int, -99);
    n00b_result_t(int) e2 = double_result(e);
    assert(n00b_result_is_err(e2));
    assert(n00b_result_get_err(e2) == -99);

    printf("PASS: cross-header\n");
}

// ============================================================================
// Many uses of same type — all deduplicated
// ============================================================================

static n00b_result_t(int)
safe_divide(int a, int b)
{
    if (b == 0) {
        return n00b_result_err(int, -1);
    }
    return n00b_result_ok(int, a / b);
}

static void
test_function_return(void)
{
    n00b_result_t(int) r = safe_divide(10, 2);
    assert(n00b_result_is_ok(r));
    assert(n00b_result_get(r) == 5);

    n00b_result_t(int) r2 = safe_divide(10, 0);
    assert(n00b_result_is_err(r2));

    printf("PASS: function return\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(void)
{
    test_result_ok();
    test_result_err();
    test_result_double();
    test_result_pointer();
    test_cross_header();
    test_function_return();

    printf("All generic_struct result tests passed!\n");
    return 0;
}
