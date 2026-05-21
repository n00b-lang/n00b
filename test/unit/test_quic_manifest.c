/*
 * test_quic_manifest.c — manifest JSON loader + preflight tests.
 *
 * Coverage:
 *   1. Parse a minimal valid manifest (single endpoint, kind=static).
 *   2. Parse rejects: missing version, wrong version, missing fields.
 *   3. Parse all three cert kinds (static, external, acme).
 *   4. Preflight against a fully-formed manifest:
 *        - port-bind on an ephemeral 127.0.0.1 port → INFO
 *        - port-bind on a privileged port → ERROR (skipped if root)
 *        - missing chain_pem_path → ERROR
 *        - external argv with nonexistent argv[0] → ERROR
 *        - ACME directory_url with valid scheme but unresolved host
 *          → WARN
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"
#include "net/quic/quic_types.h"
#include "net/quic/manifest.h"

static n00b_buffer_t *
mk_json(const char *s)
{
    return n00b_buffer_from_bytes((char *)s, (int64_t)strlen(s));
}

/* ============================================================================
 * 1. Parse a minimal valid manifest
 * ============================================================================ */

static void
test_parse_minimal_static(void)
{
    const char *json =
        "{"
        " \"version\": 1,"
        " \"service_name\": \"test-service\","
        " \"endpoints\": ["
        "   {"
        "     \"id\": \"main\","
        "     \"bind_host\": \"127.0.0.1\","
        "     \"bind_port\": 0,"
        "     \"alpn\": [\"h3\"],"
        "     \"cert\": {"
        "       \"kind\": \"static\","
        "       \"chain_pem_path\": \"/tmp/n00b_manifest_test_cert.pem\","
        "       \"key_secret_uri\": \"ephemeral:test-key\""
        "     }"
        "   }"
        " ]"
        "}";
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    assert(n00b_result_is_ok(r));
    n00b_quic_manifest_t *m = n00b_result_get(r);
    assert(m->version == 1);
    assert(n00b_quic_mfbuf_eq_cstr(m->service_name, "test-service"));
    assert(n00b_list_len(*m->endpoints) == 1);
    n00b_quic_manifest_endpoint_t *ep0 = n00b_list_get(*m->endpoints, 0);
    assert(n00b_quic_mfbuf_eq_cstr(ep0->id, "main"));
    assert(n00b_quic_mfbuf_eq_cstr(ep0->bind_host, "127.0.0.1"));
    assert(ep0->bind_port == 0);
    assert(n00b_list_len(*ep0->alpns) == 1);
    assert(n00b_quic_mfbuf_eq_cstr(n00b_list_get(*ep0->alpns, 0), "h3"));
    assert(ep0->cert.kind == N00B_QUIC_MANIFEST_CERT_STATIC);
    assert(n00b_quic_mfbuf_eq_cstr(ep0->cert.chain_pem_path,
                                   "/tmp/n00b_manifest_test_cert.pem"));
    assert(n00b_quic_mfbuf_eq_cstr(ep0->cert.key_secret_uri,
                                   "ephemeral:test-key"));
    printf("  [PASS] parse minimal static manifest\n");
}

static void
test_parse_external(void)
{
    const char *json =
        "{"
        " \"version\": 1, \"service_name\": \"e\","
        " \"endpoints\": [{"
        "   \"id\": \"e1\", \"bind_host\": \"::\", \"bind_port\": 4433,"
        "   \"alpn\": [\"h3\", \"n00b/1\"],"
        "   \"cert\": {"
        "     \"kind\": \"external\","
        "     \"argv\": [\"step\", \"ca\", \"certificate\", \"d\", \"c\", \"k\"],"
        "     \"chain_pem_path\": \"/etc/n00b/cert.pem\","
        "     \"key_secret_uri\": \"keychain:n00b-cert-key\""
        "   }"
        " }]"
        "}";
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    assert(n00b_result_is_ok(r));
    n00b_quic_manifest_t *m = n00b_result_get(r);
    n00b_quic_manifest_endpoint_t *ep0 = n00b_list_get(*m->endpoints, 0);
    assert(ep0->cert.kind == N00B_QUIC_MANIFEST_CERT_EXTERNAL);
    assert(ep0->cert.argv && n00b_list_len(*ep0->cert.argv) == 6);
    n00b_buffer_t *cmd = n00b_list_get(*ep0->cert.argv, 0);
    assert(cmd && strcmp(cmd->data, "step") == 0);
    printf("  [PASS] parse external (argv-form) manifest\n");
}

