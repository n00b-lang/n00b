// AVL interval tree from Dr. Raid, ported to n00b stack/allocator.

#include "core/interval_tree.h"

n00b_interval_tree_t *
n00b_new_interval_tree(n00b_allocator_t *allocator)
{
    n00b_interval_tree_t *tree = n00b_alloc(n00b_interval_tree_t,
                                            .allocator = allocator);

    tree->stack     = n00b_stack_new(void *, allocator);
    tree->allocator = allocator;
    return tree;
}

int
n00b_init_interval_tree(n00b_interval_tree_t *tree, n00b_allocator_t *allocator)
{
    tree->root      = NULL;
    tree->stack     = n00b_stack_new(void *, allocator);
    tree->allocator = allocator;
    return 0;
}

static inline int64_t
_n00b_get_height(n00b_interval_node_t *node)
{
    if (NULL == node) {
        return 0;
    }
    return node->height;
}

static void
_n00b_update_node(n00b_interval_node_t *node)
{
    if (NULL == node) {
        return;
    }

    int64_t left_height  = _n00b_get_height(node->left);
    int64_t right_height = _n00b_get_height(node->right);
    if (left_height > right_height) {
        node->height = left_height + 1;
    }
    else {
        node->height = right_height + 1;
    }

    node->minimum = node->low;
    node->maximum = node->high;

    if (NULL != node->left) {
        n00b_interval_node_t *left = node->left;
        if (left->maximum > node->maximum) {
            node->maximum = left->maximum;
        }
        if (left->minimum < node->minimum) {
            node->minimum = left->minimum;
        }
    }

    if (NULL != node->right) {
        n00b_interval_node_t *right = node->right;
        if (right->maximum > node->maximum) {
            node->maximum = right->maximum;
        }
        if (right->minimum < node->minimum) {
            node->minimum = right->minimum;
        }
    }
}

static n00b_interval_node_t *
_n00b_avl_rotate_left(n00b_interval_node_t *node)
{
    n00b_interval_node_t *right_child = node->right;
    node->right                       = right_child->left;
    right_child->left                 = node;
    _n00b_update_node(node);
    _n00b_update_node(right_child);
    return right_child;
}

static n00b_interval_node_t *
_n00b_avl_rotate_right(n00b_interval_node_t *node)
{
    n00b_interval_node_t *left_child = node->left;
    node->left                       = left_child->right;
    left_child->right                = node;
    _n00b_update_node(node);
    _n00b_update_node(left_child);
    return left_child;
}

static inline int64_t
_n00b_avl_balance_factor(n00b_interval_node_t *node)
{
    if (NULL == node) {
        return 0;
    }
    return _n00b_get_height(node->left) - _n00b_get_height(node->right);
}

static n00b_interval_node_t *
_n00b_avl_balance_node(n00b_interval_node_t *node)
{
    _n00b_update_node(node);
    int64_t balance_factor = _n00b_avl_balance_factor(node);
    if (2 == balance_factor) {
        if (_n00b_avl_balance_factor(node->left) >= 0) {
            return _n00b_avl_rotate_right(node);
        }
        else {
            node->left = _n00b_avl_rotate_left(node->left);
            return _n00b_avl_rotate_right(node);
        }
    }
    else if (-2 == balance_factor) {
        if (_n00b_avl_balance_factor(node->right) <= 0) {
            return _n00b_avl_rotate_left(node);
        }
        else {
            node->right = _n00b_avl_rotate_right(node->right);
            return _n00b_avl_rotate_left(node);
        }
    }
    return node;
}

// clang-format off
n00b_result_t(n00b_interval_node_t *)
n00b_interval_insert(n00b_interval_tree_t *tree,
                     uint64_t              low,
                     uint64_t              high,
                     void                 *data)
// clang-format on
{
    if (low > high) {
        return n00b_result_err(n00b_interval_node_t *, N00B_INTERVAL_ERR_INVALID);
    }

    n00b_interval_node_t *node = n00b_alloc(n00b_interval_node_t,
                                            .allocator = tree->allocator);

    node->low     = low;
    node->high    = high;
    node->minimum = low;
    node->maximum = high;
    node->height  = 1;
    node->data    = data;

    if (NULL == tree->root) {
        tree->root = node;
        return n00b_result_ok(n00b_interval_node_t *, node);
    }

    n00b_interval_node_t *current = tree->root;
    n00b_stack_clear(tree->stack);

    while (NULL != current) {
        n00b_stack_push(tree->stack, (void *)current);
        if (low < current->low) {
            if (NULL == current->left) {
                current->left = node;
                break;
            }
            current = current->left;
        }
        else {
            if (NULL == current->right) {
                current->right = node;
                break;
            }
            current = current->right;
        }
    }

    n00b_interval_node_t *inserted = node;
    n00b_interval_node_t *parent;
    size_t                count;
    for (count = n00b_stack_len(tree->stack); count > 0; count--) {
        parent = (n00b_interval_node_t *)n00b_stack_pop(tree->stack);
        if (parent->left == node) {
            parent->left = _n00b_avl_balance_node(node);
        }
        else {
            parent->right = _n00b_avl_balance_node(node);
        }
        node = parent;
    }

    tree->root = _n00b_avl_balance_node(node);
    return n00b_result_ok(n00b_interval_node_t *, inserted);
}

