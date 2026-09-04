/** @file src/hostmeta/aws_ecs.c — AWS ECS task metadata collector.
 *
 *  ECS injects `ECS_CONTAINER_METADATA_URI_V4` (or the older `_URI`)
 *  into every container; the endpoint behind it describes the
 *  container, its task, and the task's resource usage. Presence of
 *  that variable is also the most reliable signal that we are on ECS
 *  rather than plain EC2 — the generic IMDS probe cannot tell the
 *  difference on its own, which is why this collector registers ahead
 *  of it.
 *
 *  Task metadata v2 is deliberately ignored: it is unmaintained, and
 *  v3/v4 cover every currently supported launch type.
 *
 *  https://docs.aws.amazon.com/AmazonECS/latest/developerguide/task-metadata-endpoint-v4.html
 */

#define N00B_USE_INTERNAL_API

#include "hostmeta/n00b_hostmeta_builtins.h"

#include "hostmeta/n00b_hostmeta_aws.h"
#include "internal/hostmeta/hostmeta_internal.h"
#include "text/strings/format.h"
#include "text/strings/string_ops.h"

/** The endpoint base, preferring v4 over v3. */
static n00b_string_t *
ecs_base_url(void)
{
    n00b_string_t *v4 = n00b_hostmeta_env("ECS_CONTAINER_METADATA_URI_V4");
    if (v4 != nullptr) {
        return v4;
    }
    return n00b_hostmeta_env("ECS_CONTAINER_METADATA_URI");
}

/**
 * Fetch and parse one sub-resource of the ECS metadata endpoint.
 *
 * @param err_out Receives failure text when the fetch or parse fails.
 */
static n00b_json_node_t *
ecs_get(n00b_hostmeta_ctx_t *ctx,
        n00b_string_t       *base,
        const char          *suffix,
        n00b_string_t      **err_out)
{
    n00b_string_t *url = suffix[0] == '\0'
                           ? base
                           : n00b_cformat("[|#|][|#|]",
                                          base,
                                          n00b_hostmeta_str(suffix));

    n00b_hostmeta_fetch_t f = n00b_hostmeta_fetch(ctx, url, .strict = true);
    if (!f.ok) {
        *err_out = f.error;
        return nullptr;
    }

    n00b_json_node_t *node = n00b_hostmeta_parse_json(f.body, err_out);
    if (node == nullptr && *err_out == nullptr) {
        *err_out = n00b_cformat("[|#|] returned invalid JSON", url);
    }
    return node;
}

/**
 * Assemble `{container, task, task/stats}`.
 *
 * The container document is required — without it there is nothing to
 * report and the caller records a single failure. The task and stats
 * documents are optional: a task can legitimately deny them, so each
 * missing one records its own failure and the rest is still reported.
 *
 * @return The metadata object, or nullptr when not on ECS or when the
 *         container document could not be read.
 */
static n00b_json_node_t *
ecs_read_metadata(n00b_hostmeta_ctx_t *ctx,
                  n00b_string_t       *failure_key)
{
    n00b_string_t *base = ecs_base_url();
    if (base == nullptr) {
        return nullptr;
    }

    n00b_string_t    *err       = nullptr;
    n00b_json_node_t *container = ecs_get(ctx, base, "", &err);
    if (container == nullptr) {
        n00b_hostmeta_add_failure(
            ctx,
            failure_key,
            "ECS_METADATA_ERROR",
            err,
            "The ECS container metadata endpoint was unreachable or "
            "returned invalid data");
        return nullptr;
    }

    n00b_json_node_t *out = n00b_json_object_new();
    n00b_json_object_put(out, "container", container);

    err                 = nullptr;
    n00b_json_node_t *task = ecs_get(ctx, base, "/task", &err);
    if (task != nullptr) {
        n00b_json_object_put(out, "task", task);
    }
    else {
        n00b_hostmeta_add_failure(
            ctx,
            failure_key,
            "ECS_TASK_METADATA_ERROR",
            err,
            "The ECS task metadata endpoint was unreachable or returned "
            "invalid data");
    }

    err                     = nullptr;
    n00b_json_node_t *stats = ecs_get(ctx, base, "/task/stats", &err);
    if (stats != nullptr) {
        n00b_json_object_put(out, "task/stats", stats);
    }
    else {
        n00b_hostmeta_add_failure(
            ctx,
            failure_key,
            "ECS_TASK_STATS_METADATA_ERROR",
            err,
            "The ECS task/stats metadata endpoint was unreachable or "
            "returned invalid data");
    }

    return out;
}

/** Wrap the ECS blob as `{"aws_ecs": <blob>}`. */
static n00b_json_node_t *
wrap_cloud_data(n00b_json_node_t *ecs)
{
    n00b_json_node_t *cloud = n00b_json_object_new();
    n00b_json_object_put(cloud, "aws_ecs", ecs);
    return cloud;
}

static void
ecs_chalk_time(n00b_hostmeta_collector_t *self, n00b_hostmeta_ctx_t *ctx)
{
    (void)self;

    n00b_string_t *key = r"CLOUD_METADATA_WHEN_CHALKED";
    if (!n00b_hostmeta_subscribed(ctx, key)) {
        return;
    }

    n00b_json_node_t *ecs = ecs_read_metadata(ctx, key);
    if (ecs != nullptr) {
        n00b_hostmeta_put(ctx, key, wrap_cloud_data(ecs));
    }
}

static void
ecs_run_time(n00b_hostmeta_collector_t *self, n00b_hostmeta_ctx_t *ctx)
{
    (void)self;

    // `_OP_CLOUD_*` keys are already run-time-scoped by name; they do
    // not take the phase prefix a second time.
    n00b_string_t *key = r"_OP_CLOUD_METADATA";

    n00b_json_node_t *ecs = ecs_read_metadata(ctx, key);
    if (ecs == nullptr) {
        return;
    }

    n00b_hostmeta_put(ctx, key, wrap_cloud_data(ecs));
    n00b_hostmeta_put_string(ctx, r"_OP_CLOUD_PROVIDER", r"aws");
    n00b_hostmeta_put_string(ctx, r"_OP_CLOUD_PROVIDER_SERVICE_TYPE", r"aws_ecs");

    // The container ARN is the only place the endpoint states the
    // account and region outright.
    n00b_json_node_t *arn_node = n00b_hostmeta_json_path(ecs,
                                                         "container.ContainerARN");
    if (!n00b_json_is_string(arn_node)) {
        return;
    }

    auto parsed = n00b_hostmeta_arn_parse(n00b_json_as_string(arn_node));
    if (!n00b_option_is_set(parsed)) {
        return;
    }
    n00b_hostmeta_arn_t *arn = n00b_option_get(parsed);

    n00b_hostmeta_put_string(ctx,
                             r"_OP_CLOUD_PROVIDER_ACCOUNT_INFO",
                             arn->account);
    n00b_hostmeta_put_string(ctx, r"_OP_CLOUD_PROVIDER_REGION", arn->region);
    n00b_hostmeta_put_string(ctx, r"_AWS_REGION", arn->region);
}

n00b_hostmeta_collector_t n00b_hostmeta_collector_aws_ecs = {
    .chalk_time = ecs_chalk_time,
    .run_time   = ecs_run_time,
};
