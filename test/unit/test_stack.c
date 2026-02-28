#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "adt/stack.h"
#include "adt/array.h"


// ============================================================================
// 1. Construction
// ============================================================================

static void
test_construction(void)
{
    n00b_stack_t(int) s = n00b_stack_new(int);
    assert(n00b_stack_len(s) == 0);
    assert(n00b_stack_is_empty(s));
    n00b_stack_free(s);

    n00b_stack_t(int) s2 = n00b_stack_new_cap(int, 100);
    assert(n00b_stack_len(s2) == 0);
    n00b_stack_free(s2);

    printf("  [PASS] construction\n");
}

// ============================================================================
// 2. Push / Pop
// ============================================================================

static void
test_push_pop(void)
{
    n00b_stack_t(int) s = n00b_stack_new(int);

    n00b_stack_push(s, 10);
    n00b_stack_push(s, 20);
    n00b_stack_push(s, 30);
    assert(n00b_stack_len(s) == 3);

    assert(n00b_option_get(n00b_stack_pop(int, s)) == 30);
    assert(n00b_option_get(n00b_stack_pop(int, s)) == 20);
    assert(n00b_option_get(n00b_stack_pop(int, s)) == 10);
    assert(n00b_stack_is_empty(s));

    n00b_stack_free(s);
    printf("  [PASS] push/pop\n");
}

// ============================================================================
// 3. Peek
// ============================================================================

static void
test_peek(void)
{
    n00b_stack_t(int) s = n00b_stack_new(int);

    n00b_stack_push(s, 42);
    n00b_stack_push(s, 99);

    assert(n00b_stack_peek(s) == 99);
    assert(n00b_stack_len(s) == 2); // peek doesn't remove

    (void)n00b_stack_pop(int, s);
    assert(n00b_stack_peek(s) == 42);

    n00b_stack_free(s);
    printf("  [PASS] peek\n");
}

// ============================================================================
// 4. Clear
// ============================================================================

static void
test_clear(void)
{
    n00b_stack_t(int) s = n00b_stack_new(int);

    n00b_stack_push(s, 1);
    n00b_stack_push(s, 2);
    n00b_stack_push(s, 3);
    n00b_stack_clear(s);

    assert(n00b_stack_len(s) == 0);
    assert(n00b_stack_is_empty(s));

    // Can reuse after clear
    n00b_stack_push(s, 99);
    assert(n00b_option_get(n00b_stack_pop(int, s)) == 99);

    n00b_stack_free(s);
    printf("  [PASS] clear\n");
}

// ============================================================================
// 5. Dynamic growth
// ============================================================================

static void
test_dynamic_growth(void)
{
    n00b_stack_t(int) s = n00b_stack_new(int);

    for (int i = 0; i < 1000; i++) {
        n00b_stack_push(s, i);
    }

    assert(n00b_stack_len(s) == 1000);

    // Pop in reverse order
    for (int i = 999; i >= 0; i--) {
        assert(n00b_option_get(n00b_stack_pop(int, s)) == i);
    }

    assert(n00b_stack_is_empty(s));

    n00b_stack_free(s);
    printf("  [PASS] dynamic growth\n");
}

// ============================================================================
// 6. Foreach
// ============================================================================

static void
test_foreach(void)
{
    n00b_stack_t(int) s = n00b_stack_new(int);

    n00b_stack_push(s, 10);
    n00b_stack_push(s, 20);
    n00b_stack_push(s, 30);

    int sum   = 0;
    int count = 0;

    n00b_stack_foreach(s, p) {
        sum += *p;
        count++;
    }

    assert(count == 3);
    assert(sum == 60);

    n00b_stack_free(s);
    printf("  [PASS] foreach\n");
}

// ============================================================================
// 7. Pointer type (char *)
// ============================================================================

static void
test_pointer_type(void)
{
    n00b_stack_t(char *) s = n00b_stack_new(char *);

    char *a = "hello";
    char *b = "world";

    n00b_stack_push(s, a);
    n00b_stack_push(s, b);

    assert(n00b_stack_peek(s) == b);
    assert(strcmp(n00b_option_get(n00b_stack_pop(char *, s)), "world") == 0);
    assert(strcmp(n00b_option_get(n00b_stack_pop(char *, s)), "hello") == 0);

    n00b_stack_free(s);
    printf("  [PASS] pointer type\n");
}

// ============================================================================
// 8. Pop empty returns none
// ============================================================================

static void
test_pop_empty(void)
{
    n00b_stack_t(int) s = n00b_stack_new(int);
    assert(!n00b_option_is_set(n00b_stack_pop(int, s)));
    n00b_stack_free(s);
    printf("  [PASS] pop empty\n");
}

// ============================================================================
// 9. Stack to array -- conversion
// ============================================================================

static void
test_to_array(void)
{
    n00b_stack_t(int) s = n00b_stack_new(int);
    n00b_stack_push(s, 10);
    n00b_stack_push(s, 20);
    n00b_stack_push(s, 30);

    n00b_array_t(int) arr = n00b_stack_to_array(int, s);

    assert(n00b_array_len(arr) == 3);
    assert(n00b_array_get(arr, 0) == 10);
    assert(n00b_array_get(arr, 1) == 20);
    assert(n00b_array_get(arr, 2) == 30);

    // Stack should be empty after conversion.
    assert(s.data == nullptr);
    assert(s.len == 0);

    n00b_array_free(arr);
    printf("  [PASS] to_array\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running stack tests...\n");

    test_construction();
    test_push_pop();
    test_pop_empty();
    test_peek();
    test_clear();
    test_dynamic_growth();
    test_foreach();
    test_pointer_type();
    test_to_array();

    printf("All stack tests passed.\n");
    n00b_shutdown();
    return 0;
}
