#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/array.h"

n00b_array_decl(int);

// ============================================================================
// 1. n00b_array_set updates len correctly (off-by-one regression test)
// ============================================================================

static void
test_array_set_len(void)
{
    n00b_array_t(int) arr = n00b_array_new(int, 16);

    // Setting index 0 on an empty array should make len = 1, not 0.
    n00b_array_set(arr, 0, 42);
    assert(n00b_array_len(arr) == 1);

    // Setting index 3 should make len = 4.
    n00b_array_set(arr, 3, 99);
    assert(n00b_array_len(arr) == 4);

    // Setting index 1 (already within len) should not change len.
    n00b_array_set(arr, 1, 7);
    assert(n00b_array_len(arr) == 4);

    // Verify values.
    assert(n00b_array_get(arr, 0) == 42);
    assert(n00b_array_get(arr, 1) == 7);
    assert(n00b_array_get(arr, 3) == 99);

    n00b_array_free(arr);
    printf("  [PASS] array_set_len\n");
}

// ============================================================================
// 2. n00b_init is idempotent (double-init guard)
// ============================================================================

static void
test_init_idempotent(n00b_runtime_t *rt, int argc, char **argv)
{
    // Second call should be a no-op (returns immediately).
    n00b_init(rt, argc, argv);

    // Runtime should still be functional.
    n00b_array_t(int) arr = n00b_array_new(int, 4);
    n00b_array_set(arr, 0, 1);
    assert(n00b_array_get(arr, 0) == 1);
    n00b_array_free(arr);

    printf("  [PASS] init_idempotent\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running array tests...\n");

    test_array_set_len();
    test_init_idempotent(&runtime, argc, argv);

    printf("All array tests passed.\n");
    n00b_shutdown();
    return 0;
}