static void
test_parse_acme(void)
{
    const char *json =
        "{"
        " \"version\": 1, \"service_name\": \"a\","
        " \"endpoints\": [{"
        "   \"id\": \"acme-ep\", \"bind_host\": \"0.0.0.0\","
        "   \"bind_port\": 443, \"alpn\": [\"h3\"],"
        "   \"cert\": {"
        "     \"kind\": \"acme\","
        "     \"directory_url\": \"https://acme-v02.api.letsencrypt.org/directory\","
        "     \"subject_names\": [\"api.example.com\", \"www.example.com\"],"
        "     \"challenge\": \"http-01\","
        "     \"account_key_uri\": \"keychain:le-account\","
        "     \"contact_email\": \"ops@example.com\""
        "   }"
        " }]"
        "}";
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    assert(n00b_result_is_ok(r));
    n00b_quic_manifest_t *m = n00b_result_get(r);
    n00b_quic_manifest_endpoint_t *ep0 = n00b_list_get(*m->endpoints, 0);
    n00b_quic_manifest_cert_t *c = &ep0->cert;
    assert(c->kind == N00B_QUIC_MANIFEST_CERT_ACME);
    assert(n00b_quic_mfbuf_eq_cstr(c->challenge, "http-01"));
    assert(n00b_list_len(*c->subject_names) == 2);
    assert(n00b_quic_mfbuf_eq_cstr(n00b_list_get(*c->subject_names, 0),
                                   "api.example.com"));
    printf("  [PASS] parse ACME manifest\n");
}

/* ============================================================================
 * 2. Parse rejects malformed manifests
 * ============================================================================ */

static void
test_parse_rejects(void)
{
    /* Missing version. */
    {
        auto r = n00b_quic_manifest_load_json(mk_json(
            "{\"service_name\":\"s\",\"endpoints\":[]}"));
        assert(n00b_result_is_err(r));
    }
    /* Wrong version. */
    {
        auto r = n00b_quic_manifest_load_json(mk_json(
            "{\"version\":2,\"service_name\":\"s\",\"endpoints\":[]}"));
        assert(n00b_result_is_err(r));
    }
    /* Empty endpoints. */
    {
        auto r = n00b_quic_manifest_load_json(mk_json(
            "{\"version\":1,\"service_name\":\"s\",\"endpoints\":[]}"));
        assert(n00b_result_is_err(r));
    }
    /* Endpoint missing bind_port. */
    {
        auto r = n00b_quic_manifest_load_json(mk_json(
            "{\"version\":1,\"service_name\":\"s\","
            "\"endpoints\":[{\"id\":\"x\",\"bind_host\":\"::\","
            "\"alpn\":[\"h3\"],\"cert\":{\"kind\":\"static\","
            "\"chain_pem_path\":\"/x\",\"key_secret_uri\":\"e:k\"}}]}"));
        assert(n00b_result_is_err(r));
    }
    /* Static cert missing chain_pem_path. */
    {
        auto r = n00b_quic_manifest_load_json(mk_json(
            "{\"version\":1,\"service_name\":\"s\","
            "\"endpoints\":[{\"id\":\"x\",\"bind_host\":\"::\","
            "\"bind_port\":0,\"alpn\":[\"h3\"],"
            "\"cert\":{\"kind\":\"static\","
            "\"key_secret_uri\":\"e:k\"}}]}"));
        assert(n00b_result_is_err(r));
    }
    /* Unknown cert kind. */
    {
        auto r = n00b_quic_manifest_load_json(mk_json(
            "{\"version\":1,\"service_name\":\"s\","
            "\"endpoints\":[{\"id\":\"x\",\"bind_host\":\"::\","
            "\"bind_port\":0,\"alpn\":[\"h3\"],"
            "\"cert\":{\"kind\":\"vault\"}}]}"));
        assert(n00b_result_is_err(r));
    }
    printf("  [PASS] parse rejects malformed manifests\n");
}

/* ============================================================================
 * 3. Preflight runs cleanly on an ephemeral-port + missing-cert manifest
 * ============================================================================ */

static int
finding_count_with_severity(n00b_quic_preflight_report_t *r,
                            n00b_quic_preflight_severity_t s)
{
    int n = 0;
    for (size_t i = 0; i < n00b_list_len(*r->findings); i++) {
        if (n00b_list_get(*r->findings, i)->severity == s) n++;
    }
    return n;
}

static void
test_preflight_nonexistent_cert(void)
{
    const char *json =
        "{"
        " \"version\": 1, \"service_name\": \"pf\","
        " \"endpoints\": [{"
        "   \"id\": \"e\", \"bind_host\": \"127.0.0.1\","
        "   \"bind_port\": 0, \"alpn\": [\"h3\"],"
        "   \"cert\": {"
        "     \"kind\": \"static\","
        "     \"chain_pem_path\": \"/nonexistent/n00b-pf-fixture\","
        "     \"key_secret_uri\": \"ephemeral:pf\""
        "   }"
        " }]"
        "}";
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    assert(n00b_result_is_ok(r));

    auto pr = n00b_quic_preflight(n00b_result_get(r));
    assert(n00b_result_is_ok(pr));
    n00b_quic_preflight_report_t *rep = n00b_result_get(pr);
    assert(rep->ok == false);  /* nonexistent cert is ERROR */
    assert(finding_count_with_severity(rep,
            N00B_QUIC_PREFLIGHT_ERROR) >= 1);
    /* Still reports the port-bind ok since 127.0.0.1:0 binds. */
    assert(finding_count_with_severity(rep,
            N00B_QUIC_PREFLIGHT_INFO) >= 1);
    printf("  [PASS] preflight surfaces missing-cert ERROR\n");
}

