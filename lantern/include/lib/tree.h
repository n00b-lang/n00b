#pragma once

#include "core.h"
#include "lib/alloc.h"
#include <string.h>

#ifndef CORE_TREE_INITIAL_CAPACITY
#define CORE_TREE_INITIAL_CAPACITY 4
#endif

#define core_tree_tid(N, L) typeid(core_tree, N, __, L)
#define _core_tree_helper(name, N, L) typeid(name, N, __, L)

#define core_tree_decl(N, L)                                                   \
    struct core_tree_tid(N, L) {                                               \
        bool is_leaf;                                                          \
        struct core_tree_tid(N, L) *first_leaf;                                \
        struct core_tree_tid(N, L) **parents;                                  \
        size_t num_parents;                                                    \
        size_t parent_capacity;                                                \
        void (*refresh_first_leaf)(void *);                                    \
        union {                                                                \
            struct {                                                           \
                N value;                                                       \
                struct core_tree_tid(N, L) * *children;                        \
                size_t num_children;                                           \
                size_t capacity;                                               \
            } node;                                                            \
            L leaf;                                                            \
        };                                                                     \
    };                                                                         \
    static inline void _core_tree_helper(_core_tree_refresh_up, N, L)(         \
        void *opaque)                                                          \
    {                                                                          \
        core_tree_t(N, L) *node = opaque;                                      \
        if (!node) return;                                                     \
        core_tree_t(N, L) *previous = node->first_leaf;                        \
        core_tree_t(N, L) *first = node->is_leaf ? node : nullptr;             \
        if (!node->is_leaf) {                                                  \
            for (size_t i = 0; i < node->node.num_children; i++) {             \
                core_tree_t(N, L) *child = node->node.children[i];             \
                if (child && child->first_leaf) {                              \
                    first = child->first_leaf;                                 \
                    break;                                                     \
                }                                                              \
            }                                                                  \
        }                                                                      \
        node->first_leaf = first;                                              \
        if (previous == first) return;                                         \
        for (size_t i = 0; i < node->num_parents; i++)                         \
            node->parents[i]->refresh_first_leaf(node->parents[i]);            \
    }

#define core_tree_t(N, L) struct core_tree_tid(N, L)

#define core_tree_node(N, L, val, ...)                                         \
    ({                                                                         \
        core_tree_t(N, L) *_bt = core_alloc(core_tree_t(N, L));               \
        if (_bt) {                                                             \
            _bt->is_leaf    = false;                                           \
            _bt->node.value = (val);                                           \
            _bt->first_leaf = nullptr;                                         \
            _bt->parents = nullptr;                                            \
            _bt->num_parents = 0;                                              \
            _bt->parent_capacity = 0;                                          \
            _bt->refresh_first_leaf =                                          \
                _core_tree_helper(_core_tree_refresh_up, N, L);                \
            _bt->node.children =                                               \
                core_alloc_array(core_tree_t(N, L) *,                          \
                                CORE_TREE_INITIAL_CAPACITY);                   \
            _bt->node.num_children = 0;                                        \
            _bt->node.capacity     = CORE_TREE_INITIAL_CAPACITY;               \
        }                                                                      \
        _bt;                                                                   \
    })

#define core_tree_leaf(N, L, val)                                              \
    ({                                                                         \
        core_tree_t(N, L) *_bt = core_alloc(core_tree_t(N, L));               \
        if (_bt) {                                                             \
            _bt->is_leaf = true;                                               \
            _bt->leaf    = (val);                                              \
            _bt->first_leaf = _bt;                                             \
            _bt->parents = nullptr;                                            \
            _bt->num_parents = 0;                                              \
            _bt->parent_capacity = 0;                                          \
            _bt->refresh_first_leaf =                                          \
                _core_tree_helper(_core_tree_refresh_up, N, L);                \
        }                                                                      \
        _bt;                                                                   \
    })

#define _core_tree_grow_children(_ptr, _new_cap)                               \
    do {                                                                       \
        size_t _old_n   = (_ptr)->node.num_children;                           \
        size_t _elem_sz = sizeof(*(_ptr)->node.children);                      \
        void  *_new_buf = core_alloc_size((_new_cap), _elem_sz);              \
        if (_old_n > 0) {                                                      \
            memcpy(_new_buf, (_ptr)->node.children, _old_n * _elem_sz);        \
        }                                                                      \
        core_free((_ptr)->node.children);                                      \
        (_ptr)->node.children = _new_buf;                                      \
        (_ptr)->node.capacity = (_new_cap);                                    \
    } while (0)

