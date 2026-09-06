#pragma once

/**
 * @file n00b_hostmeta.h
 * @brief Host / build-environment metadata collection.
 *
 * This is the n00b port of chalk's metadata "plugins" — the pieces
 * that answer *where did this run* rather than *what artifact is
 * this*. libchalk (`chalk/n00b_chalk.h`) marks artifacts; hostmeta
 * describes the machine and pipeline that produced or is running
 * them. The two compose: a hostmeta result serializes to JSON that
 * can be handed to `n00b_chalk_mark_set_attestation()` or emitted as
 * a standalone report.
 *
 * ### Collectors
 *
 * | Collector       | Source of truth                                |
 * |-----------------|------------------------------------------------|
 * | `cloud_metadata`| AWS IMDSv2 / Azure IMDS / GCP metadata server  |
 * | `aws_ecs`       | `ECS_CONTAINER_METADATA_URI[_V4]` endpoint     |
 * | `aws_lambda`    | `AWS_LAMBDA_*` env vars (+ optional STS hook)  |
 * | `ci_github`     | `GITHUB_*` env vars + GitHub REST API          |
 * | `ci_gitlab`     | GitLab predefined variables                    |
 * | `ci_jenkins`    | Jenkins pipeline env vars                      |
 * | `ci_circleci`   | CircleCI built-in env vars                     |
 * | `ci_azure_devops`| Azure Pipelines `BUILD_*` / `SYSTEM_*` vars   |
 * | `ci_bitbucket`  | Bitbucket Pipelines env vars                   |
 * | `ci_buildkite`  | Buildkite env vars                             |
 * | `ci_codebuild`  | AWS CodeBuild env vars                         |
 * | `ci_teamcity`   | TeamCity env vars + build-properties file      |
 *
 * ### The 80% case
 *
 * @code
 *   n00b_hostmeta_module_init();   // once, at startup
 *
 *   auto r = n00b_hostmeta_collect();
 *   if (n00b_result_is_ok(r)) {
 *       n00b_hostmeta_result_t *m = n00b_result_get(r);
 *       char *json = n00b_json_encode(m->keys, .canonical = true);
 *   }
 * @endcode
 *
 * Every knob chalk exposed through its config file is an `_kargs`
 * field on `n00b_hostmeta_collect()`; nothing reads a config file and
 * nothing is cached across calls.
 */

#include <n00b.h>

#include "adt/list.h"
#include "adt/result.h"
#include "core/string.h"
#include "parsers/json.h"

// ============================================================================
// Errors
// ============================================================================

/**
 * @brief Error domain for the hostmeta surface.
 */
typedef enum : int32_t {
    N00B_HOSTMETA_OK               = 0,
    N00B_HOSTMETA_ERR_ARG          = -1, /**< Null / malformed argument. */
    N00B_HOSTMETA_ERR_NOT_INIT     = -2, /**< Registry never initialized. */
    N00B_HOSTMETA_ERR_NO_SUCH_NAME = -3, /**< Named collector not registered. */
    N00B_HOSTMETA_ERR_DUPLICATE    = -4, /**< Name already registered. */
    N00B_HOSTMETA_ERR_FULL         = -5, /**< Registry at capacity. */
} n00b_hostmeta_err_t;

/** @brief Human-readable spelling of a hostmeta error code. */
extern n00b_string_t *n00b_hostmeta_err_str(n00b_hostmeta_err_t err);

// ============================================================================
// Phases
// ============================================================================

/**
 * @brief When the collection runs, which selects the key namespace.
 *
 * chalk splits host metadata into "what was true when the artifact was
 * chalked" and "what is true right now"; the latter is reported under
 * the same key names with a leading underscore. `n00b_hostmeta_scoped_key()`
 * applies that convention so collectors name a key once.
 */
typedef enum : uint8_t {
    N00B_HOSTMETA_PHASE_CHALK_TIME = 0, /**< Keys unprefixed. */
    N00B_HOSTMETA_PHASE_RUN_TIME   = 1, /**< Keys prefixed with `_`. */
} n00b_hostmeta_phase_t;

// ============================================================================
// Results
// ============================================================================

/**
 * @brief One key that was wanted but could not be collected.
 *
 * The port of chalk's `addFailedKey`. A failure is not an error: the
 * surrounding collect call still succeeds, because "IMDS is firewalled
 * off" is ordinary and the caller usually wants whatever else was
 * reachable. Failures exist so that absence is distinguishable from
 * emptiness downstream.
 */
typedef struct {
    n00b_string_t *key;         /**< Key that could not be filled in. */
    n00b_string_t *code;        /**< Stable machine-readable reason code. */
    n00b_string_t *error;       /**< Underlying error text, may be empty. */
    n00b_string_t *description; /**< Operator-facing explanation. */
} n00b_hostmeta_failure_t;

