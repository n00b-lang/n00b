#pragma once

// TODO: make this a typed interface.

#include "core/atomic.h"
#include "core/random.h"

typedef struct n00b_stack_node_t n00b_stack_node_t;
typedef struct n00b_head_t       n00b_head_t;

struct n00b_stack_node_t {
    n00b_stack_node_t *next;
};

struct n00b_head_t {
    alignas(N00B_ALIGN) n00b_stack_node_t *head;
    uint64_t aba_guard;
};

typedef _Atomic(n00b_head_t) n00b_stack_t;

static inline void
n00b_stack_init(n00b_stack_t *stack)
{
    n00b_atomic_store(stack, ((n00b_head_t){.head = nullptr, .aba_guard = 0}));
}

// Expect to do a layer of type checking on the item w/ a macro;
// it should then cast to our n00b_stack_node_t.
static inline void
n00b_stack_push(n00b_stack_t *stack, n00b_stack_node_t *item)
{
    n00b_head_t expected = {.head = nullptr, .aba_guard = 0};
    n00b_head_t desired  = {.head = item, .aba_guard = n00b_rand64()};

    do {
        item->next = expected.head;
    } while (!n00b_atomic_cas(stack, &expected, desired));
}

static inline void *
n00b_stack_pop(n00b_stack_t *stack) _kargs
{
    bool *found = nullptr;
}
{
    n00b_head_t        expected = n00b_atomic_load(stack);
    n00b_head_t        desired;
    n00b_stack_node_t *node;

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
    } while (!n00b_atomic_cas(stack, &expected, desired));

    return node;
}
