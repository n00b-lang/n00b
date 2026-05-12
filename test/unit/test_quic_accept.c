/*
 * test_quic_accept.c — Server accept-topic + multi-conn lifecycle.
 *
 * Validates that:
 *   - n00b_quic_endpoint_accept_topic exists on listen-mode endpoints
 *     and publishes one event per accepted picoquic cnx.
 *   - Server-side conns auto-wrap: subscribers see a real
 *     n00b_quic_conn_t in the event payload.
 *   - Peer-initiated streams auto-create channels visible via
 *     n00b_quic_conn_first_chan / n00b_quic_chan_next_in_conn.
 *   - When a client closes, the conn is unlinked from the endpoint's
 *     accepted list (no leak).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"
#include "core/time.h"
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "net/quic/quic_types.h"
#include "net/quic/endpoint.h"
#include "net/quic/conn.h"
#include "net/quic/chan.h"
#include "internal/net/quic/endpoint_internal.h"
#include "internal/net/quic/conn_internal.h"

#include "picoquic.h"

#include "../fixtures/quic_test_pki.h"

/* ============================================================================
 * 1. Listen endpoint exposes accept_topic; non-listen endpoint does not
 * ============================================================================ */

static void
test_accept_topic_exists(void)
{
    char *key_pem_path = n00b_quic_test_write_key_pem();
    if (!key_pem_path) { printf("  [SKIP] accept_topic_exists\n"); return; }

    auto cr = n00b_conduit_new();
    n00b_conduit_t *c = n00b_result_get(cr);
    auto ir = n00b_conduit_io_new_default(c);
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    auto sr = n00b_quic_endpoint_new(c, io,
                                     .listen         = true,
                                     .bind_host      = "127.0.0.1",
                                     .alpn           = "n00b-test/1",
                                     .cert_der_bytes = n00b_quic_test_cert_der,
                                     .cert_der_len   = n00b_quic_test_cert_der_len,
                                     .key_pem_path   = key_pem_path);
    if (n00b_result_is_err(sr)) {
        printf("  [SKIP] accept_topic_exists (server bind failed)\n");
        unlink(key_pem_path); free(key_pem_path);
        n00b_conduit_io_destroy(io); n00b_conduit_destroy(c);
        return;
    }
    n00b_quic_endpoint_t *server = n00b_result_get(sr);

    assert(n00b_quic_endpoint_accept_topic(server) != nullptr);

    auto cur = n00b_quic_endpoint_new(c, io,
                                      .bind_host = "127.0.0.1",
                                      .alpn      = "n00b-test/1");
    n00b_quic_endpoint_t *client = n00b_result_get(cur);
    /* Client (non-listen) endpoint has no accept topic. */
    assert(n00b_quic_endpoint_accept_topic(client) == nullptr);

    n00b_quic_endpoint_close(client);
    n00b_quic_endpoint_close(server);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    unlink(key_pem_path); free(key_pem_path);
    printf("  [PASS] accept_topic exists on listen endpoint, NULL on client\n");
}

/* ============================================================================
 * 2. Three clients each produce one accept event; channels auto-create.
 *    After all close, server's accepted list is empty (no leak).
 * ============================================================================ */