// clang-format off
n00b_result_t(uint64_t)
n00b_interval_max(n00b_interval_tree_t *tree)
// clang-format on
{
    if (NULL == tree->root) {
        return n00b_result_err(uint64_t, 1);
    }

    return n00b_result_ok(uint64_t, tree->root->maximum);
}

// clang-format off
n00b_result_t(uint64_t)
n00b_interval_min(n00b_interval_tree_t *tree)
// clang-format on
{
    if (NULL == tree->root) {
        return n00b_result_err(uint64_t, 1);
    }

    return n00b_result_ok(uint64_t, tree->root->minimum);
}

// clang-format off
n00b_result_t(n00b_interval_node_t *)
n00b_interval_search_any(n00b_interval_tree_t *tree,
                         uint64_t              low,
                         uint64_t              high)
// clang-format on
{
    if (low > high) {
        return n00b_result_err(n00b_interval_node_t *, N00B_INTERVAL_ERR_INVALID);
    }

    if (NULL == tree->root) {
        return n00b_result_ok(n00b_interval_node_t *, NULL);
    }

    n00b_stack_clear(tree->stack);
    n00b_stack_push(tree->stack, tree->root);

    while (0 != n00b_stack_len(tree->stack)) {
        n00b_interval_node_t *node = n00b_stack_pop(tree->stack);
        if (node->low < high && low < node->high) {
            return n00b_result_ok(n00b_interval_node_t *, node);
        }

        n00b_interval_node_t *left = node->left;
        if (NULL != left && left->maximum > low && left->minimum < high) {
            n00b_stack_push(tree->stack, left);
        }

        n00b_interval_node_t *right = node->right;
        if (NULL != right && right->maximum > low && right->minimum < high) {
            n00b_stack_push(tree->stack, right);
        }
    }

    return n00b_result_ok(n00b_interval_node_t *, NULL);
}

// clang-format off
n00b_result_t(n00b_interval_node_t *)
n00b_interval_next_low(n00b_interval_tree_t *tree,
                       uint64_t              point)
// clang-format on
{
    if (NULL == tree->root) {
        return n00b_result_ok(n00b_interval_node_t *, NULL);
    }

    n00b_interval_node_t *node = tree->root;
    n00b_interval_node_t *prev = NULL;

    while (NULL != node) {
        if (point == node->low) {
            return n00b_result_ok(n00b_interval_node_t *, node);
        }

        if (point < node->low) {
            if (NULL != node->left) {
                prev = node;
                node = node->left;
                continue;
            }

            return n00b_result_ok(n00b_interval_node_t *, node);
        }

        if (NULL == node->right) {
            return n00b_result_ok(n00b_interval_node_t *, prev);
        }

        node = node->right;
    }

    return n00b_result_ok(n00b_interval_node_t *, NULL);
}

static void
_n00b_record_if_match(uint64_t              low,
                      uint64_t              high,
                      n00b_interval_node_t *node,
                      n00b_stack_t(void *)  *intersections)
{
    if (node->low < high && low < node->high) {
        n00b_stack_push(*intersections, (void *)node);
    }
}

// clang-format off
n00b_result_t(int)
n00b_interval_search_ordered(n00b_interval_tree_t *tree,
                             uint64_t              low,
                             uint64_t              high,
                             n00b_stack_t(void *)  *intersections)
// clang-format on
{
    if (low > high) {
        return n00b_result_err(int, N00B_INTERVAL_ERR_INVALID);
    }

    n00b_interval_node_t *node = tree->root;
    if (NULL == node) {
        return n00b_result_ok(int, 0);
    }

    n00b_stack_clear(tree->stack);

    int searching = 1;
    while (searching) {
        if (node->maximum > low && node->minimum < high) {
            if (NULL != node->left) {
                n00b_stack_push(tree->stack, node);
                node = node->left;
                continue;
            }

            _n00b_record_if_match(low, high, node, intersections);

            if (NULL != node->right) {
                node = node->right;
                continue;
            }
        }

        while ((searching = !!n00b_stack_len(tree->stack))) {
            node = n00b_stack_pop(tree->stack);
            _n00b_record_if_match(low, high, node, intersections);

            if (NULL != node->right) {
                node = node->right;
                break;
            }
        }
    }

    return n00b_result_ok(int, 0);
}

