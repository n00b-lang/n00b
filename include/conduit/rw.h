/**
 * @file rw.h
 * @brief High-level read/write API for the conduit system.
 *
 * Provides blocking and async read/write operations using the same
 * `typeid()`-based dispatch as topics, inboxes, and subscriptions.
 *
 * The blocking read creates an inbox, subscribes to the topic, waits
 * for a message, and returns it.  The blocking write submits data and
 * waits for the completion reply.
 *
 * Usage:
 * @code
 *     // Blocking read from an FD stream topic:
 *     n00b_conduit_read_result_t(n00b_conduit_fd_stream_payload_t) r =
 *         n00b_conduit_read(n00b_conduit_fd_stream_payload_t, topic);
 *
 *     // Blocking read with timeout:
 *     auto r = n00b_conduit_read(n00b_conduit_fd_stream_payload_t, topic,
 *                                .timeout_ms = 5000);
 * @endcode
 */
#pragma once

#include "conduit/conduit.h"

// n00b#496: ceilings for the synchronous-write completion wait below.
//
// DONE_POLL_MS is a re-check interval, not a timeout: the loop still waits on
// the inbox CV, it just wakes periodically to re-test its own predicates
// instead of trusting that a notify can only ever arrive while it is asleep.
//
// DONE_WAIT_MS is the overall ceiling. It has to outlast a real write to a
// slow sink (a full pipe whose reader is descheduled), so it is seconds rather
// than milliseconds; it exists to convert a permanent hang into a late return,
// not to bound normal operation. A completion on a healthy topic arrives in
// microseconds, so neither constant is reached in the ordinary case.
#define N00B_CONDUIT_DONE_POLL_MS 50
#define N00B_CONDUIT_DONE_WAIT_MS 10000
#include "conduit/io.h"
#include "conduit/fd_managed.h"
#include "conduit/service.h"
#include "conduit/xform_types.h"
#include "conduit/timer.h"
#include "conduit/user_event.h"
#ifndef _WIN32
#include "conduit/signal.h"
#endif
#include "core/time.h"
#include "core/runtime.h"

// ============================================================================
// Read result type — wraps the message pointer in a result
// ============================================================================

#define n00b_conduit_read_result_t(T) n00b_result_t(n00b_conduit_message_t(T) *)

// ============================================================================
// Async read result — carries both inbox and subscription handle
// ============================================================================

#define n00b_conduit_async_read_t(T) struct typeid("n00b_conduit_async_read", T)

// ============================================================================
// Write result type
// ============================================================================

#define n00b_conduit_write_result_t(T) n00b_result_t(bool)

// ============================================================================
// Per-type read/write function name mangling
// ============================================================================

#define _N00B_RW_FN(fn, T) typeid("n00b_conduit_rw_" #fn, T)

// ============================================================================
// N00B_CONDUIT_RW_IMPL(T) — Generate typed read/write functions.
//
// Requires N00B_CONDUIT_INBOX_IMPL(T), N00B_CONDUIT_SUBSCRIPTION_IMPL(T),
// and N00B_CONDUIT_TOPIC_IMPL(T) to have been called already.
// ============================================================================