#define _core_tree_add_parent(child, parent)                                   \
    ({                                                                         \
        auto _ap_c = (child);                                                  \
        auto _ap_p = (parent);                                                 \
        bool _ap_found = false;                                                \
        for (size_t _ap_i = 0; _ap_i < _ap_c->num_parents; _ap_i++)            \
            _ap_found |= _ap_c->parents[_ap_i] == _ap_p;                       \
        if (!_ap_found) {                                                      \
            if (_ap_c->num_parents == _ap_c->parent_capacity) {                \
                size_t _ap_cap = _ap_c->parent_capacity                        \
                    ? _ap_c->parent_capacity * 2 : 4;                          \
                _ap_c->parents = core_realloc(_ap_c->parents,                  \
                    _ap_cap * sizeof(*_ap_c->parents));                        \
                _ap_c->parent_capacity = _ap_cap;                              \
            }                                                                  \
            _ap_c->parents[_ap_c->num_parents++] = _ap_p;                      \
        }                                                                      \
        true;                                                                  \
    })

#define _core_tree_remove_parent(child, parent)                               \
    do {                                                                       \
        auto _rp_c = (child);                                                  \
        auto _rp_p = (parent);                                                 \
        for (size_t _rp_i = 0; _rp_i < _rp_c->num_parents; _rp_i++) {          \
            if (_rp_c->parents[_rp_i] != _rp_p) continue;                     \
            memmove(&_rp_c->parents[_rp_i], &_rp_c->parents[_rp_i + 1],        \
                (_rp_c->num_parents - _rp_i - 1) * sizeof(*_rp_c->parents));   \
            _rp_c->num_parents--;                                              \
            break;                                                             \
        }                                                                      \
    } while (0)

#define _core_tree_has_child(parent, child)                                   \
    ({                                                                         \
        auto _hc_p = (parent);                                                 \
        auto _hc_c = (child);                                                  \
        bool _hc_found = false;                                                \
        for (size_t _hc_i = 0; _hc_i < _hc_p->node.num_children; _hc_i++)      \
            _hc_found |= _hc_p->node.children[_hc_i] == _hc_c;                \
        _hc_found;                                                             \
    })

#define core_tree_add_child(parent, child)                                     \
    ({                                                                         \
        auto _p  = (parent);                                                   \
        auto _c  = (child);                                                    \
        bool _ok = false;                                                      \
        if (_p && !_p->is_leaf && _c) {                                        \
            if (_p->node.num_children >= _p->node.capacity) {                  \
                size_t _nc = _p->node.capacity                                 \
                    ? _p->node.capacity * 2                                    \
                    : CORE_TREE_INITIAL_CAPACITY;                              \
                _core_tree_grow_children(_p, _nc);                             \
            }                                                                  \
            _p->node.children[_p->node.num_children++] = _c;                   \
            _core_tree_add_parent(_c, _p);                                     \
            _p->refresh_first_leaf(_p);                                        \
            _ok = true;                                                        \
        }                                                                      \
        _ok;                                                                   \
    })

#define core_tree_add_node(parent, N, L, val)                                  \
    ({                                                                         \
        core_tree_t(N, L) *_an = core_tree_node(N, L, val);                    \
        if (_an && !core_tree_add_child(parent, _an)) {                        \
            core_free(_an->node.children);                                     \
            core_free(_an);                                                    \
            _an = nullptr;                                                     \
        }                                                                      \
        _an;                                                                   \
    })

#define core_tree_add_leaf(parent, N, L, val)                                  \
    ({                                                                         \
        core_tree_t(N, L) *_al = core_tree_leaf(N, L, val);                    \
        if (_al && !core_tree_add_child(parent, _al)) {                        \
            core_free(_al);                                                    \
            _al = nullptr;                                                     \
        }                                                                      \
        _al;                                                                   \
    })

#define core_tree_set_child(t, i, child)                                       \
    ({                                                                         \
        auto   _sc_p  = (t);                                                   \
        auto   _sc_c  = (child);                                               \
        size_t _sc_i  = (i);                                                   \
        auto   _sc_old = _sc_p && !_sc_p->is_leaf                              \
            && _sc_i < _sc_p->node.num_children                                \
                ? _sc_p->node.children[_sc_i] : nullptr;                      \
        bool   _sc_ok = false;                                                 \
        if (_sc_p && !_sc_p->is_leaf && _sc_i < _sc_p->node.num_children) {    \
            if (_sc_c == _sc_old || !_sc_c                                     \
                || _core_tree_add_parent(_sc_c, _sc_p)) {                      \
                _sc_p->node.children[_sc_i] = _sc_c;                           \
                if (_sc_old && !_core_tree_has_child(_sc_p, _sc_old))          \
                    _core_tree_remove_parent(_sc_old, _sc_p);                  \
                _sc_p->refresh_first_leaf(_sc_p);                              \
                _sc_ok = true;                                                 \
            }                                                                  \
        }                                                                      \
        _sc_ok;                                                                \
    })

#define core_tree_remove_child(t, i)                                           \
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
            if (_rc_removed                                                \
                && !_core_tree_has_child(_rc_p, _rc_removed))                 \
                _core_tree_remove_parent(_rc_removed, _rc_p);                  \
            _rc_p->refresh_first_leaf(_rc_p);                                  \
        }                                                                      \
        _rc_removed;                                                           \
    })