static void
test_preflight_external_no_argv0(void)
{
    const char *json =
        "{"
        " \"version\": 1, \"service_name\": \"pf2\","
        " \"endpoints\": [{"
        "   \"id\": \"e2\", \"bind_host\": \"127.0.0.1\","
        "   \"bind_port\": 0, \"alpn\": [\"h3\"],"
        "   \"cert\": {"
        "     \"kind\": \"external\","
        "     \"argv\": [\"/no/such/binary-12345\", \"arg\"],"
        "     \"chain_pem_path\": \"/tmp/x.pem\","
        "     \"key_secret_uri\": \"ephemeral:pf2\""
        "   }"
        " }]"
        "}";
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    auto pr = n00b_quic_preflight(n00b_result_get(r));
    n00b_quic_preflight_report_t *rep = n00b_result_get(pr);
    assert(rep->ok == false);
    /* Find a check id that starts with "cert-external:" with severity ERROR. */
    bool found = false;
    for (size_t i = 0; i < n00b_list_len(*rep->findings); i++) {
        if (strstr(n00b_list_get(*rep->findings, i)->check->data, "cert-external:") &&
            n00b_list_get(*rep->findings, i)->severity == N00B_QUIC_PREFLIGHT_ERROR) {
            found = true;
            break;
        }
    }
    assert(found);
    printf("  [PASS] preflight surfaces nonexistent-argv0 ERROR\n");
}

static void
test_preflight_acme_directory_url_shape(void)
{
    const char *json =
        "{"
        " \"version\": 1, \"service_name\": \"pf3\","
        " \"endpoints\": [{"
        "   \"id\": \"e3\", \"bind_host\": \"127.0.0.1\","
        "   \"bind_port\": 0, \"alpn\": [\"h3\"],"
        "   \"cert\": {"
        "     \"kind\": \"acme\","
        "     \"directory_url\": \"ftp://wrong-scheme/directory\","
        "     \"subject_names\": [\"x.example\"],"
        "     \"challenge\": \"http-01\","
        "     \"account_key_uri\": \"ephemeral:acct\","
        "     \"contact_email\": \"x@y\""
        "   }"
        " }]"
        "}";
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    auto pr = n00b_quic_preflight(n00b_result_get(r));
    n00b_quic_preflight_report_t *rep = n00b_result_get(pr);
    assert(rep->ok == false);
    bool found = false;
    for (size_t i = 0; i < n00b_list_len(*rep->findings); i++) {
        if (strstr(n00b_list_get(*rep->findings, i)->check->data, "cert-acme-directory:") &&
            n00b_list_get(*rep->findings, i)->severity == N00B_QUIC_PREFLIGHT_ERROR) {
            found = true;
            break;
        }
    }
    assert(found);
    printf("  [PASS] preflight surfaces non-https ACME directory ERROR\n");
}

/* ============================================================================
 * Phase 3 § 11 — auth section parsing + preflight
 * ============================================================================ */

static void
test_parse_auth_section(void)
{
    const char *json =
        "{"
        " \"version\": 1,"
        " \"service_name\": \"svc\","
        " \"endpoints\": [{"
        "   \"id\": \"main\", \"bind_host\": \"127.0.0.1\","
        "   \"bind_port\": 0, \"alpn\": [\"h3\"],"
        "   \"cert\": {\"kind\": \"static\","
        "     \"chain_pem_path\": \"/tmp/x\","
        "     \"key_secret_uri\": \"ephemeral:k\"}"
        " }],"
        " \"auth\": {"
        "   \"idps\": [{"
        "     \"id\": \"primary\","
        "     \"issuer\": \"https://login.example.com\","
        "     \"jwks_cache_ttl_seconds\": 1800"
        "   }],"
        "   \"policies\": [{"
        "     \"id\": \"rpc-rw\","
        "     \"idp\": \"primary\","
        "     \"audience\": \"checkout-api\","
        "     \"require_dpop\": true,"
        "     \"require_claim\": ["
        "       {\"name\":\"scope\",\"contains\":\"rpc:write\"},"
        "       {\"name\":\"role\",\"equals\":\"admin\"}"
        "     ]"
        "   }]"
        " }"
        "}";
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    assert(n00b_result_is_ok(r));
    n00b_quic_manifest_t *m = n00b_result_get(r);
    assert(n00b_list_len(*m->auth_idps) == 1);
    n00b_quic_manifest_idp_t *idp0 = n00b_list_get(*m->auth_idps, 0);
    assert(n00b_quic_mfbuf_eq_cstr(idp0->id, "primary"));
    assert(n00b_quic_mfbuf_eq_cstr(idp0->issuer, "https://login.example.com"));
    assert(idp0->jwks_cache_ttl_seconds == 1800);
    assert(n00b_list_len(*m->auth_policies) == 1);
    n00b_quic_manifest_policy_t *pol = n00b_list_get(*m->auth_policies, 0);
    assert(n00b_quic_mfbuf_eq_cstr(pol->id, "rpc-rw"));
    assert(n00b_quic_mfbuf_eq_cstr(pol->idp, "primary"));
    assert(n00b_quic_mfbuf_eq_cstr(pol->audience, "checkout-api"));
    assert(pol->require_dpop);
    assert(!pol->require_mtls);
    assert(n00b_list_len(*pol->required_claims) == 2);
    n00b_quic_manifest_required_claim_t *rc0 =
        n00b_list_get(*pol->required_claims, 0);
    n00b_quic_manifest_required_claim_t *rc1 =
        n00b_list_get(*pol->required_claims, 1);
    assert(n00b_quic_mfbuf_eq_cstr(rc0->name, "scope"));
    assert(rc0->op == N00B_QUIC_MANIFEST_CLAIM_CONTAINS);
    assert(n00b_quic_mfbuf_eq_cstr(rc1->name, "role"));
    assert(rc1->op == N00B_QUIC_MANIFEST_CLAIM_EQUALS);
    printf("  [PASS] auth.idps + auth.policies parse with claims\n");
}

