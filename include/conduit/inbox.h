/**
 * @file inbox.h
 * @brief Type-safe per-consumer message queues for the conduit system.
 *
 * Inboxes are FIFO queues where publishers deliver messages to consumers.
 * @c n00b_conduit_inbox_t(T) provides type-safe message delivery for
 * payload type T.
 *
 * Every inbox has two queues:
 * - A typed message queue for payload type T
 * - A system message queue for lifecycle messages (SUB_ACCEPTED, TOPIC_CLOSED, etc.)
 *
 * Usage:
 * @code
 *     N00B_CONDUIT_INBOX_IMPL(my_payload_t);
 *     n00b_conduit_inbox_t(my_payload_t) *inbox = ...;
 *     n00b_conduit_inbox_init(my_payload_t, inbox, c, N00B_CONDUIT_BP_UNBOUNDED, 0);
 *     n00b_conduit_inbox_push_msg(my_payload_t, inbox, msg);
 *     n00b_conduit_message_t(my_payload_t) *m = n00b_conduit_inbox_pop_msg(my_payload_t, inbox);
 * @endcode
 */
#pragma once

#include "conduit/message.h"
#include "core/atomic.h"
#include "core/condition.h"

// ============================================================================
// Backpressure policies
// ============================================================================

typedef enum {
    N00B_CONDUIT_BP_UNBOUNDED,   /**< No limit (can grow indefinitely) */
    N00B_CONDUIT_BP_DROP_OLDEST, /**< Drop oldest message when full */
    N00B_CONDUIT_BP_DROP_NEWEST, /**< Drop new message when full */
    N00B_CONDUIT_BP_SIGNAL,      /**< Send OVERFLOW message, drop new */
} n00b_conduit_backpressure_t;

// ============================================================================
// System message type and queue
// ============================================================================

N00B_CONDUIT_MESSAGE_IMPL(n00b_conduit_empty_payload_t);
typedef n00b_conduit_message_t(n00b_conduit_empty_payload_t) n00b_conduit_sys_msg_t;

/**
 * @brief Lock-free MPSC queue for system (lifecycle) messages.
 *
 * Embedded in every typed inbox.
 */
typedef struct n00b_conduit_sys_queue {
    _Atomic(n00b_conduit_sys_msg_t *) head;
    _Atomic(n00b_conduit_sys_msg_t *) tail;
    _Atomic(uint32_t)                 count;
} n00b_conduit_sys_queue_t;

static inline void
n00b_conduit_sys_queue_init(n00b_conduit_sys_queue_t *q)
{
    n00b_atomic_store(&q->head, nullptr);
    n00b_atomic_store(&q->tail, nullptr);
    n00b_atomic_store(&q->count, 0);
}

static inline bool
n00b_conduit_sys_queue_push(n00b_conduit_sys_queue_t *q, n00b_conduit_sys_msg_t *msg)
{
    if (!q || !msg) return false;
    msg->header.next = nullptr;

    n00b_conduit_sys_msg_t *prev = n00b_atomic_read_then_set(&q->tail, msg);
    if (prev) {
        prev->header.next = (n00b_conduit_msg_hdr_t *)msg;
    } else {
        n00b_atomic_store(&q->head, msg);
    }
    n00b_atomic_add(&q->count, 1);
    return true;
}

static inline n00b_conduit_sys_msg_t *
n00b_conduit_sys_queue_pop(n00b_conduit_sys_queue_t *q)
{
    if (!q) return nullptr;

    n00b_conduit_sys_msg_t *head = n00b_atomic_load(&q->head);
    if (!head) return nullptr;

    n00b_conduit_sys_msg_t *next = (n00b_conduit_sys_msg_t *)head->header.next;
    if (next) {
        n00b_atomic_store(&q->head, next);
    } else {
        n00b_conduit_sys_msg_t *expected = head;
        if (n00b_atomic_cas(&q->head, &expected, nullptr)) {
            expected = head;
            n00b_atomic_cas(&q->tail, &expected, nullptr);
        } else {
            while ((next = (n00b_conduit_sys_msg_t *)head->header.next) == nullptr) {}
            n00b_atomic_store(&q->head, next);
        }
    }
    n00b_atomic_add(&q->count, (uint32_t)-1);
    head->header.next = nullptr;
    return head;
}

