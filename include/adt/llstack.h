/**
 * @file llstack.h
 * @brief Lock-free (Treiber) stack.
 *
 * A compare-and-swap based stack with ABA protection via a random
 * guard word.  Used internally by the pool allocator's free lists.
 */
#pragma once

// TODO: make this a typed interface.

#include "core/atomic.h"
#include "core/random.h"
#include "core/align.h"

typedef struct n00b_llstack_node_t n00b_llstack_node_t;
typedef struct n00b_head_t         n00b_head_t;

struct n00b_llstack_node_t {
    n00b_llstack_node_t *next;
};

struct n00b_head_t {
    alignas(N00B_ALIGN) n00b_llstack_node_t *head;
    uint64_t aba_guard;
};

typedef _Atomic(n00b_head_t) n00b_llstack_t;

/**
 * @brief Initialize a lock-free stack to empty.
 * @param llstack Stack to initialize.
 */
static inline void
n00b_llstack_init(n00b_llstack_t *llstack)
{
    n00b_atomic_store(llstack, ((n00b_head_t){.head = nullptr, .aba_guard = 0}));
}

/**
 * @brief Push a node onto the lock-free stack.
 * @param llstack Stack to push onto.
 * @param item    Node to push (caller-owned, not allocated by stack).
 */
static inline void
n00b_llstack_push_node(n00b_llstack_t *llstack, n00b_llstack_node_t *item)
{
    n00b_head_t expected = {.head = nullptr, .aba_guard = 0};
    n00b_head_t desired  = {.head = item, .aba_guard = n00b_rand64()};

    do {
        item->next = expected.head;
    } while (!n00b_atomic_cas(llstack, &expected, desired));
}

/**
 * @brief Pop a node from the lock-free stack.
 * @param llstack Stack to pop from.
 * @return        Popped node, or nullptr if empty.
 *
 * @kw found Pointer to bool; set to false if the stack was empty (may be nullptr).
 */
static inline void *
n00b_llstack_pop_node(n00b_llstack_t *llstack) _kargs
{
    bool *found = nullptr;
}
{
    n00b_head_t          expected = n00b_atomic_load(llstack);
    n00b_head_t          desired;
    n00b_llstack_node_t *node;

    desired.aba_guard = n00b_rand64();

    do {
        node = expected.head;

        if (!node) {
            if (found) {
                *found = false;
            }
            return nullptr;
        }

        desired.head = node->next;
    } while (!n00b_atomic_cas(llstack, &expected, desired));

    /* pool_free zeroes the entire entry before pushing; the push then
     * stamps the first sizeof(void*) bytes with the freelist link.
     * Restore the all-zero payload by clearing the link slot before
     * handing the node back, so callers can rely on the contract that
     * a freshly-popped entry's bytes are all zero. */
    node->next = nullptr;
    return node;
}