static void
test_preflight_idp_invalid(void)
{
    /* http:// (not https) → ERROR. */
    const char *json =
        "{ \"version\": 1, \"service_name\": \"s\","
        " \"endpoints\": [{\"id\":\"e\",\"bind_host\":\"127.0.0.1\","
        "  \"bind_port\":0,\"alpn\":[\"h3\"],"
        "  \"cert\":{\"kind\":\"static\","
        "    \"chain_pem_path\":\"/tmp/n00b_manifest_test_cert.pem\","
        "    \"key_secret_uri\":\"ephemeral:k\"}}],"
        " \"auth\":{\"idps\":[{\"id\":\"bad\","
        "                     \"issuer\":\"http://insecure\"}]}"
        "}";
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    assert(n00b_result_is_ok(r));
    n00b_quic_manifest_t *m = n00b_result_get(r);
    auto pr = n00b_quic_preflight(m);
    n00b_quic_preflight_report_t *rep = n00b_result_get(pr);
    bool found = false;
    for (size_t i = 0; i < n00b_list_len(*rep->findings); i++) {
        if (n00b_list_get(*rep->findings, i)->severity == N00B_QUIC_PREFLIGHT_ERROR
            && strstr(n00b_list_get(*rep->findings, i)->check->data, "auth-idp:bad")) {
            found = true;
            break;
        }
    }
    assert(found);
    printf("  [PASS] preflight surfaces non-https IdP issuer ERROR\n");
}

static void
test_preflight_policy_undefined_idp(void)
{
    const char *json =
        "{ \"version\": 1, \"service_name\": \"s\","
        " \"endpoints\": [{\"id\":\"e\",\"bind_host\":\"127.0.0.1\","
        "  \"bind_port\":0,\"alpn\":[\"h3\"],"
        "  \"cert\":{\"kind\":\"static\","
        "    \"chain_pem_path\":\"/tmp/n00b_manifest_test_cert.pem\","
        "    \"key_secret_uri\":\"ephemeral:k\"}}],"
        " \"auth\":{"
        "   \"idps\":[{\"id\":\"primary\","
        "             \"issuer\":\"https://login.example.com\"}],"
        "   \"policies\":[{\"id\":\"p1\",\"idp\":\"nonexistent\","
        "                  \"audience\":\"a\"}]"
        " }}";
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    assert(n00b_result_is_ok(r));
    n00b_quic_manifest_t *m = n00b_result_get(r);
    auto pr = n00b_quic_preflight(m);
    n00b_quic_preflight_report_t *rep = n00b_result_get(pr);
    bool found = false;
    for (size_t i = 0; i < n00b_list_len(*rep->findings); i++) {
        if (n00b_list_get(*rep->findings, i)->severity == N00B_QUIC_PREFLIGHT_ERROR
            && strstr(n00b_list_get(*rep->findings, i)->check->data, "auth-policy:p1")
            && n00b_list_get(*rep->findings, i)->detail
            && strstr(n00b_list_get(*rep->findings, i)->detail->data, "nonexistent")) {
            found = true;
            break;
        }
    }
    assert(found);
    printf("  [PASS] preflight surfaces dangling policy.idp reference\n");
}

static void
test_preflight_policy_audience_empty(void)
{
    /* Policy without audience — should produce a WARN. */
    const char *json =
        "{ \"version\": 1, \"service_name\": \"s\","
        " \"endpoints\": [{\"id\":\"e\",\"bind_host\":\"127.0.0.1\","
        "  \"bind_port\":0,\"alpn\":[\"h3\"],"
        "  \"cert\":{\"kind\":\"static\","
        "    \"chain_pem_path\":\"/tmp/n00b_manifest_test_cert.pem\","
        "    \"key_secret_uri\":\"ephemeral:k\"}}],"
        " \"auth\":{"
        "   \"idps\":[{\"id\":\"primary\","
        "             \"issuer\":\"https://login.example.com\"}],"
        "   \"policies\":[{\"id\":\"p\",\"idp\":\"primary\"}]"
        " }}";
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    assert(n00b_result_is_ok(r));
    n00b_quic_manifest_t *m = n00b_result_get(r);
    auto pr = n00b_quic_preflight(m);
    n00b_quic_preflight_report_t *rep = n00b_result_get(pr);
    bool warn_found = false;
    for (size_t i = 0; i < n00b_list_len(*rep->findings); i++) {
        if (n00b_list_get(*rep->findings, i)->severity == N00B_QUIC_PREFLIGHT_WARN
            && strstr(n00b_list_get(*rep->findings, i)->check->data, "auth-policy:p")
            && n00b_list_get(*rep->findings, i)->detail
            && strstr(n00b_list_get(*rep->findings, i)->detail->data, "no audience")) {
            warn_found = true;
            break;
        }
    }
    assert(warn_found);
    printf("  [PASS] preflight WARNs on policy without audience\n");
}

