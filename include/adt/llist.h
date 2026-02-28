/**
 * @file llist.h
 * @brief Type-safe doubly linked list (optionally thread-safe).
 *
 * Provides `n00b_linked_list_t(T)` — an intrusive doubly linked list
 * with head/tail tracking.  Elements are nullable (wrapping in
 * `n00b_option_t` is not currently supported due to ncc limitations
 * with nested `typeid()` inside `typeof()`).
 *
 * When the list head's `lock` pointer is non-null, mutating operations
 * acquire a write lock and read operations acquire a read lock.
 * When null, locking is a no-op.
 *
 * **Memory:** Nodes are allocated via the list's allocator (or the default
 * allocator).  In a GC environment, removed nodes are reclaimed automatically.
 * Without GC, the caller is responsible for freeing removed nodes.
 *
 * Operations: `append`, `prepend`, `remove`, `first`, `last`, `next`, `prev`,
 * `node_contents`, `is_empty`, `len`, `foreach`, `zero`, `set_allocator`.
 *
 * Usage:
 * @code
 *     n00b_linked_list_decl(int);
 *     n00b_linked_list_t(int) list = {};
 *     n00b_linked_list_append(&list, 42);
 *     n00b_linked_list_prepend(&list, 7);
 *
 *     n00b_linked_list_foreach(&list, node) {
 *         printf("%d\n", n00b_linked_list_node_contents(node));
 *     }
 *
 *     auto first = n00b_linked_list_first(&list);
 *     n00b_linked_list_remove(&list, first);
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
            = n00b_alloc_with_opts(typeof(*(_l->head)),                                        \
                                   &(n00b_alloc_opts_t){.allocator = _l->allocator});          \
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

/**
 * @brief Prepend an item to the front of the linked list.
 * @param lptr Pointer to the list head.
 * @param item Value to prepend.
 */
#define n00b_linked_list_prepend(lptr, item)                                                   \
    {                                                                                          \
        auto                 _l = (lptr);                                                      \
        n00b_data_write_lock(_l->lock);                                                        \
        typeof(*(_l->head)) *nodep                                                             \
            = n00b_alloc_with_opts(typeof(*(_l->head)),                                        \
                                   &(n00b_alloc_opts_t){.allocator = _l->allocator});          \
        nodep->contents = (item);                                                              \
        nodep->next     = _l->head;                                                            \
        nodep->prev     = nullptr;                                                             \
        if (_l->head) {                                                                        \
            _l->head->prev = nodep;                                                            \
        }                                                                                      \
        else {                                                                                 \
            _l->tail = nodep;                                                                  \
        }                                                                                      \
        _l->head = nodep;                                                                      \
        n00b_data_unlock(_l->lock);                                                            \
    }

/**
 * @brief Get the last node of the linked list.
 * @param lptr Pointer to the list head.
 * @return Pointer to the tail node, or nullptr if empty.
 */
#define n00b_linked_list_last(lptr)                                                            \
    ({                                                                                         \
        auto _l = (lptr);                                                                      \
        n00b_data_read_lock(_l->lock);                                                         \
        auto _r = _l->tail;                                                                    \
        n00b_data_unlock(_l->lock);                                                            \
        _r;                                                                                    \
    })

/**
 * @brief Get the previous node (toward head).
 * @param nptr Pointer to a node.
 * @return Pointer to the previous node, or nullptr if at head.
 */
#define n00b_linked_list_prev(nptr)                                                            \
    ({                                                                                         \
        auto _n = (nptr);                                                                      \
        _n->prev;                                                                              \
    })

/**
 * @brief Remove a node from the linked list.
 *
 * The node is unlinked from the list.  In a GC environment the memory
 * is reclaimed automatically; otherwise the caller must free it.
 *
 * @param lptr Pointer to the list head.
 * @param nptr Pointer to the node to remove.
 */
#define n00b_linked_list_remove(lptr, nptr)                                                    \
    {                                                                                          \
        auto _l = (lptr);                                                                      \
        auto _n = (nptr);                                                                      \
        n00b_data_write_lock(_l->lock);                                                        \
        if (_n->prev) {                                                                        \
            _n->prev->next = _n->next;                                                         \
        }                                                                                      \
        else {                                                                                 \
            _l->head = _n->next;                                                               \
        }                                                                                      \
        if (_n->next) {                                                                        \
            _n->next->prev = _n->prev;                                                         \
        }                                                                                      \
        else {                                                                                 \
            _l->tail = _n->prev;                                                               \
        }                                                                                      \
        _n->prev = nullptr;                                                                    \
        _n->next = nullptr;                                                                    \
        n00b_data_unlock(_l->lock);                                                            \
    }

/**
 * @brief Test whether the linked list is empty.
 * @param lptr Pointer to the list head.
 * @return true if the list has no nodes.
 */
#define n00b_linked_list_is_empty(lptr) ((lptr)->head == nullptr)

/**
 * @brief Count the number of nodes in the linked list.
 *
 * O(n) traversal.  **Not locked** — if the list is shared between
 * threads, the caller must hold an external lock for the duration
 * of the call to get a consistent count.
 *
 * @param lptr Pointer to the list head.
 * @return Number of nodes.
 */
#define n00b_linked_list_len(lptr)                                                             \
    ({                                                                                         \
        auto   _l   = (lptr);                                                                  \
        size_t _cnt = 0;                                                                       \
        for (auto _n = _l->head; _n; _n = _n->next) {                                         \
            ++_cnt;                                                                            \
        }                                                                                      \
        _cnt;                                                                                  \
    })

/**
 * @brief Iterate over all nodes in the linked list (head to tail).
 *
 * **Intentionally unlocked:** Linked lists are designed for
 * single-owner or caller-managed synchronization.  If the list
 * is shared between threads, the caller must hold an external
 * lock (or arrange exclusive access) for the duration of the loop.
 *
 * Example:
 * @code
 *     n00b_linked_list_foreach(&list, node) {
 *         printf("%d\n", n00b_linked_list_node_contents(node));
 *     }
 * @endcode
 *
 * @param lptr Pointer to the list head.
 * @param var  Node pointer variable name for the loop body.
 *
 * @pre Caller ensures exclusive access if the list is shared.
 */
#define n00b_linked_list_foreach(lptr, var)                                                    \
    for (auto var = (lptr)->head; var; var = var->next)

#define n00b_linked_list_set_allocator(lptr, a) ((lptr)->allocator = a);
