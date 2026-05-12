/**
 * @file manifest.h
 * @brief Deployment manifest schema + loader (RFC 8555-driven n00b/QUIC).
 *
 * The manifest is the single declaration of how a server's QUIC
 * surface is configured: which ports it binds, what cert it
 * presents, where the cert comes from (static / external /
 * ACME-driven), and how shared symmetric secrets (stateless-reset,
 * address-validation token) are sourced.  An operator writes one
 * JSON file; the application loads it once at startup and uses it
 * to construct endpoints + run a `--preflight` validator before
 * actually binding ports.
 *
 * **Phase 2 v1 scope** (this header):
 *
 *   - Top-level: version (== 1), service_name, endpoints[].
 *   - Per-endpoint: id, bind_host, bind_port, alpns[], cert{}.
 *   - cert.kind ∈ {static, external, acme}:
 *       - static:   chain_pem_path + key_secret_uri
 *       - external: argv[] + chain_pem_path + key_secret_uri
 *       - acme:     directory_url + subject_names[] + challenge +
 *                   account_key_uri + contact_email
 *                   (full wiring waits for the cloud DNS
 *                   providers; v1 shape-validates only).
 *
 * **Deferred to Phase 2 follow-ups**: stateless_reset / address_-
 * validation / lb / dns_expectations sections of the design-doc
 * schema (§ 9.1 of `~/dd/quic_2.md`).  These reference secret
 * providers (vault:, kms:) that haven't shipped.  When they do,
 * they extend this schema additively — fields the loader doesn't
 * recognize today are flagged with an INFO finding from preflight,
 * not rejected.
 *
 * @see ~/dd/quic_2.md § 9
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "n00b.h"
#include "adt/list.h"
#include "adt/result.h"
#include "core/buffer.h"

/* ===========================================================================
 * Field helpers
 *
 * Manifest identifier fields are `n00b_buffer_t *` carrying NUL-
 * terminated UTF-8 byte strings (the trailing NUL is reserved past
 * `byte_len` so `->data` can be passed unchanged to libc string APIs).
 * These helpers are convenience wrappers around the raw bytes.
 * =========================================================================== */

/** @brief True if @p b is null or empty. */
static inline bool
n00b_quic_mfbuf_empty(const n00b_buffer_t *b)
{
    return !b || b->byte_len == 0;
}

/** @brief Byte-equality between two manifest buffers (null-tolerant). */
static inline bool
n00b_quic_mfbuf_eq(const n00b_buffer_t *a, const n00b_buffer_t *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    return a->byte_len == b->byte_len
           && memcmp(a->data, b->data, a->byte_len) == 0;
}

/** @brief Byte-equality between a manifest buffer and a NUL-terminated string. */
static inline bool
n00b_quic_mfbuf_eq_cstr(const n00b_buffer_t *a, const char *s)
{
    if (!a || !s) return false;
    size_t sl = strlen(s);
    return a->byte_len == sl && memcmp(a->data, s, sl) == 0;
}

/* ===========================================================================
 * Manifest types
 * =========================================================================== */

typedef enum {
    N00B_QUIC_MANIFEST_CERT_STATIC,
    N00B_QUIC_MANIFEST_CERT_EXTERNAL,
    N00B_QUIC_MANIFEST_CERT_ACME,
} n00b_quic_manifest_cert_kind_t;

/**
 * @brief Per-endpoint cert configuration.  Owned by the manifest.
 *
 * Identifier byte-strings are conduit-pool-allocated `n00b_buffer_t *`
 * (treated as opaque bytes, not displayable text).  Lists hold
 * `n00b_buffer_t *` per entry.  Fields not valid for a given `kind`
 * are nullptr.
 */
typedef struct {
    n00b_quic_manifest_cert_kind_t kind;

    /* static + external: */
    n00b_buffer_t *chain_pem_path;     /**< PEM cert chain on disk. */
    n00b_buffer_t *key_secret_uri;     /**< e.g., "keychain:..." or "ephemeral:..." */

    /* external only: */
    n00b_list_t(n00b_buffer_t *) *argv;  /**< argv[0] is the command. */

    /* acme only: */
    n00b_buffer_t *directory_url;     /**< https://acme-... */
    n00b_list_t(n00b_buffer_t *) *subject_names;  /**< DNS names */
    n00b_buffer_t *challenge;         /**< "http-01" / "dns-01" / "tls-alpn-01" */
    n00b_buffer_t *account_key_uri;
    n00b_buffer_t *contact_email;
} n00b_quic_manifest_cert_t;

/**
 * @brief One declared endpoint.
 */
typedef struct {
    n00b_buffer_t *id;
    n00b_buffer_t *bind_host;       /**< e.g., "0.0.0.0", "127.0.0.1" */
    uint16_t       bind_port;
    n00b_list_t(n00b_buffer_t *) *alpns;
    n00b_quic_manifest_cert_t cert;
} n00b_quic_manifest_endpoint_t;

/* ===========================================================================
 * Phase 3 § 11 — auth section
 * =========================================================================== */

/** @brief One declared IdP. */
typedef struct {
    n00b_buffer_t *id;                  /**< Stable identifier referenced by policies. */
    n00b_buffer_t *issuer;              /**< https:// URL */
    int32_t        jwks_cache_ttl_seconds; /**< 0 = use library default. */
} n00b_quic_manifest_idp_t;

typedef enum {
    N00B_QUIC_MANIFEST_CLAIM_EQUALS   = 0,
    N00B_QUIC_MANIFEST_CLAIM_CONTAINS = 1,
} n00b_quic_manifest_claim_op_t;

