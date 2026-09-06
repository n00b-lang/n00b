/** @file src/hostmeta/aws_lambda.c — AWS Lambda collector.
 *
 *  Lambda has no instance metadata service. Everything it tells a
 *  function about itself arrives as environment variables, and those
 *  stop short of the account id — so the function, version, and
 *  log-stream ARNs cannot be assembled from the environment alone.
 *
 *  chalk closes that gap with `sts:GetCallerIdentity`. libn00b core
 *  does not depend on the optional AWS substrate, so the identity call
 *  is a caller-supplied hook (`ctx->caller_arn`): wire it to
 *  `n00b_aws_sts_get_caller_identity()` if you link `libn00b_aws`, and
 *  the ARN keys appear. Without it the env-derived keys are still
 *  collected and the ARN keys are recorded as failures, which is the
 *  honest answer rather than a silently short report.
 *
 *  https://docs.aws.amazon.com/lambda/latest/dg/configuration-envvars.html
 */

#define N00B_USE_INTERNAL_API

#include "hostmeta/n00b_hostmeta_builtins.h"

#include "hostmeta/n00b_hostmeta_aws.h"
#include "internal/hostmeta/hostmeta_internal.h"
#include "text/strings/format.h"
#include "text/strings/string_ops.h"

/** ARN keys that depend on resolving the caller identity. */
static const char *k_arn_keys[] = {
    "AWS_ACCOUNT_ID",
    "AWS_ROLE_ARN",
    "AWS_LAMBDA_FUNCTION_ARN",
    "AWS_LAMBDA_VERSION_ARN",
    "AWS_LAMBDA_LOG_STREAM_ARN",
};

static void
put_env_key(n00b_json_node_t *obj, const char *name)
{
    n00b_string_t *v = n00b_hostmeta_env(name);
    if (v != nullptr) {
        n00b_json_object_put(obj, name, n00b_json_string_new_from_n00b(v));
    }
}

static void
put_string_key(n00b_json_node_t *obj, const char *name, n00b_string_t *value)
{
    if (value != nullptr && value->u8_bytes > 0) {
        n00b_json_object_put(obj, name, n00b_json_string_new_from_n00b(value));
    }
}

/**
 * Reduce a caller-identity ARN to the ARN of the role behind it.
 *
 * An `assumed-role/NAME/SESSION` STS ARN names a session, not a
 * durable principal; the role it assumed is the stable identity, and
 * roles are global so they carry no region. Credentials that are
 * already a plain IAM user need no reduction.
 */
static n00b_hostmeta_arn_t *
role_arn_of(n00b_hostmeta_arn_t *caller)
{
    if (!n00b_unicode_str_starts_with(caller->resource, r"assumed-role/")) {
        return caller;
    }

    n00b_array_t(n00b_string_t *) parts = n00b_unicode_str_split(
        caller->resource,
        r"/");
    if (parts.len < 2) {
        return caller;
    }

    return n00b_hostmeta_arn_with(caller,
                                  .service  = r"iam",
                                  .resource = n00b_unicode_str_cat(
                                      r"role/",
                                      parts.data[1]));
}

/**
 * Collect the Lambda environment into a flat object.
 *
 * @return The metadata object, or nullptr when not running in Lambda.
 */
