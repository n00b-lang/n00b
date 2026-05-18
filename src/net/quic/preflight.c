/*
 * preflight.c — Manifest readiness checks.
 *
 * Each finding is structured (check id, severity, detail,
 * remediation) so an LLM-driven ops loop can read the report,
 * apply fixes, and re-run.  See `~/dd/quic_2.md` § 9.3.
 */

#define N00B_USE_INTERNAL_API
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/result.h"
#include "conduit/print.h"
#include "net/quic/quic_types.h"
#include "net/quic/manifest.h"
#include "net/quic/secret.h"
#include "internal/net/quic/cert_provisioner.h"
#include "internal/net/quic/cert_provisioner_common.h"

/* ===========================================================================
 * Allocator + finding builders
 * =========================================================================== */

static n00b_allocator_t *
pf_alloc(void)
{
    return (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
}

static char *
pf_strdup(const char *s)
{
    if (!s) return nullptr;
    size_t l = strlen(s);
    char  *o = n00b_alloc_array_with_opts(char, (int64_t)(l + 1),
                                          &(n00b_alloc_opts_t){
                                              .allocator = pf_alloc(),
                                              .no_scan   = true,
                                          });
    memcpy(o, s, l + 1);
    return o;
}

/* Findings accumulator: typed `n00b_list_t` head with conduit-pool
 * allocator.  Replaces an earlier hand-rolled growable-array. */
typedef n00b_list_t(n00b_quic_preflight_finding_t *) finding_buf_t;

static void
fb_init(finding_buf_t *fb)
{
    *fb = n00b_list_new(n00b_quic_preflight_finding_t *);
    fb->allocator = pf_alloc();
}

static void
fb_push(finding_buf_t                  *fb,
        const char                     *check,
        n00b_quic_preflight_severity_t  severity,
        const char                     *detail,
        const char                     *remediation)
{
    n00b_quic_preflight_finding_t *f = n00b_alloc_with_opts(
        n00b_quic_preflight_finding_t,
        &(n00b_alloc_opts_t){.allocator = pf_alloc()});
    f->check       = check       ? n00b_string_from_cstr(check)       : nullptr;
    f->severity    = severity;
    f->detail      = detail      ? n00b_string_from_cstr(detail)      : nullptr;
    f->remediation = remediation ? n00b_string_from_cstr(remediation) : nullptr;
    n00b_list_push(*fb, f);
}

/* ===========================================================================
 * Individual checks
 * =========================================================================== */

static void
check_port_bind(finding_buf_t *fb, const n00b_buffer_t *host_buf, uint16_t port)
{
    const char *host = host_buf ? (const char *)host_buf->data : "";
    char check_id[256];
    snprintf(check_id, sizeof(check_id),
             "port-bind:%s:%u", host, (unsigned)port);

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;  /* QUIC is UDP */
    hints.ai_flags    = AI_PASSIVE;

    struct addrinfo *res = nullptr;
    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0 || !res) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Cannot resolve bind host: %s",
                 gai_strerror(rc));
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR, detail,
                "Check `bind_host` in the manifest; use a literal "
                "IPv4 (\"0.0.0.0\") or IPv6 (\"::\") wildcard for "
                "all-interfaces binds.");
        return;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "socket() failed",
                "Check process limits (ulimit -n) and kernel-side "
                "socket creation policies.");
        return;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (bind(fd, res->ai_addr, res->ai_addrlen) != 0) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "bind() failed: %s", strerror(errno));
        const char *remediation = (errno == EACCES)
            ? "Privileged port (<1024); run with elevated capabilities "
              "(e.g., setcap cap_net_bind_service=+ep on Linux) or "
              "configure the OS to allow binding."
            : (errno == EADDRINUSE)
                  ? "Another process already holds this port; identify "
                    "with `lsof -i :PORT` and resolve."
                  : "Inspect kernel networking state.";
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                detail, remediation);
        close(fd);
        freeaddrinfo(res);
        return;
    }
    close(fd);
    freeaddrinfo(res);
    fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_INFO,
            "bind() succeeded", nullptr);
}

