#pragma once

/**
 * @file n00b_hostmeta_builtins.h
 * @brief The collectors `n00b_hostmeta_module_init()` registers.
 *
 * Exposed so a host can assemble its own registry — register only the
 * CI collectors in a build agent, only the cloud ones in a runtime
 * agent — instead of taking all of them and filtering afterwards.
 *
 * Registration order in `n00b_hostmeta_module_init()` matters: the
 * first collector to set a key wins, so the specific AWS services run
 * ahead of the generic IMDS probe.
 */

#include <n00b.h>

#include "hostmeta/n00b_hostmeta.h"

/** AWS ECS task metadata endpoint. Chalk-time and run-time. */
extern n00b_hostmeta_collector_t n00b_hostmeta_collector_aws_ecs;

/** AWS Lambda execution environment. Run-time only. */
extern n00b_hostmeta_collector_t n00b_hostmeta_collector_aws_lambda;

/** AWS IMDSv2 / Azure IMDS / GCP metadata server. Run-time only. */
extern n00b_hostmeta_collector_t n00b_hostmeta_collector_cloud_metadata;

/* CI / CD pipelines. All are chalk-time and run-time. */
extern n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_github;
extern n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_gitlab;
extern n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_jenkins;
extern n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_circleci;
extern n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_azure_devops;
extern n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_bitbucket;
extern n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_buildkite;
extern n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_codebuild;
extern n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_teamcity;

/**
 * @brief Populate the collector vtables.
 *
 * The vtables carry `n00b_string_t *name`, so they cannot be
 * file-scope constants — the names are built at run time. Called by
 * `n00b_hostmeta_module_init()`; call it yourself only if you are
 * building a custom registry from these symbols without going through
 * the module init.
 */
extern void n00b_hostmeta_builtins_init(void);
