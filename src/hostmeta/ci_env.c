/** @file src/hostmeta/ci_env.c — CI collectors that read only env vars.
 *
 *  GitLab, Jenkins, CircleCI, Azure Pipelines, Bitbucket Pipelines,
 *  Buildkite, and AWS CodeBuild all publish everything chalk wants
 *  through predefined environment variables, so each collector here is
 *  a mapping from that vendor's variable names onto the shared
 *  `BUILD_*` vocabulary. GitHub (REST call) and TeamCity (properties
 *  file) need more than env and live in their own files.
 *
 *  Each collector opens with the same guard the chalk plugin used: if
 *  the vendor's marker variables are absent we are not running there,
 *  and the collector contributes nothing.
 *
 *  Vendor references:
 *    GitLab     https://docs.gitlab.com/ee/ci/variables/predefined_variables.html
 *    Jenkins    https://www.jenkins.io/doc/book/pipeline/jenkinsfile/#using-environment-variables
 *    CircleCI   https://circleci.com/docs/variables/#built-in-environment-variables
 *    Azure      https://learn.microsoft.com/en-us/azure/devops/pipelines/build/variables
 *    Bitbucket  https://support.atlassian.com/bitbucket-cloud/docs/variables-and-secrets/
 *    Buildkite  https://buildkite.com/docs/pipelines/environment-variables
 *    CodeBuild  https://docs.aws.amazon.com/codebuild/latest/userguide/build-env-ref-env-vars.html
 */

#define N00B_USE_INTERNAL_API

#include "hostmeta/n00b_hostmeta_builtins.h"

#include "internal/hostmeta/hostmeta_internal.h"
#include "text/strings/format.h"
#include "text/strings/string_ops.h"

/**
 * Normalize a tag or branch to a `refs/...` ref, preferring the tag.
 * Several vendors set both during a tag build; the tag is the more
 * specific fact.
 */
static n00b_string_t *
tag_or_branch_ref(n00b_string_t *tag, n00b_string_t *branch)
{
    if (tag != nullptr && tag->u8_bytes > 0) {
        return n00b_unicode_str_cat(r"refs/tags/", tag);
    }
    return n00b_hostmeta_branch_to_ref(branch);
}

/** The portion of @p s before the first `/`, or all of it. */
static n00b_string_t *
first_path_segment(n00b_string_t *s)
{
    if (s == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < s->u8_bytes; i++) {
        if (s->data[i] == '/') {
            return n00b_string_from_raw(s->data, (int64_t)i);
        }
    }
    return s;
}

// ======================================================================
// GitLab
// ======================================================================

