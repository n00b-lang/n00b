/*
 * test_quic_conn.c — Tests for n00b_quic_conn_t lifecycle.
 *
 * Loopback over two endpoints; the client opens a connection toward
 * the server.  Without a server cert (Phase 1 doesn't yet ship
 * cert wiring) the handshake will not complete, so we test the
 * state machine before completion + after close.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "net/quic/quic_types.h"
#include "net/quic/endpoint.h"
#include "net/quic/conn.h"

/* ============================================================================
 * 1. Outbound connect — initial state and stats accessible
 * ============================================================================ */

static void
test_conn_connect_initial_state(void)
{
    auto cr = n00b_conduit_new();
    n00b_conduit_t *c = n00b_result_get(cr);
    auto ir = n00b_conduit_io_new_default(c);
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    auto er = n00b_quic_endpoint_new(c, io,
                                     .bind_host = "127.0.0.1",
                                     .alpn      = "n00b-echo/1");
    if (n00b_result_is_err(er)) {
        printf("  [SKIP] connect (bind failed)\n");
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }
    n00b_quic_endpoint_t *ep = n00b_result_get(er);

    /* Pick an arbitrary unused-but-valid sockaddr; the connect call
     * should still succeed (the TX hasn't fired yet). */
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(54321);
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    n00b_string_t *sni = n00b_string_from_cstr("test.invalid");

    auto rr = n00b_quic_connect(ep, (const struct sockaddr *)&dst, sni);
    assert(n00b_result_is_ok(rr));
    n00b_quic_conn_t *conn = n00b_result_get(rr);
    assert(conn != nullptr);

    /* Pre-handshake: state should be CONNECTING. */
    assert(n00b_quic_conn_state(conn) == N00B_QUIC_CONN_STATE_CONNECTING);
    assert(n00b_quic_conn_endpoint(conn) == ep);

    /* Stats accessor doesn't crash; values may be zero. */
    auto stats = n00b_quic_conn_stats(conn);
    (void)stats;

    /* Close cleanly. */
    n00b_quic_close(conn, 0);
    /* Idempotent. */
    n00b_quic_close(conn, 0);

    n00b_quic_endpoint_close(ep);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] connect + initial CONNECTING state + close (idempotent)\n");
}

/* ============================================================================
 * 2. Null arg rejection
 * ============================================================================ */

static void
test_conn_null_args(void)
{
    /* connect with NULL ep → INVALID_ARG. */
    n00b_string_t *sni = n00b_string_from_cstr("test.invalid");
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(1234);
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    auto rr = n00b_quic_connect(nullptr,
                                (const struct sockaddr *)&dst, sni);
    assert(n00b_result_is_err(rr));
    assert(n00b_result_get_err(rr) == N00B_QUIC_ERR_INVALID_ARG);

    /* state on NULL → CLOSED. */
    assert(n00b_quic_conn_state(nullptr) == N00B_QUIC_CONN_STATE_CLOSED);

    /* stats on NULL is zeroed. */
    auto s = n00b_quic_conn_stats(nullptr);
    assert(s.rtt_us == 0);
    assert(s.bytes_sent == 0);

    /* endpoint on NULL → NULL. */
    assert(n00b_quic_conn_endpoint(nullptr) == nullptr);

    /* close on NULL is safe. */
    n00b_quic_close(nullptr, 0);

    printf("  [PASS] conn null-arg behavior\n");
}

/* ============================================================================
 * 3. Loopback connect with handshake driven by run_once.
 *
 * Without a server cert the handshake cannot complete; the connection
 * should either remain CONNECTING (until idle timeout, which is long)
 * or transition into CLOSING/CLOSED/FAILED via picoquic's own error
 * path.  Either is acceptable for this test — we verify the state
 * function doesn't lie about what happened, and that endpoint stats
 * show packets actually flowing.
 * ============================================================================ */

static void
test_conn_loopback_drives_handshake(void)
{
    auto cr = n00b_conduit_new();
    n00b_conduit_t *c = n00b_result_get(cr);
    auto ir = n00b_conduit_io_new_default(c);
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    auto sr = n00b_quic_endpoint_new(c, io,
                                     .bind_host = "127.0.0.1",
                                     .alpn      = "n00b-echo/1");
    if (n00b_result_is_err(sr)) {
        printf("  [SKIP] loopback (server bind failed)\n");
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }
    n00b_quic_endpoint_t *server = n00b_result_get(sr);

    auto cur = n00b_quic_endpoint_new(c, io,
                                      .bind_host = "127.0.0.1",
                                      .alpn      = "n00b-echo/1");
    if (n00b_result_is_err(cur)) {
        printf("  [SKIP] loopback (client bind failed)\n");
        n00b_quic_endpoint_close(server);
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }
    n00b_quic_endpoint_t *client = n00b_result_get(cur);

    uint16_t           sport = n00b_quic_endpoint_local_port(server);
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(sport);
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    auto rr = n00b_quic_connect(client,
                                (const struct sockaddr *)&dst,
                                n00b_string_from_cstr("localhost"));
    assert(n00b_result_is_ok(rr));
    n00b_quic_conn_t *conn = n00b_result_get(rr);

    /* Drive both sides for a fixed budget; track state. */
    bool                   server_saw_traffic = false;
    n00b_quic_conn_state_t final_state        = N00B_QUIC_CONN_STATE_CONNECTING;
    for (int i = 0; i < 30; i++) {
        n00b_quic_endpoint_run_once(client, 5);
        n00b_quic_endpoint_run_once(server, 5);
        auto ss = n00b_quic_endpoint_stats(server);
        if (ss.rx_packets > 0) server_saw_traffic = true;
        final_state = n00b_quic_conn_state(conn);
        if (final_state != N00B_QUIC_CONN_STATE_CONNECTING) {
            break;
        }
    }
    assert(server_saw_traffic);
    /* Acceptable terminal-or-still-connecting states.  CONNECTED would
     * mean the handshake completed without a cert which would be a
     * bug; assert against that explicitly. */
    assert(final_state != N00B_QUIC_CONN_STATE_CONNECTED);

    /* Local close drops cleanly. */
    n00b_quic_close(conn, 0);
    n00b_quic_endpoint_run_once(client, 5);

    n00b_quic_endpoint_close(client);
    n00b_quic_endpoint_close(server);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] loopback drives handshake (no completion sans cert)\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_quic_conn:\n");
    fflush(stdout);

    test_conn_connect_initial_state();
    fflush(stdout);
    test_conn_null_args();
    fflush(stdout);
    test_conn_loopback_drives_handshake();
    fflush(stdout);

    printf("All quic conn tests passed.\n");
    n00b_shutdown();
    return 0;
}