// clang-format off
n00b_result_t(int)
n00b_interval_delete(n00b_interval_tree_t *tree, n00b_interval_node_t *target)
// clang-format on
{
    if (NULL == tree || NULL == target || NULL == tree->root) {
        return n00b_result_err(int, N00B_INTERVAL_ERR_NOT_FOUND);
    }

    n00b_stack_clear(tree->stack);

    // Walk the BST to find the target by pointer identity.
    // Equal keys can end up on either side after rotations, so when
    // target->low == current->low we tag the stack entry (bit 0) and
    // try right first; on a dead-end we backtrack to the tag and try left.
    n00b_interval_node_t *current = tree->root;

    while (current != target) {
        if (NULL == current) {
            int retrying = 0;
            while (n00b_stack_len(tree->stack) > 0) {
                void *top = n00b_stack_pop(tree->stack);
                if ((uintptr_t)top & 1) {
                    current = (n00b_interval_node_t *)((uintptr_t)top & ~(uintptr_t)1);
                    n00b_stack_push(tree->stack, current);
                    current  = current->left;
                    retrying = 1;
                    break;
                }
            }
            if (!retrying) {
                return n00b_result_err(int, N00B_INTERVAL_ERR_NOT_FOUND);
            }
            continue;
        }

        if (target->low < current->low) {
            n00b_stack_push(tree->stack, current);
            current = current->left;
        }
        else if (target->low > current->low) {
            n00b_stack_push(tree->stack, current);
            current = current->right;
        }
        else {
            // equal key: tag and try right first
            n00b_stack_push(tree->stack, (void *)((uintptr_t)current | 1));
            current = current->right;
        }
    }

    // current == target.  Determine the node to physically remove.
    n00b_interval_node_t *to_remove = target;

    if (NULL != target->left && NULL != target->right) {
        // Two children: find in-order successor (leftmost in right subtree).
        n00b_stack_push(tree->stack, target);
        n00b_interval_node_t *successor = target->right;
        while (NULL != successor->left) {
            n00b_stack_push(tree->stack, successor);
            successor = successor->left;
        }

        // Detach successor's right child (its only possible child).
        n00b_interval_node_t *succ_child = successor->right;

        // Rebalance from successor's parent up to (but not including) target.
        n00b_interval_node_t *subtree  = succ_child;
        n00b_interval_node_t *old_node = successor;
        n00b_interval_node_t *parent;

        while (n00b_stack_len(tree->stack) > 0) {
            parent = (n00b_interval_node_t *)((uintptr_t)n00b_stack_pop(tree->stack)
                                              & ~(uintptr_t)1);
            if (parent == target) {
                n00b_stack_push(tree->stack, target);
                break;
            }
            if (parent->left == old_node) {
                parent->left = subtree;
            }
            else {
                parent->right = subtree;
            }
            old_node = parent;
            subtree  = _n00b_avl_balance_node(parent);
        }

        // Place successor into target's position.
        successor->left  = target->left;
        successor->right = (old_node == successor) ? succ_child : subtree;
        _n00b_update_node(successor);

        subtree  = _n00b_avl_balance_node(successor);
        old_node = target;

        // Pop target itself off the stack.
        (void)n00b_stack_pop(tree->stack);

        while (n00b_stack_len(tree->stack) > 0) {
            parent = (n00b_interval_node_t *)((uintptr_t)n00b_stack_pop(tree->stack)
                                              & ~(uintptr_t)1);
            if (parent->left == old_node) {
                parent->left = subtree;
            }
            else {
                parent->right = subtree;
            }
            old_node = parent;
            subtree  = _n00b_avl_balance_node(parent);
        }

        tree->root = subtree;
        return n00b_result_ok(int, 0);
    }

    // Zero or one child: simple removal.
    n00b_interval_node_t *child;
    if (NULL != to_remove->left) {
        child = to_remove->left;
    }
    else {
        child = to_remove->right;
    }

    n00b_interval_node_t *subtree  = child;
    n00b_interval_node_t *old_node = to_remove;

    while (n00b_stack_len(tree->stack) > 0) {
        n00b_interval_node_t *parent;
        parent = (n00b_interval_node_t *)((uintptr_t)n00b_stack_pop(tree->stack)
                                          & ~(uintptr_t)1);
        if (parent->left == old_node) {
            parent->left = subtree;
        }
        else {
            parent->right = subtree;
        }
        old_node = parent;
        subtree  = _n00b_avl_balance_node(parent);
    }

    tree->root = subtree;
    return n00b_result_ok(int, 0);
}

// clang-format off
n00b_result_t(int)
n00b_interval_search(n00b_interval_tree_t *tree,
                     uint64_t              low,
                     uint64_t              high,
                     n00b_stack_t(void *)  *intersections)
// clang-format on
{
    if (low > high) {
        return n00b_result_err(int, N00B_INTERVAL_ERR_INVALID);
    }

    if (NULL == tree->root) {
        return n00b_result_ok(int, 0);
    }

    n00b_stack_clear(tree->stack);
    n00b_stack_push(tree->stack, tree->root);

    while (0 != n00b_stack_len(tree->stack)) {
        n00b_interval_node_t *node = n00b_stack_pop(tree->stack);

        _n00b_record_if_match(low, high, node, intersections);

        n00b_interval_node_t *left = node->left;
        if (NULL != left && left->maximum > low && left->minimum < high) {
            n00b_stack_push(tree->stack, left);
        }

        n00b_interval_node_t *right = node->right;
        if (NULL != right && right->maximum > low && right->minimum < high) {
            n00b_stack_push(tree->stack, right);
        }
    }

    return n00b_result_ok(int, 0);
}
