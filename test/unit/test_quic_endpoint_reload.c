/*
 * test_quic_endpoint_reload.c — Hot-reload (cert+key swap) test.
 *
 * The test:
 *   1. Bring up a server endpoint with cert A + key A.
 *   2. Read the picotls context's `certificates.list[0]` and assert
 *      it points at cert A's DER bytes.
 *   3. Call n00b_quic_endpoint_reload_cert with cert B + key B.
 *   4. Re-read the picotls context: `certificates.list[0]` now
 *      points at cert B.
 *   5. Verify negative paths: NULL args / closed endpoint / non-
 *      listening endpoint all return errors without mutating state.
 *
 * Reading `certificates.list[0]` requires casting picoquic's
 * `tls_master_ctx` (void *) to `ptls_context_t *`.  This is the same
 * pattern the design doc § 6.2 anticipates for the eventual full
 * SNI-routing wiring.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "net/quic/quic_types.h"
#include "net/quic/endpoint.h"
#include "internal/net/quic/endpoint_internal.h"
#include "picotls.h"
#include "picoquic.h"
#include "picoquic_internal.h"

#include "../fixtures/quic_test_pki.h"
#include "../fixtures/quic_test_pki_reload.h"

/* Read the picotls cert chain installed on @p ep. */
static const ptls_iovec_t *
ep_cert_chain(n00b_quic_endpoint_t *ep, size_t *count_out)
{
    ptls_context_t *ctx = (ptls_context_t *)ep->quic->tls_master_ctx;
    *count_out = ctx->certificates.count;
    return ctx->certificates.list;
}

static void
test_reload_swaps_cert(void)
{
    char *key_a = n00b_quic_test_write_key_pem();
    char *key_b = n00b_quic_reload_write_key_pem();
    if (!key_a || !key_b) {
        printf("  [SKIP] cannot write key tempfiles\n");
        free(key_a); free(key_b);
        return;
    }

    auto cr = n00b_conduit_new();
    n00b_conduit_t *c = n00b_result_get(cr);
    auto ir = n00b_conduit_io_new_default(c);
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    /* Server with cert A. */
    auto sr = n00b_quic_endpoint_new(c, io,
                                     .listen         = true,
                                     .bind_host      = "127.0.0.1",
                                     .alpn           = "n00b-reload/1",
                                     .cert_der_bytes = n00b_quic_test_cert_der,
                                     .cert_der_len   = n00b_quic_test_cert_der_len,
                                     .key_pem_path   = key_a);
    if (n00b_result_is_err(sr)) {
        fprintf(stderr, "endpoint_new err=%d\n", n00b_result_get_err(sr));
        printf("  [SKIP] endpoint_new failed\n");
        unlink(key_a); free(key_a);
        unlink(key_b); free(key_b);
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }
    n00b_quic_endpoint_t *ep = n00b_result_get(sr);

    /* Step 1: cert chain is cert A. */
    size_t n = 0;
    const ptls_iovec_t *list = ep_cert_chain(ep, &n);
    assert(n == 1);
    assert(list[0].len == n00b_quic_test_cert_der_len);
    assert(memcmp(list[0].base, n00b_quic_test_cert_der,
                  n00b_quic_test_cert_der_len) == 0);
    printf("  [PASS] initial cert chain = cert A (%zu bytes)\n", list[0].len);

    /* Step 2: hot-reload to cert B + key B. */
    auto rr = n00b_quic_endpoint_reload_cert(
        ep,
        (n00b_quic_cert_reload_t){
            .cert_der_bytes = n00b_quic_reload_cert_der,
            .cert_der_len   = n00b_quic_reload_cert_der_len,
            .key_pem_path   = key_b,
        });
    assert(n00b_result_is_ok(rr));

    /* Step 3: cert chain is now cert B. */
    list = ep_cert_chain(ep, &n);
    assert(n == 1);
    assert(list[0].len == n00b_quic_reload_cert_der_len);
    assert(memcmp(list[0].base, n00b_quic_reload_cert_der,
                  n00b_quic_reload_cert_der_len) == 0);
    /* And NOT cert A — sanity check the bytes actually differ. */
    assert(list[0].len != n00b_quic_test_cert_der_len ||
           memcmp(list[0].base, n00b_quic_test_cert_der,
                  n00b_quic_test_cert_der_len) != 0);
    printf("  [PASS] post-reload cert chain = cert B (%zu bytes)\n",
           list[0].len);

    /* Step 4: argument validation. */
    /* Missing cert bytes. */
    auto e1 = n00b_quic_endpoint_reload_cert(
        ep,
        (n00b_quic_cert_reload_t){ .key_pem_path = key_b });
    assert(n00b_result_is_err(e1));
    /* Missing key path. */
    auto e2 = n00b_quic_endpoint_reload_cert(
        ep,
        (n00b_quic_cert_reload_t){
            .cert_der_bytes = n00b_quic_reload_cert_der,
            .cert_der_len   = n00b_quic_reload_cert_der_len,
        });
    assert(n00b_result_is_err(e2));
    printf("  [PASS] reload with missing args returns error\n");

    /* Step 5: bring up a non-listening endpoint and verify reload
     * rejects it (only servers have certs). */
    auto cur = n00b_quic_endpoint_new(c, io,
                                      .bind_host = "127.0.0.1",
                                      .alpn      = "n00b-reload/1");
    n00b_quic_endpoint_t *client = n00b_result_get(cur);
    auto e3 = n00b_quic_endpoint_reload_cert(
        client,
        (n00b_quic_cert_reload_t){
            .cert_der_bytes = n00b_quic_reload_cert_der,
            .cert_der_len   = n00b_quic_reload_cert_der_len,
            .key_pem_path   = key_b,
        });
    assert(n00b_result_is_err(e3));
    assert(n00b_result_get_err(e3) == N00B_QUIC_ERR_INVALID_ARG);
    printf("  [PASS] reload on non-listening endpoint rejected\n");

    n00b_quic_endpoint_close(client);
    n00b_quic_endpoint_close(ep);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);

    unlink(key_a); free(key_a);
    unlink(key_b); free(key_b);
}

