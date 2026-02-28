#include <stdio.h>
#include <assert.h>
#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "display/table/table.h"
#include "display/render/box.h"

// ====================================================================
// Helpers
// ====================================================================

static n00b_string_t
make_str(const char *s)
{
    return n00b_string_from_raw(s, (int64_t)strlen(s));
}

// ====================================================================
// Tests
// ====================================================================

static void
test_create_empty(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table);

    assert(t != nullptr);
    assert(t->rows.len == 0);
    assert(t->col_specs.len == 0);

    n00b_table_destroy(t);
    printf("  [PASS] create empty table\n");
}

static void
test_create_with_cols(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table, .num_cols = 3);

    assert(t != nullptr);
    assert(t->col_specs.len == 3);

    n00b_table_destroy(t);
    printf("  [PASS] create table with preset columns\n");
}

static void
test_add_single_row(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table);

    n00b_table_add_cell(t, make_str("A"));
    n00b_table_add_cell(t, make_str("B"));
    n00b_table_add_cell(t, make_str("C"));
    n00b_table_end_row(t);

    assert(t->rows.len == 1);
    assert(t->col_specs.len == 3); // Auto-detected.
    assert(t->cols_locked);

    n00b_table_destroy(t);
    printf("  [PASS] add single row, auto-detect columns\n");
}

static void
test_add_multiple_rows(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table, .num_cols = 2);

    n00b_table_add_cell(t, make_str("R0C0"));
    n00b_table_add_cell(t, make_str("R0C1"));
    n00b_table_end_row(t);

    n00b_table_add_cell(t, make_str("R1C0"));
    n00b_table_add_cell(t, make_str("R1C1"));
    n00b_table_end_row(t);

    assert(t->rows.len == 2);
    assert(t->col_specs.len == 2);

    n00b_table_destroy(t);
    printf("  [PASS] add multiple rows\n");
}

static void
test_empty_cell(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table, .num_cols = 3);

    n00b_table_add_cell(t, make_str("A"));
    n00b_table_empty_cell(t);
    n00b_table_add_cell(t, make_str("C"));
    n00b_table_end_row(t);

    assert(t->rows.len == 1);
    assert(t->rows.data[0].cells.len == 3);
    assert(t->rows.data[0].cells.data[1].content.u8_bytes == 0);

    n00b_table_destroy(t);
    printf("  [PASS] empty cell\n");
}

static void
test_col_span(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table, .num_cols = 3);

    n00b_table_add_cell(t, make_str("spans two"), .col_span = 2);
    n00b_table_add_cell(t, make_str("one col"));
    n00b_table_end_row(t);

    assert(t->rows.len == 1);
    assert(t->rows.data[0].cells.data[0].col_span == 2);
    assert(t->rows.data[0].cells.data[1].col_span == 1);

    n00b_table_destroy(t);
    printf("  [PASS] column span\n");
}

static void
test_span_all(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table, .num_cols = 4);

    // First row establishes column count.
    for (int i = 0; i < 4; i++) {
        n00b_table_add_cell(t, make_str("x"));
    }
    n00b_table_end_row(t);

    // Span-all row.
    n00b_table_add_cell(t, make_str("full width"), .col_span = -1);
    n00b_table_end_row(t);

    assert(t->rows.len == 2);
    assert(t->rows.data[1].cells.data[0].col_span == 4);

    n00b_table_destroy(t);
    printf("  [PASS] span-all (-1)\n");
}

static void
test_add_row_convenience(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table, .num_cols = 3);

    n00b_string_t cells[3] = {
        make_str("X"),
        make_str("Y"),
        make_str("Z"),
    };

    n00b_table_add_row(t, cells, 3);

    assert(t->rows.len == 1);
    assert(t->rows.data[0].cells.len == 3);

    n00b_table_destroy(t);
    printf("  [PASS] add_row convenience\n");
}

static void
test_table_end_flushes(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table, .num_cols = 2);

    n00b_table_add_cell(t, make_str("A"));
    n00b_table_add_cell(t, make_str("B"));
    // Don't call end_row — let end() flush it.
    n00b_table_end(t);

    assert(t->rows.len == 1);

    n00b_table_destroy(t);
    printf("  [PASS] table_end flushes partial row\n");
}

