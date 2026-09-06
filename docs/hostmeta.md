# hostmeta — Host and Build-Environment Metadata

A C library for answering *where did this run*: which cloud, which
instance, which CI job. It is the counterpart to
[libchalk](libchalk.md), which answers *what artifact is this*. The two
compose — a hostmeta result serializes to JSON you can hand to
`n00b_chalk_mark_set_attestation()`, or emit as a standalone report.

This is the C port of the metadata plugins in
[chalk](https://github.com/crashappsec/chalk) v2.0.x, and it produces
the same key names, so reports from the two are directly comparable.

---

## Table of contents

1. [What's in the box](#whats-in-the-box)
2. [Linking and headers](#linking-and-headers)
3. [Collecting](#collecting)
4. [Phases and key names](#phases-and-key-names)
5. [Subscriptions](#subscriptions)
6. [Failures](#failures)
7. [Configuration](#configuration)
8. [AWS Lambda and the caller-identity hook](#aws-lambda-and-the-caller-identity-hook)
9. [Writing a collector](#writing-a-collector)
10. [Key reference](#key-reference)

---

## What's in the box

| Collector          | Source of truth                                     |
|--------------------|-----------------------------------------------------|
| `aws_ecs`          | `ECS_CONTAINER_METADATA_URI[_V4]` endpoint          |
| `aws_lambda`       | `AWS_LAMBDA_*` env vars (+ optional STS hook)       |
| `cloud_metadata`   | AWS IMDSv2 / Azure IMDS / GCP metadata server       |
| `ci_github`        | `GITHUB_*` env vars + one GitHub REST call          |
| `ci_gitlab`        | GitLab predefined variables                         |
| `ci_jenkins`       | Jenkins pipeline env vars                           |
| `ci_circleci`      | CircleCI built-in env vars                          |
| `ci_azure_devops`  | Azure Pipelines `BUILD_*` / `SYSTEM_*` vars         |
| `ci_bitbucket`     | Bitbucket Pipelines env vars                        |
| `ci_buildkite`     | Buildkite env vars                                  |
| `ci_codebuild`     | AWS CodeBuild env vars                              |
| `ci_teamcity`      | TeamCity env vars + build-properties file           |

Every collector guards on its own environment. Off-cloud and off-CI, a
full collect returns an empty key set rather than failing.

---

## Linking and headers

hostmeta is part of libn00b. Include the umbrella:

```c
#include <hostmeta/n00b_hostmeta.h>
```

or the pieces you need:

```c
#include <hostmeta/n00b_hostmeta.h>           // types, context, collect
#include <hostmeta/n00b_hostmeta_builtins.h>  // the collector objects
#include <hostmeta/n00b_hostmeta_aws.h>       // ARN parse / rebuild
```

The n00b runtime must be initialized, and the module registered once,
before the first collect:

```c
int main(int argc, char **argv) {
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);
    n00b_hostmeta_module_init();
    // ... collect ...
    n00b_shutdown();
}
```

`n00b_hostmeta_module_init()` is idempotent and does not use
`[[gnu::constructor]]`, per the libn00b module-init convention.

---

## Collecting

```c
auto r = n00b_hostmeta_collect();
if (n00b_result_is_err(r)) {
    // Only happens when `.only` names an unregistered collector.
    return 1;
}

n00b_hostmeta_result_t *m = n00b_result_get(r);
char *json = n00b_json_encode(m->keys, .canonical = true);
```

`m->keys` is a JSON object mapping key name to value; `m->failures` is
a list of keys that were wanted but unreachable. Both are always
non-null.

Run a single collector with `.only`:

```c
auto r = n00b_hostmeta_collect(.only = n00b_string_from_cstr("ci_github"));
```

Collectors run in registration order and merge into one key space; the
first collector to set a key keeps it. That is why `aws_ecs` and
`aws_lambda` register ahead of `cloud_metadata` — they know which AWS
service they are, whereas the generic IMDS probe can only infer it.

To assemble a narrower registry, skip `n00b_hostmeta_module_init()` and
register the collectors you want yourself:

```c
n00b_hostmeta_builtins_init();   // fills in the vtable names
n00b_hostmeta_register_collector(&n00b_hostmeta_collector_ci_github);
n00b_hostmeta_register_collector(&n00b_hostmeta_collector_ci_gitlab);
```

---

## Phases and key names

chalk splits host metadata into what was true when an artifact was
*chalked* and what is true *now*, reporting the latter under the same
names with a leading underscore. hostmeta keeps that convention:

```c
n00b_hostmeta_collect(.phase = N00B_HOSTMETA_PHASE_CHALK_TIME);  // BUILD_ID
n00b_hostmeta_collect(.phase = N00B_HOSTMETA_PHASE_RUN_TIME);    // _BUILD_ID
```

Run time is the default. The cloud collectors are an exception: their
`_OP_CLOUD_*` and `_AWS_*` keys carry the underscore as part of the
name itself and are not prefixed a second time. `aws_lambda` and
`cloud_metadata` are run-time only.

---

## Subscriptions

A collector will not make a network round trip for a key nobody wants.
Pass a predicate to say which keys those are:

```c
static bool
wanted(void *user, n00b_string_t *key)
{
    return n00b_unicode_str_starts_with(key, n00b_string_from_cstr("_AWS_"));
}

auto r = n00b_hostmeta_collect(.subscribed = wanted);
```

The predicate sees the fully scoped key name. The default — no
predicate — subscribes to everything.

---

## Failures

A source being unreachable is ordinary: IMDS is often firewalled off, a
GitHub token is often not passed to the job. Those do not fail the
collect; they land in `m->failures` so that *absent* stays
distinguishable from *empty*:

Failures respect the subscription filter — a key nobody asked for does
not get a failure entry either. They are *not* suppressed when the key
is already populated: a failure names the key it is about, which is not
always the key that went missing.

```c
for (size_t i = 0; i < m->failures.len; i++) {
    n00b_hostmeta_failure_t *f = m->failures.data[i];
    n00b_printf("«#»: «#» — «#»\n", f->key, f->code, f->description);
}
```

| Code                              | Meaning                                        |
|-----------------------------------|------------------------------------------------|
| `IMDS_DISABLED`                   | IMDSv2 answered 403; metadata is turned off    |
| `IMDS_UNREACHABLE`                | No IMDSv2 token — often a container hop limit  |
| `CLOUD_TAG_FETCH_ERROR`           | IMDS listed a tag it would not then serve      |
| `AZURE_IMDS_ERROR`                | Azure `/metadata/instance` unreachable         |
| `AZURE_IMDS_PARSE_ERROR`          | Azure IMDS returned non-JSON                   |
| `GCP_IMDS_ERROR`                  | GCP instance metadata unreachable              |
| `GCP_IMDS_PARSE_ERROR`            | GCP IMDS returned non-JSON                     |
| `GCP_PROJECT_METADATA_ERROR`      | GCP project metadata unreachable               |
| `ECS_METADATA_ERROR`              | ECS container document unreachable             |
| `ECS_TASK_METADATA_ERROR`         | ECS task document unreachable                  |
| `ECS_TASK_STATS_METADATA_ERROR`   | ECS task stats unreachable                     |
| `LAMBDA_NO_CALLER_IDENTITY`       | No `caller_arn` hook, so no ARN keys           |
| `LAMBDA_CALLER_IDENTITY_FAILED`   | The hook returned something that is not an ARN |
| `GITHUB_NO_TOKEN`                 | `GITHUB_TOKEN` not passed to the job           |
| `GITHUB_INVALID_API_URL`          | `GITHUB_API_URL` is not an http(s) URL         |
| `GITHUB_API_ERROR`                | GitHub REST call failed                        |
| `GITHUB_API_BAD_JSON`             | GitHub REST call returned non-JSON             |
| `TEAMCITY_PROPERTIES_UNREADABLE`  | Build-properties file could not be read        |

---

## Configuration

Everything chalk exposed through its config file is an `_kargs` field:

```c
auto r = n00b_hostmeta_collect(
    .timeout_ms    = 500,        // per request; default 1000
    .retries       = 1,          // extra attempts; default 2
    .metadata_ip   = n00b_string_from_cstr("169.254.169.254"),
    .allow_network = false);     // env / file sources only
```

`allow_network = false` is the switch for builds that must not touch
the network at all. It stops the request rather than letting it time
out.

The hardware-identity probe paths are overridable too, which is how the
AWS / Azure / GCP discrimination gets tested without a cloud:

```c
auto r = n00b_hostmeta_collect(
    .sys_vendor_path     = fixture,   // /sys/class/dmi/id/sys_vendor
    .sys_hypervisor_path = fixture,   // /sys/hypervisor/uuid
    .sys_product_path    = fixture,   // /sys/class/dmi/id/product_uuid
    .sys_board_asset_tag_path = fixture,
    .resolv_path         = fixture);  // /etc/resolv.conf
```

These are Linux sysfs locations. Elsewhere they simply do not exist,
the reads fail quietly, and no cloud is detected.

---

## AWS Lambda and the caller-identity hook

Lambda publishes no account id, so the function, version, and
log-stream ARNs cannot be built from the environment alone. chalk calls
`sts:GetCallerIdentity` to close the gap. libn00b core does not depend
on the optional AWS substrate, so the identity call is a hook instead:

```c
static n00b_string_t *
caller_arn(void *user)
{
    auto r = n00b_aws_sts_get_caller_identity((n00b_aws_config_t *)user);
    return n00b_result_is_ok(r) ? n00b_result_get(r)->arn : nullptr;
}

auto r = n00b_hostmeta_collect(.caller_arn      = caller_arn,
                               .caller_arn_user = my_aws_config);
```

Wire it up if you link `libn00b_aws` and the ARN keys appear. Without
it the env-derived keys are still collected and the five ARN keys are
reported as `LAMBDA_NO_CALLER_IDENTITY` failures.

An assumed-role session ARN is reduced to the durable role before the
function ARNs are derived, matching chalk: a session is not a stable
identity, and roles are global so they carry no region.

---

## Writing a collector

A collector is a vtable plus two optional callbacks:

```c
static void
my_collect(n00b_hostmeta_collector_t *self, n00b_hostmeta_ctx_t *ctx)
{
    n00b_string_t *v = n00b_hostmeta_env("MY_BUILD_SYSTEM_JOB");
    if (v == nullptr) {
        return;   // not running here
    }
    n00b_hostmeta_ci_put(ctx, "BUILD_ID", v);
}

n00b_hostmeta_collector_t my_collector = {
    .chalk_time = my_collect,
    .run_time   = my_collect,
};

my_collector.name = n00b_string_from_cstr("my_ci");
n00b_hostmeta_register_collector(&my_collector);
```

The `n00b_hostmeta_put_*` helpers are no-ops when the key is
unsubscribed, already set, or the value is empty — so you can enumerate
every key you know about unconditionally and let the gate decide, which
is what keeps the per-vendor files readable as mapping tables.

Callbacks never fail the enclosing collect. An unreachable source calls
`n00b_hostmeta_add_failure()` and returns.

---

## Key reference

**Build keys** (all CI collectors, unprefixed at chalk time and
`_`-prefixed at run time):

| Key                     | Meaning                                    |
|-------------------------|--------------------------------------------|
| `BUILD_ID`              | Job identifier                             |
| `BUILD_URI`             | Deep link to the job in the vendor's UI    |
| `BUILD_API_URI`         | Vendor API root                            |
| `BUILD_COMMIT_ID`       | Commit being built                         |
| `BUILD_REF`             | `refs/heads/...` or `refs/tags/...`        |
| `BUILD_TRIGGER`         | What started the build                     |
| `BUILD_CONTACT`         | List; who or what to attribute it to       |
| `BUILD_ORIGIN_ID`       | Repository / project id                    |
| `BUILD_ORIGIN_OWNER_ID` | Owner / org / namespace id                 |
| `BUILD_ORIGIN_URI`      | Repository URL                             |
| `BUILD_ORIGIN_KEY`      | GitHub only; repository GraphQL node id    |
| `BUILD_ORIGIN_OWNER_KEY`| GitHub only; owner GraphQL node id         |
| `BUILD_WORKFLOW_NAME`   | Pipeline / workflow name                   |
| `BUILD_WORKFLOW_PATH`   | Pipeline definition path or id             |
| `BUILD_WORKFLOW_HASH`   | GitHub only; workflow file commit          |
| `BUILD_ATTEMPT`         | GitHub only; run attempt number            |
| `BUILD_UNIQUE_ID`       | GitHub only; stable id for the whole job   |

**Cloud keys** (run time only):

| Key                                | Meaning                              |
|------------------------------------|--------------------------------------|
| `_OP_CLOUD_PROVIDER`               | `aws`, `azure`, or `gcp`             |
| `_OP_CLOUD_PROVIDER_SERVICE_TYPE`  | `aws_ec2` / `aws_ecs` / `aws_eks` / `aws_lambda` / `gcp_cloud_run_service` |
| `_OP_CLOUD_PROVIDER_REGION`        | Region                               |
| `_OP_CLOUD_PROVIDER_IP`            | First public IP                      |
| `_OP_CLOUD_PROVIDER_TAGS`          | Instance tags                        |
| `_OP_CLOUD_PROVIDER_ACCOUNT_INFO`  | Account / subscription / service acct|
| `_OP_CLOUD_PROVIDER_INSTANCE_TYPE` | Instance size                        |
| `_OP_CLOUD_PROVIDER_INSTANCE_ARCH` | AWS only; CPU architecture           |
| `_OP_CLOUD_SYS_VENDOR`             | Raw DMI vendor string                |
| `_OP_CLOUD_METADATA`               | Full blob, keyed by service          |
| `_AWS_*`                           | ~45 raw IMDS attributes              |
| `_AZURE_INSTANCE_METADATA`         | Full Azure IMDS document             |
| `_GCP_INSTANCE_METADATA`           | Full GCP instance document           |
| `_GCP_PROJECT_METADATA`            | Full GCP project document            |

`CLOUD_METADATA_WHEN_CHALKED` is the chalk-time counterpart of
`_OP_CLOUD_METADATA` and is currently populated by `aws_ecs` only.

The IMDS credentials document
(`_AWS_IDENTITY_CREDENTIALS_EC2_SECURITY_CREDENTIALS_EC2_INSTANCE`) is
reported for its structure and expiry: `SecretAccessKey` and `Token`
are replaced with `<<redacted>>` on read.