/* ============================================================================
 * Phase 4 § 4.11 — rpc.services parsing + preflight
 * ============================================================================ */

static const char *
rpc_manifest_with_policy(const char *services_json)
{
    /* The caller must keep this static buffer's lifetime correct;
     * it's reused per call. */
    static char buf[2048];
    snprintf(buf, sizeof(buf),
        "{ \"version\": 1, \"service_name\": \"s\","
        " \"endpoints\": [{\"id\":\"e\",\"bind_host\":\"127.0.0.1\","
        "  \"bind_port\":0,\"alpn\":[\"h3\"],"
        "  \"cert\":{\"kind\":\"static\","
        "    \"chain_pem_path\":\"/tmp/n00b_manifest_test_cert.pem\","
        "    \"key_secret_uri\":\"ephemeral:k\"}}],"
        " \"auth\":{"
        "   \"idps\":[{\"id\":\"primary\","
        "             \"issuer\":\"https://login.example.com\"}],"
        "   \"policies\":[{\"id\":\"rpc-readwrite\",\"idp\":\"primary\","
        "                 \"audience\":\"svc\"}]"
        " },"
        " \"rpc\":{ \"services\": %s }"
        "}", services_json);
    return buf;
}

static void
test_parse_rpc_services(void)
{
    const char *json = rpc_manifest_with_policy(
        "[{\"id\":\"checkout.v1.Checkout\","
        "  \"auth_policy\":\"rpc-readwrite\"}]");
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    assert(n00b_result_is_ok(r));
    n00b_quic_manifest_t *m = n00b_result_get(r);
    assert(m->rpc_services != nullptr);
    assert(n00b_list_len(*m->rpc_services) == 1);
    n00b_quic_manifest_rpc_service_t *svc = n00b_list_get(*m->rpc_services, 0);
    assert(n00b_quic_mfbuf_eq_cstr(svc->id, "checkout.v1.Checkout"));
    assert(n00b_quic_mfbuf_eq_cstr(svc->auth_policy, "rpc-readwrite"));
    printf("  [PASS] parse rpc.services entry\n");
}

static void
test_parse_rpc_services_missing_fields(void)
{
    /* Missing id → reject. */
    const char *no_id = rpc_manifest_with_policy(
        "[{\"auth_policy\":\"rpc-readwrite\"}]");
    auto r1 = n00b_quic_manifest_load_json(mk_json(no_id));
    assert(!n00b_result_is_ok(r1));

    /* Missing auth_policy → reject. */
    const char *no_pol = rpc_manifest_with_policy(
        "[{\"id\":\"x.v1.X\"}]");
    auto r2 = n00b_quic_manifest_load_json(mk_json(no_pol));
    assert(!n00b_result_is_ok(r2));

    printf("  [PASS] rpc.services rejects missing id / auth_policy\n");
}

static void
test_preflight_rpc_service_resolves(void)
{
    const char *json = rpc_manifest_with_policy(
        "[{\"id\":\"checkout.v1.Checkout\","
        "  \"auth_policy\":\"rpc-readwrite\"}]");
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    assert(n00b_result_is_ok(r));
    n00b_quic_manifest_t *m = n00b_result_get(r);
    auto pr = n00b_quic_preflight(m);
    n00b_quic_preflight_report_t *rep = n00b_result_get(pr);
    bool info_found = false;
    for (size_t i = 0; i < n00b_list_len(*rep->findings); i++) {
        if (n00b_list_get(*rep->findings, i)->severity == N00B_QUIC_PREFLIGHT_INFO
            && strstr(n00b_list_get(*rep->findings, i)->check->data,
                      "rpc-service:checkout.v1.Checkout")
            && n00b_list_get(*rep->findings, i)->detail
            && strstr(n00b_list_get(*rep->findings, i)->detail->data, "resolves")) {
            info_found = true;
            break;
        }
    }
    assert(info_found);
    /* No ERROR for the rpc-service check. */
    for (size_t i = 0; i < n00b_list_len(*rep->findings); i++) {
        if (n00b_list_get(*rep->findings, i)->severity == N00B_QUIC_PREFLIGHT_ERROR
            && strstr(n00b_list_get(*rep->findings, i)->check->data, "rpc-service:")) {
            assert(0 && "rpc-service should not error when ref resolves");
        }
    }
    printf("  [PASS] preflight INFO when rpc.service.auth_policy resolves\n");
}

static void
test_preflight_rpc_service_dangling_policy(void)
{
    const char *json = rpc_manifest_with_policy(
        "[{\"id\":\"checkout.v1.Checkout\","
        "  \"auth_policy\":\"missing-policy\"}]");
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    assert(n00b_result_is_ok(r));
    n00b_quic_manifest_t *m = n00b_result_get(r);
    auto pr = n00b_quic_preflight(m);
    n00b_quic_preflight_report_t *rep = n00b_result_get(pr);
    assert(!rep->ok);
    bool err_found = false;
    for (size_t i = 0; i < n00b_list_len(*rep->findings); i++) {
        if (n00b_list_get(*rep->findings, i)->severity == N00B_QUIC_PREFLIGHT_ERROR
            && strstr(n00b_list_get(*rep->findings, i)->check->data,
                      "rpc-service:checkout.v1.Checkout")
            && n00b_list_get(*rep->findings, i)->detail
            && strstr(n00b_list_get(*rep->findings, i)->detail->data, "missing-policy")) {
            err_found = true;
            break;
        }
    }
    assert(err_found);
    printf("  [PASS] preflight ERRORs on dangling rpc.service auth_policy\n");
}

