#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/tree.h"

n00b_tree_decl(int, char *);

// ============================================================================
// 1. Node construction
// ============================================================================

static void
test_node_construction(void)
{
    n00b_tree_t(int, char *) *root = n00b_tree_node(int, char *, 42);
    assert(root != nullptr);
    assert(n00b_tree_is_node(root));
    assert(!n00b_tree_is_leaf(root));
    assert(n00b_tree_node_value(root) == 42);
    assert(n00b_tree_num_children(root) == 0);

    n00b_tree_free_node(root);
    printf("  [PASS] node construction\n");
}

// ============================================================================
// 2. Leaf construction
// ============================================================================

static void
test_leaf_construction(void)
{
    n00b_tree_t(int, char *) *leaf = n00b_tree_leaf(int, char *, "hello");
    assert(leaf != nullptr);
    assert(n00b_tree_is_leaf(leaf));
    assert(!n00b_tree_is_node(leaf));
    assert(strcmp(n00b_tree_leaf_value(leaf), "hello") == 0);
    assert(n00b_tree_num_children(leaf) == 0);

    n00b_tree_free_node(leaf);
    printf("  [PASS] leaf construction\n");
}

// ============================================================================
// 3. Add leaf children
// ============================================================================

static void
test_add_leaf(void)
{
    n00b_tree_t(int, char *) *root = n00b_tree_node(int, char *, 1);

    n00b_tree_t(int, char *) *l1 = n00b_tree_add_leaf(root, int, char *, "a");
    n00b_tree_t(int, char *) *l2 = n00b_tree_add_leaf(root, int, char *, "b");
    n00b_tree_t(int, char *) *l3 = n00b_tree_add_leaf(root, int, char *, "c");

    assert(l1 != nullptr);
    assert(l2 != nullptr);
    assert(l3 != nullptr);
    assert(n00b_tree_num_children(root) == 3);

    assert(n00b_tree_is_leaf(n00b_tree_child(root, 0)));
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 0)), "a") == 0);
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 1)), "b") == 0);
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 2)), "c") == 0);

    printf("  [PASS] add leaf\n");
}

// ============================================================================
// 4. Add node children
// ============================================================================

static void
test_add_node(void)
{
    n00b_tree_t(int, char *) *root = n00b_tree_node(int, char *, 0);

    n00b_tree_t(int, char *) *c1 = n00b_tree_add_node(root, int, char *, 10);
    n00b_tree_t(int, char *) *c2 = n00b_tree_add_node(root, int, char *, 20);

    assert(c1 != nullptr);
    assert(c2 != nullptr);
    assert(n00b_tree_num_children(root) == 2);
    assert(n00b_tree_is_node(n00b_tree_child(root, 0)));
    assert(n00b_tree_node_value(n00b_tree_child(root, 0)) == 10);
    assert(n00b_tree_node_value(n00b_tree_child(root, 1)) == 20);

    // Add leaves to child node
    (void)n00b_tree_add_leaf(c1, int, char *, "x");
    (void)n00b_tree_add_leaf(c1, int, char *, "y");
    assert(n00b_tree_num_children(c1) == 2);

    printf("  [PASS] add node\n");
}

// ============================================================================
// 5. Add child (pre-created)
// ============================================================================

static void
test_add_child(void)
{
    n00b_tree_t(int, char *) *root  = n00b_tree_node(int, char *, 0);
    n00b_tree_t(int, char *) *leaf  = n00b_tree_leaf(int, char *, "manual");
    n00b_tree_t(int, char *) *child = n00b_tree_node(int, char *, 5);

    assert(n00b_tree_add_child(root, leaf));
    assert(n00b_tree_add_child(root, child));
    assert(n00b_tree_num_children(root) == 2);

    // Can't add child to a leaf
    n00b_tree_t(int, char *) *extra = n00b_tree_leaf(int, char *, "nope");
    assert(!n00b_tree_add_child(leaf, extra));

    // Can't add null child
    assert(!n00b_tree_add_child(root, (n00b_tree_t(int, char *) *)nullptr));

    printf("  [PASS] add child\n");
}

// ============================================================================
// 6. Set child (replace)
// ============================================================================

static void
test_set_child(void)
{
    n00b_tree_t(int, char *) *root = n00b_tree_node(int, char *, 0);
    (void)n00b_tree_add_leaf(root, int, char *, "old");
    (void)n00b_tree_add_leaf(root, int, char *, "keep");

    n00b_tree_t(int, char *) *replacement = n00b_tree_leaf(int, char *, "new");
    assert(n00b_tree_set_child(root, 0, replacement));
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 0)), "new") == 0);
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 1)), "keep") == 0);

    // Out of bounds
    assert(!n00b_tree_set_child(root, 5, replacement));

    printf("  [PASS] set child\n");
}

// ============================================================================
// 7. Remove child
// ============================================================================