/* Phase 5 § 5.2 — TCP-bind sanity for the metrics listener. */
static void
check_metrics_bind(finding_buf_t                      *fb,
                   const n00b_quic_manifest_metrics_t *m)
{
    const char *host = m->bind_host ? (const char *)m->bind_host->data
                                    : "::1";
    char check_id[128];
    snprintf(check_id, sizeof(check_id),
             "metrics-bind:%s:%u", host, (unsigned)m->bind_port);

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)m->bind_port);

    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;
    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0 || !res) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "Cannot resolve metrics bind host '%s': %s",
                 host, gai_strerror(rc));
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR, detail,
                "Check `observability.metrics.bind_host`; use a "
                "literal IPv4/IPv6 wildcard or loopback.");
        return;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "socket() failed for metrics bind",
                "Check process FD ulimit.");
        return;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (bind(fd, res->ai_addr, res->ai_addrlen) != 0) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "metrics bind() failed: %s", strerror(errno));
        const char *remediation = (errno == EACCES)
            ? "Pick a non-privileged port (>= 1024) for metrics."
            : (errno == EADDRINUSE)
                  ? "Another process holds the metrics port; "
                    "use `lsof -i :PORT` to identify it."
                  : "Inspect kernel networking state.";
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR, detail, remediation);
        close(fd);
        freeaddrinfo(res);
        return;
    }
    close(fd);
    freeaddrinfo(res);
    fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_INFO,
            "metrics bind() succeeded", nullptr);
}

static void
check_static_cert(finding_buf_t                       *fb,
                  const n00b_buffer_t                 *ep_id,
                  const n00b_quic_manifest_cert_t     *cert)
{
    char check_id[256];
    snprintf(check_id, sizeof(check_id),
             "cert-static:%s", ep_id ? (const char *)ep_id->data : "?");

    auto fr = n00b_certp_load_file(cert->chain_pem_path
                                   ? cert->chain_pem_path->data : nullptr);
    if (!n00b_result_is_ok(fr)) {
        char detail[512];
        snprintf(detail, sizeof(detail),
                 "Cannot read chain_pem_path '%s'",
                 cert->chain_pem_path ? (const char *)cert->chain_pem_path->data
                                      : "?");
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR, detail,
                "Make the file readable by the n00b-server process; "
                "check ownership and SELinux/AppArmor labels.");
        return;
    }
    n00b_buffer_t *pem = n00b_result_get(fr);
    auto dr = n00b_certp_pem_first_cert_to_der(pem);
    if (!n00b_result_is_ok(dr)) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "chain_pem_path doesn't contain a parseable PEM "
                "CERTIFICATE block",
                "Verify with `openssl x509 -in <path> -text -noout`.");
        return;
    }
    n00b_buffer_t *der = n00b_result_get(dr);
    int64_t nb = 0, na = 0;
    if (n00b_certp_parse_validity((const uint8_t *)der->data,
                                  (size_t)der->byte_len,
                                  &nb, &na) != 0) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "Could not parse Validity period from leaf cert",
                "Verify the cert isn't malformed.");
        return;
    }
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    int64_t now_ms = (int64_t)tv.tv_sec * 1000;
    if (now_ms < nb) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "Cert is not yet valid (notBefore is in the future)",
                "Wait or replace the cert.");
        return;
    }
    if (now_ms > na) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "Cert has already expired",
                "Renew or replace the cert.");
        return;
    }
    /* Soft-warn if the cert expires within 30 days. */
    int64_t margin_ms = (int64_t)30 * 24 * 60 * 60 * 1000;
    if (now_ms > na - margin_ms) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_WARN,
                "Cert expires within 30 days",
                "Schedule a renewal soon.");
    } else {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_INFO,
                "Cert is valid", nullptr);
    }
}

static void
check_external_argv(finding_buf_t                          *fb,
                    const n00b_buffer_t                    *ep_id,
                    n00b_list_t(n00b_buffer_t *)           *argv)
{
    char check_id[256];
    snprintf(check_id, sizeof(check_id),
             "cert-external:%s", ep_id ? (const char *)ep_id->data : "?");

    if (!argv || n00b_list_len(*argv) == 0) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "External cert provisioner has no argv[0]",
                "Add the command name to the manifest's argv array.");
        return;
    }
    n00b_buffer_t *cmd_buf = n00b_list_get(*argv, 0);
    if (!cmd_buf || cmd_buf->byte_len == 0) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "External cert provisioner has no argv[0]",
                "Add the command name to the manifest's argv array.");
        return;
    }
    const char *cmd = cmd_buf->data;

    /* Naïve PATH search: if argv[0] contains '/', treat as absolute /
     * relative; otherwise search PATH. */
    if (strchr(cmd, '/')) {
        if (access(cmd, X_OK) != 0) {
            char detail[512];
            snprintf(detail, sizeof(detail),
                     "argv[0] '%s' is not executable", cmd);
            fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR, detail,
                    "Check the file's permission bits (chmod +x).");
            return;
        }
    } else {
        const char *path_env = getenv("PATH");
        if (!path_env) {
            fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_WARN,
                    "PATH not set; cannot validate argv[0] location",
                    "Set PATH or use an absolute path in argv[0].");
            return;
        }
        bool found = false;
        char *path_copy = pf_strdup(path_env);
        char *saveptr   = nullptr;
        for (char *dir = strtok_r(path_copy, ":", &saveptr); dir;
             dir = strtok_r(nullptr, ":", &saveptr)) {
            char candidate[1024];
            snprintf(candidate, sizeof(candidate), "%s/%s", dir, cmd);
            if (access(candidate, X_OK) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            char detail[512];
            snprintf(detail, sizeof(detail),
                     "argv[0] '%s' not found in PATH", cmd);
            fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR, detail,
                    "Install the binary, or pass an absolute path in argv[0].");
            return;
        }
    }
    fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_INFO,
            "External cert command is executable", nullptr);
}

