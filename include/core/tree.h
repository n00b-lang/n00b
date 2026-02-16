/**
 * @file tree.h
 * @brief Type-safe n-ary tree — pure macros, no IMPL required.
 *
 * @c base_tree_t(N, L) represents an n-ary tree where internal nodes have
 * type @p N and leaves have type @p L. Children are stored in a dynamic array.
 *
 * Usage:
 * @code
 *     base_tree_t(int, char *) *root = base_tree_node(int, char *, 42);
 *     base_tree_add_leaf(root, int, char *, "hello");
 *     base_tree_add_leaf(root, int, char *, "world");
 * @endcode
 */
#pragma once

#include "generic.h"
#include "alloc.h"
#include <stdbool.h>
#include <string.h>

/** @brief Default initial capacity for a node's children array. */
#ifndef BASE_TREE_INITIAL_CAPACITY
#define BASE_TREE_INITIAL_CAPACITY 4
#endif

// ============================================================================
// Type definition
// ============================================================================

/**
 * @brief Declare + define a tree type with node type @p N and leaf type @p L.
 *        Use in variable declarations.
 *
 * @param N  Internal node value type.
 * @param L  Leaf value type.
 */
#define base_tree_t(N, L)                                                      \
    struct BASE_TREE_NAME(N, L) {                                              \
        bool is_leaf;                                                          \
        union {                                                                \
            struct {                                                           \
                N                              value;                          \
                struct BASE_TREE_NAME(N, L)  **children;                       \
                size_t                         num_children;                    \
                size_t                         capacity;                        \
            } node;                                                            \
            L leaf;                                                            \
        };                                                                     \
    }

/**
 * @brief Tag-only reference (no body) — use after the type has already been
 *        defined by @c base_tree_t in the same scope.
 */
#define base_tree_tag(N, L) struct BASE_TREE_NAME(N, L)

// ============================================================================
// Construction
// ============================================================================

/**
 * @brief Create a new internal node with value @p val.
 * @param N    Node type.
 * @param L    Leaf type.
 * @param val  Value for the internal node.
 * @return Pointer to the new node, or @c nullptr on failure.
 */
#define base_tree_node(N, L, val)                                              \
    ({                                                                         \
        base_tree_tag(N, L) *_bt =                                             \
            base_alloc(sizeof(base_tree_tag(N, L)));                           \
        if (_bt) {                                                             \
            _bt->is_leaf            = false;                                   \
            _bt->node.value         = (val);                                   \
            _bt->node.children      = base_alloc(                              \
                BASE_TREE_INITIAL_CAPACITY                                     \
                * sizeof(base_tree_tag(N, L) *));                              \
            _bt->node.num_children  = 0;                                       \
            _bt->node.capacity      = BASE_TREE_INITIAL_CAPACITY;             \
        }                                                                      \
        _bt;                                                                   \
    })

/**
 * @brief Create a new leaf with value @p val.
 * @param N    Node type.
 * @param L    Leaf type.
 * @param val  Value for the leaf.
 * @return Pointer to the new leaf, or @c nullptr on failure.
 */
#define base_tree_leaf(N, L, val)                                              \
    ({                                                                         \
        base_tree_tag(N, L) *_bt =                                             \
            base_alloc(sizeof(base_tree_tag(N, L)));                           \
        if (_bt) {                                                             \
            _bt->is_leaf = true;                                               \
            _bt->leaf    = (val);                                              \
        }                                                                      \
        _bt;                                                                   \
    })

// ============================================================================
// Child addition
// ============================================================================

/**
 * @brief Add a pre-created child to the end of a parent's children.
 *
 * Uses @c auto to infer types — no type parameters needed.
 *
 * @param parent  Parent node (must not be a leaf).
 * @param child   Child node or leaf to add.
 * @return @c true on success, @c false if parent is a leaf or null.
 */
#define base_tree_add_child(parent, child)                                     \
    ({                                                                         \
        auto _p  = (parent);                                                   \
        auto _c  = (child);                                                    \
        bool _ok = false;                                                      \
        if (_p && !_p->is_leaf && _c) {                                        \
            if (_p->node.num_children >= _p->node.capacity) {                  \
                size_t _nc = _p->node.capacity                                 \
                                 ? _p->node.capacity * 2                       \
                                 : BASE_TREE_INITIAL_CAPACITY;                 \
                _p->node.children = base_realloc(                              \
                    _p->node.children, _nc * sizeof(*_p->node.children));      \
                _p->node.capacity = _nc;                                       \
            }                                                                  \
            _p->node.children[_p->node.num_children++] = _c;                   \
            _ok = true;                                                        \
        }                                                                      \
        _ok;                                                                   \
    })

