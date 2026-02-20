/**
 * @file llist.h
 * @brief Type-safe doubly linked list (optionally thread-safe).
 *
 * Provides `n00b_linked_list_t(T)` — an intrusive doubly linked list
 * with head/tail tracking.  Elements are nullable (wrapping in
 * `n00b_option_t` is not currently supported due to ncc limitations
 * with nested `typeid()` inside `typeof()`).
 *
 * When the list head's `lock` pointer is non-null, operations
 * acquire the appropriate rwlock.  When null, locking is a no-op.
 *
 * Usage:
 * @code
 *     n00b_linked_list_decl(int);
 *     n00b_linked_list_t(int) list = {};
 *     n00b_linked_list_append(&list, 42);
 *     auto node = n00b_linked_list_first(&list);
 *     int val   = n00b_linked_list_node_contents(node);
 * @endcode
 */
#pragma once

#include "n00b.h"
#include "core/data_lock.h"

// ============================================================================
// Type definition
// ============================================================================

#define n00b_linked_list_node_tid(T) typeid("link_node", T)
#define n00b_linked_list_tid(T)      typeid("linked_list", T)

#define n00b_linked_list_node_t(T) struct n00b_linked_list_node_tid(T)
#define n00b_linked_list_t(T)      struct n00b_linked_list_tid(T)

#define n00b_linked_list_node_decl(T)                                                          \
    n00b_linked_list_node_t(T)                                                                 \
    {                                                                                          \
        n00b_linked_list_node_t(T) * prev;                                                     \
        n00b_linked_list_node_t(T) * next;                                                     \
        T contents;                                                                            \
    }

#define n00b_linked_list_head_decl(T)                                                          \
    n00b_linked_list_t(T)                                                                      \
    {                                                                                          \
        n00b_linked_list_node_t(T) * head;                                                     \
        n00b_linked_list_node_t(T) * tail;                                                     \
        n00b_rwlock_t    *lock;                                                                \
        n00b_allocator_t *allocator;                                                           \
    }

#define n00b_linked_list_decl(T)                                                               \
    n00b_linked_list_node_decl(T);                                                             \
    n00b_linked_list_head_decl(T)

#define n00b_linked_list_zero(list)                                                            \
    {                                                                                          \
        list = (typeof(list)){};                                                               \
    }

#define n00b_linked_list_append(lptr, item)                                                    \
    {                                                                                          \
        auto                 _l = (lptr);                                                      \
        n00b_data_write_lock(_l->lock);                                                        \
        typeof(*(_l->head)) *nodep                                                             \
            = n00b_alloc(typeof(*(_l->head)), .allocator = _l->allocator);                     \
        nodep->contents = (item);                                                              \
        nodep->prev     = _l->tail;                                                            \
        nodep->next     = nullptr;                                                             \
        if (_l->tail) {                                                                        \
            _l->tail->next = nodep;                                                            \
        }                                                                                      \
        else {                                                                                 \
            _l->head = nodep;                                                                  \
        }                                                                                      \
        _l->tail = nodep;                                                                      \
        n00b_data_unlock(_l->lock);                                                            \
    }

#define n00b_linked_list_first(lptr)                                                           \
    ({                                                                                         \
        auto _l = (lptr);                                                                      \
        n00b_data_read_lock(_l->lock);                                                         \
        auto _r = _l->head;                                                                    \
        n00b_data_unlock(_l->lock);                                                            \
        _r;                                                                                    \
    })

#define n00b_linked_list_next(nptr)                                                            \
    ({                                                                                         \
        auto _n = (nptr);                                                                      \
        _n->next;                                                                              \
    })

#define n00b_linked_list_node_contents(nptr)                                                   \
    ({                                                                                         \
        auto _n = (nptr);                                                                      \
        _n->contents;                                                                          \
    })

#define n00b_linked_list_set_allocator(lptr, a) ((lptr)->allocator = a);
