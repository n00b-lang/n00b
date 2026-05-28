/**
 * @file aws/n00b_aws_sns.h
 * @brief SNS — Simple Notification Service.
 *
 * Minimal initial wrap: the two ops the Crayon worker / SKP API
 * pattern needs (subscribe an SQS queue to a topic; publish a
 * message to a topic). The Rust shim already exports the full SNS
 * Smithy surface; more wraps land here as consumers need them.
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "adt/result.h"
#include "util/queue.h"
#include "aws/n00b_aws.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Subscribe an endpoint (typically an SQS queue ARN) to an
 *        SNS topic. Idempotent: the SDK returns the existing
 *        subscription ARN when the same endpoint is already
 *        subscribed.
 *
 * @param topic_arn  e.g. `arn:aws:sns:us-east-1:...:crayon-ingest-topic`.
 * @param protocol   `sqs`, `email`, `https`, ...
 * @param endpoint   For `sqs`: the queue ARN. Per-protocol shape
 *                   otherwise.
 * @return  The subscription ARN on success, or an
 *          `n00b_aws_status_t` error.
 */
extern n00b_result_t(n00b_string_t *)
n00b_aws_sns_subscribe(n00b_aws_config_t *cfg,
                       n00b_string_t     *topic_arn,
                       n00b_string_t     *protocol,
                       n00b_string_t     *endpoint);

/**
 * @brief Publish @p message to @p topic_arn.
 *
 * @return  The SNS-assigned message id on success, or an
 *          `n00b_aws_status_t` error.
 */
extern n00b_result_t(n00b_string_t *)
n00b_aws_sns_publish(n00b_aws_config_t *cfg,
                     n00b_string_t     *topic_arn,
                     n00b_string_t     *message);

/**
 * @brief Wrap an SNS topic as a producer-only `n00b_queue_t`.
 *
 * The returned handle's `send` slot publishes to @p topic_arn via
 * `n00b_aws_sns_publish`. All other vtable ops (receive_batch,
 * delete, change_visibility, enqueue_fake, pending_count) are NULL —
 * callers that consume from the queue should use the consumer-side
 * factory (e.g. `n00b_aws_sqs_queue` over the subscribed queue).
 *
 * Used by the SKP public API service so its `n00b_queue_send` call
 * site is identical whether it's publishing to an in-process fake,
 * a direct SQS queue, or an SNS topic.
 *
 * @kw allocator  Override n00b's default allocator. NULL keeps the
 *                default arena.
 */
extern n00b_queue_t *
n00b_aws_sns_publisher_queue(n00b_aws_config_t *cfg,
                             n00b_string_t     *topic_arn) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

#ifdef __cplusplus
}
#endif
