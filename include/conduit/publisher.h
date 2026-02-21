/**
 * @file publisher.h
 * @brief Publisher structures for the conduit event system.
 *
 * Publishers are threads that have claimed the right to linearize
 * and distribute events for a topic. Each publisher tracks the owning
 * thread, a monotonic claim ID, and a liveness sentinel for detecting
 * dead publishers.
 */
#pragma once

#include "conduit/conduit_types.h"
#include "core/atomic.h"
#include "core/futex.h"
#include "core/platform.h"

// ============================================================================
// Publisher state
// ============================================================================

typedef enum {
    N00B_CONDUIT_PUB_ACTIVE,    /**< Publisher is actively working */
    N00B_CONDUIT_PUB_FINISHING, /**< Publisher is in final poll loop */
    N00B_CONDUIT_PUB_YIELDED,   /**< Publisher has yielded */
} n00b_conduit_pub_state_t;

// ============================================================================
// Publisher struct
// ============================================================================

struct n00b_conduit_publisher {
    n00b_conduit_topic_base_t *topic;
    base_thread_id_t           thread;
    uint64_t                   claim_id;
    uint32_t                   thread_slot;       /**< Slot in rt->threads[] at claim time. */
    uint32_t                   thread_generation;  /**< Generation at claim time (detects reuse). */
    _Atomic(int)               state;
    n00b_futex_t               waiters;
    _Atomic(uint32_t)          waiter_count;
};

// ============================================================================
// Thread identity helpers
// ============================================================================

static inline base_thread_id_t
n00b_conduit_current_thread(void)
{
    return base_current_thread_id();
}

static inline bool
n00b_conduit_thread_equal(base_thread_id_t a, base_thread_id_t b)
{
    return base_thread_id_equal(a, b);
}