static void
test_multi_accept_and_unlink(void)
{
    char *key_pem_path = n00b_quic_test_write_key_pem();
    if (!key_pem_path) { printf("  [SKIP] multi_accept\n"); return; }

    auto cr = n00b_conduit_new();
    n00b_conduit_t *c = n00b_result_get(cr);
    auto ir = n00b_conduit_io_new_default(c);
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    auto sr = n00b_quic_endpoint_new(c, io,
                                     .listen         = true,
                                     .bind_host      = "127.0.0.1",
                                     .alpn           = "n00b-test/1",
                                     .cert_der_bytes = n00b_quic_test_cert_der,
                                     .cert_der_len   = n00b_quic_test_cert_der_len,
                                     .key_pem_path   = key_pem_path);
    if (n00b_result_is_err(sr)) {
        printf("  [SKIP] multi_accept (server bind failed)\n");
        unlink(key_pem_path); free(key_pem_path);
        n00b_conduit_io_destroy(io); n00b_conduit_destroy(c);
        return;
    }
    n00b_quic_endpoint_t *server = n00b_result_get(sr);

    n00b_conduit_topic_base_t *atopic =
        n00b_quic_endpoint_accept_topic(server);
    n00b_quic_accept_inbox_t *ainbox = n00b_quic_accept_inbox_new(c);
    n00b_quic_accept_subscribe(atopic, ainbox,
                               .operations = N00B_CONDUIT_OP_ALL);

    uint16_t           sport = n00b_quic_endpoint_local_port(server);
    struct sockaddr_in dst   = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(sport);
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    /* Three clients in sequence (not parallel — single-threaded
     * test driver, easier to reason about). */
    enum { N_CLIENTS = 3 };
    n00b_quic_endpoint_t *clients[N_CLIENTS];
    n00b_quic_conn_t     *client_conns[N_CLIENTS];
    n00b_quic_chan_t     *client_chans[N_CLIENTS];

    for (int i = 0; i < N_CLIENTS; i++) {
        auto cur = n00b_quic_endpoint_new(c, io,
                                          .bind_host = "127.0.0.1",
                                          .alpn      = "n00b-test/1");
        clients[i] = n00b_result_get(cur);
        picoquic_set_null_verifier(clients[i]->quic);

        auto ccr = n00b_quic_connect(clients[i],
                                     (const struct sockaddr *)&dst,
                                     n00b_string_from_cstr("quic-test.n00b.local"));
        client_conns[i] = n00b_result_get(ccr);

        /* Drive handshake (and pump the server too). */
        for (int it = 0; it < 200; it++) {
            n00b_quic_endpoint_run_once(clients[i], 5);
            n00b_quic_endpoint_run_once(server, 5);
            if (n00b_quic_conn_state(client_conns[i])
                == N00B_QUIC_CONN_STATE_CONNECTED) break;
        }
        assert(n00b_quic_conn_state(client_conns[i])
               == N00B_QUIC_CONN_STATE_CONNECTED);

        /* Open a channel and send a tiny payload + FIN. */
        client_chans[i] = n00b_result_get(n00b_quic_chan_open(client_conns[i]));
        char msg[32];
        int  mlen = snprintf(msg, sizeof(msg), "client %d", i);
        n00b_quic_chan_send(client_chans[i], (const uint8_t *)msg,
                            (size_t)mlen, .fin = true);

        for (int it = 0; it < 100; it++) {
            n00b_quic_endpoint_run_once(clients[i], 5);
            n00b_quic_endpoint_run_once(server, 5);
        }
    }

    /* Drain the accept inbox; we should have exactly N_CLIENTS conns. */
    int                   n_accepts = 0;
    n00b_quic_conn_t     *accepted_conns[N_CLIENTS] = {0};
    while (n00b_quic_accept_inbox_has_messages(ainbox)) {
        n00b_quic_accept_msg_t *m = n00b_quic_accept_inbox_pop(ainbox);
        if (m && n_accepts < N_CLIENTS) {
            accepted_conns[n_accepts++] = m->payload.conn;
        }
    }
    assert(n_accepts == N_CLIENTS);

    /* Each accepted conn should have exactly one channel (the one
     * the client opened) — verify peer-initiated stream auto-create. */
    for (int i = 0; i < N_CLIENTS; i++) {
        n00b_quic_chan_t *first =
            n00b_quic_conn_first_chan(accepted_conns[i]);
        assert(first != nullptr);
        assert(n00b_quic_chan_next_in_conn(first) == nullptr);

        /* Channel state can be anywhere from OPEN through
         * RECV_HALF_CLOSED depending on whether server has FIN'd back
         * (echo callback isn't involved here — we don't have one).
         * Just assert the chan is in a known state. */
        n00b_quic_chan_state_t cst = n00b_quic_chan_state(first);
        assert(cst == N00B_QUIC_CHAN_STATE_OPEN ||
               cst == N00B_QUIC_CHAN_STATE_RECV_HALF_CLOSED ||
               cst == N00B_QUIC_CHAN_STATE_SEND_HALF_CLOSED ||
               cst == N00B_QUIC_CHAN_STATE_CLOSED);
    }

    /* All N_CLIENTS conns should be linked into the endpoint's
     * accepted list. */
    int linked = 0;
    for (n00b_quic_conn_t *c2 = server->accepted; c2;
         c2 = c2->next_in_endpoint) {
        linked++;
    }
    assert(linked == N_CLIENTS);

    /* Now close every client; the server-side conn should be
     * unlinked from the accepted list once the close events
     * propagate. */
    for (int i = 0; i < N_CLIENTS; i++) {
        n00b_quic_close(client_conns[i], 0);
    }

    /* Drive long enough for the close frames to flow.  ~50 iters of
     * 5ms each = 250ms which is comfortably more than loopback RTT. */
    for (int it = 0; it < 200; it++) {
        for (int i = 0; i < N_CLIENTS; i++) {
            n00b_quic_endpoint_run_once(clients[i], 5);
        }
        n00b_quic_endpoint_run_once(server, 5);

        int still_linked = 0;
        for (n00b_quic_conn_t *c2 = server->accepted; c2;
             c2 = c2->next_in_endpoint) {
            still_linked++;
        }
        if (still_linked == 0) break;
    }

    int still_linked = 0;
    for (n00b_quic_conn_t *c2 = server->accepted; c2;
         c2 = c2->next_in_endpoint) {
        still_linked++;
    }
    assert(still_linked == 0);

    for (int i = 0; i < N_CLIENTS; i++) {
        n00b_quic_endpoint_close(clients[i]);
    }
    n00b_quic_endpoint_close(server);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    unlink(key_pem_path); free(key_pem_path);
    printf("  [PASS] %d clients accepted; %d unlinked after close (no leak)\n",
           N_CLIENTS, N_CLIENTS);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_quic_accept:\n");
    fflush(stdout);

    test_accept_topic_exists();
    fflush(stdout);
    test_multi_accept_and_unlink();
    fflush(stdout);

    printf("All quic accept tests passed.\n");
    n00b_shutdown();
    return 0;
}
