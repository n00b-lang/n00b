/** @file src/hostmeta/ci_github.c — GitHub Actions collector.
 *
 *  Most of GitHub's build facts arrive in `GITHUB_*` environment
 *  variables, but two do not: the repository's and owner's GraphQL
 *  node ids, which identify the repo across renames the way the
 *  `owner/name` slug cannot. Those come from one REST call, gated on
 *  the caller actually subscribing to those keys — no token, no call.
 *
 *  https://docs.github.com/en/actions/reference/workflows-and-actions/variables
 *  https://docs.github.com/en/rest/repos/repos#get-a-repository
 */

#define N00B_USE_INTERNAL_API

#include "hostmeta/n00b_hostmeta_builtins.h"

#include "internal/hostmeta/hostmeta_internal.h"
#include "internal/net/http/http_h1.h"
#include "text/strings/format.h"
#include "text/strings/string_ops.h"
#include "util/path.h"

/** GitHub API round trips leave the host, so they get a real timeout. */
#define GITHUB_API_TIMEOUT_MS 10000

/**
 * Fetch `{api}/repos/{owner}/{name}` and hand back the parsed body.
 *
 * @param err_out Receives operator-facing failure text, and @p code_out
 *                a stable reason code, when the call cannot be made.
 */
static n00b_json_node_t *
github_get_repo(n00b_hostmeta_ctx_t *ctx,
                n00b_string_t       *api,
                n00b_string_t       *repo,
                const char         **code_out,
                n00b_string_t      **err_out)
{
    *code_out = nullptr;
    *err_out  = nullptr;

    n00b_string_t *token = n00b_hostmeta_env("GITHUB_TOKEN");
    if (token == nullptr) {
        // The token is not injected into the job environment
        // automatically; a workflow has to pass it explicitly.
        *code_out = "GITHUB_NO_TOKEN";
        *err_out  = r"GITHUB_TOKEN is empty. If this is running inside a "
                    r"GitHub action, make sure ${{ github.token }} is "
                    r"explicitly passed. See https://docs.github.com/en/"
                    r"actions/security-guides/automatic-token-authentication"
                    r"#using-the-github_token-in-a-workflow";
        return nullptr;
    }

    if (!n00b_unicode_str_starts_with(api, r"https://")
        && !n00b_unicode_str_starts_with(api, r"http://")) {
        *code_out = "GITHUB_INVALID_API_URL";
        *err_out  = n00b_cformat("invalid api url ([|#|]); cannot query repo "
                                 "node id",
                                 api);
        return nullptr;
    }

    n00b_string_t *url = n00b_cformat("[|#|]/repos/[|#|]",
                                      n00b_hostmeta_strip_slashes(api,
                                                                  false,
                                                                  true),
                                      n00b_hostmeta_strip_slashes(repo,
                                                                  true,
                                                                  false));

    n00b_http_h1_headers_t *headers = n00b_http_h1_headers_new();
    n00b_string_t          *bearer  = n00b_cformat("Bearer [|#|]", token);
    n00b_http_h1_headers_set(headers, "Authorization", bearer->data);
    n00b_http_h1_headers_set(headers, "Accept", "application/vnd.github+json");

    n00b_hostmeta_fetch_t f = n00b_hostmeta_fetch(ctx,
                                                  url,
                                                  .headers    = headers,
                                                  .strict     = true,
                                                  .timeout_ms = GITHUB_API_TIMEOUT_MS);
    if (!f.ok) {
        *code_out = "GITHUB_API_ERROR";
        *err_out  = f.error;
        return nullptr;
    }

    n00b_string_t    *parse_err = nullptr;
    n00b_json_node_t *body      = n00b_hostmeta_parse_json(f.body, &parse_err);
    if (body == nullptr) {
        *code_out = "GITHUB_API_BAD_JSON";
        *err_out  = parse_err;
        return nullptr;
    }
    return body;
}