static n00b_json_node_t *
collect_lambda_metadata(n00b_hostmeta_ctx_t *ctx)
{
    n00b_string_t *function_name = n00b_hostmeta_env("AWS_LAMBDA_FUNCTION_NAME");
    n00b_string_t *function_version = n00b_hostmeta_env(
        "AWS_LAMBDA_FUNCTION_VERSION");

    if (function_name == nullptr || function_version == nullptr) {
        return nullptr;
    }

    n00b_string_t *region = n00b_hostmeta_env("AWS_REGION");
    if (region == nullptr) {
        region = n00b_hostmeta_env("AWS_DEFAULT_REGION");
    }
    n00b_string_t *log_group  = n00b_hostmeta_env("AWS_LAMBDA_LOG_GROUP_NAME");
    n00b_string_t *log_stream = n00b_hostmeta_env("AWS_LAMBDA_LOG_STREAM_NAME");

    n00b_json_node_t *out = n00b_json_object_new();

    put_string_key(out, "AWS_REGION", region);
    put_env_key(out, "AWS_LAMBDA_FUNCTION_NAME");
    put_env_key(out, "AWS_LAMBDA_FUNCTION_VERSION");
    put_env_key(out, "AWS_LAMBDA_FUNCTION_MEMORY_SIZE");
    put_env_key(out, "AWS_LAMBDA_LOG_GROUP_NAME");
    put_env_key(out, "AWS_LAMBDA_LOG_STREAM_NAME");
    put_env_key(out, "AWS_EXECUTION_ENV");
    put_env_key(out, "LAMBDA_TASK_ROOT");
    put_env_key(out, "LAMBDA_RUNTIME_DIR");

    if (ctx->caller_arn == nullptr) {
        for (size_t i = 0; i < sizeof(k_arn_keys) / sizeof(k_arn_keys[0]); i++) {
            n00b_hostmeta_add_failure(
                ctx,
                n00b_hostmeta_str(k_arn_keys[i]),
                "LAMBDA_NO_CALLER_IDENTITY",
                nullptr,
                "Lambda does not publish the AWS account id; set the "
                "`caller_arn` hook on the collection to resolve it via "
                "sts:GetCallerIdentity");
        }
        return out;
    }

    n00b_string_t *caller = ctx->caller_arn(ctx->caller_arn_user);
    auto           parsed = n00b_hostmeta_arn_parse(caller);
    if (!n00b_option_is_set(parsed)) {
        for (size_t i = 0; i < sizeof(k_arn_keys) / sizeof(k_arn_keys[0]); i++) {
            n00b_hostmeta_add_failure(
                ctx,
                n00b_hostmeta_str(k_arn_keys[i]),
                "LAMBDA_CALLER_IDENTITY_FAILED",
                caller,
                "The `caller_arn` hook did not return a parseable ARN");
        }
        return out;
    }

    n00b_hostmeta_arn_t *role = role_arn_of(n00b_option_get(parsed));

    // The role ARN carries the account but no region; the function's
    // ARNs are regional, so the region goes back on here.
    n00b_hostmeta_arn_t *function = n00b_hostmeta_arn_with(
        role,
        .service  = r"lambda",
        .region   = region,
        .resource = n00b_unicode_str_cat(r"function:", function_name));

    n00b_hostmeta_arn_t *version = n00b_hostmeta_arn_with(
        function,
        .resource = n00b_cformat("function:[|#|]:[|#|]",
                                 function_name,
                                 function_version));

    put_string_key(out, "AWS_ACCOUNT_ID", role->account);
    put_string_key(out, "AWS_ROLE_ARN", n00b_hostmeta_arn_format(role));
    put_string_key(out,
                   "AWS_LAMBDA_FUNCTION_ARN",
                   n00b_hostmeta_arn_format(function));
    put_string_key(out,
                   "AWS_LAMBDA_VERSION_ARN",
                   n00b_hostmeta_arn_format(version));

    if (log_group != nullptr && log_stream != nullptr) {
        n00b_hostmeta_arn_t *stream = n00b_hostmeta_arn_with(
            function,
            .service  = r"logs",
            .resource = n00b_cformat("log-group:[|#|]:log-stream:[|#|]",
                                     log_group,
                                     log_stream));
        put_string_key(out,
                       "AWS_LAMBDA_LOG_STREAM_ARN",
                       n00b_hostmeta_arn_format(stream));
    }

    return out;
}

static void
lambda_run_time(n00b_hostmeta_collector_t *self, n00b_hostmeta_ctx_t *ctx)
{
    (void)self;

    n00b_json_node_t *data = collect_lambda_metadata(ctx);
    if (data == nullptr) {
        return;
    }

    n00b_json_node_t *cloud = n00b_json_object_new();
    n00b_json_object_put(cloud, "aws_lambda", data);

    // `_OP_CLOUD_*` keys are already run-time-scoped by name.
    n00b_hostmeta_put(ctx, r"_OP_CLOUD_METADATA", cloud);
    n00b_hostmeta_put_string(ctx, r"_OP_CLOUD_PROVIDER", r"aws");
    n00b_hostmeta_put_string(ctx,
                             r"_OP_CLOUD_PROVIDER_SERVICE_TYPE",
                             r"aws_lambda");

    n00b_json_node_t *account = n00b_hostmeta_json_path(data, "AWS_ACCOUNT_ID");
    if (n00b_json_is_string(account)) {
        n00b_hostmeta_put_string(ctx,
                                 r"_OP_CLOUD_PROVIDER_ACCOUNT_INFO",
                                 n00b_json_as_string(account));
    }

    n00b_json_node_t *region = n00b_hostmeta_json_path(data, "AWS_REGION");
    if (n00b_json_is_string(region)) {
        n00b_hostmeta_put_string(ctx,
                                 r"_OP_CLOUD_PROVIDER_REGION",
                                 n00b_json_as_string(region));
        n00b_hostmeta_put_string(ctx, r"_AWS_REGION", n00b_json_as_string(region));
    }
}

n00b_hostmeta_collector_t n00b_hostmeta_collector_aws_lambda = {
    .run_time = lambda_run_time,
};
