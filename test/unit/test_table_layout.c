#include <stdio.h>
#include <assert.h>
#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "display/table/table.h"

// ====================================================================
// Helpers
// ====================================================================

static n00b_string_t
make_str(const char *s)
{
    return n00b_string_from_raw(s, (int64_t)strlen(s));
}

static int64_t
col_total(n00b_table_t *t)
{
    int64_t sum = 0;

    for (n00b_isize_t i = 0; i < (n00b_isize_t)t->col_specs.len; i++) {
        sum += t->col_results[i].size;
    }

    return sum;
}

// ====================================================================
// Tests
// ====================================================================

static void
test_fit_columns(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table);

    n00b_table_add_cell(t, make_str("short"));
    n00b_table_add_cell(t, make_str("a longer cell value"));
    n00b_table_end_row(t);

    _n00b_table_compute_layout(t, 80);

    // Both columns should get at least their content width + padding.
    assert(t->col_results[0].size > 0);
    assert(t->col_results[1].size > 0);
    assert(t->col_results[1].size >= t->col_results[0].size);

    n00b_table_destroy(t);
    printf("  [PASS] fit columns\n");
}

static void
test_fixed_columns(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table);

    n00b_table_col_fixed(t, 10);
    n00b_table_col_fixed(t, 20);

    n00b_table_add_cell(t, make_str("A"));
    n00b_table_add_cell(t, make_str("B"));
    n00b_table_end_row(t);

    _n00b_table_compute_layout(t, 80);

    assert(t->col_results[0].size == 10);
    assert(t->col_results[1].size == 20);

    n00b_table_destroy(t);
    printf("  [PASS] fixed columns\n");
}

static void
test_flex_columns(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table);

    n00b_table_col_flex(t, 1);
    n00b_table_col_flex(t, 2);
    n00b_table_col_flex(t, 1);

    n00b_table_add_cell(t, make_str("A"));
    n00b_table_add_cell(t, make_str("B"));
    n00b_table_add_cell(t, make_str("C"));
    n00b_table_end_row(t);

    _n00b_table_compute_layout(t, 80);

    // Flex 2 column should get about twice as much as flex 1.
    assert(t->col_results[1].size > t->col_results[0].size);
    assert(t->col_results[1].size > t->col_results[2].size);
    assert(col_total(t) <= 80);

    n00b_table_destroy(t);
    printf("  [PASS] flex columns\n");
}

static void
test_mixed_columns(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table);

    n00b_table_col_fixed(t, 10);
    n00b_table_col_flex(t, 1);
    n00b_table_col_fit(t);

    n00b_table_add_cell(t, make_str("fixed"));
    n00b_table_add_cell(t, make_str("flex"));
    n00b_table_add_cell(t, make_str("fit content here"));
    n00b_table_end_row(t);

    _n00b_table_compute_layout(t, 80);

    assert(t->col_results[0].size == 10);
    assert(t->col_results[1].size > 0);
    assert(t->col_results[2].size > 0);

    n00b_table_destroy(t);
    printf("  [PASS] mixed columns (fixed/flex/fit)\n");
}

static void
test_row_heights(void)
{
    // Use a narrow table so content wraps.
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table, .num_cols = 1);

    n00b_table_add_cell(t, make_str("This is a long string "
                                      "that should wrap across "
                                      "multiple lines."));
    n00b_table_end_row(t);

    n00b_table_add_cell(t, make_str("Short"));
    n00b_table_end_row(t);

    _n00b_table_compute_layout(t, 20);

    // First row should be taller due to wrapping.
    assert(t->row_heights[0] >= t->row_heights[1]);
    assert(t->row_heights[1] >= 1);

    n00b_table_destroy(t);
    printf("  [PASS] row heights\n");
}

static void
test_pct_columns(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table);

    n00b_table_col_pct(t, 0.0, 0.25);
    n00b_table_col_pct(t, 0.0, 0.75);

    n00b_table_add_cell(t, make_str("A"));
    n00b_table_add_cell(t, make_str("B"));
    n00b_table_end_row(t);

    _n00b_table_compute_layout(t, 100);

    assert(t->col_results[0].size <= 25);
    assert(t->col_results[1].size <= 75);

    n00b_table_destroy(t);
    printf("  [PASS] pct columns\n");
}

static void
test_narrow_table(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table, .num_cols = 3);

    n00b_table_add_cell(t, make_str("AAAA"));
    n00b_table_add_cell(t, make_str("BBBB"));
    n00b_table_add_cell(t, make_str("CCCC"));
    n00b_table_end_row(t);

    // Very narrow: columns should still get some minimum size.
    _n00b_table_compute_layout(t, 10);

    for (n00b_isize_t i = 0; i < 3; i++) {
        assert(t->col_results[i].size > 0);
    }

    n00b_table_destroy(t);
    printf("  [PASS] narrow table\n");
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running table layout tests...\n");

    test_fit_columns();
    test_fixed_columns();
    test_flex_columns();
    test_mixed_columns();
    test_row_heights();
    test_pct_columns();
    test_narrow_table();

    printf("All table layout tests passed.\n");
    n00b_shutdown();
    return 0;
}
