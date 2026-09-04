#pragma once

/**
 * @file n00b_hostmeta_aws.h
 * @brief AWS ARN parsing / rebuilding used by the AWS collectors.
 *
 * ECS reports a container ARN that carries the account and region;
 * Lambda has to build function / version / log-stream ARNs out of the
 * caller-identity ARN. Both need the same six-field split, so it lives
 * here rather than twice inside the collectors.
 *
 * Format (AWS "ARN format" reference):
 *   `arn:PARTITION:SERVICE:REGION:ACCOUNT:RESOURCE`
 * where `RESOURCE` may itself contain `:` separators and is kept whole.
 */

#include <n00b.h>

#include "adt/option.h"
#include "core/string.h"

/**
 * @brief A parsed Amazon Resource Name.
 *
 * Fields are never null after a successful parse, but `region` and
 * `account` are legitimately empty for global services (IAM roles have
 * no region; S3 bucket ARNs have neither).
 */
typedef struct {
    n00b_string_t *partition;
    n00b_string_t *service;
    n00b_string_t *region;
    n00b_string_t *account;
    n00b_string_t *resource;
} n00b_hostmeta_arn_t;

/**
 * @brief Parse an ARN.
 *
 * @return The parsed ARN, or an unset option if @p s is not an ARN
 *         (missing `arn:` prefix or fewer than six `:`-separated fields).
 */
extern n00b_option_t(n00b_hostmeta_arn_t *)
    n00b_hostmeta_arn_parse(n00b_string_t *s);

/** @brief Render an ARN back to its `arn:...` string form. */
extern n00b_string_t *n00b_hostmeta_arn_format(n00b_hostmeta_arn_t *arn);

/**
 * @brief Copy @p arn, replacing the named fields.
 *
 * @kw service   Replacement service, or nullptr to keep.
 * @kw region    Replacement region, or nullptr to keep.
 * @kw account   Replacement account, or nullptr to keep.
 * @kw resource  Replacement resource, or nullptr to keep.
 * @kw partition Replacement partition, or nullptr to keep.
 */
extern n00b_hostmeta_arn_t *
n00b_hostmeta_arn_with(n00b_hostmeta_arn_t *arn) _kargs
{
    n00b_string_t *partition = nullptr;
    n00b_string_t *service   = nullptr;
    n00b_string_t *region    = nullptr;
    n00b_string_t *account   = nullptr;
    n00b_string_t *resource  = nullptr;
};