static void
test_remove_child(void)
{
    n00b_tree_t(int, char *) *root = n00b_tree_node(int, char *, 0);
    (void)n00b_tree_add_leaf(root, int, char *, "a");
    (void)n00b_tree_add_leaf(root, int, char *, "b");
    (void)n00b_tree_add_leaf(root, int, char *, "c");
    assert(n00b_tree_num_children(root) == 3);

    // Remove middle child
    n00b_tree_t(int, char *) *removed = n00b_tree_remove_child(root, 1);
    assert(removed != nullptr);
    assert(strcmp(n00b_tree_leaf_value(removed), "b") == 0);
    assert(n00b_tree_num_children(root) == 2);

    // Remaining: "a", "c"
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 0)), "a") == 0);
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 1)), "c") == 0);

    // Remove first
    removed = n00b_tree_remove_child(root, 0);
    assert(strcmp(n00b_tree_leaf_value(removed), "a") == 0);
    assert(n00b_tree_num_children(root) == 1);
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 0)), "c") == 0);

    // Remove last
    removed = n00b_tree_remove_child(root, 0);
    assert(strcmp(n00b_tree_leaf_value(removed), "c") == 0);
    assert(n00b_tree_num_children(root) == 0);

    // Remove from empty → null
    removed = n00b_tree_remove_child(root, 0);
    assert(removed == nullptr);

    printf("  [PASS] remove child\n");
}

// ============================================================================
// 8. Insert child at position
// ============================================================================

static void
test_insert_child_at(void)
{
    n00b_tree_t(int, char *) *root = n00b_tree_node(int, char *, 0);
    (void)n00b_tree_add_leaf(root, int, char *, "a");
    (void)n00b_tree_add_leaf(root, int, char *, "c");

    // Insert "b" at index 1 → [a, b, c]
    n00b_tree_t(int, char *) *b = n00b_tree_leaf(int, char *, "b");
    assert(n00b_tree_insert_child_at(root, 1, b));
    assert(n00b_tree_num_children(root) == 3);
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 0)), "a") == 0);
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 1)), "b") == 0);
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 2)), "c") == 0);

    // Insert at front
    n00b_tree_t(int, char *) *z = n00b_tree_leaf(int, char *, "z");
    assert(n00b_tree_insert_child_at(root, 0, z));
    assert(n00b_tree_num_children(root) == 4);
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 0)), "z") == 0);

    // Insert at end
    n00b_tree_t(int, char *) *end = n00b_tree_leaf(int, char *, "end");
    assert(n00b_tree_insert_child_at(root, n00b_tree_num_children(root), end));
    assert(n00b_tree_num_children(root) == 5);
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 4)), "end") == 0);

    printf("  [PASS] insert child at\n");
}

// ============================================================================
// 9. Replace children
// ============================================================================

static void
test_replace_children(void)
{
    n00b_tree_t(int, char *) *root = n00b_tree_node(int, char *, 0);
    (void)n00b_tree_add_leaf(root, int, char *, "old1");
    (void)n00b_tree_add_leaf(root, int, char *, "old2");
    assert(n00b_tree_num_children(root) == 2);

    // Create new children array
    n00b_tree_t(int, char *) *new1 = n00b_tree_leaf(int, char *, "new1");
    n00b_tree_t(int, char *) *new2 = n00b_tree_leaf(int, char *, "new2");
    n00b_tree_t(int, char *) *new3 = n00b_tree_leaf(int, char *, "new3");

    n00b_tree_t(int, char *) **arr
        = n00b_alloc_array(n00b_tree_t(int, char *) *, 3);
    arr[0] = new1;
    arr[1] = new2;
    arr[2] = new3;

    n00b_tree_replace_children(root, arr, 3);
    assert(n00b_tree_num_children(root) == 3);
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 0)), "new1") == 0);
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 1)), "new2") == 0);
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 2)), "new3") == 0);

    printf("  [PASS] replace children\n");
}

// ============================================================================
// 10. Foreach child
// ============================================================================

static void
test_foreach_child(void)
{
    n00b_tree_t(int, char *) *root = n00b_tree_node(int, char *, 0);
    (void)n00b_tree_add_node(root, int, char *, 10);
    (void)n00b_tree_add_node(root, int, char *, 20);
    (void)n00b_tree_add_node(root, int, char *, 30);

    int sum   = 0;
    int count = 0;

    n00b_tree_foreach_child(root, child)
    {
        sum += n00b_tree_node_value(child);
        count++;
    }

    assert(count == 3);
    assert(sum == 60);

    printf("  [PASS] foreach child\n");
}

// ============================================================================
// 11. Deep tree
// ============================================================================

