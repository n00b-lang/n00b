/** @file src/hostmeta/module.c — collector registry and the collect
 *        facade.
 *
 *  Registration is an explicit module-init call rather than a
 *  `[[gnu::constructor]]`, matching the rest of libn00b: a host that
 *  wants a narrower registry builds one from the symbols in
 *  `hostmeta/n00b_hostmeta_builtins.h` instead of taking everything
 *  and filtering after the fact.
 *
 *  Registration order is collection order, and the first collector to
 *  set a key keeps it. The AWS service collectors therefore go first:
 *  ECS and Lambda know exactly which service they are, while the
 *  generic IMDS probe can only infer it.
 */

#define N00B_USE_INTERNAL_API

#include "hostmeta/n00b_hostmeta_builtins.h"

#include "core/alloc.h"
#include "internal/hostmeta/hostmeta_internal.h"
#include "text/strings/string_ops.h"
#include "util/panic.h"

/** Bounded so the registry needs no allocation or lock of its own. */
#define N00B_HOSTMETA_MAX_COLLECTORS 32

static n00b_hostmeta_collector_t *registry[N00B_HOSTMETA_MAX_COLLECTORS];
static size_t                     registry_len  = 0;
static bool                       builtins_done = false;

n00b_hostmeta_collector_t *
n00b_hostmeta_find_collector(n00b_string_t *name)
{
    if (name == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < registry_len; i++) {
        if (n00b_unicode_str_eq(registry[i]->name, name)) {
            return registry[i];
        }
    }
    return nullptr;
}

n00b_result_t(bool)
    n00b_hostmeta_register_collector(n00b_hostmeta_collector_t *collector)
{
    if (collector == nullptr || collector->name == nullptr
        || collector->name->u8_bytes == 0) {
        return n00b_result_err(bool, N00B_HOSTMETA_ERR_ARG);
    }
    if (n00b_hostmeta_find_collector(collector->name) != nullptr) {
        return n00b_result_err(bool, N00B_HOSTMETA_ERR_DUPLICATE);
    }
    if (registry_len >= N00B_HOSTMETA_MAX_COLLECTORS) {
        return n00b_result_err(bool, N00B_HOSTMETA_ERR_FULL);
    }

    registry[registry_len++] = collector;
    return n00b_result_ok(bool, true);
}

void
n00b_hostmeta_builtins_init(void)
{
    if (builtins_done) {
        return;
    }
    builtins_done = true;

    n00b_hostmeta_collector_aws_ecs.name         = r"aws_ecs";
    n00b_hostmeta_collector_aws_lambda.name      = r"aws_lambda";
    n00b_hostmeta_collector_cloud_metadata.name  = r"cloud_metadata";
    n00b_hostmeta_collector_ci_github.name       = r"ci_github";
    n00b_hostmeta_collector_ci_gitlab.name       = r"ci_gitlab";
    n00b_hostmeta_collector_ci_jenkins.name      = r"ci_jenkins";
    n00b_hostmeta_collector_ci_circleci.name     = r"ci_circleci";
    n00b_hostmeta_collector_ci_azure_devops.name = r"ci_azure_devops";
    n00b_hostmeta_collector_ci_bitbucket.name    = r"ci_bitbucket";
    n00b_hostmeta_collector_ci_buildkite.name    = r"ci_buildkite";
    n00b_hostmeta_collector_ci_codebuild.name    = r"ci_codebuild";
    n00b_hostmeta_collector_ci_teamcity.name     = r"ci_teamcity";
}

void
n00b_hostmeta_module_init(void)
{
    if (registry_len > 0) {
        return;
    }

    n00b_hostmeta_builtins_init();

    n00b_hostmeta_collector_t *builtins[] = {
        // Specific AWS services before the generic IMDS probe.
        &n00b_hostmeta_collector_aws_ecs,
        &n00b_hostmeta_collector_aws_lambda,
        &n00b_hostmeta_collector_cloud_metadata,
        // CI collectors are mutually exclusive in practice; each guards
        // on its own vendor markers.
        &n00b_hostmeta_collector_ci_github,
        &n00b_hostmeta_collector_ci_gitlab,
        &n00b_hostmeta_collector_ci_jenkins,
        &n00b_hostmeta_collector_ci_circleci,
        &n00b_hostmeta_collector_ci_azure_devops,
        &n00b_hostmeta_collector_ci_bitbucket,
        &n00b_hostmeta_collector_ci_buildkite,
        &n00b_hostmeta_collector_ci_codebuild,
        &n00b_hostmeta_collector_ci_teamcity,
    };

    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        // A registration failure here would mean every later collect
        // silently under-reports, which is worse than not starting.
        n00b_result_t(bool) r = n00b_hostmeta_register_collector(builtins[i]);
        if (n00b_result_is_err(r)) {
            n00b_panic("n00b_hostmeta_module_init: collector registration "
                       "failed");
        }
    }
}