static void
check_acme_directory(finding_buf_t       *fb,
                     const n00b_buffer_t *ep_id,
                     const n00b_buffer_t *directory_url_buf)
{
    char check_id[256];
    snprintf(check_id, sizeof(check_id),
             "cert-acme-directory:%s", ep_id ? (const char *)ep_id->data : "?");
    if (!directory_url_buf || directory_url_buf->byte_len < 8
        || memcmp(directory_url_buf->data, "https://", 8) != 0) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "ACME directory_url must start with https://",
                "Use the canonical URL for your ACME provider.");
        return;
    }
    const char *directory_url = directory_url_buf->data;
    /* DNS resolution of the host portion (best-effort; not network-
     * gated since `getaddrinfo` will hit the system resolver). */
    const char *host_start = directory_url + 8;
    const char *slash      = strchr(host_start, '/');
    size_t      host_len   = slash ? (size_t)(slash - host_start)
                                   : strlen(host_start);
    char host_buf[256];
    if (host_len >= sizeof(host_buf)) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "directory_url host part is implausibly long", nullptr);
        return;
    }
    memcpy(host_buf, host_start, host_len);
    host_buf[host_len] = '\0';
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host_buf, "443", &hints, &res);
    if (rc != 0) {
        char detail[512];
        snprintf(detail, sizeof(detail),
                 "Cannot resolve ACME directory host '%s': %s",
                 host_buf, gai_strerror(rc));
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_WARN, detail,
                "Check DNS, or run preflight again with network "
                "access.");
        return;
    }
    if (res) freeaddrinfo(res);
    fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_INFO,
            "ACME directory host resolves", nullptr);
}

static void
check_secret_uri(finding_buf_t       *fb,
                 const char          *check_id,
                 const n00b_buffer_t *uri)
{
    if (!uri) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "Secret URI is missing",
                "Add the URI to the manifest.");
        return;
    }
    auto r = n00b_quic_secret_open((n00b_buffer_t *)uri);
    if (!n00b_result_is_ok(r)) {
        int err = (int)n00b_result_get_err(r);
        char detail[512];
        snprintf(detail, sizeof(detail),
                 "Cannot open secret URI '%s': %s",
                 (const char *)uri->data,
                 n00b_quic_err_str((n00b_quic_err_t)err));
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR, detail,
                "Check the secret-provider configuration "
                "(Keychain ACL / KMS policy / vault token / etc.) "
                "and that the URI scheme is registered.");
        return;
    }
    n00b_quic_secret_t *sec = n00b_result_get(r);
    n00b_quic_secret_close(sec);
    fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_INFO,
            "Secret URI opens cleanly", nullptr);
}

/* ===========================================================================
 * Phase 3 § 11 — auth checks
 * =========================================================================== */

static void
check_idp(finding_buf_t *fb, const n00b_quic_manifest_idp_t *idp)
{
    char check_id[256];
    snprintf(check_id, sizeof(check_id), "auth-idp:%s",
             idp->id ? (const char *)idp->id->data : "?");

    if (!idp->id) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "IdP entry missing 'id'", "Add an `id` to the IdP.");
        return;
    }
    if (!idp->issuer || idp->issuer->byte_len < 8
        || memcmp(idp->issuer->data, "https://", 8) != 0) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "IdP issuer must start with https://",
                "Use the canonical issuer URL for your IdP.");
        return;
    }
    /* DNS resolution of the host portion (same pattern as ACME). */
    const char *host_start = (const char *)idp->issuer->data + 8;
    const char *slash      = strchr(host_start, '/');
    size_t      host_len   = slash ? (size_t)(slash - host_start)
                                   : strlen(host_start);
    if (host_len == 0 || host_len >= 256) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "IdP issuer host is empty or implausibly long",
                "Check the issuer URL.");
        return;
    }
    char host_buf[256];
    memcpy(host_buf, host_start, host_len);
    host_buf[host_len] = '\0';
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host_buf, "443", &hints, &res);
    if (rc != 0) {
        char detail[512];
        snprintf(detail, sizeof(detail),
                 "Cannot resolve IdP host '%s': %s",
                 host_buf, gai_strerror(rc));
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_WARN, detail,
                "Check DNS, or run preflight again with network access.");
        return;
    }
    if (res) freeaddrinfo(res);
    fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_INFO,
            "IdP issuer host resolves", nullptr);
}

