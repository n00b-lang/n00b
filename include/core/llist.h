/**
 * @file llist.h
 * @brief Type-safe doubly linked list. NOT thread safe.
 *
 * Unfortunately, unless I build basically a whole lot more of a C
 * compiler, being able to nest typeid() inside a typeof() isn't going
 * to work, so the only way I can come up with to do this is to still
 * use nullable items (I was trying to stick them in an option).
 *
 * If you want to stick an option inside manually, you should be able
 * to do it, just by using a typedef.
 *
 * I probably can rig ncc to provide a work-around w/o too much fuss, but not important right
 * now.
 */
#pragma once

#include "n00b.h"

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
    }

#define n00b_linked_list_first(lptr)                                                           \
    ({                                                                                         \
        auto _l = (lptr);                                                                      \
        _l->head;                                                                              \
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
