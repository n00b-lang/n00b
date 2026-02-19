#include <stdio.h>
#include <assert.h>
#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/layout.h"

// ====================================================================
// Helpers
// ====================================================================

static int64_t
total_size(n00b_layout_result_t *results, n00b_isize_t n)
{
    int64_t sum = 0;
    for (n00b_isize_t i = 0; i < n; i++) {
        sum += results[i].size;
    }
    return sum;
}

// ====================================================================
// Tests
// ====================================================================

static void
test_single_item_gets_all_space(void)
{
    n00b_layout_t        items[1]   = { { .pref = { .value.i = 40 } } };
    n00b_layout_result_t results[1] = {};

    n00b_layout_calculate(items, results, 1, 80);

    assert(results[0].size == 40);
    printf("  [PASS] single item gets preferred size\n");
}

static void
test_two_items_with_min(void)
{
    n00b_layout_t items[2] = {
        { .min = { .value.i = 10 }, .pref = { .value.i = 30 } },
        { .min = { .value.i = 10 }, .pref = { .value.i = 30 } },
    };
    n00b_layout_result_t results[2] = {};

    n00b_layout_calculate(items, results, 2, 80);

    assert(results[0].size >= 10);
    assert(results[1].size >= 10);
    assert(results[0].size == 30);
    assert(results[1].size == 30);
    printf("  [PASS] two items with min get preferred sizes\n");
}

static void
test_max_constraint(void)
{
    n00b_layout_t items[2] = {
        { .pref = { .value.i = 20 }, .max = { .value.i = 25 } },
        { .pref = { .value.i = 20 }, .max = { .value.i = 25 } },
    };
    n00b_layout_result_t results[2] = {};

    n00b_layout_calculate(items, results, 2, 80);

    assert(results[0].size <= 25);
    assert(results[1].size <= 25);
    printf("  [PASS] max constraint respected\n");
}

static void
test_flex_distribution(void)
{
    n00b_layout_t items[3] = {
        { .min = { .value.i = 10 }, .flex_multiple = 1 },
        { .min = { .value.i = 10 }, .flex_multiple = 2 },
        { .min = { .value.i = 10 }, .flex_multiple = 1 },
    };
    n00b_layout_result_t results[3] = {};

    n00b_layout_calculate(items, results, 3, 90);

    // 30 used for mins, 60 remaining.
    // Flex units: 1+2+1 = 4, so each unit = 15.
    // item0: 10 + 15 = 25, item1: 10 + 30 = 40, item2: 10 + 15 = 25
    assert(results[0].size >= 10);
    assert(results[1].size >= 10);
    assert(results[2].size >= 10);
    assert(total_size(results, 3) == 90);

    // Item with flex=2 should be larger than item with flex=1.
    assert(results[1].size > results[0].size);
    assert(results[1].size > results[2].size);
    printf("  [PASS] flex distribution proportional\n");
}

static void
test_overflow_shrink(void)
{
    n00b_layout_t items[3] = {
        { .min = { .value.i = 5 }, .pref = { .value.i = 30 } },
        { .min = { .value.i = 5 }, .pref = { .value.i = 30 } },
        { .min = { .value.i = 5 }, .pref = { .value.i = 30 } },
    };
    n00b_layout_result_t results[3] = {};

    // Only 50 available but 90 preferred.
    n00b_layout_calculate(items, results, 3, 50);

    assert(total_size(results, 3) <= 50);
    assert(results[0].size >= 5);
    assert(results[1].size >= 5);
    assert(results[2].size >= 5);
    printf("  [PASS] overflow shrinks items\n");
}

static void
test_priority_cropping(void)
{
    n00b_layout_t items[3] = {
        { .min = { .value.i = 10 }, .pref = { .value.i = 20 }, .priority = 0 },
        { .min = { .value.i = 10 }, .pref = { .value.i = 20 }, .priority = 10 },
        { .min = { .value.i = 10 }, .pref = { .value.i = 20 }, .priority = 5 },
    };
    n00b_layout_result_t results[3] = {};

    // Only 15 available but 60 preferred, 30 min.
    n00b_layout_calculate(items, results, 3, 15);

    // High priority item (index 1, priority=10) should get the most.
    assert(results[1].size >= results[0].size);
    assert(results[1].size >= results[2].size);
    assert(total_size(results, 3) <= 15);
    printf("  [PASS] priority-based cropping\n");
}

static void
test_percentage_dims(void)
{
    n00b_layout_t items[2] = {
        { .pref = { .value.d = 0.25, .pct = true } },
        { .pref = { .value.d = 0.50, .pct = true } },
    };
    n00b_layout_result_t results[2] = {};

    n00b_layout_calculate(items, results, 2, 100);

    assert(results[0].size == 25);
    assert(results[1].size == 50);
    printf("  [PASS] percentage dimensions\n");
}

static void
test_zero_items(void)
{
    n00b_layout_calculate(nullptr, nullptr, 0, 100);
    printf("  [PASS] zero items\n");
}

static void
test_exact_fit(void)
{
    n00b_layout_t items[3] = {
        { .pref = { .value.i = 20 } },
        { .pref = { .value.i = 30 } },
        { .pref = { .value.i = 50 } },
    };
    n00b_layout_result_t results[3] = {};

    n00b_layout_calculate(items, results, 3, 100);

    assert(results[0].size == 20);
    assert(results[1].size == 30);
    assert(results[2].size == 50);
    assert(total_size(results, 3) == 100);
    printf("  [PASS] exact fit (sum == available)\n");
}

static void
test_min_greater_than_available(void)
{
    n00b_layout_t items[2] = {
        { .min = { .value.i = 40 }, .pref = { .value.i = 40 } },
        { .min = { .value.i = 40 }, .pref = { .value.i = 40 } },
    };
    n00b_layout_result_t results[2] = {};

    // Only 50 but min totals 80.
    n00b_layout_calculate(items, results, 2, 50);

    // Priority crop should force some items to lose allocation.
    assert(total_size(results, 2) <= 50);
    printf("  [PASS] min > available triggers crop\n");
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running layout tests...\n");

    test_single_item_gets_all_space();
    test_two_items_with_min();
    test_max_constraint();
    test_flex_distribution();
    test_overflow_shrink();
    test_priority_cropping();
    test_percentage_dims();
    test_zero_items();
    test_exact_fit();
    test_min_greater_than_available();

    printf("All layout tests passed.\n");
    return 0;
}