static void
check_policy(finding_buf_t                       *fb,
             const n00b_quic_manifest_policy_t   *pol,
             const n00b_quic_manifest_t          *m)
{
    char check_id[256];
    snprintf(check_id, sizeof(check_id), "auth-policy:%s",
             pol->id ? (const char *)pol->id->data : "?");

    if (!pol->id) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "policy entry missing 'id'",
                "Add a unique `id` to each policy.");
        return;
    }
    if (!pol->idp) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "policy entry missing 'idp'",
                "Reference an IdP from `auth.idps` by id.");
        return;
    }
    /* The referenced idp must be defined. */
    bool found = false;
    size_t idp_n = m->auth_idps ? (size_t)n00b_list_len(*m->auth_idps) : 0;
    for (size_t i = 0; i < idp_n; i++) {
        n00b_quic_manifest_idp_t *idp = n00b_list_get(*m->auth_idps, i);
        if (n00b_quic_mfbuf_eq(idp->id, pol->idp)) {
            found = true;
            break;
        }
    }
    if (!found) {
        char detail[512];
        snprintf(detail, sizeof(detail),
                 "policy '%s' references undefined idp '%s'",
                 (const char *)pol->id->data,
                 (const char *)pol->idp->data);
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR, detail,
                "Add the idp to `auth.idps[]` or correct the reference.");
        return;
    }
    /* Audience-empty WARN — a policy with no audience will accept
     * any token from the configured IdP regardless of who minted it
     * for whom; that's almost always a misconfiguration. */
    if (n00b_quic_mfbuf_empty(pol->audience)) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_WARN,
                "policy has no audience requirement",
                "Set `audience` to your service identifier so tokens "
                "minted for other services aren't accepted.");
    }

    fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_INFO,
            "policy idp reference resolves", nullptr);
}

/* ===========================================================================
 * Phase 4 § 4.11 — RPC service auth-policy reference check
 * =========================================================================== */

static void
check_rpc_service(finding_buf_t                          *fb,
                  const n00b_quic_manifest_rpc_service_t *svc,
                  const n00b_quic_manifest_t             *m)
{
    char check_id[256];
    snprintf(check_id, sizeof(check_id),
             "rpc-service:%s",
             svc->id ? (const char *)svc->id->data : "(unnamed)");

    if (!svc->id) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "rpc.services entry missing 'id'",
                "Set `id` to the gRPC-style service name "
                "(e.g., `pkg.v1.Service`).");
        return;
    }
    if (!svc->auth_policy) {
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR,
                "rpc.services entry missing 'auth_policy'",
                "Reference an `auth.policies[].id` so inbound calls "
                "are gated by a defined policy.");
        return;
    }
    bool found = false;
    size_t pol_n = m->auth_policies ? (size_t)n00b_list_len(*m->auth_policies) : 0;
    for (size_t i = 0; i < pol_n; i++) {
        n00b_quic_manifest_policy_t *pol2 = n00b_list_get(*m->auth_policies, i);
        if (n00b_quic_mfbuf_eq(pol2->id, svc->auth_policy)) {
            found = true;
            break;
        }
    }
    if (!found) {
        char detail[512];
        snprintf(detail, sizeof(detail),
                 "rpc.service '%s' references undefined auth_policy '%s'",
                 (const char *)svc->id->data,
                 (const char *)svc->auth_policy->data);
        fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_ERROR, detail,
                "Add the policy to `auth.policies[]` or correct "
                "the reference.");
        return;
    }
    fb_push(fb, check_id, N00B_QUIC_PREFLIGHT_INFO,
            "rpc service auth_policy reference resolves", nullptr);
}

/* ===========================================================================
 * Top-level walker
 * =========================================================================== */