static inline bool
n00b_conduit_sys_queue_has_messages(n00b_conduit_sys_queue_t *q)
{
    return n00b_atomic_load(&q->count) > 0;
}

static inline uint32_t
n00b_conduit_sys_queue_count(n00b_conduit_sys_queue_t *q)
{
    return n00b_atomic_load(&q->count);
}

// ============================================================================
// Typed inbox macro
// ============================================================================

#define n00b_conduit_inbox_t(T) struct typeid("n00b_conduit_inbox", T)

#define _N00B_INBOX_FN(fn, T) typeid("n00b_conduit_inbox_" #fn, T)

/**
 * @brief Instantiate an inbox type for payload T without instantiating the
 *        message type (use when message type is already instantiated).
 */
#define N00B_CONDUIT_INBOX_IMPL_NO_MSG(T)                                                      \
    n00b_conduit_inbox_t(T) {                                                                  \
        /* Typed message queue */                                                              \
        _Atomic(n00b_conduit_message_t(T) *) head;                                             \
        _Atomic(n00b_conduit_message_t(T) *) tail;                                             \
        n00b_conduit_backpressure_t          backpressure;                                     \
        uint32_t                             limit;                                            \
        _Atomic(uint32_t)                    count;                                            \
        /* System message queue */                                                             \
        n00b_conduit_sys_queue_t             sys_queue;                                        \
        /* Notification */                                                                     \
        n00b_condition_t                     cv;                                               \
        n00b_conduit_t                      *conduit;                                          \
        const char                          *name;                                             \
    };                                                                                         \
                                                                                               \
    static inline void                                                                         \
    _N00B_INBOX_FN(init, T)(n00b_conduit_inbox_t(T) *inbox,                                    \
                            n00b_conduit_t           *c,                                       \
                            n00b_conduit_backpressure_t bp,                                    \
                            uint32_t                  lim)                                     \
    {                                                                                          \
        n00b_atomic_store(&inbox->head, nullptr);                                              \
        n00b_atomic_store(&inbox->tail, nullptr);                                              \
        inbox->backpressure = bp;                                                              \
        inbox->limit = lim;                                                                    \
        n00b_atomic_store(&inbox->count, 0);                                                   \
        n00b_conduit_sys_queue_init(&inbox->sys_queue);                                        \
        n00b_condition_init(&inbox->cv);                                                        \
        inbox->conduit = c;                                                                    \
        inbox->name = nullptr;                                                                 \
    }                                                                                          \
                                                                                               \
    static inline n00b_conduit_message_t(T) *                                                  \
    _N00B_INBOX_FN(pop, T)(n00b_conduit_inbox_t(T) *inbox)                                     \
    {                                                                                          \
        if (!inbox) return nullptr;                                                            \
        n00b_conduit_message_t(T) *head = n00b_atomic_load(&inbox->head);                      \
        if (!head) return nullptr;                                                             \
        n00b_conduit_message_t(T) *next =                                                      \
            (n00b_conduit_message_t(T) *)head->header.next;                                    \
        if (next) {                                                                            \
            n00b_atomic_store(&inbox->head, next);                                             \
        } else {                                                                               \
            n00b_conduit_message_t(T) *expected = head;                                        \
            if (n00b_atomic_cas(&inbox->head, &expected, nullptr)) {                           \
                expected = head;                                                               \
                n00b_atomic_cas(&inbox->tail, &expected, nullptr);                             \
            } else {                                                                           \
                while ((next = (n00b_conduit_message_t(T) *)head->header.next)                 \
                       == nullptr) {}                                                          \
                n00b_atomic_store(&inbox->head, next);                                         \
            }                                                                                  \
        }                                                                                      \
        n00b_atomic_add(&inbox->count, (uint32_t)-1);                                          \
        head->header.next = nullptr;                                                           \
        return head;                                                                           \
    }                                                                                          \
                                                                                               \
    static inline bool                                                                         \
    _N00B_INBOX_FN(push, T)(n00b_conduit_inbox_t(T)      *inbox,                               \
                            n00b_conduit_message_t(T) *msg)                                    \
    {                                                                                          \
        if (!inbox || !msg) return false;                                                      \
        msg->header.next = nullptr;                                                            \
        if (inbox->limit > 0) {                                                                \
            uint32_t cnt = n00b_atomic_load(&inbox->count);                                    \
            if (cnt >= inbox->limit) {                                                         \
                if (inbox->backpressure == N00B_CONDUIT_BP_DROP_NEWEST ||                      \
                    inbox->backpressure == N00B_CONDUIT_BP_SIGNAL) {                           \
                    return false;                                                              \
                }                                                                              \
                if (inbox->backpressure == N00B_CONDUIT_BP_DROP_OLDEST) {                      \
                    _N00B_INBOX_FN(pop, T)(inbox);                                             \
                }                                                                              \
            }                                                                                  \
        }                                                                                      \
        n00b_conduit_message_t(T) *prev =                                                      \
            n00b_atomic_read_then_set(&inbox->tail, msg);                                      \
        if (prev) {                                                                            \
            prev->header.next = (n00b_conduit_msg_hdr_t *)msg;                                 \
        } else {                                                                               \
            n00b_atomic_store(&inbox->head, msg);                                              \
        }                                                                                      \
        n00b_atomic_add(&inbox->count, 1);                                                     \
        n00b_condition_notify(&inbox->cv, .auto_unlock = true);                                \
        return true;                                                                           \
    }                                                                                          \
                                                                                               \
    static inline uint32_t                                                                     \
    _N00B_INBOX_FN(count, T)(n00b_conduit_inbox_t(T) *inbox)                                   \
    {                                                                                          \
        return n00b_atomic_load(&inbox->count);                                                \
    }                                                                                          \
                                                                                               \
    static inline bool                                                                         \
    _N00B_INBOX_FN(has_messages, T)(n00b_conduit_inbox_t(T) *inbox)                            \
    {                                                                                          \
        return n00b_atomic_load(&inbox->count) > 0;                                            \
    }                                                                                          \
                                                                                               \
    static inline bool                                                                         \
    _N00B_INBOX_FN(is_full, T)(n00b_conduit_inbox_t(T) *inbox)                                 \
    {                                                                                          \
        if (inbox->limit == 0) return false;                                                   \
        return n00b_atomic_load(&inbox->count) >= inbox->limit;                                \
    }