static void
test_col_spec_fit(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table);

    n00b_isize_t idx = n00b_table_col_fit(t);

    assert(idx == 0);
    assert(t->col_specs.len == 1);
    assert(t->col_specs.data[0].mode == N00B_COL_FIT);

    n00b_table_destroy(t);
    printf("  [PASS] col_fit\n");
}

static void
test_col_spec_flex(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table);

    n00b_table_col_flex(t, 3);

    assert(t->col_specs.len == 1);
    assert(t->col_specs.data[0].mode == N00B_COL_FLEX);
    assert(t->col_specs.data[0].flex_multiple == 3);

    n00b_table_destroy(t);
    printf("  [PASS] col_flex\n");
}

static void
test_col_spec_fixed(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table);

    n00b_table_col_fixed(t, 20);

    assert(t->col_specs.len == 1);
    assert(t->col_specs.data[0].mode == N00B_COL_FIXED);
    assert(t->col_specs.data[0].pref.value.i == 20);
    assert(t->col_specs.data[0].min.value.i == 20);
    assert(t->col_specs.data[0].max.value.i == 20);

    n00b_table_destroy(t);
    printf("  [PASS] col_fixed\n");
}

static void
test_col_spec_range(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table);

    n00b_table_col_range(t, 5, 30);

    assert(t->col_specs.data[0].min.value.i == 5);
    assert(t->col_specs.data[0].max.value.i == 30);

    n00b_table_destroy(t);
    printf("  [PASS] col_range\n");
}

static void
test_col_spec_pct(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table);

    n00b_table_col_pct(t, 0.1, 0.5);

    assert(t->col_specs.data[0].min.pct == true);
    assert(t->col_specs.data[0].max.pct == true);

    n00b_table_destroy(t);
    printf("  [PASS] col_pct\n");
}

static void
test_set_col_priority(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table, .num_cols = 2);

    n00b_table_set_col_priority(t, 0, 10);
    n00b_table_set_col_priority(t, 1, 5);

    assert(t->col_specs.data[0].priority == 10);
    assert(t->col_specs.data[1].priority == 5);

    n00b_table_destroy(t);
    printf("  [PASS] set_col_priority\n");
}

static void
test_style_cascade(void)
{
    n00b_box_props_t *cell_style = n00b_box_props_new(.pad_left = 2);
    n00b_box_props_t *hdr_style  = n00b_box_props_new(.pad_left = 3);
    n00b_box_props_t *per_cell   = n00b_box_props_new(.pad_left = 4);

    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table,
        .num_cols     = 2,
        .cell_props   = cell_style,
        .header_props = hdr_style);

    n00b_table_add_cell(t, make_str("A"));
    n00b_table_add_cell(t, make_str("B"), .cell_props = per_cell);
    n00b_table_end_row(t);

    n00b_table_add_cell(t, make_str("C"));
    n00b_table_add_cell(t, make_str("D"));
    n00b_table_end_row(t);

    // Row 0, col 0: header_props wins (row 0 special).
    const n00b_box_props_t *p0 =
        _n00b_table_resolve_cell_props(t, 0, 0,
                                        &t->rows.data[0].cells.data[0]);
    assert(p0 == hdr_style);

    // Row 0, col 1: per-cell override wins.
    const n00b_box_props_t *p1 =
        _n00b_table_resolve_cell_props(t, 0, 1,
                                        &t->rows.data[0].cells.data[1]);
    assert(p1 == per_cell);

    // Row 1, col 0: default cell_props.
    const n00b_box_props_t *p2 =
        _n00b_table_resolve_cell_props(t, 1, 0,
                                        &t->rows.data[1].cells.data[0]);
    assert(p2 == cell_style);

    n00b_table_destroy(t);
    printf("  [PASS] style cascade\n");
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running table build tests...\n");

    test_create_empty();
    test_create_with_cols();
    test_add_single_row();
    test_add_multiple_rows();
    test_empty_cell();
    test_col_span();
    test_span_all();
    test_add_row_convenience();
    test_table_end_flushes();
    test_col_spec_fit();
    test_col_spec_flex();
    test_col_spec_fixed();
    test_col_spec_range();
    test_col_spec_pct();
    test_set_col_priority();
    test_style_cascade();

    printf("All table build tests passed.\n");
    n00b_shutdown();
    return 0;
}
