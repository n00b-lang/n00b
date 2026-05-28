/**
 * @file aws/n00b_aws_sqs.h
 * @brief SQS — Simple Queue Service.
 *
 * Phase 3c MVP coverage centred on what SKP's Phase 3d ingestion
 * worker needs (receive / delete / change_visibility / send /
 * get_queue_url / get_queue_attributes). The Rust shim
 * (`subprojects/n00b-aws-shim`) already exports the full Smithy
 * surface; the wraps below grow over time without API breakage.
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "adt/list.h"
#include "adt/result.h"
#include "util/queue.h"
#include "aws/n00b_aws.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * Message + batch shapes
 * ------------------------------------------------------------------ */

typedef struct {
    n00b_string_t *message_id;
    n00b_string_t *receipt_handle;
    n00b_string_t *body;
    n00b_string_t *md5_of_body;
    int32_t        approximate_receive_count;
    int64_t        sent_timestamp_ms;
    int64_t        first_receive_timestamp_ms;
} n00b_aws_sqs_message_t;

typedef struct {
    n00b_aws_sqs_message_t **messages;
    size_t                   count;
} n00b_aws_sqs_receive_batch_t;

typedef struct {
    n00b_string_t *message_id;
    n00b_string_t *md5_of_message_body;
    n00b_string_t *sequence_number;
} n00b_aws_sqs_send_result_t;

/* ------------------------------------------------------------------
 * Receive / delete / change-visibility / send (worker hot path)
 * ------------------------------------------------------------------ */

/**
 * @brief Receive up to @kw max_messages messages from @p queue_url.
 *
 * @kw max_messages         1-10. Default 10.
 * @kw wait_time_seconds    SQS long-poll. 0-20. Default 20.
 * @kw visibility_timeout   Per-receive visibility override (seconds).
 *                          0 → inherit queue default.
 * @kw receive_request_attempt_id  FIFO-only retry deduplication.
 */
extern n00b_result_t(n00b_aws_sqs_receive_batch_t *)
n00b_aws_sqs_receive(n00b_aws_config_t *cfg,
                     n00b_string_t     *queue_url) _kargs {
    int32_t         max_messages                = 10;
    int32_t         wait_time_seconds           = 20;
    int32_t         visibility_timeout          = 0;
    n00b_string_t  *receive_request_attempt_id  = nullptr;
};

/** @brief Delete a single message identified by @p receipt_handle. */
extern n00b_aws_status_t
n00b_aws_sqs_delete_message(n00b_aws_config_t *cfg,
                            n00b_string_t     *queue_url,
                            n00b_string_t     *receipt_handle);

/** @brief Extend / reset visibility on an in-flight message. */
extern n00b_aws_status_t
n00b_aws_sqs_change_message_visibility(n00b_aws_config_t *cfg,
                                       n00b_string_t     *queue_url,
                                       n00b_string_t     *receipt_handle,
                                       int32_t            visibility_timeout);

/** @brief Send @p body to @p queue_url. */
extern n00b_result_t(n00b_aws_sqs_send_result_t *)
n00b_aws_sqs_send_message(n00b_aws_config_t *cfg,
                          n00b_string_t     *queue_url,
                          n00b_string_t     *body) _kargs {
    int32_t         delay_seconds            = 0;
    n00b_string_t  *message_group_id         = nullptr;
    n00b_string_t  *message_deduplication_id = nullptr;
};

/* ------------------------------------------------------------------
 * Queue management (worker startup path)
 * ------------------------------------------------------------------ */

/**
 * @brief Build an SQS URL from a queue name (calls GetQueueUrl).
 *
 * @kw queue_owner_account  Optional cross-account owner account id.
 */
extern n00b_result_t(n00b_string_t *)
n00b_aws_sqs_get_queue_url(n00b_aws_config_t *cfg,
                           n00b_string_t     *queue_name) _kargs {
    n00b_string_t *queue_owner_account = nullptr;
};

/**
 * @brief Read queue attributes.
 *
 * Returns a flat alternating-key-value list:
 *   [key0, value0, key1, value1, ...]
 *
 * @kw attribute_names  Optional list of names to fetch. NULL/empty
 *                      requests `All`.
 */
extern n00b_result_t(n00b_list_t(n00b_string_t *) *)
n00b_aws_sqs_get_queue_attributes(n00b_aws_config_t *cfg,
                                  n00b_string_t     *queue_url) _kargs {
    n00b_list_t(n00b_string_t *) *attribute_names = nullptr;
};

/* ------------------------------------------------------------------
 * Broker-neutral queue factory
 * ------------------------------------------------------------------ */

/**
 * @brief Wrap an SQS queue in `n00b_queue_t`.
 *
 * The returned handle plugs into the generic receive / delete /
 * change_visibility surface declared in `<util/queue.h>`, so
 * consumer code (worker loops, test scaffolds) doesn't depend on
 * libn00b_aws's specific surface. The handle holds @p cfg and
 * @p queue_url by reference; both must outlive the returned queue
 * (the GC keeps them alive as long as anything references the
 * queue).
 *
 * @kw allocator  Override n00b's default allocator. NULL keeps the
 *                default arena.
 */
extern n00b_queue_t *
n00b_aws_sqs_queue(n00b_aws_config_t *cfg, n00b_string_t *queue_url) _kargs {
    n00b_allocator_t *allocator = nullptr;
};

#ifdef __cplusplus
}
#endif