static void
test_deep_tree(void)
{
    // Build: root(0) -> child(1) -> child(2) -> ... -> leaf("bottom")
    n00b_tree_t(int, char *) *root = n00b_tree_node(int, char *, 0);
    n00b_tree_t(int, char *) *cur  = root;

    for (int i = 1; i < 10; i++) {
        cur = n00b_tree_add_node(cur, int, char *, i);
        assert(cur != nullptr);
    }
    (void)n00b_tree_add_leaf(cur, int, char *, "bottom");

    // Walk down to the bottom
    n00b_tree_t(int, char *) *walk = root;
    for (int i = 0; i < 9; i++) {
        assert(n00b_tree_is_node(walk));
        assert(n00b_tree_node_value(walk) == i);
        assert(n00b_tree_num_children(walk) == 1);
        walk = n00b_tree_child(walk, 0);
    }
    // walk is now the deepest internal node (value 9)
    assert(n00b_tree_node_value(walk) == 9);
    assert(n00b_tree_num_children(walk) == 1);

    // Its child is the leaf
    n00b_tree_t(int, char *) *bottom = n00b_tree_child(walk, 0);
    assert(n00b_tree_is_leaf(bottom));
    assert(strcmp(n00b_tree_leaf_value(bottom), "bottom") == 0);

    printf("  [PASS] deep tree\n");
}

// ============================================================================
// 12. Dynamic growth (exceed initial capacity)
// ============================================================================

static void
test_dynamic_growth(void)
{
    n00b_tree_t(int, char *) *root = n00b_tree_node(int, char *, 0);

    // Add more children than N00B_TREE_INITIAL_CAPACITY (4)
    for (int i = 0; i < 20; i++) {
        n00b_tree_t(int, char *) *child = n00b_tree_add_node(root, int, char *, i);
        assert(child != nullptr);
    }

    assert(n00b_tree_num_children(root) == 20);

    // Verify all children
    for (int i = 0; i < 20; i++) {
        assert(n00b_tree_node_value(n00b_tree_child(root, i)) == i);
    }

    printf("  [PASS] dynamic growth\n");
}

// ============================================================================
// 13. Wide tree with mixed nodes and leaves
// ============================================================================

static void
test_mixed_children(void)
{
    n00b_tree_t(int, char *) *root = n00b_tree_node(int, char *, 0);

    (void)n00b_tree_add_node(root, int, char *, 1);
    (void)n00b_tree_add_leaf(root, int, char *, "leaf1");
    (void)n00b_tree_add_node(root, int, char *, 2);
    (void)n00b_tree_add_leaf(root, int, char *, "leaf2");

    assert(n00b_tree_num_children(root) == 4);

    assert(n00b_tree_is_node(n00b_tree_child(root, 0)));
    assert(n00b_tree_is_leaf(n00b_tree_child(root, 1)));
    assert(n00b_tree_is_node(n00b_tree_child(root, 2)));
    assert(n00b_tree_is_leaf(n00b_tree_child(root, 3)));

    assert(n00b_tree_node_value(n00b_tree_child(root, 0)) == 1);
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 1)), "leaf1") == 0);
    assert(n00b_tree_node_value(n00b_tree_child(root, 2)) == 2);
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 3)), "leaf2") == 0);

    printf("  [PASS] mixed children\n");
}

// ============================================================================
// 14. Insert with growth
// ============================================================================

static void
test_insert_with_growth(void)
{
    n00b_tree_t(int, char *) *root = n00b_tree_node(int, char *, 0);

    // Fill to capacity
    for (int i = 0; i < N00B_TREE_INITIAL_CAPACITY; i++) {
        (void)n00b_tree_add_node(root, int, char *, i * 10);
    }
    assert((size_t)n00b_tree_num_children(root) == N00B_TREE_INITIAL_CAPACITY);

    // Insert at the beginning — forces growth + shift
    n00b_tree_t(int, char *) *front = n00b_tree_leaf(int, char *, "front");
    assert(n00b_tree_insert_child_at(root, 0, front));
    assert(n00b_tree_num_children(root) == N00B_TREE_INITIAL_CAPACITY + 1);
    assert(n00b_tree_is_leaf(n00b_tree_child(root, 0)));
    assert(strcmp(n00b_tree_leaf_value(n00b_tree_child(root, 0)), "front") == 0);

    // Original first child is now at index 1
    assert(n00b_tree_is_node(n00b_tree_child(root, 1)));
    assert(n00b_tree_node_value(n00b_tree_child(root, 1)) == 0);

    printf("  [PASS] insert with growth\n");
}

// ============================================================================
// 15. Free node (shallow)
// ============================================================================

static void
test_free_node(void)
{
    // Free a leaf
    n00b_tree_t(int, char *) *leaf = n00b_tree_leaf(int, char *, "disposable");
    n00b_tree_free_node(leaf);

    // Free an internal node (frees children array, not children themselves)
    n00b_tree_t(int, char *) *node = n00b_tree_node(int, char *, 99);
    (void)n00b_tree_add_leaf(node, int, char *, "child");
    // We only free the node itself — children leak (by design; GC handles it)
    n00b_tree_free_node(node);

    printf("  [PASS] free node\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running tree tests...\n");

    test_node_construction();
    test_leaf_construction();
    test_add_leaf();
    test_add_node();
    test_add_child();
    test_set_child();
    test_remove_child();
    test_insert_child_at();
    test_replace_children();
    test_foreach_child();
    test_deep_tree();
    test_dynamic_growth();
    test_mixed_children();
    test_insert_with_growth();
    test_free_node();

    printf("All tree tests passed.\n");
    n00b_shutdown();
    return 0;
}