/**
 * @brief Create a new internal node and add it as a child.
 * @param parent  Parent node.
 * @param N       Node type.
 * @param L       Leaf type.
 * @param val     Value for the new child node.
 * @return Pointer to the new child, or @c nullptr on failure.
 */
#define base_tree_add_node(parent, N, L, val)                                  \
    ({                                                                         \
        base_tree_tag(N, L) *_an = base_tree_node(N, L, val);                 \
        if (_an && !base_tree_add_child(parent, _an)) {                        \
            base_dealloc(_an->node.children);                                  \
            base_dealloc(_an);                                                 \
            _an = nullptr;                                                     \
        }                                                                      \
        _an;                                                                   \
    })

/**
 * @brief Create a new leaf and add it as a child.
 * @param parent  Parent node.
 * @param N       Node type.
 * @param L       Leaf type.
 * @param val     Value for the new leaf.
 * @return Pointer to the new leaf, or @c nullptr on failure.
 */
#define base_tree_add_leaf(parent, N, L, val)                                  \
    ({                                                                         \
        base_tree_tag(N, L) *_al = base_tree_leaf(N, L, val);                 \
        if (_al && !base_tree_add_child(parent, _al)) {                        \
            base_dealloc(_al);                                                 \
            _al = nullptr;                                                     \
        }                                                                      \
        _al;                                                                   \
    })

// ============================================================================
// Child mutation
// ============================================================================

/**
 * @brief Replace the child at index @p i.
 *
 * Does not free the old child — the caller is responsible for it.
 *
 * @param t      Pointer to the parent node.
 * @param i      Zero-based child index.
 * @param child  New child to place at index @p i.
 * @return @c true on success, @c false if @p t is null/leaf or index is out of bounds.
 */
#define base_tree_set_child(t, i, child)                                       \
    ({                                                                         \
        auto _sc_p = (t);                                                      \
        size_t _sc_i = (i);                                                    \
        bool _sc_ok = false;                                                   \
        if (_sc_p && !_sc_p->is_leaf && _sc_i < _sc_p->node.num_children) {    \
            _sc_p->node.children[_sc_i] = (child);                             \
            _sc_ok = true;                                                     \
        }                                                                      \
        _sc_ok;                                                                \
    })

/**
 * @brief Remove the child at index @p i, shifting remaining children left.
 *
 * Does not free the removed child — the caller owns it.
 *
 * @param t  Pointer to the parent node.
 * @param i  Zero-based child index.
 * @return Pointer to the removed child, or @c nullptr if invalid.
 */
#define base_tree_remove_child(t, i)                                           \
    ({                                                                         \
        auto _rc_p = (t);                                                      \
        size_t _rc_i = (i);                                                    \
        typeof(_rc_p) _rc_removed = nullptr;                                   \
        if (_rc_p && !_rc_p->is_leaf                                           \
            && _rc_i < _rc_p->node.num_children) {                             \
            _rc_removed = _rc_p->node.children[_rc_i];                         \
            size_t _rc_rem = _rc_p->node.num_children - _rc_i - 1;            \
            if (_rc_rem > 0) {                                                 \
                memmove(&_rc_p->node.children[_rc_i],                          \
                        &_rc_p->node.children[_rc_i + 1],                      \
                        _rc_rem * sizeof(*_rc_p->node.children));              \
            }                                                                  \
            _rc_p->node.num_children--;                                        \
        }                                                                      \
        _rc_removed;                                                           \
    })

/**
 * @brief Insert a child at index @p i, shifting existing children right.
 *
 * Grows the children array if needed.
 *
 * @param t      Pointer to the parent node.
 * @param i      Insertion index (0 .. num_children).
 * @param child  Child to insert.
 * @return @c true on success.
 */