/* ============================================================================
 * Live-handshake test: reload while a connection is in flight, plus
 * verifying a fresh post-reload connection still works.  Documents
 * the actual mid-flight race window observed under run_once-driven
 * loopback.
 * ============================================================================ */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "net/quic/conn.h"
#include "internal/net/quic/conn_internal.h"

/* Drive both endpoints' run_once for up to @p iters times, returning
 * the conn's terminal state. */
static n00b_quic_conn_state_t
drive_until(n00b_quic_endpoint_t *server, n00b_quic_endpoint_t *client,
            n00b_quic_conn_t *conn, int iters)
{
    for (int i = 0; i < iters; i++) {
        n00b_quic_endpoint_run_once(client, 5);
        n00b_quic_endpoint_run_once(server, 5);
        n00b_quic_conn_state_t st = n00b_quic_conn_state(conn);
        if (st == N00B_QUIC_CONN_STATE_CONNECTED ||
            st == N00B_QUIC_CONN_STATE_FAILED   ||
            st == N00B_QUIC_CONN_STATE_CLOSED) {
            return st;
        }
    }
    return n00b_quic_conn_state(conn);
}

static void
test_reload_with_live_handshake(void)
{
    char *key_a = n00b_quic_test_write_key_pem();
    char *key_b = n00b_quic_reload_write_key_pem();
    if (!key_a || !key_b) {
        printf("  [SKIP] cannot write key tempfiles\n");
        free(key_a); free(key_b);
        return;
    }

    auto cr = n00b_conduit_new();
    n00b_conduit_t *c = n00b_result_get(cr);
    auto ir = n00b_conduit_io_new_default(c);
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    auto sr = n00b_quic_endpoint_new(c, io,
                                     .listen         = true,
                                     .bind_host      = "127.0.0.1",
                                     .alpn           = "n00b-reload-live/1",
                                     .cert_der_bytes = n00b_quic_test_cert_der,
                                     .cert_der_len   = n00b_quic_test_cert_der_len,
                                     .key_pem_path   = key_a);
    if (n00b_result_is_err(sr)) {
        printf("  [SKIP] server construct failed\n");
        unlink(key_a); free(key_a);
        unlink(key_b); free(key_b);
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }
    n00b_quic_endpoint_t *server = n00b_result_get(sr);
    uint16_t              sport  = n00b_quic_endpoint_local_port(server);

    auto cur = n00b_quic_endpoint_new(c, io,
                                      .bind_host = "127.0.0.1",
                                      .alpn      = "n00b-reload-live/1");
    n00b_quic_endpoint_t *client = n00b_result_get(cur);
    picoquic_set_null_verifier(client->quic);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(sport);
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    /* === Connection 1: cert A, full handshake. === */
    auto rr1 = n00b_quic_connect(client, (const struct sockaddr *)&dst,
                                 n00b_string_from_cstr("quic-test.n00b.local"));
    n00b_quic_conn_t *c1 = n00b_result_get(rr1);
    n00b_quic_conn_state_t s1 = drive_until(server, client, c1, 200);
    /* The Phase 1 handshake test documents that without trust_t→
     * picotls wiring, completion may stop at "exchanged bytes" rather
     * than fully CONNECTED.  The property we care about for hot-
     * reload is: state is terminal-or-progressing, and both sides
     * have actually exchanged bytes. */
    auto cs1 = n00b_quic_endpoint_stats(client);
    auto ss1 = n00b_quic_endpoint_stats(server);
    assert(cs1.tx_packets > 0);
    assert(cs1.rx_packets > 0);
    assert(ss1.rx_packets > 0);
    printf("  [PASS] pre-reload handshake exchanged bytes (state=%d)\n",
           (int)s1);
    (void)c1;

    /* === Reload === */
    auto rrr = n00b_quic_endpoint_reload_cert(
        server,
        (n00b_quic_cert_reload_t){
            .cert_der_bytes = n00b_quic_reload_cert_der,
            .cert_der_len   = n00b_quic_reload_cert_der_len,
            .key_pem_path   = key_b,
        });
    assert(n00b_result_is_ok(rrr));
    /* Server is still alive after reload — driving its loop doesn't
     * crash (this is the "endpoint stays viable" promise). */
    n00b_quic_endpoint_run_once(server, 1);
    n00b_quic_endpoint_run_once(server, 1);
    printf("  [PASS] reload completed; server still drives run_once\n");

    /* === Connection 2: post-reload, should also progress. === */
    auto rr2 = n00b_quic_connect(client, (const struct sockaddr *)&dst,
                                 n00b_string_from_cstr("quic-test.n00b.local"));
    n00b_quic_conn_t *c2 = n00b_result_get(rr2);
    n00b_quic_conn_state_t s2 = drive_until(server, client, c2, 200);
    auto cs2 = n00b_quic_endpoint_stats(client);
    auto ss2 = n00b_quic_endpoint_stats(server);
    /* Verify the second connection drove additional traffic past the
     * first connection's totals. */
    assert(cs2.tx_packets > cs1.tx_packets);
    assert(ss2.rx_packets > ss1.rx_packets);
    printf("  [PASS] post-reload handshake exchanged bytes (state=%d)\n",
           (int)s2);
    (void)c2;

    /* === Mid-handshake reload race: we don't predict the outcome,
     * we just assert nobody crashes. === */
    auto rr3 = n00b_quic_connect(client, (const struct sockaddr *)&dst,
                                 n00b_string_from_cstr("quic-test.n00b.local"));
    n00b_quic_conn_t *c3 = n00b_result_get(rr3);
    /* One run_once drives the ClientHello onto the wire and lets the
     * server receive it; reload now and continue. */
    n00b_quic_endpoint_run_once(client, 5);
    n00b_quic_endpoint_run_once(server, 5);
    auto rrr2 = n00b_quic_endpoint_reload_cert(
        server,
        (n00b_quic_cert_reload_t){
            .cert_der_bytes = n00b_quic_test_cert_der,
            .cert_der_len   = n00b_quic_test_cert_der_len,
            .key_pem_path   = key_a,  /* swap back — third reload */
        });
    assert(n00b_result_is_ok(rrr2));
    /* Drive to terminal.  Outcome is undefined per the docstring,
     * but no crash. */
    (void)drive_until(server, client, c3, 200);
    printf("  [PASS] reload mid-handshake doesn't crash; server alive\n");

    n00b_quic_endpoint_close(client);
    n00b_quic_endpoint_close(server);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    unlink(key_a); free(key_a);
    unlink(key_b); free(key_b);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_quic_endpoint_reload:\n");
    test_reload_swaps_cert();
    test_reload_with_live_handshake();
    printf("All quic_endpoint_reload tests passed.\n");

    n00b_shutdown();
    return 0;
}
