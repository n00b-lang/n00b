/**
 * @file stack.h
 * @brief Type-safe lock-free stack wrapper — pure macros, no IMPL required.
 *
 * @c base_lf_stack_typed_t(T) wraps @ref base_lf_stack_t with compile-time
 * type safety. @p T must have @ref base_lf_node_t (or compatible) as its
 * first member.
 *
 * Usage:
 * @code
 *     typedef struct my_item {
 *         base_lf_node_t next;  // MUST be first member
 *         int data;
 *     } my_item_t;
 *
 *     base_lf_stack_check(my_item_t);
 *
 *     base_lf_stack_typed_t(my_item_t) stack;
 *     base_lf_stack_typed_init(my_item_t, &stack);
 *     base_lf_stack_typed_push(my_item_t, &stack, item);
 *     my_item_t *popped = base_lf_stack_typed_pop(my_item_t, &stack);
 * @endcode
 */
#pragma once

#include "core/atomic.h"
#include "core/random.h"

#define N00B_ALIGN 16

// ============================================================================
// Type definition
// ============================================================================

/**
 * @brief Declare + define a typed lock-free stack for element type @p T.
 *        Use in variable declarations.
 * @param T  Element type (must have @c next as first member).
 */
#define n00b_stack_node_tid(T) typeid("stack_node", T)
#define n00b_stack_tid(T)      typeid("stack", T)

#define n00b_stack_node_t(T) struct n00b_stack_node_tid(T)
#define _n00b_stack_t(T)     struct n00b_stack_tid(T)

#define n00b_stack_node_decl(T)                                                                \
    n00b_stack_node_t(T)                                                                       \
    {                                                                                          \
        struct n00b_stack_node_t(T) * next;                                                    \
        T contents;                                                                            \
    }

#define n00b_stack_head_decl(T)                                                                \
    _n00b_stack_t(T)                                                                           \
    {                                                                                          \
        alignas(N00B_ALIGN) n00b_stack_node_t(T) * head;                                       \
        uint64_t aba_guard;                                                                    \
    }

#define n00b_stack_decl(T)                                                                     \
    _n00b_stack_node_decl(T);                                                                  \
    n00b_stack_head_decl(T)

#define n00b_stack_t(T) _Atomic(_n00b_stack_t(T))

#define n00b_stack_init(sptr)                                                                  \
    n00b_atomic_store((sptr), (typeof(*(sptr))){.head = nullptr, .aba_guard = 0})

#define n00b_stack_push_node(sptr, item_ptr)                                                   \
    {                                                                                          \
        typeof(*(sptr)) expected = (typeof(*(sptr))){                                          \
            .head      = nullptr,                                                              \
            .aba_guard = 0,                                                                    \
        };                                                                                     \
        typeof(*(sptr)) desired = (typeof(*(sptr))){                                           \
            .head      = item_ptr,                                                             \
            .aba_guard = n00b_rand64(),                                                        \
        };                                                                                     \
        do {                                                                                   \
            item_ptr->next = expected.head;                                                    \
        } while (!n00b_atomic_cas(stack, &expected, desired));                                 \
    }

#define n00b_stack_pop_node(sptr)                                                              \
    ({                                                                                         \
        auto               expected = n00b_atomic_load(sptr);                                  \
        typeof(*(sptr))    desired;                                                            \
        typeof(sptr->head) node;                                                               \
        desired.aba_guard = n00b_rand64();                                                     \
        do {                                                                                   \
            node = expected.head;                                                              \
            if (!node) {                                                                       \
                break;                                                                         \
            }                                                                                  \
            desired.head = node->next;                                                         \
        } while (!n00b_atomic_cas(stack, &expected, desired));                                 \
        node;                                                                                  \
    })