static void
gitlab_collect(n00b_hostmeta_collector_t *self, n00b_hostmeta_ctx_t *ctx)
{
    (void)self;

    if (n00b_hostmeta_env("CI") == nullptr
        && n00b_hostmeta_env("GITLAB_CI") == nullptr) {
        return;
    }

    n00b_hostmeta_ci_put(ctx, "BUILD_ID", n00b_hostmeta_env("CI_JOB_ID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_COMMIT_ID",
                         n00b_hostmeta_env("CI_COMMIT_SHA"));
    n00b_hostmeta_ci_put(ctx, "BUILD_URI", n00b_hostmeta_env("CI_JOB_URL"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_API_URI",
                         n00b_hostmeta_env("CI_API_V4_URL"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_ID",
                         n00b_hostmeta_env("CI_PROJECT_ID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_OWNER_ID",
                         n00b_hostmeta_env("CI_PROJECT_NAMESPACE_ID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_URI",
                         n00b_hostmeta_env("CI_PROJECT_URL"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_WORKFLOW_NAME",
                         n00b_hostmeta_env("CI_PIPELINE_NAME"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_WORKFLOW_PATH",
                         n00b_hostmeta_env("CI_CONFIG_PATH"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_REF",
                         n00b_hostmeta_env("CI_MERGE_REQUEST_REF_PATH"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_TRIGGER",
                         n00b_hostmeta_env("CI_PIPELINE_SOURCE"));

    // GitLab exposes several plausible "user" variables; chalk picks
    // the login of whoever started the pipeline.
    n00b_hostmeta_ci_put_contact(ctx, n00b_hostmeta_env("GITLAB_USER_LOGIN"));
}

n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_gitlab = {
    .chalk_time = gitlab_collect,
    .run_time   = gitlab_collect,
};

// ======================================================================
// Jenkins
// ======================================================================

static void
jenkins_collect(n00b_hostmeta_collector_t *self, n00b_hostmeta_ctx_t *ctx)
{
    (void)self;

    n00b_string_t *build_id = n00b_hostmeta_env("BUILD_ID");
    if (n00b_hostmeta_env("CI") == nullptr && build_id == nullptr) {
        return;
    }

    n00b_hostmeta_ci_put(ctx, "BUILD_ID", build_id);
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_COMMIT_ID",
                         n00b_hostmeta_env("GIT_COMMIT"));
    n00b_hostmeta_ci_put(ctx, "BUILD_URI", n00b_hostmeta_env("BUILD_URL"));
    n00b_hostmeta_ci_put(ctx, "BUILD_API_URI", n00b_hostmeta_env("JENKINS_URL"));
    n00b_hostmeta_ci_put(ctx, "BUILD_ORIGIN_ID", n00b_hostmeta_env("JOB_NAME"));
    n00b_hostmeta_ci_put(ctx, "BUILD_ORIGIN_URI", n00b_hostmeta_env("GIT_URL"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_WORKFLOW_NAME",
                         n00b_hostmeta_env("JOB_NAME"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_WORKFLOW_PATH",
                         n00b_hostmeta_env("JOB_URL"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_TRIGGER",
                         n00b_hostmeta_env("BUILD_CAUSE"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_REF",
                         n00b_hostmeta_branch_to_ref(
                             n00b_hostmeta_env("GIT_BRANCH")));

    // Jenkins has no notion of "who triggered this" in the base env;
    // the executing node is the closest available attribution.
    n00b_hostmeta_ci_put_contact(ctx, n00b_hostmeta_env("NODE_NAME"));
}

n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_jenkins = {
    .chalk_time = jenkins_collect,
    .run_time   = jenkins_collect,
};

// ======================================================================
// CircleCI
// ======================================================================

static void
circleci_collect(n00b_hostmeta_collector_t *self, n00b_hostmeta_ctx_t *ctx)
{
    (void)self;

    if (n00b_hostmeta_env("CIRCLECI") == nullptr
        && n00b_hostmeta_env("CIRCLE_BUILD_NUM") == nullptr) {
        return;
    }

    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ID",
                         n00b_hostmeta_env("CIRCLE_WORKFLOW_JOB_ID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_COMMIT_ID",
                         n00b_hostmeta_env("CIRCLE_SHA1"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_URI",
                         n00b_hostmeta_env("CIRCLE_BUILD_URL"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_ID",
                         n00b_hostmeta_env("CIRCLE_PROJECT_ID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_OWNER_ID",
                         n00b_hostmeta_env("CIRCLE_ORGANIZATION_ID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_URI",
                         n00b_hostmeta_env("CIRCLE_REPOSITORY_URL"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_WORKFLOW_NAME",
                         n00b_hostmeta_env("CIRCLE_WORKFLOW_ID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_WORKFLOW_PATH",
                         n00b_hostmeta_env("CIRCLE_JOB"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_REF",
                         tag_or_branch_ref(n00b_hostmeta_env("CIRCLE_TAG"),
                                           n00b_hostmeta_env("CIRCLE_BRANCH")));
    n00b_hostmeta_ci_put_contact(ctx, n00b_hostmeta_env("CIRCLE_USERNAME"));
}

n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_circleci = {
    .chalk_time = circleci_collect,
    .run_time   = circleci_collect,
};

// ======================================================================
// Azure Pipelines
// ======================================================================

static void
azure_devops_collect(n00b_hostmeta_collector_t *self, n00b_hostmeta_ctx_t *ctx)
{
    (void)self;

    n00b_string_t *build_id = n00b_hostmeta_env("BUILD_BUILDID");
    if (n00b_hostmeta_env("TF_BUILD") == nullptr && build_id == nullptr) {
        return;
    }

    n00b_string_t *collection_uri = n00b_hostmeta_env(
        "SYSTEM_TEAMFOUNDATIONCOLLECTIONURI");
    n00b_string_t *team_project = n00b_hostmeta_env("SYSTEM_TEAMPROJECT");

    n00b_hostmeta_ci_put(ctx, "BUILD_ID", n00b_hostmeta_env("SYSTEM_JOBID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_COMMIT_ID",
                         n00b_hostmeta_env("BUILD_SOURCEVERSION"));
    n00b_hostmeta_ci_put(ctx, "BUILD_API_URI", collection_uri);
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_ID",
                         n00b_hostmeta_env("BUILD_REPOSITORY_ID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_OWNER_ID",
                         n00b_hostmeta_env("SYSTEM_TEAMPROJECTID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_URI",
                         n00b_hostmeta_env("BUILD_REPOSITORY_URI"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_WORKFLOW_NAME",
                         n00b_hostmeta_env("BUILD_DEFINITIONNAME"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_WORKFLOW_PATH",
                         n00b_hostmeta_env("SYSTEM_DEFINITIONID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_REF",
                         n00b_hostmeta_env("BUILD_SOURCEBRANCH"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_TRIGGER",
                         n00b_hostmeta_env("BUILD_REASON"));

    // Azure publishes no single build-results URL, so assemble the one
    // the web UI uses.
    if (collection_uri != nullptr && team_project != nullptr
        && build_id != nullptr) {
        n00b_hostmeta_ci_put(
            ctx,
            "BUILD_URI",
            n00b_cformat("[|#|]/[|#|]/_build/results?buildId=[|#|]",
                         n00b_hostmeta_strip_slashes(collection_uri,
                                                     false,
                                                     true),
                         team_project,
                         build_id));
    }

    n00b_hostmeta_ci_put_contact(ctx,
                                 n00b_hostmeta_env("BUILD_REQUESTEDFOR"));
}

n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_azure_devops = {
    .chalk_time = azure_devops_collect,
    .run_time   = azure_devops_collect,
};

// ======================================================================
// Bitbucket Pipelines
// ======================================================================

static void
bitbucket_collect(n00b_hostmeta_collector_t *self, n00b_hostmeta_ctx_t *ctx)
{
    (void)self;

    n00b_string_t *build_number  = n00b_hostmeta_env("BITBUCKET_BUILD_NUMBER");
    n00b_string_t *pipeline_uuid = n00b_hostmeta_env("BITBUCKET_PIPELINE_UUID");
    if (build_number == nullptr && pipeline_uuid == nullptr) {
        return;
    }

    n00b_string_t *origin = n00b_hostmeta_env("BITBUCKET_GIT_HTTP_ORIGIN");
    n00b_string_t *tag    = n00b_hostmeta_env("BITBUCKET_TAG");

    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ID",
                         n00b_hostmeta_env("BITBUCKET_STEP_UUID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_COMMIT_ID",
                         n00b_hostmeta_env("BITBUCKET_COMMIT"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_ID",
                         n00b_hostmeta_env("BITBUCKET_REPO_UUID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_OWNER_ID",
                         n00b_hostmeta_env("BITBUCKET_WORKSPACE"));
    n00b_hostmeta_ci_put(ctx, "BUILD_ORIGIN_URI", origin);
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_WORKFLOW_NAME",
                         n00b_hostmeta_env("BITBUCKET_REPO_FULL_NAME"));
    n00b_hostmeta_ci_put(ctx, "BUILD_WORKFLOW_PATH", pipeline_uuid);

    if (origin != nullptr && build_number != nullptr) {
        n00b_hostmeta_ci_put(
            ctx,
            "BUILD_URI",
            n00b_cformat("[|#|]/pipelines/results/[|#|]",
                         n00b_hostmeta_strip_slashes(origin, false, true),
                         build_number));
    }

    n00b_hostmeta_ci_put(ctx,
                         "BUILD_REF",
                         tag_or_branch_ref(tag,
                                           n00b_hostmeta_env(
                                               "BITBUCKET_BRANCH")));

    // Bitbucket has no trigger variable; the shape of the build is
    // inferred from which of the PR / tag variables is populated.
    const char *trigger = "push";
    if (n00b_hostmeta_env("BITBUCKET_PR_ID") != nullptr) {
        trigger = "pullrequest";
    }
    else if (tag != nullptr) {
        trigger = "tag";
    }
    n00b_hostmeta_ci_put(ctx, "BUILD_TRIGGER", n00b_hostmeta_str(trigger));
}

n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_bitbucket = {
    .chalk_time = bitbucket_collect,
    .run_time   = bitbucket_collect,
};

// ======================================================================
// Buildkite
// ======================================================================

static void
buildkite_collect(n00b_hostmeta_collector_t *self, n00b_hostmeta_ctx_t *ctx)
{
    (void)self;

    if (n00b_hostmeta_env("BUILDKITE") == nullptr
        && n00b_hostmeta_env("BUILDKITE_BUILD_ID") == nullptr) {
        return;
    }

    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ID",
                         n00b_hostmeta_env("BUILDKITE_JOB_ID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_COMMIT_ID",
                         n00b_hostmeta_env("BUILDKITE_COMMIT"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_URI",
                         n00b_hostmeta_env("BUILDKITE_BUILD_URL"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_ID",
                         n00b_hostmeta_env("BUILDKITE_PIPELINE_ID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_OWNER_ID",
                         n00b_hostmeta_env("BUILDKITE_ORGANIZATION_SLUG"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_URI",
                         n00b_hostmeta_env("BUILDKITE_REPO"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_WORKFLOW_NAME",
                         n00b_hostmeta_env("BUILDKITE_PIPELINE_SLUG"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_WORKFLOW_PATH",
                         n00b_hostmeta_env("BUILDKITE_STEP_KEY"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_TRIGGER",
                         n00b_hostmeta_env("BUILDKITE_SOURCE"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_REF",
                         tag_or_branch_ref(n00b_hostmeta_env("BUILDKITE_TAG"),
                                           n00b_hostmeta_env(
                                               "BUILDKITE_BRANCH")));
    n00b_hostmeta_ci_put_contact(ctx,
                                 n00b_hostmeta_env("BUILDKITE_BUILD_CREATOR"));
}

n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_buildkite = {
    .chalk_time = buildkite_collect,
    .run_time   = buildkite_collect,
};

// ======================================================================
// AWS CodeBuild
// ======================================================================

static void
codebuild_collect(n00b_hostmeta_collector_t *self, n00b_hostmeta_ctx_t *ctx)
{
    (void)self;

    n00b_string_t *build_arn = n00b_hostmeta_env("CODEBUILD_BUILD_ARN");
    n00b_string_t *repo_url  = n00b_hostmeta_env("CODEBUILD_SOURCE_REPO_URL");
    if (build_arn == nullptr && repo_url == nullptr) {
        return;
    }

    // An S3 source has no commit; its "version" is an object version
    // id, which belongs on the origin URI rather than in a commit key.
    bool           is_s3          = n00b_hostmeta_istarts_with(repo_url, "s3://");
    n00b_string_t *source_version = n00b_hostmeta_env("CODEBUILD_SOURCE_VERSION");

    n00b_hostmeta_ci_put(ctx,
                         "BUILD_URI",
                         n00b_hostmeta_env("CODEBUILD_PUBLIC_BUILD_URL"));
    n00b_hostmeta_ci_put(ctx, "BUILD_ID", build_arn);

    if (is_s3) {
        n00b_string_t *origin = repo_url;
        if (source_version != nullptr) {
            origin = n00b_cformat("[|#|]?versionId=[|#|]",
                                  repo_url,
                                  source_version);
        }
        n00b_hostmeta_ci_put(ctx,
                             "BUILD_ORIGIN_URI",
                             origin);
    }
    else {
        n00b_hostmeta_ci_put(ctx, "BUILD_ORIGIN_URI", repo_url);
        n00b_hostmeta_ci_put(ctx,
                             "BUILD_COMMIT_ID",
                             n00b_hostmeta_env(
                                 "CODEBUILD_RESOLVED_SOURCE_VERSION"));
    }

    // CODEBUILD_WEBHOOK_TRIGGER is `<event>/<name>`, e.g. `branch/main`.
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_TRIGGER",
                         first_path_segment(
                             n00b_hostmeta_env("CODEBUILD_WEBHOOK_TRIGGER")));
    n00b_hostmeta_ci_put_contact(ctx,
                                 n00b_hostmeta_env("CODEBUILD_INITIATOR"));
}

n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_codebuild = {
    .chalk_time = codebuild_collect,
    .run_time   = codebuild_collect,
};
