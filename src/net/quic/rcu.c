/*
 * rcu.c — Implementation of the tiny RCU-style atomic-view-swap
 * primitive shared by cert_store and sticky_secret.
 *
 * The graveyard is intentionally unbounded for v1: the only
 * consumers swap on the order of weeks (cert renewals) or on
 * deployment-driven rotation events.  Memory pressure from the
 * graveyard is bounded by `swap_count * sizeof(n00b_rcu_node_t)`
 * + the views themselves, which the caller owns and which live in
 * the conduit pool.  Reclaim machinery (epoch-based, hazard
 * pointers, RCU-grace-period) was considered and explicitly
 * deferred — see `~/dd/quic_2.md` § 6 + § 7.
 */

#define N00B_USE_INTERNAL_API
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "internal/net/quic/rcu.h"

struct n00b_rcu_node {
    struct n00b_rcu_node *next;   /* singly-linked, oldest at head. */
    void                 *view;
};

static n00b_allocator_t *
rcu_alloc(void)
{
    return (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
}

void
n00b_rcu_init(n00b_rcu_t *r, void *initial_view)
{
    if (!r) return;
    atomic_store_explicit(&r->current, initial_view, memory_order_release);
    r->writer_lock = n00b_data_lock_new(); 
    r->graveyard = nullptr;
    r->inited    = true;
}

void *
n00b_rcu_load(n00b_rcu_t *r)
{
    if (!r || !r->inited) return nullptr;
    return atomic_load_explicit(&r->current, memory_order_acquire);
}

void *
n00b_rcu_swap(n00b_rcu_t *r, void *new_view)
{
    if (!r || !r->inited) return nullptr;

    n00b_data_write_lock(r->writer_lock);
    void *prev = atomic_load_explicit(&r->current, memory_order_acquire);
    if (prev) {
        n00b_rcu_node_t *node = n00b_alloc_with_opts(n00b_rcu_node_t,
            &(n00b_alloc_opts_t){.allocator = rcu_alloc()});
        node->view = prev;
        node->next = r->graveyard;
        r->graveyard = node;
    }
    atomic_store_explicit(&r->current, new_view, memory_order_release);
    n00b_data_unlock(r->writer_lock);
    return prev;
}

void
n00b_rcu_for_each_view(n00b_rcu_t *r,
                       void (*cb)(void *view, void *ctx),
                       void *ctx)
{
    if (!r || !cb) return;
    void *cur = atomic_load_explicit(&r->current, memory_order_acquire);
    if (cur) {
        cb(cur, ctx);
    }
    for (n00b_rcu_node_t *n = r->graveyard; n; n = n->next) {
        if (n->view) {
            cb(n->view, ctx);
        }
    }
}

void
n00b_rcu_close(n00b_rcu_t *r)
{
    if (!r || !r->inited) return;
    atomic_store_explicit(&r->current, (void *)nullptr, memory_order_release);
    
    r->inited = false;
}
