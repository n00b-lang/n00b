/* Tests for the hostmeta collectors.
 *
 * Everything here is driven from process state the test controls
 * outright — environment variables, files it writes itself — so no
 * test needs a cloud, a CI runner, or a network. The two collectors
 * that would reach the network (`cloud_metadata`, `aws_ecs`) are
 * exercised with `allow_network = false`, which is also the assertion
 * that the flag actually stops them.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/env.h"
#include "core/file.h"
#include "core/runtime.h"
#include "hostmeta/n00b_hostmeta.h"
#include "hostmeta/n00b_hostmeta_aws.h"
#include "parsers/json.h"
#include "text/strings/string_ops.h"
#include "util/path.h"

static void
set_env(const char *name, const char *value)
{
    assert(n00b_putenv(n00b_string_from_cstr(name),
                       n00b_string_from_cstr(value))
           == true);
}

/** Clear by setting empty: hostmeta treats empty and unset alike. */
static void
clear_env(const char *name)
{
    assert(n00b_putenv(n00b_string_from_cstr(name), n00b_string_from_cstr(""))
           == true);
}

static n00b_hostmeta_result_t *
collect_only(const char *name, n00b_hostmeta_phase_t phase)
{
    auto r = n00b_hostmeta_collect(.only          = n00b_string_from_cstr(name),
                                   .phase         = phase,
                                   .allow_network = false);
    assert(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

/** Assert `key` holds exactly `expected`. */
static void
assert_key(n00b_hostmeta_result_t *m, const char *key, const char *expected)
{
    n00b_json_node_t *node = n00b_json_object_get_cstr(m->keys, key);
    if (node == nullptr) {
        printf("  [FAIL] key %s is absent (expected \"%s\")\n", key, expected);
        assert(false);
    }
    assert(n00b_json_is_string(node));
    n00b_string_t *got = n00b_json_as_string(node);
    if (strcmp(got->data, expected) != 0) {
        printf("  [FAIL] key %s = \"%s\", expected \"%s\"\n",
               key,
               got->data,
               expected);
        assert(false);
    }
}

static void
assert_absent(n00b_hostmeta_result_t *m, const char *key)
{
    if (n00b_json_object_get_cstr(m->keys, key) != nullptr) {
        printf("  [FAIL] key %s should be absent\n", key);
        assert(false);
    }
}

/** Assert `key` holds a one-element array whose entry is `expected`. */
static void
assert_key_list1(n00b_hostmeta_result_t *m,
                 const char             *key,
                 const char             *expected)
{
    n00b_json_node_t *node = n00b_json_object_get_cstr(m->keys, key);
    assert(node != nullptr);
    assert(n00b_json_is_array(node));
    assert(n00b_json_array_len(node) == 1);

    n00b_json_node_t *first = n00b_json_array_get(node, 0);
    assert(n00b_json_is_string(first));
    assert(strcmp(n00b_json_as_string(first)->data, expected) == 0);
}

// ======================================================================
// ARN
// ======================================================================

static void
test_arn_parse(void)
{
    auto r = n00b_hostmeta_arn_parse(
        n00b_string_from_cstr("arn:aws:iam::123456789012:role/my-role"));
    assert(n00b_option_is_set(r));

    n00b_hostmeta_arn_t *a = n00b_option_get(r);
    assert(strcmp(a->partition->data, "aws") == 0);
    assert(strcmp(a->service->data, "iam") == 0);
    assert(a->region->u8_bytes == 0);
    assert(strcmp(a->account->data, "123456789012") == 0);
    assert(strcmp(a->resource->data, "role/my-role") == 0);

    printf("  [PASS] arn_parse\n");
}

static void
test_arn_resource_keeps_colons(void)
{
    // A log-stream resource carries its own `:` separators; splitting
    // on them would corrupt the resource.
    auto r = n00b_hostmeta_arn_parse(n00b_string_from_cstr(
        "arn:aws:logs:us-east-1:123456789012:log-group:/aws/lambda/f:"
        "log-stream:2024/01/01/[$LATEST]abc"));
    assert(n00b_option_is_set(r));

    n00b_hostmeta_arn_t *a = n00b_option_get(r);
    assert(strcmp(a->service->data, "logs") == 0);
    assert(strcmp(a->region->data, "us-east-1") == 0);
    assert(strcmp(a->resource->data,
                  "log-group:/aws/lambda/f:log-stream:2024/01/01/[$LATEST]abc")
           == 0);

    printf("  [PASS] arn_resource_keeps_colons\n");
}

static void
test_arn_rejects_non_arn(void)
{
    assert(!n00b_option_is_set(
        n00b_hostmeta_arn_parse(n00b_string_from_cstr("not-an-arn"))));
    // `arn:` prefix but too few fields.
    assert(!n00b_option_is_set(
        n00b_hostmeta_arn_parse(n00b_string_from_cstr("arn:aws:iam"))));
    assert(!n00b_option_is_set(n00b_hostmeta_arn_parse(nullptr)));

    printf("  [PASS] arn_rejects_non_arn\n");
}

static void
test_arn_with_roundtrip(void)
{
    auto r = n00b_hostmeta_arn_parse(
        n00b_string_from_cstr("arn:aws:iam::123456789012:role/my-role"));
    n00b_hostmeta_arn_t *role = n00b_option_get(r);

    n00b_hostmeta_arn_t *fn = n00b_hostmeta_arn_with(
        role,
        .service  = n00b_string_from_cstr("lambda"),
        .region   = n00b_string_from_cstr("us-west-2"),
        .resource = n00b_string_from_cstr("function:hello"));

    assert(strcmp(n00b_hostmeta_arn_format(fn)->data,
                  "arn:aws:lambda:us-west-2:123456789012:function:hello")
           == 0);
    // The source ARN is untouched.
    assert(strcmp(role->service->data, "iam") == 0);

    printf("  [PASS] arn_with_roundtrip\n");
}

// ======================================================================
// CI collectors
// ======================================================================

static void
test_ci_gitlab(void)
{
    set_env("GITLAB_CI", "true");
    set_env("CI_JOB_ID", "42");
    set_env("CI_COMMIT_SHA", "deadbeef");
    set_env("CI_JOB_URL", "https://gitlab.example/jobs/42");
    set_env("CI_PROJECT_ID", "7");
    set_env("CI_PIPELINE_SOURCE", "merge_request_event");
    set_env("GITLAB_USER_LOGIN", "someone");

    n00b_hostmeta_result_t *m = collect_only("ci_gitlab",
                                             N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_ID", "42");
    assert_key(m, "BUILD_COMMIT_ID", "deadbeef");
    assert_key(m, "BUILD_URI", "https://gitlab.example/jobs/42");
    assert_key(m, "BUILD_ORIGIN_ID", "7");
    assert_key(m, "BUILD_TRIGGER", "merge_request_event");
    assert_key_list1(m, "BUILD_CONTACT", "someone");
    // Unset vendor variables produce no key at all.
    assert_absent(m, "BUILD_WORKFLOW_NAME");

    printf("  [PASS] ci_gitlab\n");
}

static void
test_run_time_phase_prefixes_keys(void)
{
    // Same environment as the previous test, run-time phase.
    n00b_hostmeta_result_t *m = collect_only("ci_gitlab",
                                             N00B_HOSTMETA_PHASE_RUN_TIME);
    assert_key(m, "_BUILD_ID", "42");
    assert_key(m, "_BUILD_COMMIT_ID", "deadbeef");
    assert_key_list1(m, "_BUILD_CONTACT", "someone");
    assert_absent(m, "BUILD_ID");

    printf("  [PASS] run_time_phase_prefixes_keys\n");
}

static void
test_ci_gitlab_absent_when_not_gitlab(void)
{
    clear_env("GITLAB_CI");
    clear_env("CI");

    n00b_hostmeta_result_t *m = collect_only("ci_gitlab",
                                             N00B_HOSTMETA_PHASE_CHALK_TIME);
    // The vendor variables are still set, but the guard did not fire.
    assert_absent(m, "BUILD_ID");

    printf("  [PASS] ci_gitlab_absent_when_not_gitlab\n");
}

static void
clear_gitlab_env(void)
{
    clear_env("CI_JOB_ID");
    clear_env("CI_COMMIT_SHA");
    clear_env("CI_JOB_URL");
    clear_env("CI_PROJECT_ID");
    clear_env("CI_PIPELINE_SOURCE");
    clear_env("GITLAB_USER_LOGIN");
}

static void
test_ci_circleci_ref_from_tag(void)
{
    set_env("CIRCLECI", "true");
    set_env("CIRCLE_SHA1", "abc123");
    set_env("CIRCLE_TAG", "v1.2.3");
    set_env("CIRCLE_BRANCH", "main");

    n00b_hostmeta_result_t *m = collect_only("ci_circleci",
                                             N00B_HOSTMETA_PHASE_CHALK_TIME);
    // Tag wins over branch when a build carries both.
    assert_key(m, "BUILD_REF", "refs/tags/v1.2.3");

    clear_env("CIRCLE_TAG");
    m = collect_only("ci_circleci", N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_REF", "refs/heads/main");

    clear_env("CIRCLECI");
    clear_env("CIRCLE_SHA1");
    clear_env("CIRCLE_BRANCH");

    printf("  [PASS] ci_circleci_ref_from_tag\n");
}

static void
test_ci_jenkins_ref_normalization(void)
{
    set_env("BUILD_ID", "17");
    set_env("GIT_BRANCH", "origin/release");
    set_env("NODE_NAME", "worker-3");

    n00b_hostmeta_result_t *m = collect_only("ci_jenkins",
                                             N00B_HOSTMETA_PHASE_CHALK_TIME);
    // The remote prefix is not part of the ref.
    assert_key(m, "BUILD_REF", "refs/heads/release");
    assert_key(m, "BUILD_ID", "17");
    assert_key_list1(m, "BUILD_CONTACT", "worker-3");

    // An already-qualified ref passes through untouched.
    set_env("GIT_BRANCH", "refs/tags/v9");
    m = collect_only("ci_jenkins", N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_REF", "refs/tags/v9");

    clear_env("BUILD_ID");
    clear_env("GIT_BRANCH");
    clear_env("NODE_NAME");

    printf("  [PASS] ci_jenkins_ref_normalization\n");
}

static void
test_ci_azure_devops_build_uri(void)
{
    set_env("TF_BUILD", "True");
    set_env("BUILD_BUILDID", "9001");
    set_env("SYSTEM_JOBID", "job-uuid");
    // Trailing slash on the collection URI must not double up.
    set_env("SYSTEM_TEAMFOUNDATIONCOLLECTIONURI", "https://dev.azure.com/org/");
    set_env("SYSTEM_TEAMPROJECT", "proj");
    set_env("BUILD_REQUESTEDFOR", "Ada Lovelace");

    n00b_hostmeta_result_t *m = collect_only("ci_azure_devops",
                                             N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_ID", "job-uuid");
    assert_key(m,
               "BUILD_URI",
               "https://dev.azure.com/org/proj/_build/results?buildId=9001");
    assert_key(m, "BUILD_API_URI", "https://dev.azure.com/org/");
    assert_key_list1(m, "BUILD_CONTACT", "Ada Lovelace");

    clear_env("TF_BUILD");
    clear_env("BUILD_BUILDID");
    clear_env("SYSTEM_JOBID");
    clear_env("SYSTEM_TEAMFOUNDATIONCOLLECTIONURI");
    clear_env("SYSTEM_TEAMPROJECT");
    clear_env("BUILD_REQUESTEDFOR");

    printf("  [PASS] ci_azure_devops_build_uri\n");
}

static void
test_ci_bitbucket_trigger(void)
{
    set_env("BITBUCKET_BUILD_NUMBER", "12");
    set_env("BITBUCKET_GIT_HTTP_ORIGIN", "https://bitbucket.org/ws/repo");
    set_env("BITBUCKET_BRANCH", "main");

    n00b_hostmeta_result_t *m = collect_only("ci_bitbucket",
                                             N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_TRIGGER", "push");
    assert_key(m, "BUILD_URI", "https://bitbucket.org/ws/repo/pipelines/results/12");
    assert_key(m, "BUILD_REF", "refs/heads/main");

    set_env("BITBUCKET_PR_ID", "5");
    m = collect_only("ci_bitbucket", N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_TRIGGER", "pullrequest");

    clear_env("BITBUCKET_PR_ID");
    set_env("BITBUCKET_TAG", "v2");
    m = collect_only("ci_bitbucket", N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_TRIGGER", "tag");
    assert_key(m, "BUILD_REF", "refs/tags/v2");

    clear_env("BITBUCKET_BUILD_NUMBER");
    clear_env("BITBUCKET_GIT_HTTP_ORIGIN");
    clear_env("BITBUCKET_BRANCH");
    clear_env("BITBUCKET_TAG");

    printf("  [PASS] ci_bitbucket_trigger\n");
}

static void
test_ci_codebuild_s3_source(void)
{
    set_env("CODEBUILD_BUILD_ARN",
            "arn:aws:codebuild:us-east-1:1234:build/p:id");
    set_env("CODEBUILD_SOURCE_REPO_URL", "s3://bucket/key.zip");
    set_env("CODEBUILD_SOURCE_VERSION", "objver1");
    set_env("CODEBUILD_RESOLVED_SOURCE_VERSION", "shouldnotappear");
    set_env("CODEBUILD_WEBHOOK_TRIGGER", "branch/main");

    n00b_hostmeta_result_t *m = collect_only("ci_codebuild",
                                             N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_ORIGIN_URI", "s3://bucket/key.zip?versionId=objver1");
    // An S3 source has no commit, so the resolved-version variable is
    // not a commit id and must not be reported as one.
    assert_absent(m, "BUILD_COMMIT_ID");
    assert_key(m, "BUILD_TRIGGER", "branch");

    clear_env("CODEBUILD_SOURCE_VERSION");
    m = collect_only("ci_codebuild", N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_ORIGIN_URI", "s3://bucket/key.zip");
    assert_absent(m, "BUILD_COMMIT_ID");

    // A git source does carry a commit.
    set_env("CODEBUILD_SOURCE_REPO_URL", "https://github.com/o/r.git");
    m = collect_only("ci_codebuild", N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_COMMIT_ID", "shouldnotappear");
    assert_key(m, "BUILD_ORIGIN_URI", "https://github.com/o/r.git");

    clear_env("CODEBUILD_BUILD_ARN");
    clear_env("CODEBUILD_SOURCE_REPO_URL");
    clear_env("CODEBUILD_SOURCE_VERSION");
    clear_env("CODEBUILD_RESOLVED_SOURCE_VERSION");
    clear_env("CODEBUILD_WEBHOOK_TRIGGER");

    printf("  [PASS] ci_codebuild_s3_source\n");
}

static void
test_ci_github_uri_and_trigger(void)
{
    set_env("CI", "true");
    set_env("GITHUB_SHA", "cafe1234");
    set_env("GITHUB_SERVER_URL", "https://github.com");
    set_env("GITHUB_REPOSITORY", "acme/widget");
    set_env("GITHUB_RUN_ID", "555");
    set_env("GITHUB_RUN_ATTEMPT", "2");
    set_env("GITHUB_EVENT_NAME", "push");
    set_env("GITHUB_REF_TYPE", "tag");
    set_env("GITHUB_ACTOR", "octocat");
    // No API url, so the node-id REST call is not attempted.
    clear_env("GITHUB_API_URL");
    clear_env("GITHUB_CHECK_RUN_ID");
    clear_env("RUNNER_TEMP");

    n00b_hostmeta_result_t *m = collect_only("ci_github",
                                             N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_ID", "555");
    assert_key(m, "BUILD_ORIGIN_URI", "https://github.com/acme/widget");
    assert_key(m,
               "BUILD_URI",
               "https://github.com/acme/widget/actions/runs/555/attempts/2");
    // A tag push reports as a tag, not as a push.
    assert_key(m, "BUILD_TRIGGER", "tag");
    assert_key_list1(m, "BUILD_CONTACT", "octocat");

    // A check-run id identifies the job, and takes over both the build
    // id and the deep link.
    set_env("GITHUB_CHECK_RUN_ID", "7777");
    m = collect_only("ci_github", N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_ID", "7777");
    assert_key(m,
               "BUILD_URI",
               "https://github.com/acme/widget/actions/runs/555/job/7777");

    set_env("GITHUB_REF_TYPE", "branch");
    m = collect_only("ci_github", N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_TRIGGER", "push");

    printf("  [PASS] ci_github_uri_and_trigger\n");
}

static void
test_ci_github_unique_id_is_stable(void)
{
    auto dr = n00b_new_temp_dir(n00b_string_from_cstr("hostmeta"),
                                n00b_string_from_cstr(""));
    assert(n00b_result_is_ok(dr));
    n00b_string_t *dir = n00b_result_get(dr);

    set_env("RUNNER_TEMP", dir->data);

    n00b_hostmeta_result_t *first = collect_only(
        "ci_github",
        N00B_HOSTMETA_PHASE_CHALK_TIME);
    n00b_json_node_t *a = n00b_json_object_get_cstr(first->keys,
                                                    "BUILD_UNIQUE_ID");
    assert(a != nullptr);
    assert(n00b_json_is_string(a));
    assert(n00b_json_as_string(a)->u8_bytes == 16);

    // Every later collect in the same job must see the same id: the
    // file, not the RNG, is the source of truth after the first write.
    n00b_hostmeta_result_t *second = collect_only(
        "ci_github",
        N00B_HOSTMETA_PHASE_CHALK_TIME);
    n00b_json_node_t *b = n00b_json_object_get_cstr(second->keys,
                                                    "BUILD_UNIQUE_ID");
    assert(b != nullptr);
    assert(strcmp(n00b_json_as_string(a)->data, n00b_json_as_string(b)->data)
           == 0);

    clear_env("RUNNER_TEMP");
    printf("  [PASS] ci_github_unique_id_is_stable\n");
}

static void
clear_github_env(void)
{
    clear_env("CI");
    clear_env("GITHUB_SHA");
    clear_env("GITHUB_SERVER_URL");
    clear_env("GITHUB_REPOSITORY");
    clear_env("GITHUB_RUN_ID");
    clear_env("GITHUB_RUN_ATTEMPT");
    clear_env("GITHUB_EVENT_NAME");
    clear_env("GITHUB_REF_TYPE");
    clear_env("GITHUB_ACTOR");
    clear_env("GITHUB_CHECK_RUN_ID");
}

static void
test_ci_teamcity_properties_win(void)
{
    auto dr = n00b_new_temp_dir(n00b_string_from_cstr("hostmeta"),
                                n00b_string_from_cstr(""));
    assert(n00b_result_is_ok(dr));

    n00b_list_t(n00b_string_t *) parts = n00b_list_new(n00b_string_t *);
    n00b_list_push(parts, n00b_result_get(dr));
    n00b_list_push(parts, n00b_string_from_cstr("build.properties"));
    n00b_string_t *props_path = n00b_path_join(&parts);

    const char *props = "teamcity.build.id=8899\n"
                        "teamcity.serverUrl=https://tc.example\n"
                        "teamcity.build.branch=feature/x\n"
                        "teamcity.build.triggeredBy.username=grace\n"
                        "# a comment that is not a property\n"
                        "malformed-line-without-separator\n";

    auto fr = n00b_file_open(props_path,
                             .mode = N00B_FILE_W,
                             .kind = N00B_FILE_KIND_STREAM);
    assert(n00b_result_is_ok(fr));
    n00b_file_t *f = n00b_result_get(fr);
    assert(n00b_result_is_ok(n00b_file_write(f, props, strlen(props))));
    n00b_file_close(f);

    set_env("TEAMCITY_VERSION", "2024.1");
    set_env("TEAMCITY_BUILD_PROPERTIES_FILE", props_path->data);
    set_env("BUILD_NUMBER", "user-visible-42");
    set_env("BUILD_VCS_NUMBER", "f00d");

    n00b_hostmeta_result_t *m = collect_only("ci_teamcity",
                                             N00B_HOSTMETA_PHASE_CHALK_TIME);
    // `teamcity.build.id` is the server-side build id; BUILD_NUMBER is
    // the user-visible counter. chalk's dict overwrites, so its later
    // properties write wins — this is the chalk-compatible outcome.
    assert_key(m, "BUILD_ID", "8899");
    assert_key(m, "BUILD_API_URI", "https://tc.example");
    assert_key(m, "BUILD_REF", "refs/heads/feature/x");
    assert_key_list1(m, "BUILD_CONTACT", "grace");
    assert_key(m, "BUILD_COMMIT_ID", "f00d");

    // Without the properties file, the env-var fallback supplies the id.
    clear_env("TEAMCITY_BUILD_PROPERTIES_FILE");
    m = collect_only("ci_teamcity", N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_ID", "user-visible-42");

    // An unreadable properties file must still report the diagnostic
    // even though BUILD_ID gets filled in from the environment right
    // after. The failure names BUILD_ID as its subject; that key being
    // populated is not a reason to drop the explanation.
    set_env("TEAMCITY_BUILD_PROPERTIES_FILE",
            "/nonexistent/hostmeta-test/build.properties");
    m = collect_only("ci_teamcity", N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_ID", "user-visible-42");
    assert(m->failures.len == 1);
    assert(strcmp(m->failures.data[0]->code->data,
                  "TEAMCITY_PROPERTIES_UNREADABLE")
           == 0);

    clear_env("TEAMCITY_VERSION");
    clear_env("TEAMCITY_BUILD_PROPERTIES_FILE");
    clear_env("BUILD_NUMBER");
    clear_env("BUILD_VCS_NUMBER");

    printf("  [PASS] ci_teamcity_properties_win\n");
}

// ======================================================================
// AWS
// ======================================================================

static n00b_string_t *
fake_caller_arn(void *user)
{
    (void)user;
    return n00b_string_from_cstr(
        "arn:aws:sts::123456789012:assumed-role/my-lambda-role/session-id");
}

static void
set_lambda_env(void)
{
    set_env("AWS_LAMBDA_FUNCTION_NAME", "hello");
    set_env("AWS_LAMBDA_FUNCTION_VERSION", "7");
    set_env("AWS_REGION", "us-west-2");
    set_env("AWS_LAMBDA_LOG_GROUP_NAME", "/aws/lambda/hello");
    set_env("AWS_LAMBDA_LOG_STREAM_NAME", "2024/01/01/[7]abc");
}

static void
test_aws_lambda_arn_derivation(void)
{
    set_lambda_env();

    auto r = n00b_hostmeta_collect(.only = n00b_string_from_cstr("aws_lambda"),
                                   .phase = N00B_HOSTMETA_PHASE_RUN_TIME,
                                   .allow_network = false,
                                   .caller_arn    = fake_caller_arn);
    assert(n00b_result_is_ok(r));
    n00b_hostmeta_result_t *m = n00b_result_get(r);

    assert_key(m, "_OP_CLOUD_PROVIDER", "aws");
    assert_key(m, "_OP_CLOUD_PROVIDER_SERVICE_TYPE", "aws_lambda");
    assert_key(m, "_OP_CLOUD_PROVIDER_ACCOUNT_INFO", "123456789012");
    assert_key(m, "_OP_CLOUD_PROVIDER_REGION", "us-west-2");
    assert_key(m, "_AWS_REGION", "us-west-2");

    n00b_json_node_t *cloud = n00b_json_object_get_cstr(m->keys,
                                                        "_OP_CLOUD_METADATA");
    assert(cloud != nullptr);
    n00b_json_node_t *data = n00b_json_object_get_cstr(cloud, "aws_lambda");
    assert(data != nullptr);

    // An assumed-role session ARN reduces to the durable role, which is
    // global and so carries no region.
    n00b_json_node_t *role = n00b_json_object_get_cstr(data, "AWS_ROLE_ARN");
    assert(role != nullptr);
    assert(strcmp(n00b_json_as_string(role)->data,
                  "arn:aws:iam::123456789012:role/my-lambda-role")
           == 0);

    n00b_json_node_t *fn = n00b_json_object_get_cstr(data,
                                                     "AWS_LAMBDA_FUNCTION_ARN");
    assert(strcmp(n00b_json_as_string(fn)->data,
                  "arn:aws:lambda:us-west-2:123456789012:function:hello")
           == 0);

    n00b_json_node_t *ver = n00b_json_object_get_cstr(data,
                                                      "AWS_LAMBDA_VERSION_ARN");
    assert(strcmp(n00b_json_as_string(ver)->data,
                  "arn:aws:lambda:us-west-2:123456789012:function:hello:7")
           == 0);

    n00b_json_node_t *stream = n00b_json_object_get_cstr(
        data,
        "AWS_LAMBDA_LOG_STREAM_ARN");
    assert(strcmp(n00b_json_as_string(stream)->data,
                  "arn:aws:logs:us-west-2:123456789012:log-group:"
                  "/aws/lambda/hello:log-stream:2024/01/01/[7]abc")
           == 0);

    printf("  [PASS] aws_lambda_arn_derivation\n");
}

static void
test_aws_lambda_without_caller_hook(void)
{
    set_lambda_env();

    // Same environment, no identity hook: env keys still collected, ARN
    // keys reported as failures rather than silently missing.
    n00b_hostmeta_result_t *m = collect_only("aws_lambda",
                                             N00B_HOSTMETA_PHASE_RUN_TIME);

    n00b_json_node_t *cloud = n00b_json_object_get_cstr(m->keys,
                                                        "_OP_CLOUD_METADATA");
    assert(cloud != nullptr);
    n00b_json_node_t *data = n00b_json_object_get_cstr(cloud, "aws_lambda");
    assert(n00b_json_object_get_cstr(data, "AWS_LAMBDA_FUNCTION_NAME")
           != nullptr);
    assert(n00b_json_object_get_cstr(data, "AWS_ROLE_ARN") == nullptr);

    assert(m->failures.len == 5);
    bool saw_role_arn = false;
    for (size_t i = 0; i < m->failures.len; i++) {
        n00b_hostmeta_failure_t *f = m->failures.data[i];
        assert(strcmp(f->code->data, "LAMBDA_NO_CALLER_IDENTITY") == 0);
        if (strcmp(f->key->data, "AWS_ROLE_ARN") == 0) {
            saw_role_arn = true;
        }
    }
    assert(saw_role_arn);

    printf("  [PASS] aws_lambda_without_caller_hook\n");
}

static bool
only_cloud_provider(void *user, n00b_string_t *key)
{
    (void)user;
    return n00b_unicode_str_eq(key, n00b_string_from_cstr("_OP_CLOUD_PROVIDER"));
}

static void
test_failures_respect_subscription(void)
{
    set_lambda_env();

    auto r = n00b_hostmeta_collect(.only = n00b_string_from_cstr("aws_lambda"),
                                   .phase = N00B_HOSTMETA_PHASE_RUN_TIME,
                                   .allow_network = false,
                                   .subscribed    = only_cloud_provider);
    assert(n00b_result_is_ok(r));
    n00b_hostmeta_result_t *m = n00b_result_get(r);

    assert_key(m, "_OP_CLOUD_PROVIDER", "aws");
    assert_absent(m, "_OP_CLOUD_METADATA");
    assert(m->failures.len == 0);

    printf("  [PASS] failures_respect_subscription\n");
}

static void
test_aws_lambda_absent_off_lambda(void)
{
    clear_env("AWS_LAMBDA_FUNCTION_NAME");
    clear_env("AWS_LAMBDA_FUNCTION_VERSION");
    clear_env("AWS_REGION");
    clear_env("AWS_LAMBDA_LOG_GROUP_NAME");
    clear_env("AWS_LAMBDA_LOG_STREAM_NAME");

    n00b_hostmeta_result_t *m = collect_only("aws_lambda",
                                             N00B_HOSTMETA_PHASE_RUN_TIME);
    assert_absent(m, "_OP_CLOUD_METADATA");
    assert(m->failures.len == 0);

    printf("  [PASS] aws_lambda_absent_off_lambda\n");
}

static void
test_aws_ecs_records_failure_when_offline(void)
{
    set_env("ECS_CONTAINER_METADATA_URI_V4", "http://169.254.170.2/v4/abc");

    n00b_hostmeta_result_t *m = collect_only("aws_ecs",
                                             N00B_HOSTMETA_PHASE_RUN_TIME);
    // `allow_network = false` must stop the request rather than let it
    // time out, and the unreachable endpoint is reported as a failure.
    assert_absent(m, "_OP_CLOUD_METADATA");
    assert(m->failures.len == 1);
    assert(strcmp(m->failures.data[0]->code->data, "ECS_METADATA_ERROR") == 0);

    clear_env("ECS_CONTAINER_METADATA_URI_V4");

    m = collect_only("aws_ecs", N00B_HOSTMETA_PHASE_RUN_TIME);
    // Off ECS entirely there is nothing to fail at.
    assert(m->failures.len == 0);

    printf("  [PASS] aws_ecs_records_failure_when_offline\n");
}

// ======================================================================
// Cloud metadata
// ======================================================================

/** Write @p contents to a fresh file under a temp dir and return its path. */
static n00b_string_t *
write_temp_file(const char *name, const char *contents)
{
    auto dr = n00b_new_temp_dir(n00b_string_from_cstr("hostmeta"),
                                n00b_string_from_cstr(""));
    assert(n00b_result_is_ok(dr));

    n00b_list_t(n00b_string_t *) parts = n00b_list_new(n00b_string_t *);
    n00b_list_push(parts, n00b_result_get(dr));
    n00b_list_push(parts, n00b_string_from_cstr(name));
    n00b_string_t *path = n00b_path_join(&parts);

    auto fr = n00b_file_open(path,
                             .mode = N00B_FILE_W,
                             .kind = N00B_FILE_KIND_STREAM);
    assert(n00b_result_is_ok(fr));
    n00b_file_t *f = n00b_result_get(fr);
    assert(n00b_result_is_ok(n00b_file_write(f, contents, strlen(contents))));
    n00b_file_close(f);

    return path;
}

/** Collect `cloud_metadata` against a fixture vendor file. */
static n00b_hostmeta_result_t *
collect_cloud_with_vendor(n00b_string_t *vendor_path)
{
    // Every probe path is pointed at the fixture (or at a path that
    // cannot exist) so the host's real sysfs cannot influence the
    // result on a machine that happens to be a cloud instance.
    n00b_string_t *nowhere = n00b_string_from_cstr(
        "/nonexistent/hostmeta-test/probe");

    auto r = n00b_hostmeta_collect(
        .only                     = n00b_string_from_cstr("cloud_metadata"),
        .phase                    = N00B_HOSTMETA_PHASE_RUN_TIME,
        .allow_network            = false,
        .sys_vendor_path          = vendor_path,
        .sys_hypervisor_path      = nowhere,
        .sys_product_path         = nowhere,
        .sys_board_asset_tag_path = nowhere,
        .resolv_path              = nowhere);
    assert(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

static void
test_cloud_detects_aws_from_vendor(void)
{
    n00b_hostmeta_result_t *m = collect_cloud_with_vendor(
        write_temp_file("sys_vendor", "Amazon EC2\n"));

    assert_key(m, "_OP_CLOUD_PROVIDER", "aws");
    assert_key(m, "_OP_CLOUD_SYS_VENDOR", "Amazon EC2");
    // Nothing about the environment says ECS or EKS here.
    assert_key(m, "_OP_CLOUD_PROVIDER_SERVICE_TYPE", "aws_ec2");
    // IMDS was never reachable, so the token failure is recorded.
    assert(m->failures.len >= 1);

    printf("  [PASS] cloud_detects_aws_from_vendor\n");
}

static void
test_cloud_detects_azure_from_vendor(void)
{
    n00b_hostmeta_result_t *m = collect_cloud_with_vendor(
        write_temp_file("sys_vendor", "Microsoft Corporation\n"));

    assert_key(m, "_OP_CLOUD_PROVIDER", "azure");
    assert_key(m, "_OP_CLOUD_SYS_VENDOR", "Microsoft Corporation");

    printf("  [PASS] cloud_detects_azure_from_vendor\n");
}

static void
test_cloud_detects_gcp_from_vendor(void)
{
    n00b_hostmeta_result_t *m = collect_cloud_with_vendor(
        write_temp_file("sys_vendor", "Google\n"));

    assert_key(m, "_OP_CLOUD_PROVIDER", "gcp");

    printf("  [PASS] cloud_detects_gcp_from_vendor\n");
}

static void
test_cloud_unknown_host_reports_vendor_only(void)
{
    n00b_hostmeta_result_t *m = collect_cloud_with_vendor(
        write_temp_file("sys_vendor", "QEMU\n"));

    assert_absent(m, "_OP_CLOUD_PROVIDER");
    // The vendor string is still worth having: it is the evidence for
    // why nothing else was collected.
    assert_key(m, "_OP_CLOUD_SYS_VENDOR", "QEMU");
    assert(m->failures.len == 0);

    printf("  [PASS] cloud_unknown_host_reports_vendor_only\n");
}

static void
test_cloud_aws_service_type_from_env(void)
{
    n00b_string_t *vendor = write_temp_file("sys_vendor", "Amazon EC2\n");

    set_env("ECS_CONTAINER_METADATA_URI_V4", "http://169.254.170.2/v4/abc");
    n00b_hostmeta_result_t *m = collect_cloud_with_vendor(vendor);
    assert_key(m, "_OP_CLOUD_PROVIDER_SERVICE_TYPE", "aws_ecs");
    clear_env("ECS_CONTAINER_METADATA_URI_V4");

    set_env("KUBERNETES_SERVICE_HOST", "10.100.0.1");
    m = collect_cloud_with_vendor(vendor);
    assert_key(m, "_OP_CLOUD_PROVIDER_SERVICE_TYPE", "aws_eks");
    clear_env("KUBERNETES_SERVICE_HOST");

    printf("  [PASS] cloud_aws_service_type_from_env\n");
}

// ======================================================================
// Registry and filtering
// ======================================================================

static bool
only_build_id(void *user, n00b_string_t *key)
{
    (void)user;
    return n00b_unicode_str_eq(key, n00b_string_from_cstr("BUILD_ID"));
}

static void
test_key_filter_limits_collection(void)
{
    set_env("GITLAB_CI", "true");
    set_env("CI_JOB_ID", "42");
    set_env("CI_COMMIT_SHA", "deadbeef");

    auto r = n00b_hostmeta_collect(.only  = n00b_string_from_cstr("ci_gitlab"),
                                   .phase = N00B_HOSTMETA_PHASE_CHALK_TIME,
                                   .allow_network = false,
                                   .subscribed    = only_build_id);
    assert(n00b_result_is_ok(r));
    n00b_hostmeta_result_t *m = n00b_result_get(r);

    assert_key(m, "BUILD_ID", "42");
    assert_absent(m, "BUILD_COMMIT_ID");

    clear_env("GITLAB_CI");
    clear_gitlab_env();

    printf("  [PASS] key_filter_limits_collection\n");
}

/* A collector that fills a key in and then reports a failure naming
 * that same key — the shape TeamCity has when its properties file is
 * unreadable but BUILD_ID still resolves from the environment. */
static void
set_then_fail_collect(n00b_hostmeta_collector_t *self,
                      n00b_hostmeta_ctx_t       *ctx)
{
    (void)self;

    n00b_string_t *key = n00b_string_from_cstr("BUILD_ID");
    n00b_hostmeta_put_string(ctx, key, n00b_string_from_cstr("from-env"));
    n00b_hostmeta_add_failure(ctx,
                              key,
                              "TEST_PARTIAL",
                              nullptr,
                              "richer source was unreachable");
}

static void
test_failure_survives_already_set_key(void)
{
    static n00b_hostmeta_collector_t c = {0};
    c.name       = n00b_string_from_cstr("test_set_then_fail");
    c.chalk_time = set_then_fail_collect;

    assert(n00b_result_is_ok(n00b_hostmeta_register_collector(&c)));

    n00b_hostmeta_result_t *m = collect_only("test_set_then_fail",
                                             N00B_HOSTMETA_PHASE_CHALK_TIME);
    assert_key(m, "BUILD_ID", "from-env");
    // The key being populated is not a reason to drop the diagnostic:
    // the failure explains that a better value was unavailable.
    assert(m->failures.len == 1);
    assert(strcmp(m->failures.data[0]->code->data, "TEST_PARTIAL") == 0);

    printf("  [PASS] failure_survives_already_set_key\n");
}

static void
test_unknown_collector_is_an_error(void)
{
    auto r = n00b_hostmeta_collect(.only = n00b_string_from_cstr("nope"));
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_HOSTMETA_ERR_NO_SUCH_NAME);

    printf("  [PASS] unknown_collector_is_an_error\n");
}

static void
test_registry_rejects_duplicates(void)
{
    static n00b_hostmeta_collector_t dupe = {0};
    dupe.name                             = n00b_string_from_cstr("ci_gitlab");

    auto r = n00b_hostmeta_register_collector(&dupe);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_HOSTMETA_ERR_DUPLICATE);

    // A nameless collector is rejected too.
    static n00b_hostmeta_collector_t nameless = {0};
    auto n = n00b_hostmeta_register_collector(&nameless);
    assert(n00b_result_is_err(n));
    assert(n00b_result_get_err(n) == N00B_HOSTMETA_ERR_ARG);

    printf("  [PASS] registry_rejects_duplicates\n");
}

static void
test_collect_all_is_empty_on_a_bare_host(void)
{
    // With every vendor marker cleared and the network off, a full
    // collect should produce nothing but the vendor probe's silence.
    auto r = n00b_hostmeta_collect(.allow_network = false,
                                   .sys_vendor_path = n00b_string_from_cstr(
                                       "/nonexistent/hostmeta-test/vendor"),
                                   .sys_hypervisor_path = n00b_string_from_cstr(
                                       "/nonexistent/hostmeta-test/hyp"),
                                   .sys_product_path = n00b_string_from_cstr(
                                       "/nonexistent/hostmeta-test/prod"),
                                   .sys_board_asset_tag_path
                                   = n00b_string_from_cstr(
                                       "/nonexistent/hostmeta-test/tag"),
                                   .resolv_path = n00b_string_from_cstr(
                                       "/nonexistent/hostmeta-test/resolv"));
    assert(n00b_result_is_ok(r));
    n00b_hostmeta_result_t *m = n00b_result_get(r);

    assert(m->keys != nullptr);
    assert(n00b_json_is_object(m->keys));
    assert_absent(m, "_OP_CLOUD_PROVIDER");
    assert_absent(m, "_BUILD_ID");

    printf("  [PASS] collect_all_is_empty_on_a_bare_host\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    n00b_hostmeta_module_init();

    printf("Running hostmeta tests...\n");

    test_arn_parse();
    test_arn_resource_keeps_colons();
    test_arn_rejects_non_arn();
    test_arn_with_roundtrip();

    test_ci_gitlab();
    test_run_time_phase_prefixes_keys();
    test_ci_gitlab_absent_when_not_gitlab();
    clear_gitlab_env();
    test_ci_circleci_ref_from_tag();
    test_ci_jenkins_ref_normalization();
    test_ci_azure_devops_build_uri();
    test_ci_bitbucket_trigger();
    test_ci_codebuild_s3_source();
    test_ci_github_uri_and_trigger();
    test_ci_github_unique_id_is_stable();
    clear_github_env();
    test_ci_teamcity_properties_win();

    test_aws_lambda_arn_derivation();
    test_aws_lambda_without_caller_hook();
    test_failures_respect_subscription();
    test_aws_lambda_absent_off_lambda();
    test_aws_ecs_records_failure_when_offline();

    test_cloud_detects_aws_from_vendor();
    test_cloud_detects_azure_from_vendor();
    test_cloud_detects_gcp_from_vendor();
    test_cloud_unknown_host_reports_vendor_only();
    test_cloud_aws_service_type_from_env();

    test_key_filter_limits_collection();
    test_failure_survives_already_set_key();
    test_unknown_collector_is_an_error();
    test_registry_rejects_duplicates();
    test_collect_all_is_empty_on_a_bare_host();

    printf("All hostmeta tests passed.\n");

    n00b_shutdown();
    return 0;
}