/* ============================================================================
 * Phase 5 § 5.2 — observability.metrics
 * ============================================================================ */

static const char *
manifest_with_metrics(const char *metrics_block)
{
    static char buf[2048];
    snprintf(buf, sizeof(buf),
             "{ \"version\": 1, \"service_name\": \"obs\","
             " \"endpoints\": [{\"id\":\"e\",\"bind_host\":\"127.0.0.1\","
             "  \"bind_port\":0,\"alpn\":[\"h3\"],"
             "  \"cert\":{\"kind\":\"static\","
             "    \"chain_pem_path\":\"/tmp/n00b_manifest_test_cert.pem\","
             "    \"key_secret_uri\":\"ephemeral:k\"}}],"
             " \"observability\": { \"metrics\": %s }"
             "}", metrics_block);
    return buf;
}

static void
test_parse_observability_metrics(void)
{
    auto r = n00b_quic_manifest_load_json(mk_json(manifest_with_metrics(
        "{\"bind_host\":\"127.0.0.1\",\"bind_port\":9100}")));
    assert(n00b_result_is_ok(r));
    n00b_quic_manifest_t *m = n00b_result_get(r);
    assert(m->metrics != nullptr);
    assert(n00b_quic_mfbuf_eq_cstr(m->metrics->bind_host, "127.0.0.1"));
    assert(m->metrics->bind_port == 9100);

    /* Defaults: omitted bind_host / bind_port should still parse. */
    auto r2 = n00b_quic_manifest_load_json(mk_json(manifest_with_metrics("{}")));
    assert(n00b_result_is_ok(r2));
    n00b_quic_manifest_t *m2 = n00b_result_get(r2);
    assert(m2->metrics != nullptr);
    assert(m2->metrics->bind_host == nullptr);  /* default applied at use-time */
    assert(m2->metrics->bind_port == 9100);

    /* Absent observability section → nullptr. */
    auto r3 = n00b_quic_manifest_load_json(mk_json(
        "{ \"version\":1, \"service_name\":\"x\","
        " \"endpoints\":[{\"id\":\"e\",\"bind_host\":\"127.0.0.1\","
        "  \"bind_port\":0,\"alpn\":[\"h3\"],"
        "  \"cert\":{\"kind\":\"static\","
        "    \"chain_pem_path\":\"/tmp/n00b_manifest_test_cert.pem\","
        "    \"key_secret_uri\":\"ephemeral:k\"}}]}"));
    assert(n00b_result_is_ok(r3));
    n00b_quic_manifest_t *m3 = n00b_result_get(r3);
    assert(m3->metrics == nullptr);
    printf("  [PASS] parse observability.metrics (full / defaults / absent)\n");
}

static void
test_preflight_metrics_bind(void)
{
    /* Use port 0 so we can't collide with anything; we just confirm
     * the check ran and produced an INFO. */
    auto r = n00b_quic_manifest_load_json(mk_json(manifest_with_metrics(
        "{\"bind_host\":\"127.0.0.1\",\"bind_port\":0}")));
    assert(n00b_result_is_ok(r));
    n00b_quic_manifest_t *m = n00b_result_get(r);
    auto pr = n00b_quic_preflight(m);
    n00b_quic_preflight_report_t *rep = n00b_result_get(pr);
    bool found_info = false;
    for (size_t i = 0; i < n00b_list_len(*rep->findings); i++) {
        n00b_quic_preflight_finding_t *f = n00b_list_get(*rep->findings, i);
        if (f->severity == N00B_QUIC_PREFLIGHT_INFO
            && strstr(f->check->data, "metrics-bind")) {
            found_info = true;
            break;
        }
    }
    assert(found_info);
    printf("  [PASS] preflight surfaces metrics-bind INFO on healthy config\n");

    /* Bad host → ERROR. */
    auto r2 = n00b_quic_manifest_load_json(mk_json(manifest_with_metrics(
        "{\"bind_host\":\"!!!not-a-host\",\"bind_port\":9100}")));
    assert(n00b_result_is_ok(r2));
    n00b_quic_manifest_t *m2 = n00b_result_get(r2);
    auto pr2 = n00b_quic_preflight(m2);
    n00b_quic_preflight_report_t *rep2 = n00b_result_get(pr2);
    bool found_err = false;
    for (size_t i = 0; i < n00b_list_len(*rep2->findings); i++) {
        n00b_quic_preflight_finding_t *f = n00b_list_get(*rep2->findings, i);
        if (f->severity == N00B_QUIC_PREFLIGHT_ERROR
            && strstr(f->check->data, "metrics-bind")) {
            found_err = true;
            break;
        }
    }
    assert(found_err);
    printf("  [PASS] preflight surfaces metrics-bind ERROR on bad host\n");
}