/**
 * @brief Everything one collect call produced.
 */
typedef struct {
    /** JSON object mapping key name to collected value. Never null. */
    n00b_json_node_t *keys;
    /** Keys that were wanted but unreachable. Never null; may be empty. */
    n00b_list_t(n00b_hostmeta_failure_t *) failures;
} n00b_hostmeta_result_t;

// ============================================================================
// Collection context
// ============================================================================

/**
 * @brief Predicate deciding whether a key is worth collecting.
 *
 * The port of chalk's `isSubscribedKey`. Returning false lets a
 * collector skip the network round trip that would have filled the
 * key in. A null predicate means "everything is subscribed".
 *
 * @param user Opaque pointer supplied alongside the predicate.
 * @param key  Fully scoped key name (already `_`-prefixed at run time).
 */
typedef bool (*n00b_hostmeta_key_filter_t)(void *user, n00b_string_t *key);

/**
 * @brief Supplies the ARN of the identity the process is running as.
 *
 * AWS Lambda publishes no account id, so chalk calls
 * `sts:GetCallerIdentity` to learn it. libn00b core deliberately does
 * not depend on the optional AWS substrate (`libn00b_aws`), so the
 * call is a caller-supplied hook instead: a program that links
 * `libn00b_aws` can wire `n00b_aws_sts_get_caller_identity()` straight
 * in, and everyone else gets the env-derived keys with the ARN keys
 * recorded as failures.
 *
 * @param user Opaque pointer supplied alongside the hook.
 * @return The caller-identity ARN, or nullptr if it cannot be resolved.
 */
typedef n00b_string_t *(*n00b_hostmeta_caller_arn_fn)(void *user);

/**
 * @brief Per-call state threaded through every collector.
 *
 * Collectors read the configuration fields and write through the
 * `n00b_hostmeta_put_*` / `n00b_hostmeta_add_failure` helpers rather
 * than touching `result` directly.
 */
typedef struct {
    n00b_hostmeta_phase_t phase;

    /** Key-subscription predicate; nullptr collects everything. */
    n00b_hostmeta_key_filter_t subscribed;
    void                      *subscribed_user;

    /** Caller-identity hook for AWS Lambda; nullptr disables ARN keys. */
    n00b_hostmeta_caller_arn_fn caller_arn;
    void                       *caller_arn_user;

    /** Link-local metadata address, e.g. `169.254.169.254`. */
    n00b_string_t *metadata_ip;

    /** Per-request deadline, milliseconds. */
    int32_t timeout_ms;
    /** Extra attempts after the first for each metadata request. */
    int32_t retries;
    /** When false, no collector opens a socket; env/file sources only. */
    bool allow_network;

    /**
     * Hardware-identity probe paths, overridable so the AWS / Azure /
     * GCP discrimination can be exercised from fixtures. Defaults are
     * the Linux sysfs locations chalk uses.
     */
    n00b_string_t *sys_vendor_path;
    n00b_string_t *sys_hypervisor_path;
    n00b_string_t *sys_product_path;
    n00b_string_t *sys_board_asset_tag_path;
    n00b_string_t *resolv_path;

    n00b_hostmeta_result_t *result;
    n00b_allocator_t       *allocator;
} n00b_hostmeta_ctx_t;

// ============================================================================
// Collectors
// ============================================================================

typedef struct n00b_hostmeta_collector n00b_hostmeta_collector_t;

/**
 * @brief One metadata source.
 *
 * A collector implements whichever phases it has something to say
 * about; a null callback means "nothing to contribute in that phase".
 * Callbacks report through @p ctx and never fail the enclosing collect
 * call — an unreachable source records a failure and returns.
 */
struct n00b_hostmeta_collector {
    n00b_string_t *name;
    void (*chalk_time)(n00b_hostmeta_collector_t *self,
                       n00b_hostmeta_ctx_t       *ctx);
    void (*run_time)(n00b_hostmeta_collector_t *self,
                     n00b_hostmeta_ctx_t       *ctx);
};

/**
 * @brief Register a collector under its `name`.
 *
 * Registration order is collection order. Re-registering a name is an
 * error rather than a silent replace, so a typo cannot quietly shadow
 * a builtin.
 *
 * @note The registry is unsynchronized. It is built once during
 *       startup and read-only thereafter; concurrent collects are
 *       fine, concurrent registration is not. Register everything
 *       before spawning threads.
 */
extern n00b_result_t(bool)
    n00b_hostmeta_register_collector(n00b_hostmeta_collector_t *collector);

/**
 * @brief Register every builtin collector. Idempotent.
 *
 * Call once during process startup, before the first collect. Follows
 * the libn00b module-init convention (explicit call, no
 * `[[gnu::constructor]]`).
 */
extern void n00b_hostmeta_module_init(void);

/** @brief Look up a registered collector by name, or nullptr. */
extern n00b_hostmeta_collector_t *
n00b_hostmeta_find_collector(n00b_string_t *name);

