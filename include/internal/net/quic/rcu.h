/**
 * @file rcu.h
 * @internal
 * @brief Tiny RCU-style atomic-view-swap primitive.
 *
 * Pulls together the pattern that `cert_store.c` and
 * `sticky_secret.c` were duplicating:
 *
 *   - `_Atomic(view *)` for the current snapshot, swapped with a
 *     release-store on the writer side and read with an acquire-load
 *     on the reader side.
 *   - A graveyard list that retains every previously-current view so
 *     readers that captured a stale pointer continue to observe
 *     valid bytes (no use-after-free).
 *   - A tiny mutex to serialize writers; readers never touch it.
 *   - Module-specific cleanup is supported via
 *     @c n00b_rcu_for_each_view at close time, which iterates current
 *     + graveyard and calls a caller-supplied callback per view.
 *
 * The view type is opaque to this primitive — callers cast `void *`
 * to whatever they want.  Memory for the views themselves lives
 * wherever the caller put it (typically conduit_pool); the rcu
 * struct only owns its mutex and the singly-linked graveyard nodes.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include "n00b.h"

typedef struct n00b_rcu_node n00b_rcu_node_t;

/**
 * @brief Embedded RCU state.  Callers typically embed this directly
 *        in their owning struct rather than allocating it separately.
 */
typedef struct {
    _Atomic(void *)  current;     /**< Current view pointer. */
    n00b_rwlock_t  *writer_lock; /**< Serializes writers. */
    n00b_rcu_node_t *graveyard;   /**< Singly-linked, oldest at head. */
    bool             inited;
} n00b_rcu_t;

/**
 * @brief Initialize @p r with @p initial_view as the current view.
 *
 * Must be called once before any other rcu operation.
 */
extern void
n00b_rcu_init(n00b_rcu_t *r, void *initial_view);

/**
 * @brief Acquire-load the current view.
 *
 * Lock-free.  Returns NULL after `n00b_rcu_close`.
 */
extern void *
n00b_rcu_load(n00b_rcu_t *r);

/**
 * @brief Atomically replace the current view with @p new_view; the
 *        previous view is moved to the graveyard.
 *
 * Acquires the writer mutex.  After return, fresh readers see
 * @p new_view; readers that loaded the previous view still see
 * valid bytes via the graveyard.
 *
 * @return The previous view (caller may inspect it; do not free).
 */
extern void *
n00b_rcu_swap(n00b_rcu_t *r, void *new_view);

/**
 * @brief Iterate every view that's ever been current — current
 *        first, then the graveyard from newest to oldest — and call
 *        @p cb on each.
 *
 * Used at close time for module-specific cleanup (e.g.,
 * @c sticky_secret zeros key bytes; @c cert_store has nothing to
 * do here).  Not synchronized — callers must ensure no concurrent
 * writers.
 */
extern void
n00b_rcu_for_each_view(n00b_rcu_t *r,
                       void (*cb)(void *view, void *ctx),
                       void *ctx);

/**
 * @brief Tear down the rcu's mutex and clear `current`.  Idempotent.
 */
extern void
n00b_rcu_close(n00b_rcu_t *r);