/* ============================================================================
 * Phase 5 deferral-E regression — fb_push lift to n00b_string_t *.
 *
 * Spot-checks that the byte-content of finding strings produced by
 * representative callsite patterns is preserved post-lift:
 *   - literal-only `check` / `detail` / `remediation` (acme non-https)
 *   - `n00b_cformat(...)`-built `check` with `«#»` substitution
 *     (cert-static:<ep_id>, cert-static-key:<ep_id>)
 *   - INFO finding with `nullptr` remediation (port-bind:..:0 success)
 *   - WARN with formatted detail + literal remediation (policy w/o
 *     audience)
 *
 * Per the deferral-E task definition: every callsite's finding-record
 * `check`, `detail`, `remediation` must be string-identical pre-/post-
 * lift modulo (a) edge cases where the old snprintf would have
 * truncated 256-byte buffers (which are now correct, not regressed),
 * and (b) the latent `"cert-{static,external,acme}-key:%s", ep->id`
 * bug in the walker which formatted a `n00b_buffer_t *` pointer
 * instead of `ep->id->data` — that one is now correct, which is an
 * improvement, not a regression.
 * ============================================================================ */

static n00b_quic_preflight_finding_t *
find_finding_with_check_prefix(n00b_quic_preflight_report_t *rep,
                               const char                   *prefix)
{
    for (size_t i = 0; i < n00b_list_len(*rep->findings); i++) {
        n00b_quic_preflight_finding_t *f = n00b_list_get(*rep->findings, i);
        if (f->check && f->check->data
            && strncmp(f->check->data, prefix, strlen(prefix)) == 0) {
            return f;
        }
    }
    return nullptr;
}

static void
test_fb_push_lift_check_strings_byte_identical(void)
{
    /* A non-https ACME directory_url is the simplest path that lands
     * (a) a non-format check id ("cert-acme-directory:e3") and
     * (b) literal-only detail + remediation. */
    const char *json =
        "{"
        " \"version\": 1, \"service_name\": \"pf-dE\","
        " \"endpoints\": [{"
        "   \"id\": \"e3\", \"bind_host\": \"127.0.0.1\","
        "   \"bind_port\": 0, \"alpn\": [\"h3\"],"
        "   \"cert\": {"
        "     \"kind\": \"acme\","
        "     \"directory_url\": \"ftp://wrong-scheme/directory\","
        "     \"subject_names\": [\"x.example\"],"
        "     \"challenge\": \"http-01\","
        "     \"account_key_uri\": \"ephemeral:acct\","
        "     \"contact_email\": \"x@y\""
        "   }"
        " }]"
        "}";
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    assert(n00b_result_is_ok(r));
    auto pr = n00b_quic_preflight(n00b_result_get(r));
    assert(n00b_result_is_ok(pr));
    n00b_quic_preflight_report_t *rep = n00b_result_get(pr);

    /* Pattern 1: cert-acme-directory: literal-only check + detail +
     * remediation. */
    n00b_quic_preflight_finding_t *acme =
        find_finding_with_check_prefix(rep, "cert-acme-directory:");
    assert(acme);
    assert(acme->severity == N00B_QUIC_PREFLIGHT_ERROR);
    assert(strcmp(acme->check->data, "cert-acme-directory:e3") == 0);
    assert(acme->detail
           && strcmp(acme->detail->data,
                     "ACME directory_url must start with https://") == 0);
    assert(acme->remediation
           && strcmp(acme->remediation->data,
                     "Use the canonical URL for your ACME provider.") == 0);

    /* Pattern 2: cert-acme-key check id — was previously the latent-
     * bug callsite (snprintf(..., "cert-acme-key:%s", ep->id)
     * formatting a buffer pointer).  Post-lift it correctly embeds
     * the endpoint id. */
    n00b_quic_preflight_finding_t *acme_key =
        find_finding_with_check_prefix(rep, "cert-acme-key:");
    assert(acme_key);
    assert(strcmp(acme_key->check->data, "cert-acme-key:e3") == 0);

    /* Pattern 3: port-bind INFO — successful bind on 127.0.0.1:0
     * produces a literal "bind() succeeded" detail with nullptr
     * remediation. */
    n00b_quic_preflight_finding_t *pb =
        find_finding_with_check_prefix(rep, "port-bind:");
    assert(pb);
    assert(pb->severity == N00B_QUIC_PREFLIGHT_INFO);
    assert(strcmp(pb->check->data, "port-bind:127.0.0.1:0") == 0);
    assert(pb->detail
           && strcmp(pb->detail->data, "bind() succeeded") == 0);
    assert(pb->remediation == nullptr);

    printf("  [PASS] fb_push lift: byte-identical check/detail/"
           "remediation across 3 callsite patterns\n");
}