// ============================================================================
// Collection
// ============================================================================

/**
 * @brief Run every registered collector for one phase.
 *
 * Collectors run in registration order and merge into a single key
 * space; the first collector to set a key wins, which is why the
 * specific sources (`aws_ecs`, `aws_lambda`) register ahead of the
 * general `cloud_metadata` probe.
 *
 * @kw phase          Default `N00B_HOSTMETA_PHASE_RUN_TIME`.
 * @kw only           Optional collector name; runs just that one.
 * @kw subscribed     Key-subscription predicate. Default: collect all.
 * @kw subscribed_user Opaque pointer passed to @p subscribed.
 * @kw caller_arn     AWS caller-identity hook (see the typedef).
 * @kw caller_arn_user Opaque pointer passed to @p caller_arn.
 * @kw metadata_ip    Link-local metadata host. Default `169.254.169.254`.
 * @kw timeout_ms     Per-request deadline. Default 1000.
 * @kw retries        Extra attempts per request. Default 2.
 * @kw allow_network  Default true. False restricts to env / file sources.
 * @kw sys_vendor_path      Default `/sys/class/dmi/id/sys_vendor`.
 * @kw sys_hypervisor_path  Default `/sys/hypervisor/uuid`.
 * @kw sys_product_path     Default `/sys/class/dmi/id/product_uuid`.
 * @kw sys_board_asset_tag_path
 *                          Default `/sys/class/dmi/id/board_asset_tag`.
 * @kw resolv_path          Default `/etc/resolv.conf`.
 * @kw allocator      Allocator for the result tree.
 *
 * @return The populated result, or an error if @p only names a
 *         collector that is not registered.
 */
extern n00b_result_t(n00b_hostmeta_result_t *)
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
};

// ============================================================================
// Helpers for collector authors
// ============================================================================

/**
 * @brief Scope a base key name to the context's phase.
 *
 * Run time returns `_BASE`; chalk time returns `BASE` unchanged.
 */
extern n00b_string_t *
n00b_hostmeta_scoped_key(n00b_hostmeta_ctx_t *ctx, const char *base);

/** @brief Whether @p key is subscribed (and not already set). */
extern bool
n00b_hostmeta_subscribed(n00b_hostmeta_ctx_t *ctx, n00b_string_t *key);

/**
 * @brief Store @p value under @p key.
 *
 * No-op when the key is unsubscribed, already set, or @p value is
 * null — matching chalk's `setIfNeeded`, so collectors can enumerate
 * keys unconditionally and let the filter do the work.
 */
extern void n00b_hostmeta_put(n00b_hostmeta_ctx_t *ctx,
                              n00b_string_t       *key,
                              n00b_json_node_t    *value);

/** @brief `n00b_hostmeta_put` with a string value; skips empty strings. */
extern void n00b_hostmeta_put_string(n00b_hostmeta_ctx_t *ctx,
                                     n00b_string_t       *key,
                                     n00b_string_t       *value);

/** @brief `n00b_hostmeta_put_string` for a NUL-terminated value. */
extern void n00b_hostmeta_put_cstr(n00b_hostmeta_ctx_t *ctx,
                                   n00b_string_t       *key,
                                   const char          *value);

/** @brief Store a one-element array of @p value; skips empty strings. */
extern void n00b_hostmeta_put_string_list(n00b_hostmeta_ctx_t *ctx,
                                          n00b_string_t       *key,
                                          n00b_string_t       *value);

/** @brief Store the value of environment variable @p env_name under @p key. */
extern void n00b_hostmeta_put_env(n00b_hostmeta_ctx_t *ctx,
                                  n00b_string_t       *key,
                                  const char          *env_name);

/** @brief Read the already-stored value of @p key, or nullptr. */
extern n00b_json_node_t *
n00b_hostmeta_get(n00b_hostmeta_ctx_t *ctx, n00b_string_t *key);

/** @brief Record that @p key was wanted but could not be collected. */
extern void n00b_hostmeta_add_failure(n00b_hostmeta_ctx_t *ctx,
                                      n00b_string_t       *key,
                                      const char          *code,
                                      n00b_string_t       *error,
                                      const char          *description);

/**
 * @brief Environment lookup returning nullptr for unset *and* empty.
 *
 * Chalk's plugins treat `FOO=` and unset `FOO` identically; every
 * `getEnv(...) != ""` guard in the originals becomes a null check here.
 */
extern n00b_string_t *n00b_hostmeta_env(const char *name);

/**
 * @brief Read a whole file, or nullptr if it cannot be read.
 *
 * The port of chalk's `tryToLoadFile`: a missing or unreadable probe
 * path is the normal case off-cloud, not an error.
 */
extern n00b_string_t *n00b_hostmeta_read_file(n00b_string_t *path);
