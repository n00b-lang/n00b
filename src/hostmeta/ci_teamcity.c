/** @file src/hostmeta/ci_teamcity.c — TeamCity collector.
 *
 *  TeamCity exposes only a handful of facts as environment variables
 *  and puts the rest in a Java properties file whose path arrives in
 *  `TEAMCITY_BUILD_PROPERTIES_FILE`. The environment values claim
 *  their keys first, matching chalk's first-writer-wins behavior; the
 *  properties file fills in richer fields that the environment lacks.
 *
 *  https://www.jetbrains.com/help/teamcity/predefined-build-parameters.html
 */

#define N00B_USE_INTERNAL_API

#include "hostmeta/n00b_hostmeta_builtins.h"

#include "internal/hostmeta/hostmeta_internal.h"
#include "text/strings/string_ops.h"

#include <string.h>

/**
 * Apply one `key=value` line from the build-properties file.
 *
 * Properties files are `key=value` with the value running to end of
 * line, so only the first `=` separates.
 */
static void
apply_property(n00b_hostmeta_ctx_t *ctx, n00b_string_t *line)
{
    const char *eq = memchr(line->data, '=', line->u8_bytes);
    if (eq == nullptr) {
        return;
    }

    size_t key_len = (size_t)(eq - line->data);
    n00b_string_t *key = n00b_unicode_str_trim(
        n00b_string_from_raw(line->data, (int64_t)key_len));
    n00b_string_t *val = n00b_unicode_str_trim(
        n00b_string_from_raw(eq + 1,
                             (int64_t)(line->u8_bytes - key_len - 1)));

    if (val->u8_bytes == 0) {
        return;
    }

    if (n00b_unicode_str_eq(key, r"teamcity.build.id")) {
        n00b_hostmeta_ci_put(ctx, "BUILD_ID", val);
    }
    else if (n00b_unicode_str_eq(key, r"teamcity.serverUrl")) {
        n00b_hostmeta_ci_put(ctx, "BUILD_API_URI", val);
    }
    else if (n00b_unicode_str_eq(key, r"teamcity.buildType.id")) {
        n00b_hostmeta_ci_put(ctx, "BUILD_WORKFLOW_PATH", val);
    }
    else if (n00b_unicode_str_eq(key, r"teamcity.build.branch")) {
        n00b_hostmeta_ci_put(ctx, "BUILD_REF", n00b_hostmeta_branch_to_ref(val));
    }
    else if (n00b_unicode_str_eq(key, r"teamcity.build.triggeredBy.username")) {
        n00b_hostmeta_ci_put_contact(ctx, val);
    }
}

static void
teamcity_collect(n00b_hostmeta_collector_t *self, n00b_hostmeta_ctx_t *ctx)
{
    (void)self;

    if (n00b_hostmeta_env("TEAMCITY_VERSION") == nullptr) {
        return;
    }

    n00b_string_t *props_file = n00b_hostmeta_env(
        "TEAMCITY_BUILD_PROPERTIES_FILE");

    // Properties are read BEFORE the env-var fallbacks below, which is
    // the reverse of the statement order in chalk's ciTeamcity.nim. Do
    // not "correct" it to match: the two models have opposite
    // precedence, so matching the order would invert the result.
    // chalk's `setIfNeeded` ends in a plain table assignment
    // (`o[k] = value`, run_management.nim), so its later
    // properties-file write overwrites the earlier env write and the
    // properties win. hostmeta's gate is first-writer-wins, so the
    // same outcome requires writing the properties first. Both report
    // `teamcity.build.id` — the server-side build id — for BUILD_ID,
    // in preference to the user-visible BUILD_NUMBER counter.
    if (props_file != nullptr) {
        n00b_string_t *props = n00b_hostmeta_read_file(props_file);
        if (props != nullptr) {
            n00b_list_t(n00b_string_t *) *lines = n00b_hostmeta_split_lines(
                props);
            for (size_t i = 0; i < n00b_list_len(*lines); i++) {
                apply_property(ctx, n00b_list_get(*lines, i));
            }
        }
        else {
            n00b_hostmeta_add_failure(
                ctx,
                n00b_hostmeta_scoped_key(ctx, "BUILD_ID"),
                "TEAMCITY_PROPERTIES_UNREADABLE",
                props_file,
                "TEAMCITY_BUILD_PROPERTIES_FILE points at a file that could "
                "not be read; only environment-derived build keys are "
                "available");
        }
    }

    n00b_hostmeta_ci_put(ctx, "BUILD_ID", n00b_hostmeta_env("BUILD_NUMBER"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_COMMIT_ID",
                         n00b_hostmeta_env("BUILD_VCS_NUMBER"));
    n00b_hostmeta_ci_put(ctx, "BUILD_URI", n00b_hostmeta_env("BUILD_URL"));
    n00b_hostmeta_ci_put(ctx,
                         "BUILD_WORKFLOW_NAME",
                         n00b_hostmeta_env("TEAMCITY_BUILDCONF_NAME"));
}

n00b_hostmeta_collector_t n00b_hostmeta_collector_ci_teamcity = {
    .chalk_time = teamcity_collect,
    .run_time   = teamcity_collect,
};