static void
test_fb_push_lift_static_cert_format_and_walker_key(void)
{
    /* Static-cert path exercises:
     *   - cert-static:<ep_id>           (cformat in check_static_cert)
     *   - cert-static-key:<ep_id>       (cformat in the top-level walker;
     *                                    formerly the latent-bug site)
     * with a non-existent chain_pem_path, which produces a literal
     * `cert-static:<ep>` ERROR (with a formatted detail) and an
     * `ephemeral:` secret-uri INFO. */
    const char *json =
        "{"
        " \"version\": 1, \"service_name\": \"pf-dE2\","
        " \"endpoints\": [{"
        "   \"id\": \"ep-walker\","
        "   \"bind_host\": \"127.0.0.1\","
        "   \"bind_port\": 0,"
        "   \"alpn\": [\"h3\"],"
        "   \"cert\": {"
        "     \"kind\": \"static\","
        "     \"chain_pem_path\": \"/nonexistent/pf-dE-fixture\","
        "     \"key_secret_uri\": \"ephemeral:pf-dE\""
        "   }"
        " }]"
        "}";
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    assert(n00b_result_is_ok(r));
    auto pr = n00b_quic_preflight(n00b_result_get(r));
    n00b_quic_preflight_report_t *rep = n00b_result_get(pr);

    /* cert-static:<ep_id> finding. */
    n00b_quic_preflight_finding_t *cs =
        find_finding_with_check_prefix(rep, "cert-static:");
    assert(cs);
    assert(cs->severity == N00B_QUIC_PREFLIGHT_ERROR);
    assert(strcmp(cs->check->data, "cert-static:ep-walker") == 0);
    /* Detail is formatted via `n00b_cformat("Cannot read chain_pem_path '«#»'", path_s)`. */
    assert(cs->detail
           && strcmp(cs->detail->data,
                     "Cannot read chain_pem_path "
                     "'/nonexistent/pf-dE-fixture'") == 0);

    /* cert-static-key:<ep_id> finding — the walker-side check id.
     * (Pre-lift this had a latent `%s` of a buffer pointer; post-lift
     * it correctly embeds `ep_id->data`.) */
    n00b_quic_preflight_finding_t *cs_key =
        find_finding_with_check_prefix(rep, "cert-static-key:");
    assert(cs_key);
    assert(strcmp(cs_key->check->data, "cert-static-key:ep-walker") == 0);

    printf("  [PASS] fb_push lift: cformat-built check ids + walker-"
           "side key check id are correctly substituted\n");
}

static void
test_fb_push_lift_warn_policy_no_audience(void)
{
    /* Audience-empty WARN: the policy check produces a WARN finding
     * whose check id is `auth-policy:<id>` (cformat) and whose detail
     * + remediation are pure literals. */
    const char *json =
        "{"
        " \"version\": 1, \"service_name\": \"pf-dE3\","
        " \"endpoints\": [{"
        "   \"id\": \"e\", \"bind_host\": \"127.0.0.1\","
        "   \"bind_port\": 0, \"alpn\": [\"h3\"],"
        "   \"cert\": {"
        "     \"kind\": \"static\","
        "     \"chain_pem_path\": \"/tmp/x.pem\","
        "     \"key_secret_uri\": \"ephemeral:pf-dE3\""
        "   }"
        " }],"
        " \"auth\": {"
        "   \"idps\": [{"
        "     \"id\": \"idp-a\","
        "     \"issuer\": \"https://accounts.example/\","
        "     \"claims\": {\"sub\": \"sub\"}"
        "   }],"
        "   \"policies\": [{"
        "     \"id\": \"pol-a\","
        "     \"idp\": \"idp-a\""
        "   }]"
        " }"
        "}";
    auto r = n00b_quic_manifest_load_json(mk_json(json));
    assert(n00b_result_is_ok(r));
    auto pr = n00b_quic_preflight(n00b_result_get(r));
    n00b_quic_preflight_report_t *rep = n00b_result_get(pr);

    /* Look for the WARN finding on auth-policy:pol-a. */
    bool found_warn = false;
    for (size_t i = 0; i < n00b_list_len(*rep->findings); i++) {
        n00b_quic_preflight_finding_t *f = n00b_list_get(*rep->findings, i);
        if (f->severity == N00B_QUIC_PREFLIGHT_WARN
            && f->check
            && strcmp(f->check->data, "auth-policy:pol-a") == 0) {
            assert(f->detail
                   && strcmp(f->detail->data,
                             "policy has no audience requirement") == 0);
            assert(f->remediation
                   && strstr(f->remediation->data,
                             "Set `audience`") != nullptr);
            found_warn = true;
            break;
        }
    }
    assert(found_warn);
    printf("  [PASS] fb_push lift: WARN auth-policy audience-empty "
           "detail/remediation are byte-identical literals\n");
}

/* ============================================================================ */

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_quic_manifest:\n");
    test_parse_minimal_static();
    test_parse_external();
    test_parse_acme();
    test_parse_rejects();
    test_preflight_nonexistent_cert();
    test_preflight_external_no_argv0();
    test_preflight_acme_directory_url_shape();
    test_parse_auth_section();
    test_preflight_idp_invalid();
    test_preflight_policy_undefined_idp();
    test_preflight_policy_audience_empty();
    test_parse_rpc_services();
    test_parse_rpc_services_missing_fields();
    test_preflight_rpc_service_resolves();
    test_preflight_rpc_service_dangling_policy();
    test_parse_observability_metrics();
    test_preflight_metrics_bind();
    /* Phase 5 deferral-E regression — fb_push lift. */
    test_fb_push_lift_check_strings_byte_identical();
    test_fb_push_lift_static_cert_format_and_walker_key();
    test_fb_push_lift_warn_policy_no_audience();
    printf("All quic_manifest tests passed.\n");

    n00b_shutdown();
    return 0;
}
