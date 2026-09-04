#pragma once

/**
 * @file hostmeta_internal.h
 * @brief Module-internal helpers shared by the hostmeta collectors.
 *
 * Not part of the public surface. Doxygen for the public symbols
 * lives in `include/hostmeta/n00b_hostmeta.h`.
 */

#include <n00b.h>

#include "hostmeta/n00b_hostmeta.h"
#include "net/http/http_client.h"

/**
 * @brief Outcome of one metadata-endpoint request.
 *
 * Metadata endpoints answer 404 for "this instance has no such
 * attribute" as often as they answer 200, so a 404 is modelled as a
 * successful empty body rather than an error — the caller stores
 * nothing and moves on. `error` is set only for genuine failures
 * (unreachable, timeout, non-404 non-2xx) and carries text suitable
 * for a `n00b_hostmeta_failure_t`.
 */
typedef struct {
    bool           ok;
    int            status;
    n00b_string_t *body;  /**< Trimmed body; empty on 404. */
    n00b_string_t *error; /**< Failure text; nullptr when `ok`. */
} n00b_hostmeta_fetch_t;

/**
 * @brief Issue one plain-HTTP request against a metadata endpoint.
 *
 * Retries `ctx->retries` extra times on transport failure. Uses the
 * plain-HTTP path of the n00b HTTP client: metadata services live on
 * link-local addresses that serve no TLS.
 *
 * @param ctx Collection context (supplies retries and the default timeout).
 * @param url Absolute URL.
 *
 * @kw method     Verb. Default GET.
 * @kw headers    Extra request headers.
 * @kw strict     When true a 404 is a failure like any other status;
 *                when false a 404 yields `ok` with an empty body.
 *                Default false.
 * @kw timeout_ms Per-request deadline. Default 0 = `ctx->timeout_ms`,
 *                which is tuned for link-local metadata endpoints;
 *                override it for calls that leave the host.
 */
extern n00b_hostmeta_fetch_t
n00b_hostmeta_fetch(n00b_hostmeta_ctx_t *ctx, n00b_string_t *url) _kargs
{
    const char             *method     = nullptr;
    n00b_http_h1_headers_t *headers    = nullptr;
    bool                    strict     = false;
    int32_t                 timeout_ms = 0;
};

/** @brief `http://<ctx->metadata_ip><path>`. */
extern n00b_string_t *
n00b_hostmeta_metadata_url(n00b_hostmeta_ctx_t *ctx, const char *path);

/**
 * @brief Parse @p body as JSON, or nullptr with @p err_out set.
 *
 * An empty body parses to an empty object: metadata endpoints return
 * 200 with no content for attributes that exist but are unset.
 */
extern n00b_json_node_t *
n00b_hostmeta_parse_json(n00b_string_t *body, n00b_string_t **err_out);

/** @brief Deep-copy a JSON tree so results never alias parser state. */
extern n00b_json_node_t *n00b_hostmeta_json_copy(n00b_json_node_t *node);

/**
 * @brief Look up a dotted path (`container.ContainerARN`) in a tree.
 *
 * @return The node, or nullptr if any segment is missing or a
 *         non-terminal segment is not an object.
 */
extern n00b_json_node_t *
n00b_hostmeta_json_path(n00b_json_node_t *root, const char *dotted);

/** @brief Split on newlines, dropping empty lines. */
extern n00b_list_t(n00b_string_t *) *
    n00b_hostmeta_split_lines(n00b_string_t *s);

/** @brief ASCII-lowercase substring test. */
extern bool n00b_hostmeta_icontains(n00b_string_t *haystack,
                                    const char    *needle);

/** @brief ASCII-lowercase prefix test. */
extern bool n00b_hostmeta_istarts_with(n00b_string_t *s, const char *prefix);

/** @brief Drop leading/trailing `/` from @p s. */
extern n00b_string_t *n00b_hostmeta_strip_slashes(n00b_string_t *s,
                                                  bool           leading,
                                                  bool           trailing);

/**
 * @brief Normalize a VCS branch name to a `refs/...` ref.
 *
 * Several CI systems report a bare branch (`main`) or a remote-
 * qualified one (`origin/main`); chalk normalizes both to GitHub's
 * `refs/heads/main` spelling so `BUILD_REF` means one thing.
 */
extern n00b_string_t *n00b_hostmeta_branch_to_ref(n00b_string_t *branch);

/** @brief `n00b_string_from_cstr` that maps nullptr to nullptr. */
extern n00b_string_t *n00b_hostmeta_str(const char *s);

// ----------------------------------------------------------------------
// CI collector shared surface
//
// Every CI collector reports the same `BUILD_*` vocabulary, differing
// only in which vendor variable feeds which key. These wrappers apply
// the phase scoping so the per-vendor files read as a straight
// mapping table.
// ----------------------------------------------------------------------

/** @brief Store @p value under the phase-scoped `base` key. */
extern void n00b_hostmeta_ci_put(n00b_hostmeta_ctx_t *ctx,
                                 const char          *base,
                                 n00b_string_t       *value);

/** @brief Store `BUILD_CONTACT` as a one-element list, as chalk does. */
extern void n00b_hostmeta_ci_put_contact(n00b_hostmeta_ctx_t *ctx,
                                         n00b_string_t       *value);

/**
 * @brief Read @p path, or create it holding @p value if it is absent.
 *
 * Backs `BUILD_UNIQUE_ID`: the first process in a CI job to reach the
 * path wins, and every later process in the same job reads the same
 * value, so one identifier covers the whole job. Exclusive create is
 * what makes that race-free.
 *
 * @return The file's contents, or nullptr if it can neither be read
 *         nor created.
 */
extern n00b_string_t *
n00b_hostmeta_get_or_write_exclusive(n00b_string_t *path, n00b_string_t *value);

/** @brief 16 lowercase hex digits of `n00b_rand64()`. */
extern n00b_string_t *n00b_hostmeta_random_hex64(void);
