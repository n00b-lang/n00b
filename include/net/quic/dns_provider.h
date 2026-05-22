/**
 * @file dns_provider.h
 * @brief DNS provider trait + concrete implementations.
 *
 * The DNS provider abstraction sits between the ACME DNS-01
 * challenge wrapper (`acme_dns01.h`) and the operator's actual DNS
 * authority (Cloudflare, Route53, GCP DNS, manually-edited records).
 * The wrapper does the cryptography (compute the key-authorization,
 * SHA-256, base64url) and the protocol dance with the ACME server;
 * the provider does the network call to set / remove the underlying
 * TXT record.
 *
 * Phase 2 ships:
 *   - **manual**: prompts the operator on stderr and waits for an
 *     ENTER on stdin.  Useful for dev / CI / one-shot operator
 *     runs against Let's Encrypt staging (per `~/dd/quic_2.md` § 5.4).
 *   - **cloudflare**: API-token-driven, single-zone scope.  Calls
 *     out via the existing HTTPS shim.
 *
 * Route53 (AWS SigV4 + IMDSv2) and GCP DNS (metadata-server OAuth2)
 * are additive — they slot into this trait without API changes.
 *
 * Threading: the trait is synchronous.  Calls can take seconds
 * (DNS propagation waits, API rate limits) so callers shouldn't
 * hold a lock across them.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "n00b.h"
#include "adt/result.h"

typedef struct n00b_quic_dns_provider n00b_quic_dns_provider_t;

typedef struct {
    const char *api_token;
    const char *zone_id;
} n00b_quic_dns_cloudflare_config_t;

typedef struct {
    const char *access_key;
    const char *secret_key;
    const char *session_token;
    const char *hosted_zone_id;
} n00b_quic_dns_route53_config_t;

typedef struct {
    const char *project_id;
    const char *managed_zone;
} n00b_quic_dns_gcp_config_t;

struct n00b_quic_dns_provider {
    /** @brief Diagnostic name (logs / preflight reports). */
    const char *name;

    /**
     * @brief Set a TXT record at @p fqdn with @p value.
     *
     * Returns when the *authoritative server* has accepted the
     * record.  Global propagation is the DNS-01 challenge
     * wrapper's job — it polls `_acme-challenge.<domain>` until
     * a public resolver returns the new value.
     *
     * @return @c N00B_QUIC_OK on success; a negative
     *         @c n00b_quic_err_t on failure.
     */
    int (*set_txt)(n00b_quic_dns_provider_t *self,
                   const char               *fqdn,
                   const char               *value);

    /** @brief Remove the TXT record at @p fqdn whose value is
     *         @p value.  Idempotent: missing records aren't an error. */
    int (*remove_txt)(n00b_quic_dns_provider_t *self,
                      const char               *fqdn,
                      const char               *value);

    /** @brief Release any provider-private state.  Idempotent. */
    void (*close)(n00b_quic_dns_provider_t *self);

    /** @brief Provider-private state. */
    void *ctx;
};

/* ===========================================================================
 * Manual: prompt the operator
 * =========================================================================== */

/**
 * @brief Construct the manual DNS provider.
 *
 * `set_txt` writes to stderr:
 *
 *     [n00b acme] please set:
 *       _acme-challenge.api.example.com  TXT  "<value>"
 *     ...then press ENTER once the record is live.
 *
 * Reads one line from stdin to confirm.  `remove_txt` similarly
 * prompts for cleanup.
 *
 * Suitable for dev workflows + CI runs against Let's Encrypt
 * staging where DNS edits happen by hand.  Production deployments
 * should use a programmatic provider.
 */
extern n00b_quic_dns_provider_t *
n00b_quic_dns_provider_manual(void);

/* ===========================================================================
 * Cloudflare: API token + zone id
 * =========================================================================== */

/**
 * @brief Construct the Cloudflare DNS provider.
 *
 * @param config  Cloudflare API token plus zone identifier.  Strings are
 *                borrowed and copied internally.
 *
 * @return Owned provider; close with the trait's close() hook.
 */
extern n00b_result_t(n00b_quic_dns_provider_t *)
n00b_quic_dns_provider_cloudflare(n00b_quic_dns_cloudflare_config_t config);

extern n00b_result_t(n00b_quic_dns_provider_t *)
n00b_quic_dns_provider_cloudflare_raw(const char *api_token,
                                      const char *zone_id);

/* ===========================================================================
 * Route53: AWS Signature Version 4
 * =========================================================================== */

/**
 * @brief Construct the Route53 DNS provider with static AWS credentials.
 *
 * @param config  AWS credential fields and hosted-zone ID.
 *
 * Phase 2 v1 ships static credentials only.  Production deployments
 * typically use IMDSv2 / pod-identity to obtain temporary creds —
 * the supervisor refreshes them and rebuilds the provider.
 *
 * **Region note**: Route53 is global; the SigV4 credential scope
 * always uses `us-east-1` regardless of the caller's region.  This
 * is hardcoded in the impl.
 */
extern n00b_result_t(n00b_quic_dns_provider_t *)
n00b_quic_dns_provider_route53(n00b_quic_dns_route53_config_t config);

extern n00b_result_t(n00b_quic_dns_provider_t *)
n00b_quic_dns_provider_route53_raw(const char *access_key,
                                   const char *secret_key,
                                   const char *session_token,
                                   const char *hosted_zone_id);

/* ===========================================================================
 * GCP Cloud DNS: metadata-server / env-var token
 * =========================================================================== */

/**
 * @brief Construct the GCP Cloud DNS provider.
 *
 * @param config  GCP project ID and managed-zone *name* (not DNS name).
 *
 * Authentication: the provider fetches a short-lived OAuth2
 * access token from the GCE metadata server (any GCP-managed
 * compute environment exposes this).  For Phase 2 v1 the
 * metadata-server fetch is still TODO (waiting on plain-HTTP
 * support in the HTTP shim); the provider currently accepts a
 * pre-fetched token via the `N00B_GCP_ACCESS_TOKEN` environment
 * variable as a workaround.  Local-dev operators can run
 * `gcloud auth print-access-token` and export the result.
 *
 * Service-account-JSON auth requires RS256 signing which lands
 * with Phase 3 (RSA support arrives alongside JWT verify).
 */
extern n00b_result_t(n00b_quic_dns_provider_t *)
n00b_quic_dns_provider_gcp(n00b_quic_dns_gcp_config_t config);

extern n00b_result_t(n00b_quic_dns_provider_t *)
n00b_quic_dns_provider_gcp_raw(const char *project_id,
                               const char *managed_zone);
