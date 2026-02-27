#pragma once
/**
 * @file tree.h
 * @brief Type-safe n-ary tree -- pure macros (standalone extraction).
 *
 * Near-direct copy from the n00b original. n00b_tree_t(N, L) is a
 * tagged union tree node (internal nodes vs leaves). Children are
 * stored in a dynamically-grown pointer array.
 *
 * Usage:
 *     n00b_tree_decl(int, char *);
 *     n00b_tree_t(int, char *) *root = n00b_tree_node(int, char *, 42);
 *     n00b_tree_add_leaf(root, int, char *, "hello");
 */

#include "n00b.h"
#include "core/alloc.h"
#include <string.h>

#ifndef N00B_TREE_INITIAL_CAPACITY
#define N00B_TREE_INITIAL_CAPACITY 4
#endif

// Tree type names are formed by pasting n00b_tree_<N>___<L>.
// N and L must be single preprocessor tokens (no spaces, no *).
// For pointer types, use a typedef first: typedef char* cstr;
#define n00b_tree_tid(N, L) typeid(n00b_tree, N, __, L)

// ============================================================================
// Type definition
// ============================================================================

#define n00b_tree_decl(N, L)                                                   \
    struct n00b_tree_tid(N, L) {                                               \
        bool is_leaf;                                                          \
        union {                                                                \
            struct {                                                           \
                N value;                                                       \
                struct n00b_tree_tid(N, L) * *children;                        \
                size_t num_children;                                           \
                size_t capacity;                                               \
            } node;                                                            \
            L leaf;                                                            \
        };                                                                     \
    }

#define n00b_tree_t(N, L) struct n00b_tree_tid(N, L)

// ============================================================================
// Construction
// ============================================================================

#define n00b_tree_node(N, L, val, ...)                                         \
    ({                                                                         \
        n00b_tree_t(N, L) *_bt = n00b_alloc(n00b_tree_t(N, L));               \
        if (_bt) {                                                             \
            _bt->is_leaf    = false;                                           \
            _bt->node.value = (val);                                           \
            _bt->node.children =                                               \
                n00b_alloc_array(n00b_tree_t(N, L) *,                          \
                                N00B_TREE_INITIAL_CAPACITY);                   \
            _bt->node.num_children = 0;                                        \
            _bt->node.capacity     = N00B_TREE_INITIAL_CAPACITY;               \
        }                                                                      \
        _bt;                                                                   \
    })

#define n00b_tree_leaf(N, L, val)                                              \
    ({                                                                         \
        n00b_tree_t(N, L) *_bt = n00b_alloc(n00b_tree_t(N, L));               \
        if (_bt) {                                                             \
            _bt->is_leaf = true;                                               \
            _bt->leaf    = (val);                                              \
        }                                                                      \
        _bt;                                                                   \
    })

// ============================================================================
// Internal: grow children array
// ============================================================================

#define _n00b_tree_grow_children(_ptr, _new_cap)                               \
    do {                                                                       \
        size_t _old_n   = (_ptr)->node.num_children;                           \
        size_t _elem_sz = sizeof(*(_ptr)->node.children);                      \
        void  *_new_buf = n00b_alloc_size((_new_cap), _elem_sz);              \
        if (_old_n > 0) {                                                      \
            memcpy(_new_buf, (_ptr)->node.children, _old_n * _elem_sz);        \
        }                                                                      \
        n00b_free((_ptr)->node.children);                                      \
        (_ptr)->node.children = _new_buf;                                      \
        (_ptr)->node.capacity = (_new_cap);                                    \
    } while (0)

// ============================================================================
// Child addition
// ============================================================================

#define n00b_tree_add_child(parent, child)                                     \
    ({                                                                         \
        auto _p  = (parent);                                                   \
        auto _c  = (child);                                                    \
        bool _ok = false;                                                      \
        if (_p && !_p->is_leaf && _c) {                                        \
            if (_p->node.num_children >= _p->node.capacity) {                  \
                size_t _nc = _p->node.capacity                                 \
                    ? _p->node.capacity * 2                                    \
                    : N00B_TREE_INITIAL_CAPACITY;                              \
                _n00b_tree_grow_children(_p, _nc);                             \
            }                                                                  \
            _p->node.children[_p->node.num_children++] = _c;                   \
            _ok = true;                                                        \
        }                                                                      \
        _ok;                                                                   \
    })

