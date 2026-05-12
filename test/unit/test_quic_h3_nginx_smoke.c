/*
 * test_quic_h3_nginx_smoke.c — End-to-end H3 GET smoke test against
 * an nginx-quic fixture in Docker.
 *
 * Mirror of `test_quic_h3_caddy_smoke.c`.  Differences:
 *  - Fixture dir is `test/fixtures/nginx-quic/`.
 *  - Env vars are NGINX_QUIC_BASE_URL / NGINX_QUIC_CERT_FP (populated
 *    by `eval "$(bash test/fixtures/nginx-quic/start.sh)"`).
 *  - Asserts the response body contains `hello-from-nginx` (matches
 *    the fixture's `location = /` shape).
 *
 * Gated by N00B_TEST_DOCKER=1 (the single env gate for all docker
 * smokes); skips cleanly otherwise so the suite passes in environments
 * without Docker.
 *
 * The smoke acts as a CLIENT only — exercises `n00b_h3_client_*`
 * against nginx mainline's H3 server.  The 4.4 in-process loopback
 * test exercises the n00b H3 server.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <assert.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"
#include "core/buffer.h"
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "net/quic/quic_types.h"
#include "net/quic/endpoint.h"
#include "net/quic/conn.h"
#include "net/quic/chan.h"
#include "net/quic/h3.h"
#include "internal/net/quic/endpoint_internal.h"
#include "picoquic.h"

static int
parse_url(const char *url, char *host, size_t host_cap, uint16_t *port_out)
{
    /* Expected shape: "https://<host>:<port>".  We don't accept
     * anything else (the nginx fixture only emits this shape). */
    const char *prefix = "https://";
    size_t      plen   = strlen(prefix);
    if (strncmp(url, prefix, plen) != 0) return -1;
    const char *rest = url + plen;
    const char *colon = strchr(rest, ':');
    if (!colon) return -1;
    size_t hostlen = (size_t)(colon - rest);
    if (hostlen >= host_cap) return -1;
    memcpy(host, rest, hostlen);
    host[hostlen] = 0;
    long port = strtol(colon + 1, nullptr, 10);
    if (port <= 0 || port > 65535) return -1;
    *port_out = (uint16_t)port;
    return 0;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_quic_h3_nginx_smoke:\n");

    const char *gate = getenv("N00B_TEST_DOCKER");
    if (!gate || strcmp(gate, "1") != 0) {
        printf("  [SKIP] N00B_TEST_DOCKER!=1 — nginx-quic fixture not booted\n");
        n00b_shutdown();
        return 77;  /* meson "skip" exit code */
    }

    const char *base_url = getenv("NGINX_QUIC_BASE_URL");
    const char *cert_fp  = getenv("NGINX_QUIC_CERT_FP");
    if (!base_url || !cert_fp) {
        printf("  [SKIP] NGINX_QUIC_BASE_URL/NGINX_QUIC_CERT_FP missing "
               "(eval start.sh first)\n");
        n00b_shutdown();
        return 77;
    }

    char     host[128];
    uint16_t port;
    if (parse_url(base_url, host, sizeof(host), &port) != 0) {
        printf("  [FAIL] cannot parse NGINX_QUIC_BASE_URL=%s\n", base_url);
        n00b_shutdown();
        return 1;
    }
    /* Fixture's leaf SAN is "DNS:localhost" — connect to 127.0.0.1
     * but use SNI = "localhost". */
    const char *sni = "localhost";

    auto cr = n00b_conduit_new();
    n00b_conduit_t *c = n00b_result_get(cr);
    auto ir = n00b_conduit_io_new_default(c);
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    /* Client endpoint with ALPN = "h3". */
    auto er = n00b_quic_endpoint_new(c, io,
                                     .bind_host = "127.0.0.1",
                                     .alpn      = N00B_H3_ALPN);
    if (n00b_result_is_err(er)) {
        printf("  [FAIL] client endpoint: %d\n", n00b_result_get_err(er));
        n00b_shutdown();
        return 1;
    }
    n00b_quic_endpoint_t *client = n00b_result_get(er);

    /* For the test we disable picoquic's default verifier because the
     * fixture's self-signed CA isn't in any system trust store.  The
     * production path would pin via picotls's verify callback using
     * NGINX_QUIC_CERT_FP; that's the trust-bridge work outside this
     * sub-phase's scope. */
    picoquic_set_null_verifier(client->quic);
    (void)cert_fp;

    /* Resolve target. */
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    auto rr = n00b_quic_connect(client,
                                (const struct sockaddr *)&dst,
                                n00b_string_from_cstr(sni));
    if (n00b_result_is_err(rr)) {
        printf("  [FAIL] connect: %d\n", n00b_result_get_err(rr));
        n00b_shutdown();
        return 1;
    }
    n00b_quic_conn_t *conn = n00b_result_get(rr);

    /* Drive the handshake. */
    bool connected = false;
    for (int i = 0; i < 400; i++) {
        n00b_quic_endpoint_run_once(client, 5);
        n00b_quic_conn_state_t st = n00b_quic_conn_state(conn);
        if (st == N00B_QUIC_CONN_STATE_CONNECTED) {
            connected = true;
            break;
        }
        if (st == N00B_QUIC_CONN_STATE_FAILED ||
            st == N00B_QUIC_CONN_STATE_CLOSED) {
            break;
        }
    }
    if (!connected) {
        printf("  [FAIL] handshake did not complete; final state=%d\n",
               (int)n00b_quic_conn_state(conn));
        n00b_quic_close(conn, 0);
        n00b_quic_endpoint_close(client);
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        n00b_shutdown();
        return 1;
    }

    /* Build authority string for the request. */
    char authority[160];
    snprintf(authority, sizeof(authority), "localhost:%u", (unsigned)port);

    /* Open H3 client + GET /. */
    auto hr = n00b_h3_client_new(conn);
    if (n00b_result_is_err(hr)) {
        printf("  [FAIL] h3_client_new: %d\n", n00b_result_get_err(hr));
        n00b_quic_close(conn, 0);
        n00b_quic_endpoint_close(client);
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        n00b_shutdown();
        return 1;
    }
    n00b_h3_client_t *h3 = n00b_result_get(hr);

    /* Drive a few iterations to flush our SETTINGS + uni stream
     * preludes before issuing the request. */
    for (int i = 0; i < 30; i++) {
        n00b_quic_endpoint_run_once(client, 5);
        n00b_h3_client_drive(h3);
    }

    auto reqr = n00b_h3_client_request(h3, "GET", "https",
                                        authority, "/");
    if (n00b_result_is_err(reqr)) {
        printf("  [FAIL] h3_request: %d\n", n00b_result_get_err(reqr));
        n00b_h3_client_close(h3);
        n00b_quic_close(conn, 0);
        n00b_quic_endpoint_close(client);
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        n00b_shutdown();
        return 1;
    }
    n00b_h3_request_t *req = n00b_result_get(reqr);

    auto respr = n00b_h3_request_await(req, .deadline_ms = 15000);
    if (n00b_result_is_err(respr)) {
        printf("  [FAIL] await: %d (%s)\n", n00b_result_get_err(respr),
               n00b_quic_err_str((n00b_quic_err_t)n00b_result_get_err(respr)));
        n00b_h3_client_close(h3);
        n00b_quic_close(conn, 0);
        n00b_quic_endpoint_close(client);
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        n00b_shutdown();
        return 1;
    }
    n00b_h3_response_t *resp = n00b_result_get(respr);
    printf("  status = %u\n", (unsigned)resp->status);
    printf("  body_len = %zu\n", (size_t)resp->body->byte_len);
    printf("  body = ");
    fwrite(resp->body->data, 1, (size_t)resp->body->byte_len, stdout);
    printf("\n");

    int exit_code = 0;
    if (resp->status != 200) {
        printf("  [FAIL] expected 200, got %u\n", (unsigned)resp->status);
        exit_code = 1;
    }

    /* The nginx fixture responds with "hello-from-nginx\n" for `/`. */
    const char *expected = "hello-from-nginx";
    bool found = false;
    if (resp->body && resp->body->byte_len > 0) {
        size_t blen = (size_t)resp->body->byte_len;
        size_t elen = strlen(expected);
        if (blen >= elen) {
            for (size_t i = 0; i + elen <= blen; i++) {
                if (memcmp(resp->body->data + i, expected, elen) == 0) {
                    found = true;
                    break;
                }
            }
        }
    }
    if (!found) {
        printf("  [FAIL] body did not contain %s\n", expected);
        exit_code = 1;
    } else {
        printf("  [PASS] H3 GET / against nginx-quic\n");
    }

    n00b_h3_client_close(h3);
    n00b_quic_close(conn, 0);
    n00b_quic_endpoint_close(client);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    n00b_shutdown();
    return exit_code;
}