static void
run_collector(n00b_hostmeta_collector_t *c, n00b_hostmeta_ctx_t *ctx)
{
    void (*cb)(n00b_hostmeta_collector_t *, n00b_hostmeta_ctx_t *)
        = ctx->phase == N00B_HOSTMETA_PHASE_CHALK_TIME ? c->chalk_time
                                                       : c->run_time;
    if (cb != nullptr) {
        cb(c, ctx);
    }
}

n00b_result_t(n00b_hostmeta_result_t *)
    n00b_hostmeta_collect() _kargs
{
    n00b_hostmeta_phase_t       phase                    = N00B_HOSTMETA_PHASE_RUN_TIME;
    n00b_string_t              *only                     = nullptr;
    n00b_hostmeta_key_filter_t  subscribed               = nullptr;
    void                       *subscribed_user          = nullptr;
    n00b_hostmeta_caller_arn_fn caller_arn               = nullptr;
    void                       *caller_arn_user          = nullptr;
    n00b_string_t              *metadata_ip              = nullptr;
    int32_t                     timeout_ms               = 1000;
    int32_t                     retries                  = 2;
    bool                        allow_network            = true;
    n00b_string_t              *sys_vendor_path          = nullptr;
    n00b_string_t              *sys_hypervisor_path      = nullptr;
    n00b_string_t              *sys_product_path         = nullptr;
    n00b_string_t              *sys_board_asset_tag_path = nullptr;
    n00b_string_t              *resolv_path              = nullptr;
    n00b_allocator_t           *allocator                = nullptr;
}
{
    n00b_hostmeta_result_t *result = n00b_alloc_with_opts(
        n00b_hostmeta_result_t,
        &(n00b_alloc_opts_t){.allocator = allocator});

    result->keys     = n00b_json_object_new(.allocator = allocator);
    result->failures = n00b_list_new(n00b_hostmeta_failure_t *,
                                     .allocator = allocator);

    n00b_hostmeta_ctx_t ctx = {
        .phase           = phase,
        .subscribed      = subscribed,
        .subscribed_user = subscribed_user,
        .caller_arn      = caller_arn,
        .caller_arn_user = caller_arn_user,
        .metadata_ip     = metadata_ip ? metadata_ip : r"169.254.169.254",
        .timeout_ms      = timeout_ms,
        .retries         = retries,
        .allow_network   = allow_network,

        // Linux sysfs locations; on other platforms these simply do not
        // exist, the reads fail quietly, and no cloud is detected.
        .sys_vendor_path = sys_vendor_path
                             ? sys_vendor_path
                             : r"/sys/class/dmi/id/sys_vendor",
        .sys_hypervisor_path = sys_hypervisor_path
                                 ? sys_hypervisor_path
                                 : r"/sys/hypervisor/uuid",
        .sys_product_path    = sys_product_path
                                 ? sys_product_path
                                 : r"/sys/class/dmi/id/product_uuid",
        .sys_board_asset_tag_path
        = sys_board_asset_tag_path
              ? sys_board_asset_tag_path
              : r"/sys/class/dmi/id/board_asset_tag",
        .resolv_path = resolv_path ? resolv_path : r"/etc/resolv.conf",

        .result    = result,
        .allocator = allocator,
    };

    if (only != nullptr) {
        n00b_hostmeta_collector_t *c = n00b_hostmeta_find_collector(only);
        if (c == nullptr) {
            return n00b_result_err(n00b_hostmeta_result_t *,
                                   N00B_HOSTMETA_ERR_NO_SUCH_NAME);
        }
        run_collector(c, &ctx);
        return n00b_result_ok(n00b_hostmeta_result_t *, result);
    }

    for (size_t i = 0; i < registry_len; i++) {
        run_collector(registry[i], &ctx);
    }

    return n00b_result_ok(n00b_hostmeta_result_t *, result);
}