#define n00b_tree_add_node(parent, N, L, val)                                  \
    ({                                                                         \
        n00b_tree_t(N, L) *_an = n00b_tree_node(N, L, val);                    \
        if (_an && !n00b_tree_add_child(parent, _an)) {                        \
            n00b_free(_an->node.children);                                     \
            n00b_free(_an);                                                    \
            _an = nullptr;                                                     \
        }                                                                      \
        _an;                                                                   \
    })

#define n00b_tree_add_leaf(parent, N, L, val)                                  \
    ({                                                                         \
        n00b_tree_t(N, L) *_al = n00b_tree_leaf(N, L, val);                    \
        if (_al && !n00b_tree_add_child(parent, _al)) {                        \
            n00b_free(_al);                                                    \
            _al = nullptr;                                                     \
        }                                                                      \
        _al;                                                                   \
    })

// ============================================================================
// Child mutation
// ============================================================================

#define n00b_tree_set_child(t, i, child)                                       \
    ({                                                                         \
        auto   _sc_p  = (t);                                                   \
        size_t _sc_i  = (i);                                                   \
        bool   _sc_ok = false;                                                 \
        if (_sc_p && !_sc_p->is_leaf && _sc_i < _sc_p->node.num_children) {    \
            _sc_p->node.children[_sc_i] = (child);                             \
            _sc_ok = true;                                                     \
        }                                                                      \
        _sc_ok;                                                                \
    })

#define n00b_tree_remove_child(t, i)                                           \
    ({                                                                         \
        auto          _rc_p       = (t);                                       \
        size_t        _rc_i       = (i);                                       \
        typeof(_rc_p) _rc_removed = nullptr;                                   \
        if (_rc_p && !_rc_p->is_leaf                                           \
            && _rc_i < _rc_p->node.num_children) {                             \
            _rc_removed    = _rc_p->node.children[_rc_i];                      \
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

#define n00b_tree_insert_child_at(t, i, child)                                 \
    ({                                                                         \
        auto   _ic_p  = (t);                                                   \
        auto   _ic_c  = (child);                                               \
        size_t _ic_i  = (i);                                                   \
        bool   _ic_ok = false;                                                 \
        if (_ic_p && !_ic_p->is_leaf && _ic_c                                  \
            && _ic_i <= _ic_p->node.num_children) {                            \
            if (_ic_p->node.num_children >= _ic_p->node.capacity) {            \
                size_t _ic_nc = _ic_p->node.capacity                           \
                    ? _ic_p->node.capacity * 2                                 \
                    : N00B_TREE_INITIAL_CAPACITY;                              \
                _n00b_tree_grow_children(_ic_p, _ic_nc);                       \
            }                                                                  \
            size_t _ic_shift = _ic_p->node.num_children - _ic_i;               \
            if (_ic_shift > 0) {                                               \
                memmove(&_ic_p->node.children[_ic_i + 1],                      \
                        &_ic_p->node.children[_ic_i],                          \
                        _ic_shift * sizeof(*_ic_p->node.children));            \
            }                                                                  \
            _ic_p->node.children[_ic_i] = _ic_c;                               \
            _ic_p->node.num_children++;                                        \
            _ic_ok = true;                                                     \
        }                                                                      \
        _ic_ok;                                                                \
    })

#define n00b_tree_replace_children(t, arr, count)                              \
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

#define n00b_tree_is_leaf(t)        ((t)->is_leaf)
#define n00b_tree_is_node(t)        (!(t)->is_leaf)
#define n00b_tree_leaf_value(t)     ((t)->leaf)
#define n00b_tree_node_value(t)     ((t)->node.value)
#define n00b_tree_num_children(t)   ((t)->is_leaf ? 0 : (t)->node.num_children)
#define n00b_tree_child(t, i)       ((t)->node.children[i])

#define n00b_tree_foreach_child(t, var)                                        \
    for (size_t _i = 0, _n = n00b_tree_num_children(t); _i < _n; ++_i)        \
        for (typeof((t)->node.children[0]) var = (t)->node.children[_i],       \
                 *_once = (void *)1;                                           \
             _once;                                                            \
             _once = nullptr)

// ============================================================================
// Destruction
// ============================================================================

#define n00b_tree_free_node(t)                                                 \
    do {                                                                       \
        if ((t) && !(t)->is_leaf) {                                            \
            n00b_free((t)->node.children);                                     \
        }                                                                      \
        n00b_free(t);                                                          \
    } while (0)
