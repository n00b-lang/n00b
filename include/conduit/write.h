/**
 * @file write.h
 * @brief High-level write API for typed conduit topics.
 *
 * `n00b_write(T, topic, payload, ...)` publishes a typed payload to a
 * typed topic.  Synchronous by default — blocks until the downstream
 * subscriber has processed the message.  Pass `.sync = false` for
 * fire-and-forget.
 *
 * Usage:
 * @code
 *     n00b_write(n00b_buffer_t *, stdout_topic, buf);
 *     n00b_write(n00b_buffer_t *, topic, buf, .sync = false);
 * @endcode
 */
#pragma once

#include "conduit/rw.h"

/**
 * @brief Write a typed payload to a typed topic.
 *
 * Thin wrapper around `n00b_conduit_write` that enforces type safety
 * and defaults to synchronous delivery.
 *
 * @param T        Payload type.
 * @param topic    Typed topic to write to.
 * @param payload  Payload value to deliver.
 * @param ...      Optional kwargs: `.sync = false`, `.timeout_ms = N`.
 */
#define n00b_write(T, topic, payload, ...) \
    n00b_conduit_write(T, topic, payload, .timeout_ms = 0, ##__VA_ARGS__)
