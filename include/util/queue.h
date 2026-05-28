/**
 * @file util/queue.h
 * @brief Broker-neutral message-queue interface.
 *
 * Mirrors the receive / delete / change-visibility shape of
 * SNS-fanout-into-SQS so consumer code stays broker-agnostic. The
 * fake (in-process FIFO) impl ships with n00b for tests and local
 * development; concrete brokers expose a factory that returns the
 * same `n00b_queue_t *`.
 *
 * Factories:
 *   - `n00b_queue_new_fake()` — n00b proper.
 *   - `n00b_aws_sqs_queue(cfg, queue_url)` — libn00b_aws.
 *
 * Per D-062 (amended 2026-05-27): the broker-neutral abstraction
 * lives in n00b from day 1. SKP-side code (or any consumer) holds a
 * `n00b_queue_t *` and never knows which backend backs it.
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "core/alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct n00b_queue_t n00b_queue_t;

typedef struct {
    n00b_string_t *body;
    n00b_string_t *receipt_handle;
    uint32_t       receive_count;
} n00b_queue_message_t;

/**
 * @brief Construct a fake in-process FIFO queue.
 *
 * @kw allocator  Override n00b's default allocator. `nullptr` keeps
 *                the default arena (most callers).
 */
extern n00b_queue_t *n00b_queue_new_fake() _kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Receive up to @p max_messages messages from @p q.
 *
 * @return 0 on success, negative on error. `*out_count` reports how
 *         many slots in @p out were populated.
 */
extern int
n00b_queue_receive_batch(n00b_queue_t         *q,
                         int                   max_messages,
                         int                   wait_seconds,
                         int                   visibility_seconds,
                         n00b_queue_message_t *out,
                         int                  *out_count);

/** @brief Delete by receipt handle. */
extern int
n00b_queue_delete(n00b_queue_t *q, n00b_string_t *receipt_handle);

/** @brief Reset / extend visibility on an in-flight message. */
extern int
n00b_queue_change_visibility(n00b_queue_t   *q,
                             n00b_string_t *receipt_handle,
                             int            seconds);

/**
 * @brief Publish a message body onto the queue.
 *
 * Broker-neutral: the fake backend appends to the in-process FIFO
 * (same as `n00b_queue_enqueue_fake`), the SQS backend calls
 * `n00b_aws_sqs_send_message`. Returns 0 on success, negative on
 * error.
 */
extern int
n00b_queue_send(n00b_queue_t *q, n00b_string_t *body);

/**
 * @brief Enqueue into a fake queue (test/dev only).
 *
 * Returns negative when the backing queue is not the fake impl.
 */
extern int
n00b_queue_enqueue_fake(n00b_queue_t *q, n00b_string_t *body);

/** @brief Number of currently-visible-or-in-flight items (fake only). */
extern size_t n00b_queue_pending_count(n00b_queue_t *q);

/* ------------------------------------------------------------------
 * Backend vtable (used by factories in n00b and downstream substrate
 * libraries like libn00b_aws). Consumers do NOT call these directly.
 * ------------------------------------------------------------------ */

typedef struct {
    int    (*receive_batch)(void *self, int max_messages, int wait_seconds,
                            int visibility_seconds,
                            n00b_queue_message_t *out, int *out_count);
    int    (*delete_one)(void *self, n00b_string_t *receipt_handle);
    int    (*change_visibility)(void *self, n00b_string_t *receipt_handle,
                                int seconds);
    int    (*send)(void *self, n00b_string_t *body);
    /* Optional fake-only operations. `nullptr` on non-fake backends. */
    int    (*enqueue_fake)(void *self, n00b_string_t *body);
    size_t (*pending_count)(void *self);
} n00b_queue_vtable_t;

/**
 * @brief Allocate a fresh `n00b_queue_t` wired to @p vtable + @p self.
 *
 * Used by backend factories (`n00b_queue_new_fake`, the SQS factory
 * in libn00b_aws, future backends). Not for direct consumer use.
 *
 * @kw allocator  Override n00b's default allocator. Backends should
 *                forward whatever the consumer-facing factory was
 *                handed.
 */
extern n00b_queue_t *
n00b_queue_new_backend(const n00b_queue_vtable_t *vtable, void *self) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

#ifdef __cplusplus
}
#endif