static void
github_collect(n00b_hostmeta_collector_t *self, n00b_hostmeta_ctx_t *ctx)
{
    (void)self;

    n00b_string_t *sha = n00b_hostmeta_env("GITHUB_SHA");
    if (n00b_hostmeta_env("CI") == nullptr && sha == nullptr) {
        return;
    }

    n00b_string_t *server_url  = n00b_hostmeta_env("GITHUB_SERVER_URL");
    n00b_string_t *repository  = n00b_hostmeta_env("GITHUB_REPOSITORY");
    n00b_string_t *run_id      = n00b_hostmeta_env("GITHUB_RUN_ID");
    n00b_string_t *run_attempt = n00b_hostmeta_env("GITHUB_RUN_ATTEMPT");
    n00b_string_t *api_url     = n00b_hostmeta_env("GITHUB_API_URL");
    n00b_string_t *runner_temp = n00b_hostmeta_env("RUNNER_TEMP");

    // The job (check-run) id is not an official variable yet; the
    // setup-chalk-action injects it from the job context. When it is
    // present it identifies the job rather than the whole workflow run,
    // which is the tighter identity.
    // https://github.com/orgs/community/discussions/8945#discussioncomment-14374985
    n00b_string_t *check_run_id = n00b_hostmeta_env("GITHUB_CHECK_RUN_ID");

    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ID",
                         check_run_id ? check_run_id : run_id);
    n00b_hostmeta_ci_put(ctx, "BUILD_COMMIT_ID", sha);
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_ID",
                         n00b_hostmeta_env("GITHUB_REPOSITORY_ID"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_ORIGIN_OWNER_ID",
                         n00b_hostmeta_env("GITHUB_REPOSITORY_OWNER_ID"));
    n00b_hostmeta_ci_put(ctx, "BUILD_API_URI", api_url);
    n00b_hostmeta_ci_put(ctx, "BUILD_ATTEMPT", run_attempt);
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_WORKFLOW_NAME",
                         n00b_hostmeta_env("GITHUB_WORKFLOW"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_WORKFLOW_PATH",
                         n00b_hostmeta_env("GITHUB_WORKFLOW_REF"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_WORKFLOW_HASH",
                         n00b_hostmeta_env("GITHUB_WORKFLOW_SHA"));
    n00b_hostmeta_ci_put(ctx, "BUILD_REF", n00b_hostmeta_env("GITHUB_REF"));

    // RUNNER_TEMP is wiped by GitHub at the end of the job, so a file
    // there is unique to the job and shared by every step in it —
    // exactly the scope BUILD_UNIQUE_ID wants.
    if (runner_temp != nullptr) {
        n00b_list_t(n00b_string_t *) parts = n00b_list_new(n00b_string_t *);
        n00b_list_push(parts, runner_temp);
        n00b_list_push(parts, r"BUILD_UNIQUE_ID.chalk");

        n00b_string_t *unique = n00b_hostmeta_get_or_write_exclusive(
            n00b_path_join(&parts),
            n00b_hostmeta_random_hex64());
        n00b_hostmeta_ci_put(ctx, "BUILD_UNIQUE_ID", unique);
    }

    if (server_url != nullptr && repository != nullptr && run_id != nullptr) {
        n00b_string_t *base = n00b_cformat(
            "[|#|]/[|#|]",
            n00b_hostmeta_strip_slashes(server_url, false, true),
            n00b_hostmeta_strip_slashes(repository, true, true));

        n00b_string_t *uri = n00b_cformat("[|#|]/actions/runs/[|#|]",
                                          base,
                                          run_id);
        if (check_run_id != nullptr) {
            uri = n00b_cformat("[|#|]/job/[|#|]", uri, check_run_id);
        }
        else if (run_attempt != nullptr) {
            uri = n00b_cformat("[|#|]/attempts/[|#|]", uri, run_attempt);
        }

        n00b_hostmeta_ci_put(ctx, "BUILD_ORIGIN_URI", base);
        n00b_hostmeta_ci_put(ctx, "BUILD_URI", uri);
    }

    n00b_string_t *origin_key = n00b_hostmeta_scoped_key(ctx,
                                                         "BUILD_ORIGIN_KEY");
    n00b_string_t *owner_key  = n00b_hostmeta_scoped_key(
        ctx,
        "BUILD_ORIGIN_OWNER_KEY");

    if (api_url != nullptr && repository != nullptr
        && (n00b_hostmeta_subscribed(ctx, origin_key)
            || n00b_hostmeta_subscribed(ctx, owner_key))) {
        const char       *code = nullptr;
        n00b_string_t    *err  = nullptr;
        n00b_json_node_t *repo = github_get_repo(ctx,
                                                 api_url,
                                                 repository,
                                                 &code,
                                                 &err);
        if (repo == nullptr) {
            const char *why = "GitHub API call failed while fetching "
                              "repository node IDs";
            n00b_hostmeta_add_failure(ctx, origin_key, code, err, why);
            n00b_hostmeta_add_failure(ctx, owner_key, code, err, why);
        }
        else {
            n00b_json_node_t *node_id = n00b_hostmeta_json_path(repo, "node_id");
            n00b_json_node_t *owner   = n00b_hostmeta_json_path(repo,
                                                              "owner.node_id");
            if (n00b_json_is_string(node_id)) {
                n00b_hostmeta_put_string(ctx,
                                         origin_key,
                                         n00b_json_as_string(node_id));
            }
            if (n00b_json_is_string(owner)) {
                n00b_hostmeta_put_string(ctx,
                                         owner_key,
                                         n00b_json_as_string(owner));
            }
        }
    }

    // A tag push arrives as event "push"; the ref type is what
    // distinguishes it, and "tag" is the more useful trigger to report.
    // https://docs.github.com/en/actions/using-workflows/events-that-trigger-workflows
    n00b_string_t *event    = n00b_hostmeta_env("GITHUB_EVENT_NAME");
    n00b_string_t *ref_type = n00b_hostmeta_env("GITHUB_REF_TYPE");
    if (event != nullptr && ref_type != nullptr
        && n00b_unicode_str_eq(event, r"push")
        && n00b_unicode_str_eq(ref_type, r"tag")) {
        n00b_hostmeta_ci_put(ctx, "BUILD_TRIGGER", r"tag");
    }
    else {
        n00b_hostmeta_ci_put(ctx, "BUILD_TRIGGER", event);
    }

    n00b_hostmeta_ci_put_contact(ctx, n00b_hostmeta_env("GITHUB_ACTOR"));
}

n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_github = {
    .chalk_time = github_collect,
    .run_time   = github_collect,
};