#define core_tree_insert_child_at(t, i, child)                                 \
    ({                                                                         \
        auto   _ic_p  = (t);                                                   \
        auto   _ic_c  = (child);                                               \
        size_t _ic_i  = (i);                                                   \
        bool   _ic_ok = false;                                                 \
        if (_ic_p && !_ic_p->is_leaf && _ic_c                                  \
            && _ic_i <= _ic_p->node.num_children) {                            \
            _core_tree_add_parent(_ic_c, _ic_p);                               \
            if (_ic_p->node.num_children >= _ic_p->node.capacity) {            \
                size_t _ic_nc = _ic_p->node.capacity                           \
                    ? _ic_p->node.capacity * 2                                 \
                    : CORE_TREE_INITIAL_CAPACITY;                              \
                _core_tree_grow_children(_ic_p, _ic_nc);                       \
            }                                                                  \
            size_t _ic_shift = _ic_p->node.num_children - _ic_i;               \
            if (_ic_shift > 0) {                                               \
                memmove(&_ic_p->node.children[_ic_i + 1],                      \
                        &_ic_p->node.children[_ic_i],                          \
                        _ic_shift * sizeof(*_ic_p->node.children));            \
            }                                                                  \
            _ic_p->node.children[_ic_i] = _ic_c;                               \
            _ic_p->node.num_children++;                                        \
            _ic_p->refresh_first_leaf(_ic_p);                                  \
            _ic_ok = true;                                                     \
        }                                                                      \
        _ic_ok;                                                                \
    })

#define core_tree_replace_children(t, arr, count)                              \
    do {                                                                       \
        auto _rpl_p = (t);                                                     \
        typeof(_rpl_p->node.children) _rpl_arr = (arr);                        \
        size_t _rpl_count = (count);                                           \
        if (_rpl_p && !_rpl_p->is_leaf) {                                      \
            for (size_t _rpl_i = 0; _rpl_i < _rpl_p->node.num_children;        \
                 _rpl_i++) {                                                   \
                auto _rpl_old = _rpl_p->node.children[_rpl_i];                 \
                bool _rpl_kept = false;                                        \
                for (size_t _rpl_j = 0; _rpl_j < _rpl_count; _rpl_j++)         \
                    _rpl_kept |= _rpl_arr[_rpl_j] == _rpl_old;                 \
                if (_rpl_old && !_rpl_kept)                                    \
                    _core_tree_remove_parent(_rpl_old, _rpl_p);                \
            }                                                                  \
            _rpl_p->node.children = _rpl_arr;                                  \
            _rpl_p->node.num_children = _rpl_count;                            \
            _rpl_p->node.capacity = _rpl_count;                                \
            for (size_t _rpl_i = 0; _rpl_i < _rpl_count; _rpl_i++)             \
                _core_tree_add_parent(_rpl_arr[_rpl_i], _rpl_p);               \
            _rpl_p->refresh_first_leaf(_rpl_p);                                \
        }                                                                      \
    } while (0)

#define core_tree_is_leaf(t)        ((t)->is_leaf)
#define core_tree_is_node(t)        (!(t)->is_leaf)
#define core_tree_leaf_value(t)     ((t)->leaf)
#define core_tree_node_value(t)     ((t)->node.value)
#define core_tree_num_children(t)   ((t)->is_leaf ? 0 : (t)->node.num_children)
#define core_tree_child(t, i)       ((t)->node.children[i])
#define core_tree_first_leaf_node(t) ((t)->first_leaf)

#define core_tree_foreach_child(t, var)                                        \
    for (size_t _i = 0, _n = core_tree_num_children(t); _i < _n; ++_i)        \
        for (typeof((t)->node.children[0]) var = (t)->node.children[_i],       \
                 *_once = (void *)1;                                           \
             _once;                                                            \
             _once = nullptr)

#define core_tree_free_node(t)                                                 \
    do {                                                                       \
        while ((t) && (t)->num_parents) {                                      \
            auto _fr_parent = (t)->parents[0];                                 \
            bool _fr_removed = false;                                          \
            if (_fr_parent && !_fr_parent->is_leaf) {                          \
                for (size_t _fr_i = 0; _fr_i < _fr_parent->node.num_children; \
                     _fr_i++) {                                                \
                    if (_fr_parent->node.children[_fr_i] != (t)) continue;     \
                    core_tree_remove_child(_fr_parent, _fr_i);                 \
                    _fr_removed = true;                                        \
                    break;                                                     \
                }                                                              \
            }                                                                  \
            if (!_fr_removed) _core_tree_remove_parent((t), _fr_parent);       \
        }                                                                      \
        if ((t) && !(t)->is_leaf) {                                            \
            for (size_t _fr_i = 0; _fr_i < (t)->node.num_children; _fr_i++) {  \
                auto _fr_child = (t)->node.children[_fr_i];                    \
                if (_fr_child) _core_tree_remove_parent(_fr_child, (t));       \
            }                                                                  \
            core_free((t)->node.children);                                     \
        }                                                                      \
        if (t) core_free((t)->parents);                                        \
        core_free(t);                                                          \
    } while (0)
