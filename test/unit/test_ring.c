#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "adt/ring.h"

static void
test_ring_capacity_zero_rounds_up(void)
{
    n00b_ring_t(int) ring = n00b_ring_new(int, 0);
    int              value = 0;

    assert(ring.lock != nullptr);
    assert(n00b_ring_cap(ring) == 1);
    assert(n00b_ring_len(ring) == 0);
    assert(n00b_ring_drop_count(ring) == 0);

    assert(n00b_ring_push(ring, 7));
    assert(n00b_ring_push(ring, 9));
    assert(n00b_ring_drop_count(ring) == 1);
    assert(n00b_ring_pop(ring, &value));
    assert(value == 9);
    assert(!n00b_ring_pop(ring, &value));

    n00b_ring_free(ring);
    printf("  [PASS] capacity_zero_rounds_up\n");
}

static void
test_ring_fifo_pop(void)
{
    n00b_ring_t(int) ring = n00b_ring_new(int, 3);
    int              value = 0;

    assert(n00b_ring_push(ring, 10));
    assert(n00b_ring_push(ring, 20));
    assert(n00b_ring_push(ring, 30));
    assert(n00b_ring_len(ring) == 3);
    assert(n00b_ring_drop_count(ring) == 0);

    assert(n00b_ring_pop(ring, &value));
    assert(value == 10);
    assert(n00b_ring_pop(ring, &value));
    assert(value == 20);
    assert(n00b_ring_pop(ring, &value));
    assert(value == 30);
    assert(n00b_ring_len(ring) == 0);
    assert(n00b_ring_is_empty(ring));

    n00b_ring_free(ring);
    printf("  [PASS] fifo_pop\n");
}

static void
test_ring_drops_oldest(void)
{
    n00b_ring_t(int) ring = n00b_ring_new(int, 2);
    int              value = 0;

    assert(n00b_ring_push(ring, 10));
    assert(n00b_ring_push(ring, 20));
    assert(n00b_ring_push(ring, 30));
    assert(n00b_ring_drop_count(ring) == 1);
    assert(n00b_ring_len(ring) == 2);

    assert(n00b_ring_peek(ring, &value));
    assert(value == 20);
    assert(n00b_ring_pop(ring, &value));
    assert(value == 20);
    assert(n00b_ring_pop(ring, &value));
    assert(value == 30);

    n00b_ring_free(ring);
    printf("  [PASS] drops_oldest\n");
}

static void
test_ring_wraparound_order(void)
{
    n00b_ring_t(int) ring = n00b_ring_new_private(int, 3);
    int              value = 0;

    assert(ring.lock == nullptr);
    assert(n00b_ring_push(ring, 1));
    assert(n00b_ring_push(ring, 2));
    assert(n00b_ring_pop(ring, &value));
    assert(value == 1);

    assert(n00b_ring_push(ring, 3));
    assert(n00b_ring_push(ring, 4));
    assert(n00b_ring_len(ring) == 3);

    assert(n00b_ring_pop(ring, &value));
    assert(value == 2);
    assert(n00b_ring_pop(ring, &value));
    assert(value == 3);
    assert(n00b_ring_pop(ring, &value));
    assert(value == 4);
    assert(!n00b_ring_pop(ring, &value));

    n00b_ring_free(ring);
    printf("  [PASS] wraparound_order\n");
}

static void
test_ring_zeroed_ops_are_noops(void)
{
    n00b_ring_t(int) ring = {};
    int              value = 0;

    assert(n00b_ring_cap(ring) == 0);
    assert(n00b_ring_len(ring) == 0);
    assert(n00b_ring_drop_count(ring) == 0);
    assert(!n00b_ring_push(ring, 1));
    assert(!n00b_ring_peek(ring, &value));
    assert(!n00b_ring_pop(ring, &value));

    n00b_ring_free(ring);
    printf("  [PASS] zeroed_ops_are_noops\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running ring tests...\n");

    test_ring_capacity_zero_rounds_up();
    test_ring_fifo_pop();
    test_ring_drops_oldest();
    test_ring_wraparound_order();
    test_ring_zeroed_ops_are_noops();

    printf("All ring tests passed.\n");
    n00b_shutdown();
    return 0;
}
