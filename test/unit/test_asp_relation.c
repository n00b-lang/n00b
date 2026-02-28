#include "test_unicode_helpers.h"
#include "logic/asp_relation.h"

// ============================================================================
// Tests
// ============================================================================

TEST(test_basic_insert)
{
    n00b_dl_relation_t rel;
    n00b_dl_relation_init(&rel, 0, r"test", 2);

    n00b_dl_sym_t t1[] = {10, 20};
    n00b_dl_sym_t t2[] = {30, 40};

    ASSERT(n00b_dl_relation_insert(&rel, t1));
    ASSERT(n00b_dl_relation_insert(&rel, t2));
    ASSERT_EQ((int64_t)rel.to_add_count, 2);

    // Duplicate
    ASSERT(!n00b_dl_relation_insert(&rel, t1));
    ASSERT_EQ((int64_t)rel.to_add_count, 2);

    n00b_dl_relation_free(&rel);
}

TEST(test_swap_lifecycle)
{
    n00b_dl_relation_t rel;
    n00b_dl_relation_init(&rel, 0, r"test", 2);

    n00b_dl_sym_t t1[] = {1, 2};
    n00b_dl_sym_t t2[] = {3, 4};
    n00b_dl_sym_t t3[] = {5, 6};

    n00b_dl_relation_insert(&rel, t1);
    n00b_dl_relation_insert(&rel, t2);

    // First swap: to_add -> recent
    ASSERT(n00b_dl_relation_swap(&rel));
    ASSERT_EQ((int64_t)rel.recent_count, 2);
    ASSERT_EQ((int64_t)rel.to_add_count, 0);
    ASSERT_EQ((int64_t)rel.stable_count, 0);

    // Add more, swap again: recent -> stable, to_add -> recent
    n00b_dl_relation_insert(&rel, t3);
    ASSERT(n00b_dl_relation_swap(&rel));
    ASSERT_EQ((int64_t)rel.stable_count, 2);
    ASSERT_EQ((int64_t)rel.recent_count, 1);
    ASSERT_EQ((int64_t)rel.to_add_count, 0);

    ASSERT_EQ((int64_t)n00b_dl_relation_count(&rel), 3);

    // Swap with nothing new
    ASSERT(!n00b_dl_relation_swap(&rel));
    ASSERT_EQ((int64_t)rel.stable_count, 3);
    ASSERT_EQ((int64_t)rel.recent_count, 0);

    n00b_dl_relation_free(&rel);
}

TEST(test_column_index)
{
    n00b_dl_relation_t rel;
    n00b_dl_relation_init(&rel, 0, r"test", 2);

    n00b_dl_sym_t t1[] = {10, 20};
    n00b_dl_sym_t t2[] = {10, 30};
    n00b_dl_sym_t t3[] = {40, 20};

    n00b_dl_relation_insert(&rel, t1);
    n00b_dl_relation_insert(&rel, t2);
    n00b_dl_relation_insert(&rel, t3);

    n00b_dl_relation_swap(&rel);
    n00b_dl_relation_rebuild_index(&rel);

    // Column 0, value 10 should have 2 entries
    n00b_dl_offset_list_t *offsets =
        n00b_dl_i64_offsets_map_get(&rel.col_index[0], 10);
    ASSERT(offsets != nullptr);
    ASSERT_EQ(offsets->len, 2);

    // Column 1, value 20 should have 2 entries
    offsets = n00b_dl_i64_offsets_map_get(&rel.col_index[1], 20);
    ASSERT(offsets != nullptr);
    ASSERT_EQ(offsets->len, 2);

    // Column 0, value 40 should have 1 entry
    offsets = n00b_dl_i64_offsets_map_get(&rel.col_index[0], 40);
    ASSERT(offsets != nullptr);
    ASSERT_EQ(offsets->len, 1);

    n00b_dl_relation_free(&rel);
}

TEST(test_tuple_hash)
{
    n00b_dl_sym_t t1[] = {1, 2, 3};
    n00b_dl_sym_t t2[] = {1, 2, 3};
    n00b_dl_sym_t t3[] = {3, 2, 1};

    uint64_t h1 = n00b_dl_tuple_hash(t1, 3);
    uint64_t h2 = n00b_dl_tuple_hash(t2, 3);
    uint64_t h3 = n00b_dl_tuple_hash(t3, 3);

    ASSERT(h1 == h2);
    ASSERT(h1 != h3);
}

TEST(test_dedup_correctness)
{
    // Insert multiple distinct tuples and verify they're all stored
    n00b_dl_relation_t rel;
    n00b_dl_relation_init(&rel, 0, r"dedup", 2);

    // These are all distinct
    for (int64_t i = 0; i < 100; i++) {
        n00b_dl_sym_t t[] = {i, i * 10};
        ASSERT(n00b_dl_relation_insert(&rel, t));
    }
    ASSERT_EQ((int64_t)rel.to_add_count, 100);

    // Re-insert all — should all be rejected as duplicates
    for (int64_t i = 0; i < 100; i++) {
        n00b_dl_sym_t t[] = {i, i * 10};
        ASSERT(!n00b_dl_relation_insert(&rel, t));
    }
    ASSERT_EQ((int64_t)rel.to_add_count, 100);

    n00b_dl_relation_free(&rel);
}

// ============================================================================
// Test runner
// ============================================================================

static void
run_tests(void)
{
    RUN_TEST(test_basic_insert);
    RUN_TEST(test_swap_lifecycle);
    RUN_TEST(test_column_index);
    RUN_TEST(test_tuple_hash);
    RUN_TEST(test_dedup_correctness);
}

TEST_MAIN()
