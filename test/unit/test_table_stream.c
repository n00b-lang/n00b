#include <stdio.h>
#include <assert.h>
#include "n00b.h"
#include "core/alloc.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "table/table.h"

// ====================================================================
// Helpers
// ====================================================================

static n00b_string_t
make_str(const char *s)
{
    return n00b_string_from_raw(nullptr, s, (int64_t)strlen(s), (int64_t)strlen(s));
}

// ====================================================================
// Tests
// ====================================================================

static void
test_ring_buffer_basic(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table, .num_cols = 1, .max_rows = 3);

    // Add 3 rows — should all fit.
    n00b_table_add_cell(t, make_str("Row 0"));
    n00b_table_end_row(t);
    n00b_table_add_cell(t, make_str("Row 1"));
    n00b_table_end_row(t);
    n00b_table_add_cell(t, make_str("Row 2"));
    n00b_table_end_row(t);

    assert(t->rows.len == 3);
    assert(t->total_added == 3);

    n00b_table_destroy(t);
    printf("  [PASS] ring buffer: 3 rows in size-3 buffer\n");
}

static void
test_ring_buffer_eviction(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table, .num_cols = 1, .max_rows = 3);

    // Add 5 rows — first two should be evicted.
    for (int i = 0; i < 5; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "Row %d", i);
        n00b_table_add_cell(t, make_str(buf));
        n00b_table_end_row(t);
    }

    assert(t->rows.len == 3);
    assert(t->total_added == 5);
    // Ring should contain rows 2, 3, 4 (most recent 3).

    n00b_table_destroy(t);
    printf("  [PASS] ring buffer: eviction after overflow\n");
}

static void
test_ring_buffer_render(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table, .num_cols = 1, .max_rows = 2);

    // Add 4 rows; only last 2 should be visible.
    for (int i = 0; i < 4; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "Row %d", i);
        n00b_table_add_cell(t, make_str(buf));
        n00b_table_end_row(t);
    }

    n00b_plane_t *p = n00b_table_render(t, .width = 40);

    assert(p != nullptr);
    assert(t->rows.len == 2);

    n00b_table_destroy(t);
    printf("  [PASS] ring buffer: render after eviction\n");
}

static void
test_ring_buffer_invalidation(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table, .num_cols = 1, .max_rows = 3);

    n00b_table_add_cell(t, make_str("Row 0"));
    n00b_table_end_row(t);

    // Render to cache layout.
    n00b_table_render(t, .width = 40);
    assert(t->layout_valid);

    // Add another row — should invalidate.
    n00b_table_add_cell(t, make_str("Row 1"));
    n00b_table_end_row(t);

    assert(!t->layout_valid);

    n00b_table_destroy(t);
    printf("  [PASS] ring buffer: adding row invalidates layout\n");
}

static void
test_unlimited_mode(void)
{
    n00b_table_t *t = n00b_new_kargs(n00b_table_t, table, .num_cols = 1);

    // max_rows = 0 means unlimited.
    for (int i = 0; i < 100; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "Row %d", i);
        n00b_table_add_cell(t, make_str(buf));
        n00b_table_end_row(t);
    }

    assert(t->rows.len == 100);
    assert(t->max_rows == 0);

    n00b_table_destroy(t);
    printf("  [PASS] unlimited mode holds all rows\n");
}

static void
test_end_row_empty_noop(void)
{
    n00b_table_t *t = n00b_table_new(.num_cols = 1);

    n00b_table_add_cell(t, make_str("seed"));
    n00b_table_end_row(t);
    n00b_plane_t *p = n00b_table_render(t, 40);

    assert(p != nullptr);
    assert(t->layout_valid);
    assert(t->current_row.cells.len == 0);

    size_t       rows_before   = t->rows.len;
    n00b_isize_t added_before  = t->total_added;
    bool         layout_before = t->layout_valid;

    // Calling end_row() with an empty current row must be a no-op.
    n00b_table_end_row(t);

    assert(t->rows.len == rows_before);
    assert(t->total_added == added_before);
    assert(t->layout_valid == layout_before);
    assert(t->current_row.cells.len == 0);

    n00b_table_destroy(t);
    printf("  [PASS] end_row empty row is a no-op\n");
}

static void
test_unlimited_mode_gc_stress(void)
{
    n00b_arena_t     *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    n00b_allocator_t *alloc = (n00b_allocator_t *)arena;
    n00b_table_t     *t     = n00b_table_new(.num_cols = 1, .allocator = alloc);
    n00b_string_t     cell  = make_str("x");
    n00b_gc_register_root(t);
    n00b_gc_register_root(cell);

    uint32_t prev_count = 0;
    bool     collected  = false;
    int      nrows      = 3000;

    for (int i = 0; i < nrows; i++) {
        n00b_table_add_cell(t, cell);
        n00b_table_end_row(t);

        uint32_t cur = n00b_atomic_load(&arena->alloc_count);

        if (cur < prev_count) {
            // alloc_count reset implies at least one collection occurred.
            collected = true;
        }

        prev_count = cur;
    }

    assert(collected);
    assert(t->rows.len == (size_t)nrows);
    assert(t->max_rows == 0);

    n00b_gc_unregister_root(cell);
    n00b_gc_unregister_root(t);
    n00b_table_destroy(t);
    printf("  [PASS] unlimited mode survives GC stress\n");
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running table streaming tests...\n");

    test_ring_buffer_basic();
    test_ring_buffer_eviction();
    test_ring_buffer_render();
    test_ring_buffer_invalidation();
    test_unlimited_mode();
    test_end_row_empty_noop();
    test_unlimited_mode_gc_stress();

    printf("All table streaming tests passed.\n");
    n00b_shutdown();
    return 0;
}