n00b_result_t(n00b_quic_preflight_report_t *)
n00b_quic_preflight(n00b_quic_manifest_t *m)
{
    if (!m) {
        return n00b_result_err(n00b_quic_preflight_report_t *,
                               N00B_QUIC_ERR_NULL_ARG);
    }
    finding_buf_t fb;
    fb_init(&fb);

    size_t ep_n = m->endpoints
                  ? (size_t)n00b_list_len(*m->endpoints) : 0;
    for (size_t i = 0; i < ep_n; i++) {
        const n00b_quic_manifest_endpoint_t *ep =
            n00b_list_get(*m->endpoints, i);

        check_port_bind(&fb, ep->bind_host, ep->bind_port);

        switch (ep->cert.kind) {
        case N00B_QUIC_MANIFEST_CERT_STATIC: {
            check_static_cert(&fb, ep->id, &ep->cert);
            char check_id[256];
            snprintf(check_id, sizeof(check_id),
                     "cert-static-key:%s", ep->id);
            check_secret_uri(&fb, check_id, ep->cert.key_secret_uri);
            break;
        }
        case N00B_QUIC_MANIFEST_CERT_EXTERNAL: {
            check_external_argv(&fb, ep->id, ep->cert.argv);
            char check_id[256];
            snprintf(check_id, sizeof(check_id),
                     "cert-external-key:%s", ep->id);
            check_secret_uri(&fb, check_id, ep->cert.key_secret_uri);
            break;
        }
        case N00B_QUIC_MANIFEST_CERT_ACME: {
            check_acme_directory(&fb, ep->id, ep->cert.directory_url);
            char check_id[256];
            snprintf(check_id, sizeof(check_id),
                     "cert-acme-key:%s", ep->id);
            check_secret_uri(&fb, check_id, ep->cert.account_key_uri);
            break;
        }
        }
    }

    /* Phase 3 § 11 auth checks. */
    size_t idp_total = m->auth_idps
                       ? (size_t)n00b_list_len(*m->auth_idps)
                       : 0;
    for (size_t i = 0; i < idp_total; i++) {
        check_idp(&fb, n00b_list_get(*m->auth_idps, i));
    }
    size_t pol_total = m->auth_policies
                       ? (size_t)n00b_list_len(*m->auth_policies)
                       : 0;
    for (size_t i = 0; i < pol_total; i++) {
        check_policy(&fb, n00b_list_get(*m->auth_policies, i), m);
    }
    /* Phase 4 § 4.11 rpc service checks. */
    if (m->rpc_services) {
        size_t n = (size_t)n00b_list_len(*m->rpc_services);
        for (size_t i = 0; i < n; i++) {
            check_rpc_service(&fb,
                              n00b_list_get(*m->rpc_services, i),
                              m);
        }
    }
    /* Phase 5 § 5.2 metrics-bind sanity. */
    if (m->metrics) {
        check_metrics_bind(&fb, m->metrics);
    }

    n00b_quic_preflight_report_t *r = n00b_alloc_with_opts(
        n00b_quic_preflight_report_t,
        &(n00b_alloc_opts_t){.allocator = pf_alloc()});
    r->ok       = true;
    r->findings = n00b_alloc_with_opts(
        n00b_list_t(n00b_quic_preflight_finding_t *),
        &(n00b_alloc_opts_t){.allocator = pf_alloc()});
    *r->findings = fb;
    size_t fn = (size_t)n00b_list_len(*r->findings);
    for (size_t i = 0; i < fn; i++) {
        n00b_quic_preflight_finding_t *f = n00b_list_get(*r->findings, i);
        if (f->severity == N00B_QUIC_PREFLIGHT_ERROR) {
            r->ok = false;
            break;
        }
    }
    return n00b_result_ok(n00b_quic_preflight_report_t *, r);
}

/* ===========================================================================
 * Print helper
 * =========================================================================== */

void
n00b_quic_preflight_report_print(n00b_quic_preflight_report_t *r, int fd)
{
    if (!r) return;
    size_t fn = r->findings ? (size_t)n00b_list_len(*r->findings) : 0;
    n00b_printf("Preflight: «#» («#» findings)",
                r->ok ? "OK" : "FAILED", fn, .fd = fd);
    for (size_t i = 0; i < fn; i++) {
        n00b_quic_preflight_finding_t *f = n00b_list_get(*r->findings, i);
        const char *sev = "INFO";
        if (f->severity == N00B_QUIC_PREFLIGHT_WARN)  sev = "WARN";
        if (f->severity == N00B_QUIC_PREFLIGHT_ERROR) sev = "ERROR";
        n00b_printf("  [«#»] «#»: «#»",
                    sev,
                    f->check ? f->check->data : "<no-id>",
                    f->detail ? f->detail->data : "",
                    .fd = fd);
        if (f->remediation && f->severity != N00B_QUIC_PREFLIGHT_INFO) {
            n00b_printf("         remedy: «#»",
                        f->remediation->data, .fd = fd);
        }
    }
}