/** @brief One claim requirement. */
typedef struct {
    n00b_buffer_t                 *name;
    n00b_buffer_t                 *value;   /* needle for "contains" */
    n00b_quic_manifest_claim_op_t  op;
} n00b_quic_manifest_required_claim_t;

/** @brief One declared auth policy. */
typedef struct {
    n00b_buffer_t *id;                /**< Stable identifier (channels reference). */
    n00b_buffer_t *idp;               /**< References an idp by id. */
    n00b_buffer_t *audience;          /**< Optional audience requirement. */
    n00b_buffer_t *issuer_override;   /**< Optional override; defaults to idp.issuer. */
    bool           require_dpop;
    bool           require_mtls;
    n00b_list_t(n00b_quic_manifest_required_claim_t *) *required_claims;
} n00b_quic_manifest_policy_t;

/* ===========================================================================
 * Phase 4 § 4.11 — rpc.services section
 * =========================================================================== */

/**
 * @brief One declared RPC service binding to an auth policy.
 *
 * Phase 4 wire-format identifier; the service id is the gRPC-style
 * dotted name (e.g., `checkout.v1.Checkout`) and `auth_policy`
 * references an entry in `auth.policies[]` by id.
 */
typedef struct {
    n00b_buffer_t *id;             /**< Service identifier, e.g., `pkg.v1.Service`. */
    n00b_buffer_t *auth_policy;    /**< References an `auth.policies[].id`. */
} n00b_quic_manifest_rpc_service_t;

/* ===========================================================================
 * Phase 5 § 5.2 — observability section
 * =========================================================================== */

/** @brief Prometheus `/metrics` listener config. */
typedef struct {
    n00b_buffer_t *bind_host;     /**< nullptr = ::1 default. */
    uint16_t       bind_port;     /**< 0 = ephemeral; default 9100. */
} n00b_quic_manifest_metrics_t;

/**
 * @brief Top-level manifest.
 */
typedef struct {
    int                              version;
    n00b_buffer_t                   *service_name;
    n00b_list_t(n00b_quic_manifest_endpoint_t *) *endpoints;
    /* Phase 3 § 11 auth section.  Both lists may be empty/null. */
    n00b_list_t(n00b_quic_manifest_idp_t *)    *auth_idps;
    n00b_list_t(n00b_quic_manifest_policy_t *) *auth_policies;
    /* Phase 4 § 4.11 rpc section.  Empty list when absent. */
    n00b_list_t(n00b_quic_manifest_rpc_service_t *) *rpc_services;
    /* Phase 5 § 5.2 observability section.  nullptr if absent. */
    n00b_quic_manifest_metrics_t *metrics;
} n00b_quic_manifest_t;

/* ===========================================================================
 * Loaders
 * =========================================================================== */

/**
 * @brief Parse a manifest from JSON-encoded bytes.
 *
 * @param body  Bytes (typically the contents of a manifest.json on disk).
 *
 * @return Result: ok with the parsed manifest on success;
 *         err(@c N00B_QUIC_ERR_PROTOCOL) on JSON parse error or
 *         schema violation.
 */
extern n00b_result_t(n00b_quic_manifest_t *)
n00b_quic_manifest_load_json(n00b_buffer_t *body);

/**
 * @brief Convenience: load a manifest from a path on disk.
 */
extern n00b_result_t(n00b_quic_manifest_t *)
n00b_quic_manifest_load_path(const char *path);

/** @brief Free a manifest.  Idempotent. */
extern void
n00b_quic_manifest_close(n00b_quic_manifest_t *m);

/* ===========================================================================
 * Preflight
 *
 * Walks the manifest and runs the deployment-readiness checks
 * documented in `~/dd/quic_2.md` § 9.2.  v1 ships:
 *
 *   - Port-bind sanity (try `bind()`, close immediately).
 *   - Static-cert path exists, parses as PEM, leaf is currently valid.
 *   - External-cert command exists in PATH.
 *   - ACME directory URL has https:// shape; host resolves via DNS.
 *   - Account-key URI opens via `n00b_quic_secret_open`.
 *
 * Each failed check produces a `n00b_quic_preflight_finding_t` with
 * severity, detail, and a remediation hint.
 * =========================================================================== */

typedef enum {
    N00B_QUIC_PREFLIGHT_INFO,
    N00B_QUIC_PREFLIGHT_WARN,
    N00B_QUIC_PREFLIGHT_ERROR,
} n00b_quic_preflight_severity_t;

typedef struct {
    n00b_string_t                 *check;       /**< Stable id, e.g.,
                                                 *   "port-bind:0.0.0.0:443" */
    n00b_quic_preflight_severity_t severity;
    n00b_string_t                 *detail;      /**< Human-readable */
    n00b_string_t                 *remediation; /**< How to fix; nullable */
} n00b_quic_preflight_finding_t;

typedef struct {
    bool                             ok;        /**< false if any ERROR */
    n00b_list_t(n00b_quic_preflight_finding_t *) *findings;
} n00b_quic_preflight_report_t;

/**
 * @brief Run preflight checks against a parsed manifest.
 *
 * @return Result: always ok with a populated report; the `ok` field
 *         on the report indicates whether deployment should proceed.
 */
extern n00b_result_t(n00b_quic_preflight_report_t *)
n00b_quic_preflight(n00b_quic_manifest_t *m);

/** @brief Convenience: print the report through the conduit's stdio
 *         topology in human form.
 *
 * @param r   Report to print.
 * @param fd  Target file descriptor (1 = stdout, 2 = stderr).  The
 *            n00b conduit's managed-fd layer handles the actual write,
 *            so subscribers tee'd to either stream see the output. */
extern void
n00b_quic_preflight_report_print(n00b_quic_preflight_report_t *r, int fd);