#define base_tree_insert_child_at(t, i, child)                                 \
    ({                                                                         \
        auto _ic_p = (t);                                                      \
        auto _ic_c = (child);                                                  \
        size_t _ic_i = (i);                                                    \
        bool _ic_ok = false;                                                   \
        if (_ic_p && !_ic_p->is_leaf && _ic_c                                  \
            && _ic_i <= _ic_p->node.num_children) {                            \
            if (_ic_p->node.num_children >= _ic_p->node.capacity) {            \
                size_t _ic_nc = _ic_p->node.capacity                           \
                                    ? _ic_p->node.capacity * 2                 \
                                    : BASE_TREE_INITIAL_CAPACITY;              \
                _ic_p->node.children = base_realloc(                           \
                    _ic_p->node.children,                                      \
                    _ic_nc * sizeof(*_ic_p->node.children));                   \
                _ic_p->node.capacity = _ic_nc;                                 \
            }                                                                  \
            size_t _ic_shift = _ic_p->node.num_children - _ic_i;              \
            if (_ic_shift > 0) {                                               \
                memmove(&_ic_p->node.children[_ic_i + 1],                      \
                        &_ic_p->node.children[_ic_i],                          \
                        _ic_shift * sizeof(*_ic_p->node.children));            \
            }                                                                  \
            _ic_p->node.children[_ic_i] = _ic_c;                              \
            _ic_p->node.num_children++;                                        \
            _ic_ok = true;                                                     \
        }                                                                      \
        _ic_ok;                                                                \
    })

/**
 * @brief Replace the entire children array wholesale.
 *
 * Does not free the old children array or any child nodes.
 *
 * @param t      Pointer to the parent node.
 * @param arr    New children array (the tree takes ownership).
 * @param count  Number of elements in @p arr.
 */
#define base_tree_replace_children(t, arr, count)                              \
    do {                                                                       \
        auto _rpl_p = (t);                                                     \
        if (_rpl_p && !_rpl_p->is_leaf) {                                      \
            _rpl_p->node.children     = (arr);                                 \
            _rpl_p->node.num_children = (count);                               \
            _rpl_p->node.capacity     = (count);                               \
        }                                                                      \
    } while (0)

// ============================================================================
// Accessors
// ============================================================================

/**
 * @brief Check if a tree node is a leaf.
 * @param t  Pointer to a tree node.
 * @return @c true if @p t is a leaf.
 */
#define base_tree_is_leaf(t) ((t)->is_leaf)

/**
 * @brief Check if a tree node is an internal node.
 * @param t  Pointer to a tree node.
 * @return @c true if @p t is an internal node.
 */
#define base_tree_is_node(t) (!(t)->is_leaf)

/**
 * @brief Get the value of a leaf node.
 * @param t  Pointer to a leaf node.
 * @return The leaf value.
 */
#define base_tree_leaf_value(t) ((t)->leaf)

/**
 * @brief Get the value of an internal node.
 * @param t  Pointer to an internal node.
 * @return The node value.
 */
#define base_tree_node_value(t) ((t)->node.value)

/**
 * @brief Get the number of children of a node.
 * @param t  Pointer to a tree node.
 * @return Number of children (0 for leaves).
 */
#define base_tree_num_children(t)                                              \
    ((t)->is_leaf ? 0 : (t)->node.num_children)

/**
 * @brief Get the child at index @p i.
 * @param t  Pointer to an internal node.
 * @param i  Zero-based child index.
 * @return Pointer to the child node.
 */
#define base_tree_child(t, i) ((t)->node.children[i])

/**
 * @brief Iterate over all children of an internal node.
 * @param t    Pointer to an internal node.
 * @param var  Name of the loop variable (pointer to each child).
 */
#define base_tree_foreach_child(t, var)                                        \
    for (size_t _i = 0, _n = base_tree_num_children(t); _i < _n; ++_i)        \
        for (typeof((t)->node.children[0]) var = (t)->node.children[_i],       \
                                           *_once = (void *)1;                 \
             _once; _once = nullptr)

/**
 * @brief Free a single tree node (does not recurse into children).
 *
 * If @p t is an internal node, frees the children array.
 * Does not free children themselves — the caller must walk the tree.
 *
 * @param t  Pointer to the tree node to free.
 */
#define base_tree_free_node(t)                                                 \
    do {                                                                       \
        if ((t) && !(t)->is_leaf) {                                            \
            base_dealloc((t)->node.children);                                  \
        }                                                                      \
        base_dealloc(t);                                                       \
    } while (0)
