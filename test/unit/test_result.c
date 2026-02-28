#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "adt/result.h"

n00b_result_decl(char *);

// ============================================================================
// 1. Construction
// ============================================================================

static void
test_construction(void)
{
    n00b_result_t(int) ok = n00b_result_ok(int, 42);
    assert(n00b_result_is_ok(ok));
    assert(!n00b_result_is_err(ok));

    n00b_result_t(int) err = n00b_result_err(int, -1);
    assert(n00b_result_is_err(err));
    assert(!n00b_result_is_ok(err));

    printf("  [PASS] construction\n");
}

// ============================================================================
// 2. Get value
// ============================================================================

static void
test_get(void)
{
    n00b_result_t(int) ok = n00b_result_ok(int, 99);
    assert(n00b_result_get(ok) == 99);

    n00b_result_t(int) err = n00b_result_err(int, 5);
    assert(n00b_result_get_err(err) == 5);

    printf("  [PASS] get\n");
}

// ============================================================================
// 3. Get or else
// ============================================================================

static void
test_get_or_else(void)
{
    int fallback = -999;

    n00b_result_t(int) ok = n00b_result_ok(int, 42);
    assert(n00b_result_get_or_else(ok, fallback) == 42);

    n00b_result_t(int) err = n00b_result_err(int, 1);
    assert(n00b_result_get_or_else(err, fallback) == -999);

    printf("  [PASS] get_or_else\n");
}

// ============================================================================
// 4. Match
// ============================================================================

static void
test_match(void)
{
    n00b_result_t(int) ok = n00b_result_ok(int, 10);
    int val               = n00b_result_match(ok, 1, 0);
    assert(val == 1);

    n00b_result_t(int) err = n00b_result_err(int, 3);
    val                     = n00b_result_match(err, 1, 0);
    assert(val == 0);

    printf("  [PASS] match\n");
}

// ============================================================================
// 5. Pointer type
// ============================================================================

static void
test_pointer_type(void)
{
    char *hello = "hello";

    n00b_result_t(char *) ok = n00b_result_ok(char *, hello);
    assert(n00b_result_is_ok(ok));
    assert(n00b_result_get(ok) == hello);

    n00b_result_t(char *) err = n00b_result_err(char *, 42);
    assert(n00b_result_is_err(err));
    assert(n00b_result_get_err(err) == 42);

    printf("  [PASS] pointer type\n");
}

// ============================================================================
// 6. Postfix ! operator (auto-unwrap / early return)
// ============================================================================

static n00b_result_t(int)
returns_ok(void)
{
    return n00b_result_ok(int, 10);
}

static n00b_result_t(int)
returns_err(void)
{
    return n00b_result_err(int, -1);
}

static n00b_result_t(int)
chain_ok(void)
{
    int x = returns_ok()!;
    return n00b_result_ok(int, x + 1);
}

static n00b_result_t(int)
chain_err(void)
{
    int x = returns_err()!; // should early-return with the error
    (void)x;
    return n00b_result_ok(int, 999); // should not be reached
}

static void
test_bang_operator(void)
{
    n00b_result_t(int) r1 = chain_ok();
    assert(n00b_result_is_ok(r1));
    assert(n00b_result_get(r1) == 11);

    n00b_result_t(int) r2 = chain_err();
    assert(n00b_result_is_err(r2));
    assert(n00b_result_get_err(r2) == -1);

    printf("  [PASS] postfix ! operator\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running result tests...\n");

    test_construction();
    test_get();
    test_get_or_else();
    test_match();
    test_pointer_type();
    test_bang_operator();

    printf("All result tests passed.\n");
    n00b_shutdown();
    return 0;
}