#define N00B_CONDUIT_RW_IMPL(T)                                                                    \
                                                                                                   \
    /**                                                                                            \
     * @brief Blocking read: subscribe, wait for one message, return it.                           \
     *                                                                                             \
     * Creates a one-shot subscription, waits on the inbox condition                               \
     * variable, and returns the first message received.  If the topic                             \
     * closes before a message arrives, returns an error.                                          \
     *                                                                                             \
     * @param topic  Typed topic to read from.                                                     \
     * @kw timeout_ms  Maximum wait time in milliseconds (0 = infinite).                           \
     * @kw operations   Operation filter (default: all).                                           \
     * @return Ok(message) or Err(error_code).                                                     \
     */                                                                                            \
    static inline n00b_conduit_read_result_t(T)                                                    \
    _N00B_RW_FN(read, T)(n00b_conduit_topic_t(T) *topic) _kargs                                   \
    {                                                                                              \
        int      timeout_ms = 0;                                                                   \
        uint32_t operations = N00B_CONDUIT_OP_ALL;                                                 \
    }                                                                                              \
    {                                                                                              \
        if (!topic) {                                                                              \
            return n00b_result_err(n00b_conduit_message_t(T) *,                                    \
                                  N00B_CONDUIT_ERR_NULL_ARG);                                      \
        }                                                                                          \
                                                                                                   \
        n00b_conduit_topic_base_t *base = (n00b_conduit_topic_base_t *)topic;                      \
        if (!n00b_conduit_topic_is_active(base)) {                                                 \
            return n00b_result_err(n00b_conduit_message_t(T) *,                                    \
                                  N00B_CONDUIT_ERR_CLOSED);                                        \
        }                                                                                          \
                                                                                                   \
        /* Inbox must be heap-allocated in the traceable conduit pool —              */              \
        /* its embedded CV participates in lock accounting; stack allocation       */              \
        /* leaves dangling pointers in the thread's exclusive_locks chain.         */              \
        n00b_allocator_t *_cp =                                                                    \
            (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;                                 \
        n00b_conduit_inbox_t(T) *inbox = n00b_alloc_with_opts(                                     \
            n00b_conduit_inbox_t(T),                                                               \
            &(n00b_alloc_opts_t){.allocator = _cp});                                               \
        n00b_conduit_inbox_init(T, inbox, base->conduit,                                           \
                                N00B_CONDUIT_BP_DROP_NEWEST, 1);                                   \
                                                                                                   \
        /* One-shot subscription. */                                                               \
        n00b_conduit_sub_handle_t handle =                                                         \
            _N00B_TOPIC_FN(subscribe, T)(                                                          \
                topic, inbox,                                                                      \
                (n00b_conduit_sub_config_t){                                                       \
                    .operations = operations,                                                       \
                    .flags      = N00B_CONDUIT_SUB_F_ONE_SHOT,                                     \
                });                                                                                \
                                                                                                   \
        if (handle == N00B_CONDUIT_INVALID_SUB_HANDLE) {                                           \
            n00b_conduit_inbox_destroy(T, inbox);                                                   \
            n00b_free(inbox);                                                                       \
            return n00b_result_err(n00b_conduit_message_t(T) *,                                    \
                n00b_conduit_topic_is_active(base)                                                  \
                    ? N00B_CONDUIT_ERR_ALLOC                                                        \
                    : N00B_CONDUIT_ERR_CLOSED);                                                     \
        }                                                                                          \
                                                                                                   \
        /* Wait for a message on the inbox CV. */                                                  \
        n00b_conduit_message_t(T) *msg = nullptr;                                                  \
                                                                                                   \
        if (timeout_ms > 0) {                                                                      \
            int64_t deadline_ms = (int64_t)(n00b_ns_timestamp() / N00B_NS_PER_MS)                  \
                                + (int64_t)timeout_ms;                                              \
            while (!n00b_conduit_inbox_has_msg(T, inbox)) {                                        \
                if (n00b_conduit_inbox_has_sys(inbox)) {                                           \
                    break;                                                                         \
                }                                                                                  \
                int64_t now_ms = (int64_t)(n00b_ns_timestamp() / N00B_NS_PER_MS);                  \
                if (now_ms >= deadline_ms) break;                                                   \
                int64_t remain_ms = deadline_ms - now_ms;                                           \
                if (remain_ms <= 0) break;                                                         \
                n00b_condition_lock(&inbox->cv);                                                   \
                if (!n00b_conduit_inbox_has_msg(T, inbox) &&                                       \
                    !n00b_conduit_inbox_has_sys(inbox)) {                                          \
                    n00b_condition_wait(&inbox->cv,                                                 \
                                        .timeout_ms  = remain_ms,                                  \
                                        .auto_unlock = true);                                      \
                }                                                                                  \
                else {                                                                             \
                    n00b_condition_unlock(&inbox->cv);                                             \
                }                                                                                  \
            }                                                                                      \
        }                                                                                          \
        else {                                                                                     \
            while (!n00b_conduit_inbox_has_msg(T, inbox)) {                                        \
                if (n00b_conduit_inbox_has_sys(inbox)) {                                           \
                    break;                                                                         \
                }                                                                                  \
                n00b_condition_lock(&inbox->cv);                                                   \
                if (!n00b_conduit_inbox_has_msg(T, inbox) &&                                       \
                    !n00b_conduit_inbox_has_sys(inbox)) {                                          \
                    n00b_condition_wait(&inbox->cv, .auto_unlock = true);                          \
                }                                                                                  \
                else {                                                                             \
                    n00b_condition_unlock(&inbox->cv);                                             \
                }                                                                                  \
            }                                                                                      \
        }                                                                                          \
                                                                                                   \
        msg = n00b_conduit_inbox_pop_msg(T, inbox);                                                \
                                                                                                   \
        /* Check for topic closed / error via sys queue. */                                        \
        if (!msg) {                                                                                \
            n00b_conduit_sys_msg_t *sys = n00b_conduit_inbox_pop_sys(inbox);                       \
            n00b_err_t err_code = N00B_CONDUIT_ERR_CLOSED;                                        \
            if (sys) {                                                                             \
                if (sys->header.type == N00B_CONDUIT_MSG_TOPIC_CLOSED) {                           \
                    err_code = N00B_CONDUIT_ERR_CLOSED;                                            \
                }                                                                                  \
                else if (sys->header.type == N00B_CONDUIT_MSG_ERROR) {                             \
                    err_code = N00B_CONDUIT_ERR_IO;                                                \
                }                                                                                  \
                n00b_free(sys);                                                                    \
            }                                                                                      \
            else {                                                                                 \
                err_code = N00B_CONDUIT_ERR_TIMEOUT;                                               \
            }                                                                                      \
            n00b_conduit_sub_cancel(handle);                                                       \
            n00b_conduit_inbox_destroy(T, inbox);                                                   \
            n00b_free(inbox);                                                                       \
            return n00b_result_err(n00b_conduit_message_t(T) *, err_code);                         \
        }                                                                                          \
                                                                                                   \
        n00b_conduit_sub_cancel(handle);                                                           \
        n00b_conduit_inbox_destroy(T, inbox);                                                       \
        n00b_free(inbox);                                                                           \
        return n00b_result_ok(n00b_conduit_message_t(T) *, msg);                                   \
    }                                                                                              \
                                                                                                   \
    /* Async read result struct: carries both inbox and handle. */                                \
    n00b_conduit_async_read_t(T) {                                                                 \
        n00b_conduit_inbox_t(T)  *inbox;                                                           \
        n00b_conduit_sub_handle_t handle;                                                          \
    };                                                                                             \
                                                                                                   \
    /**                                                                                            \
     * @brief Non-blocking read: subscribe and return inbox + handle.                              \
     *                                                                                             \
     * The caller owns the inbox and polls/waits on it.  The                                       \
     * subscription is not one-shot — caller must cancel the handle                                \
     * via `n00b_conduit_sub_cancel()` when done.                                                  \
     *                                                                                             \
     * @param topic  Typed topic to read from.                                                     \
     * @kw operations   Operation filter (default: all).                                           \
     * @return Ok({inbox, handle}) or Err(error_code).                                             \
     */                                                                                            \
    static inline n00b_result_t(n00b_conduit_async_read_t(T))                                      \
    _N00B_RW_FN(read_async, T)(n00b_conduit_topic_t(T)  *topic,                                   \
                                n00b_conduit_inbox_t(T)  *inbox) _kargs                            \
    {                                                                                              \
        uint32_t operations = N00B_CONDUIT_OP_ALL;                                                 \
    }                                                                                              \
    {                                                                                              \
        if (!topic || !inbox) {                                                                    \
            return n00b_result_err(n00b_conduit_async_read_t(T),                                   \
                                  N00B_CONDUIT_ERR_NULL_ARG);                                      \
        }                                                                                          \
                                                                                                   \
        n00b_conduit_sub_handle_t handle =                                                         \
            _N00B_TOPIC_FN(subscribe, T)(                                                          \
                topic, inbox,                                                                      \
                (n00b_conduit_sub_config_t){                                                       \
                    .operations = operations,                                                       \
                });                                                                                \
                                                                                                   \
        if (handle == N00B_CONDUIT_INVALID_SUB_HANDLE) {                                           \
            return n00b_result_err(n00b_conduit_async_read_t(T),                                   \
                n00b_conduit_topic_is_active((n00b_conduit_topic_base_t *)topic)                    \
                    ? N00B_CONDUIT_ERR_ALLOC                                                        \
                    : N00B_CONDUIT_ERR_CLOSED);                                                     \
        }                                                                                          \
                                                                                                   \
        return n00b_result_ok(n00b_conduit_async_read_t(T),                                        \
            ((n00b_conduit_async_read_t(T)){ .inbox = inbox, .handle = handle }));                  \
    }                                                                                              \
                                                                                                   \
    /**                                                                                            \
     * @brief Non-blocking write: try-claim publisher, deliver, yield.                             \
     *                                                                                             \
     * Fire-and-forget publish.  Returns `ERR_ALREADY_CLAIMED` if                                  \
     * another thread holds the publisher.                                                         \
     *                                                                                             \
     * @param topic    Typed topic to write to.                                                    \
     * @param payload  Payload value to deliver.                                                   \
     * @return Ok(true) on success, or Err(error_code).                                            \
     */                                                                                            \
    static inline n00b_result_t(bool)                                                              \
    _N00B_RW_FN(write_async, T)(n00b_conduit_topic_t(T) *topic,                                   \
                                 T                        payload)                                 \
    {                                                                                              \
        if (!topic) {                                                                              \
            return n00b_result_err(bool, N00B_CONDUIT_ERR_NULL_ARG);                               \
        }                                                                                          \
                                                                                                   \
        n00b_conduit_topic_base_t *base = (n00b_conduit_topic_base_t *)topic;                      \
        if (!n00b_conduit_topic_is_active(base)) {                                                 \
            return n00b_result_err(bool, N00B_CONDUIT_ERR_CLOSED);                                 \
        }                                                                                          \
                                                                                                   \
        n00b_result_t(n00b_conduit_publisher_t *) pub_res =                                        \
            n00b_conduit_publish_try_claim(base);                                                  \
        if (n00b_result_is_err(pub_res)) {                                                         \
            return n00b_result_err(bool, n00b_result_get_err(pub_res));                             \
        }                                                                                          \
        n00b_conduit_publisher_t *pub = n00b_result_get(pub_res);                                  \
                                                                                                   \
        n00b_allocator_t *_msg_alloc = base->conduit                                              \
            ? base->conduit->allocator                                                            \
            : (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;                               \
        n00b_conduit_message_t(T) *msg = n00b_alloc_with_opts(                                     \
            n00b_conduit_message_t(T),                                                            \
            &(n00b_alloc_opts_t){.allocator = _msg_alloc});                                       \
        msg->header.type       = N00B_CONDUIT_MSG_USER;                                            \
        msg->header.topic      = base;                                                             \
        msg->header.generation = n00b_conduit_topic_generation(base);                              \
        msg->header.epoch      = n00b_conduit_topic_epoch(base);                                   \
        msg->header.timestamp  = 0;                                                                \
        msg->header.next       = nullptr;                                                          \
        msg->payload           = payload;                                                          \
                                                                                                   \
        _N00B_TOPIC_FN(deliver, T)(topic, msg, N00B_CONDUIT_OP_ALL);                               \
                                                                                                   \
        n00b_conduit_publish_yield(pub);                                                           \
        return n00b_result_ok(bool, true);                                                         \
    }                                                                                              \
                                                                                                   \
    /**                                                                                            \
     * @brief Write a payload to a typed topic.                                                    \
     *                                                                                             \
     * By default (sync=true): claims the publisher, delivers the                                  \
     * message, waits for the done-topic completion signal, then                                   \
     * returns.  Pass `.sync = false` for fire-and-forget (delegates                               \
     * to `write_async`).                                                                          \
     *                                                                                             \
     * @param topic    Typed topic to write to.                                                    \
     * @param payload  Payload value to deliver.                                                   \
     * @kw timeout_ms  Maximum wait for publisher claim (0 = try, >0 = block).                     \
     * @kw sync        If false, delegate to write_async (default: true).                          \
     * @return Ok(true) on success, or Err(error_code).                                            \
     */                                                                                            \
    static inline n00b_result_t(bool)                                                              \
    _N00B_RW_FN(write, T)(n00b_conduit_topic_t(T) *topic,                                         \
                           T                        payload) _kargs                                \
    {                                                                                              \
        int  timeout_ms = 0;                                                                       \
        bool sync       = true;                                                                    \
    }                                                                                              \
    {                                                                                              \
        /* Non-blocking path: delegate to write_async. */                                          \
        if (!sync) {                                                                               \
            return _N00B_RW_FN(write_async, T)(topic, payload);                                    \
        }                                                                                          \
                                                                                                   \
        if (!topic) {                                                                              \
            return n00b_result_err(bool, N00B_CONDUIT_ERR_NULL_ARG);                               \
        }                                                                                          \
                                                                                                   \
        n00b_conduit_topic_base_t *base = (n00b_conduit_topic_base_t *)topic;                      \
        if (!n00b_conduit_topic_is_active(base)) {                                                 \
            return n00b_result_err(bool, N00B_CONDUIT_ERR_CLOSED);                                 \
        }                                                                                          \
                                                                                                   \
        /* Set up a one-shot subscription on the done topic BEFORE                 */              \
        /* publishing, so we don't miss the completion signal.                     */              \
        /* Done topics always carry n00b_conduit_topic_base_t *.                    */              \
        /* Inbox MUST be heap-allocated (system pool) — its embedded CV             */              \
        /* participates in lock accounting; stack allocation leaves                 */              \
        /* dangling pointers in the thread's exclusive_locks chain.                */              \
        n00b_conduit_topic_t(n00b_conduit_topic_base_t *) *done_tp =                               \
            (n00b_conduit_topic_t(n00b_conduit_topic_base_t *) *)                                  \
                n00b_atomic_load(&base->done_topic);                                              \
        n00b_conduit_inbox_t(n00b_conduit_topic_base_t *) *done_inbox = nullptr;                   \
        n00b_conduit_sub_handle_t done_handle = N00B_CONDUIT_INVALID_SUB_HANDLE;                  \
                                                                                                   \
        if (done_tp) {                                                                             \
            n00b_allocator_t *_cp =                                                                \
                (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;                             \
            done_inbox = n00b_alloc_with_opts(                                                     \
                n00b_conduit_inbox_t(n00b_conduit_topic_base_t *),                                 \
                &(n00b_alloc_opts_t){.allocator = _cp});                                           \
            n00b_conduit_inbox_init(n00b_conduit_topic_base_t *,                                   \
                done_inbox, base->conduit,                                                         \
                N00B_CONDUIT_BP_DROP_NEWEST, 1);                                                   \
            done_handle = n00b_conduit_subscribe(n00b_conduit_topic_base_t *,                      \
                done_tp, done_inbox,                                                               \
                .flags = N00B_CONDUIT_SUB_F_ONE_SHOT);                                             \
            if (done_handle == N00B_CONDUIT_INVALID_SUB_HANDLE) {                                  \
                n00b_err_t _err = n00b_conduit_topic_is_active(                                    \
                    (n00b_conduit_topic_base_t *)done_tp)                                          \
                        ? N00B_CONDUIT_ERR_ALLOC                                                    \
                        : N00B_CONDUIT_ERR_CLOSED;                                                  \
                n00b_conduit_inbox_destroy(n00b_conduit_topic_base_t *, done_inbox);                \
                n00b_free(done_inbox);                                                             \
                return n00b_result_err(bool, _err);                                                \
            }                                                                                      \
        }                                                                                          \
                                                                                                   \
        /* Claim publisher (blocking or try). */                                                   \
        n00b_result_t(n00b_conduit_publisher_t *) pub_res;                                         \
        if (timeout_ms > 0) {                                                                      \
            pub_res = n00b_conduit_publish_claim(base);                                            \
        }                                                                                          \
        else {                                                                                     \
            pub_res = n00b_conduit_publish_try_claim(base);                                        \
        }                                                                                          \
        if (n00b_result_is_err(pub_res)) {                                                         \
            if (done_handle != N00B_CONDUIT_INVALID_SUB_HANDLE) {                                  \
                n00b_conduit_sub_cancel(done_handle);                                              \
            }                                                                                      \
            if (done_inbox) {                                                                       \
                n00b_conduit_inbox_destroy(n00b_conduit_topic_base_t *, done_inbox);                \
                n00b_free(done_inbox);                                                             \
            }                                                                                      \
            return n00b_result_err(bool, n00b_result_get_err(pub_res));                             \
        }                                                                                          \
        n00b_conduit_publisher_t *pub = n00b_result_get(pub_res);                                  \
                                                                                                   \
        /* Allocate and fill the message. */                                                       \
        n00b_allocator_t *_msg_alloc = base->conduit                                              \
            ? base->conduit->allocator                                                            \
            : (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;                               \
        n00b_conduit_message_t(T) *msg = n00b_alloc_with_opts(                                     \
            n00b_conduit_message_t(T),                                                            \
            &(n00b_alloc_opts_t){.allocator = _msg_alloc});                                       \
        msg->header.type       = N00B_CONDUIT_MSG_USER;                                            \
        msg->header.topic      = base;                                                             \
        msg->header.generation = n00b_conduit_topic_generation(base);                              \
        msg->header.epoch      = n00b_conduit_topic_epoch(base);                                   \
        msg->header.timestamp  = 0;                                                                \
        msg->header.next       = nullptr;                                                          \
        msg->payload           = payload;                                                          \
                                                                                                   \
        /* Deliver to all matching subscribers. */                                                 \
        _N00B_TOPIC_FN(deliver, T)(topic, msg, N00B_CONDUIT_OP_ALL);                               \
                                                                                                   \
        n00b_conduit_publish_yield(pub);                                                           \
                                                                                                   \
        /* Wait for the done topic to signal completion. */                                        \
        /*                                                                         */              \
        /* n00b#496: this wait used to have no exit but a message, and a waiter     */              \
        /* can be left with no sender. Done topics are shared per upstream topic    */              \
        /* and the subscription here is ONE-SHOT, so a single completion delivery   */              \
        /* satisfies and then cancels EVERY subscriber on that topic -- delivery is */              \
        /* OP_ALL, and _N00B_SUB_FN(deliver) marks a one-shot REMOVED, which the    */              \
        /* deliver loop then cancels and compacts out of the list. With several     */              \
        /* threads printing to one fd, a waiter can end up holding a valid handle   */              \
        /* for a subscription that is no longer in the topic's subscriber set.      */              \
        /* Measured on arm64 Linux: done_inbox->count == 0 with head and tail null, */              \
        /* done_tp->subscriptions.len == 0, and done_handle still 11 -- nothing can  */             \
        /* ever signal it, so futex_wait_forever never returns.                     */              \
        /*                                                                         */              \
        /* So also stop when the subscription is gone. Checked INSIDE the cv lock   */              \
        /* as well as outside: a sender can cancel between the outer test and the   */              \
        /* sleep, and that window is the whole bug.                                 */              \
        /*                                                                         */              \
        /* Returning Ok on that path is deliberate. The payload was published and   */              \
        /* delivered to the sink BEFORE this wait, so the bytes are in flight and   */              \
        /* will be written; we have only lost the ability to observe completion.    */              \
        /* An Err here would be worse than the hang it replaces -- print.c treats   */              \
        /* Err as "never delivered" and writes the line again through its fallback  */              \
        /* (src/conduit/print.c), so the line would be DUPLICATED, which is the     */              \
        /* failure #491 and #493 went to some trouble to avoid.                     */              \
        if (done_inbox) {                                                                          \
            int64_t _done_deadline_ms =                                                            \
                (int64_t)(n00b_ns_timestamp() / N00B_NS_PER_MS)                                    \
                + (int64_t)N00B_CONDUIT_DONE_WAIT_MS;                                              \
            while (!n00b_conduit_inbox_has_msg(                                                    \
                        n00b_conduit_topic_base_t *, done_inbox)) {                                \
                if (n00b_conduit_inbox_has_sys(done_inbox))                                        \
                    break;                                                                         \
                if (!n00b_conduit_sub_is_active(done_handle))                                      \
                    break;                                                                         \
                if ((int64_t)(n00b_ns_timestamp() / N00B_NS_PER_MS) >= _done_deadline_ms)          \
                    break;                                                                         \
                n00b_condition_lock(&done_inbox->cv);                                              \
                if (!n00b_conduit_inbox_has_msg(                                                    \
                        n00b_conduit_topic_base_t *, done_inbox) &&                                \
                    !n00b_conduit_inbox_has_sys(done_inbox) &&                                     \
                    n00b_conduit_sub_is_active(done_handle)) {                                     \
                    n00b_condition_wait(&done_inbox->cv,                                            \
                                        .timeout_ms  = N00B_CONDUIT_DONE_POLL_MS,                  \
                                        .auto_unlock = true);                                      \
                }                                                                                  \
                else {                                                                             \
                    n00b_condition_unlock(&done_inbox->cv);                                        \
                }                                                                                  \
            }                                                                                      \
            auto _done_msg = n00b_conduit_inbox_pop_msg(                                           \
                n00b_conduit_topic_base_t *, done_inbox);                                          \
            if (_done_msg) {                                                                        \
                n00b_free(_done_msg);                                                              \
            }                                                                                      \
        }                                                                                          \
        if (done_handle != N00B_CONDUIT_INVALID_SUB_HANDLE) {                                      \
            n00b_conduit_sub_cancel(done_handle);                                                  \
        }                                                                                          \
        if (done_inbox) {                                                                           \
            n00b_conduit_inbox_destroy(n00b_conduit_topic_base_t *, done_inbox);                    \
            n00b_free(done_inbox);                                                                 \
        }                                                                                          \
                                                                                                   \
        return n00b_result_ok(bool, true);                                                         \
    }

// ============================================================================
// User-facing macros
// ============================================================================

/**
 * @brief Blocking read from a typed topic.
 *
 * Returns `n00b_conduit_read_result_t(T)` — Ok(message) or Err(code).
 */
#define n00b_conduit_read(T, topic, ...) \
    _N00B_RW_FN(read, T)(topic, ##__VA_ARGS__)

/**
 * @brief Async read: subscribe to a typed topic with an existing inbox.
 *
 * Returns `n00b_result_t(n00b_conduit_async_read_t(T))` containing
 * both the inbox pointer and the subscription handle.  The caller
 * must cancel the handle via `n00b_conduit_sub_cancel()` when done.
 */
#define n00b_conduit_read_async(T, topic, inbox, ...) \
    _N00B_RW_FN(read_async, T)(topic, inbox, ##__VA_ARGS__)

/**
 * @brief Blocking write to a typed topic.
 *
 * Claims publisher, delivers payload, yields. Returns Ok(true) or Err(code).
 */
#define n00b_conduit_write(T, topic, payload, ...) \
    _N00B_RW_FN(write, T)(topic, payload, ##__VA_ARGS__)

/**
 * @brief Non-blocking write to a typed topic.
 *
 * Try-claims publisher, delivers payload, yields immediately.
 * Returns `ERR_ALREADY_CLAIMED` if the publisher is busy.
 */
#define n00b_conduit_write_async(T, topic, payload) \
    _N00B_RW_FN(write_async, T)(topic, payload)

// ============================================================================
// Instantiate for common payload types
// ============================================================================

// Done topic payload (completion signals carry originating topic pointer)
N00B_CONDUIT_RW_IMPL(n00b_conduit_topic_base_t *);

// Buffer payload (byte-oriented pipelines, used by print)
N00B_CONDUIT_RW_IMPL(n00b_buffer_t *);

// FD payloads (read topic uses n00b_buffer_t * directly, RW_IMPL above)
N00B_CONDUIT_RW_IMPL(n00b_conduit_fd_stream_payload_t);
N00B_CONDUIT_RW_IMPL(n00b_conduit_fd_status_payload_t);
N00B_CONDUIT_RW_IMPL(n00b_conduit_fd_write_payload_t);
N00B_CONDUIT_RW_IMPL(n00b_conduit_fd_write_done_payload_t);

// IO events
N00B_CONDUIT_RW_IMPL(n00b_conduit_io_payload_t);

// Timer / signal / user event
N00B_CONDUIT_RW_IMPL(n00b_conduit_timer_payload_t);
N00B_CONDUIT_RW_IMPL(n00b_conduit_user_event_payload_t);

#ifndef _WIN32
N00B_CONDUIT_RW_IMPL(n00b_conduit_signal_payload_t);
#endif