/**
 * @brief Instantiate inbox for message payload type T (also instantiates the
 *        message type).
 */
#define N00B_CONDUIT_INBOX_IMPL(T)                                                             \
    N00B_CONDUIT_MESSAGE_IMPL(T);                                                              \
    N00B_CONDUIT_INBOX_IMPL_NO_MSG(T)

// ============================================================================
// Type-safe inbox operation macros
// ============================================================================

#define n00b_conduit_inbox_init(T, inbox, c, bp, lim) \
    _N00B_INBOX_FN(init, T)(inbox, c, bp, lim)
#define n00b_conduit_inbox_push_msg(T, inbox, msg) \
    _N00B_INBOX_FN(push, T)(inbox, msg)
#define n00b_conduit_inbox_pop_msg(T, inbox) \
    _N00B_INBOX_FN(pop, T)(inbox)
#define n00b_conduit_inbox_msg_count(T, inbox) \
    _N00B_INBOX_FN(count, T)(inbox)
#define n00b_conduit_inbox_has_msg(T, inbox) \
    _N00B_INBOX_FN(has_messages, T)(inbox)
#define n00b_conduit_inbox_full(T, inbox) \
    _N00B_INBOX_FN(is_full, T)(inbox)

// ============================================================================
// System inbox type alias
// ============================================================================

N00B_CONDUIT_INBOX_IMPL_NO_MSG(n00b_conduit_empty_payload_t);
typedef n00b_conduit_inbox_t(n00b_conduit_empty_payload_t) n00b_conduit_sys_inbox_t;

// ============================================================================
// System queue access macros (work with any inbox)
// ============================================================================

#define n00b_conduit_inbox_pop_sys(inbox) \
    n00b_conduit_sys_queue_pop(&(inbox)->sys_queue)
#define n00b_conduit_inbox_has_sys(inbox) \
    n00b_conduit_sys_queue_has_messages(&(inbox)->sys_queue)
#define n00b_conduit_inbox_sys_count(inbox) \
    n00b_conduit_sys_queue_count(&(inbox)->sys_queue)
