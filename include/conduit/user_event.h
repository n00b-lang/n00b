/**
 * @file user_event.h
 * @brief User-triggered events for the conduit system.
 *
 * User events provide cross-thread wakeups and application-defined signals.
 * They use integer URIs with the @c N00B_CONDUIT_TAG_USER_EVENT tag.
 *
 * Supported on kqueue (macOS/BSD), Linux (eventfd), and Windows (CreateEvent).
 *
 * Usage:
 * @code
 *     n00b_conduit_topic_base_t *t =
 *         n00b_result_get(n00b_conduit_user_event_new(c));
 *     n00b_conduit_user_event_inbox_t *inbox =
 *         n00b_conduit_user_event_inbox_new(c);
 *     n00b_conduit_user_event_subscribe(t, inbox,
 *         .operations = N00B_CONDUIT_OP_ALL);
 *     n00b_conduit_user_event_trigger(c, t);  // wake from any thread
 * @endcode
 */
#pragma once

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) \
 || defined(__NetBSD__) || defined(__linux__) || defined(_WIN32)

#include "conduit/conduit.h"
#include "conduit/io.h"
#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// User Event Operations
// ============================================================================

/**
 * @brief User event operations.
 */
typedef enum {
    N00B_CONDUIT_USER_EVENT_TRIGGERED = (1 << 0),
    N00B_CONDUIT_USER_EVENT_ALL       = 0x01,
} n00b_conduit_user_event_op_t;

// ============================================================================
// User Event Payload
// ============================================================================

/**
 * @brief User event payload.
 */
typedef struct {
    uint64_t event_id;       /**< User event identifier */
    uint64_t trigger_count;  /**< Number of times triggered */
} n00b_conduit_user_event_payload_t;

// ============================================================================
// User event type instantiation
// ============================================================================

N00B_CONDUIT_FULL_IMPL(n00b_conduit_user_event_payload_t);

typedef n00b_conduit_message_t(n00b_conduit_user_event_payload_t)
    n00b_conduit_user_event_msg_t;
typedef n00b_conduit_inbox_t(n00b_conduit_user_event_payload_t)
    n00b_conduit_user_event_inbox_t;

// ============================================================================
// User Event API
// ============================================================================

/**
 * @brief Create a user event (auto-assigns ID).
 * @param c Conduit instance.
 * @return Ok(topic) that can be triggered from any thread.
 */
extern n00b_result_t(n00b_conduit_topic_base_t *)
n00b_conduit_user_event_new(n00b_conduit_t *c);

/**
 * @brief Trigger a user event.
 * @param c     Conduit instance.
 * @param topic User event topic.
 *
 * Can be called from any thread. Causes the event loop to wake up
 * and deliver a message to all subscribers.
 */
extern void
n00b_conduit_user_event_trigger(n00b_conduit_t *c,
                                n00b_conduit_topic_base_t *topic);

/**
 * @brief Destroy a user event.
 */
extern void
n00b_conduit_user_event_destroy(n00b_conduit_t *c,
                                n00b_conduit_topic_base_t *topic);

/**
 * @brief Get event ID from topic.
 * @return Event ID, or 0 if not a user event topic.
 */
static inline uint64_t
n00b_conduit_user_event_id(n00b_conduit_topic_base_t *topic)
{
    n00b_conduit_uri_t uri = n00b_conduit_topic_uri(topic);

    if (n00b_variant_is_type(uri, uint64_t)) {
        uint64_t v = n00b_variant_get(uri, uint64_t);
        if (N00B_CONDUIT_URI_IS_USER_EVENT(v)) {
            return N00B_CONDUIT_URI_ID(v);
        }
    }

    return 0;
}

/**
 * @brief Check if topic is a user event.
 */
static inline bool
n00b_conduit_topic_is_user_event(n00b_conduit_topic_base_t *topic)
{
    n00b_conduit_uri_t uri = n00b_conduit_topic_uri(topic);

    if (n00b_variant_is_type(uri, uint64_t)) {
        uint64_t v = n00b_variant_get(uri, uint64_t);
        return N00B_CONDUIT_URI_IS_USER_EVENT(v);
    }

    return false;
}

// ============================================================================
// User Event Subscription Macros
// ============================================================================

/** @brief Create a user event inbox. */
#define n00b_conduit_user_event_inbox_new(c)                                   \
    ({                                                                         \
        n00b_conduit_user_event_inbox_t *_inbox =                              \
            n00b_alloc_with_opts(n00b_conduit_user_event_inbox_t,              \
                &(n00b_alloc_opts_t){.allocator = (c)->allocator});            \
        n00b_conduit_inbox_init(n00b_conduit_user_event_payload_t,             \
                                _inbox, c, N00B_CONDUIT_BP_UNBOUNDED, 0);      \
        _inbox;                                                                \
    })

/** @brief Subscribe to user events. */
#define n00b_conduit_user_event_subscribe(topic, inbox, ...)                   \
    n00b_conduit_subscribe(n00b_conduit_user_event_payload_t,                  \
                           (n00b_conduit_topic_t(n00b_conduit_user_event_payload_t) *)(topic), \
                           inbox, __VA_ARGS__)

/** @brief Pop a user event message from inbox. */
#define n00b_conduit_user_event_inbox_pop(inbox) \
    n00b_conduit_inbox_pop_msg(n00b_conduit_user_event_payload_t, inbox)

/** @brief Check if user event inbox has messages. */
#define n00b_conduit_user_event_inbox_has_messages(inbox) \
    n00b_conduit_inbox_has_msg(n00b_conduit_user_event_payload_t, inbox)

// ============================================================================
// Internal: User Event Backend Interface
// ============================================================================

/**
 * @brief User event entry for backend tracking.
 */
struct n00b_conduit_user_event {
    uint64_t                        event_id;
    n00b_conduit_topic_base_t      *topic;
    _Atomic(uint64_t)               trigger_count;
    struct n00b_conduit_user_event *next;
};

/**
 * @internal Register user event with I/O backend.
 */
extern bool
n00b_conduit_user_event_register(n00b_conduit_t *c,
                                 n00b_conduit_user_event_t *event);

/**
 * @internal Unregister user event from I/O backend.
 */
extern void
n00b_conduit_user_event_unregister(n00b_conduit_t *c,
                                   n00b_conduit_user_event_t *event);

/**
 * @internal Fire user event (called by I/O backend).
 */
extern void
n00b_conduit_user_event_fire(n00b_conduit_user_event_t *event);

#endif // kqueue/Linux/Windows platforms
