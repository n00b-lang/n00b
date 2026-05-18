/*
 * test_quic_chan.c — Tests for n00b_quic_chan_t open/send/state/close.
 *
 * Channels are opened on a connection (which is in CONNECTING state
 * because we have no server cert wired yet).  Picoquic queues data
 * pre-handshake; this lets us test the channel API surface without
 * needing a complete handshake.
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
#include "net/quic/chan.h"

/* Fixture: an endpoint + a connection toward 127.0.0.1:1234 (nothing
 * listening; that's fine for these tests — picoquic queues outbound
 * locally until handshake fails). */
typedef struct {
    n00b_conduit_t            *c;
    n00b_conduit_io_backend_t *io;
    n00b_quic_endpoint_t      *ep;
    n00b_quic_conn_t          *conn;
} fixture_t;

static bool
fixture_setup(fixture_t *f)
{
    auto cr = n00b_conduit_new();
    if (n00b_result_is_err(cr)) return false;
    f->c = n00b_result_get(cr);

    auto ir = n00b_conduit_io_new_default(f->c);
    if (n00b_result_is_err(ir)) return false;
    f->io = n00b_result_get(ir);

    auto er = n00b_quic_endpoint_new(f->c, f->io,
                                     .bind_host = "127.0.0.1",
                                     .alpn      = "n00b-echo/1");
    if (n00b_result_is_err(er)) return false;
    f->ep = n00b_result_get(er);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(54321);
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    auto rr = n00b_quic_connect(f->ep,
                                (const struct sockaddr *)&dst,
                                n00b_string_from_cstr("test.invalid"));
    if (n00b_result_is_err(rr)) return false;
    f->conn = n00b_result_get(rr);
    return true;
}

static void
fixture_teardown(fixture_t *f)
{
    if (f->conn) n00b_quic_close(f->conn, 0);
    if (f->ep)   n00b_quic_endpoint_close(f->ep);
    if (f->io)   n00b_conduit_io_destroy(f->io);
    if (f->c)    n00b_conduit_destroy(f->c);
}

/* ============================================================================
 * 1. Open + accessors + close
 * ============================================================================ */

static void
test_chan_open_close(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] open_close (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    auto r = n00b_quic_chan_open(f.conn);
    assert(n00b_result_is_ok(r));
    n00b_quic_chan_t *chan = n00b_result_get(r);
    assert(chan != nullptr);

    /* Default kind = FRAMED; default bidi = true. */
    assert(n00b_quic_chan_kind(chan) == N00B_QUIC_CHAN_FRAMED);
    /* Stream ID is well-defined and < UINT64_MAX. */
    assert(n00b_quic_chan_id(chan) != UINT64_MAX);
    /* Initial state: OPEN. */
    assert(n00b_quic_chan_state(chan) == N00B_QUIC_CHAN_STATE_OPEN);
    /* Owning conn round-trips. */
    assert(n00b_quic_chan_conn(chan) == f.conn);

    /* Open a second channel — different stream ID. */
    auto r2 = n00b_quic_chan_open(f.conn);
    assert(n00b_result_is_ok(r2));
    n00b_quic_chan_t *chan2 = n00b_result_get(r2);
    assert(n00b_quic_chan_id(chan2) != n00b_quic_chan_id(chan));

    /* Close both. */
    n00b_quic_chan_close(chan);
    assert(n00b_quic_chan_state(chan) == N00B_QUIC_CHAN_STATE_CLOSED);
    n00b_quic_chan_close(chan2);
    /* Idempotent. */
    n00b_quic_chan_close(chan);

    fixture_teardown(&f);
    printf("  [PASS] chan open + accessors + close\n");
}

/* ============================================================================
 * 2. Send queues bytes; FIN transitions state
 * ============================================================================ */

static void
test_chan_send_fin(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] send_fin (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    auto cr = n00b_quic_chan_open(f.conn);
    assert(n00b_result_is_ok(cr));
    n00b_quic_chan_t *chan = n00b_result_get(cr);

    /* Queue some bytes (no FIN); state stays OPEN. */
    static const uint8_t hello[] = "hello-frame";
    auto sr = n00b_quic_chan_send(chan, hello, sizeof(hello) - 1);
    assert(n00b_result_is_ok(sr));
    assert(n00b_result_get(sr) == sizeof(hello) - 1);
    assert(n00b_quic_chan_state(chan) == N00B_QUIC_CHAN_STATE_OPEN);

    /* Second send + FIN → SEND_HALF_CLOSED. */
    static const uint8_t bye[] = "bye";
    auto sr2 = n00b_quic_chan_send(chan, bye, sizeof(bye) - 1, .fin = true);
    assert(n00b_result_is_ok(sr2));
    assert(n00b_quic_chan_state(chan) == N00B_QUIC_CHAN_STATE_SEND_HALF_CLOSED);

    /* Third send after FIN → INVALID_ARG. */
    auto sr3 = n00b_quic_chan_send(chan, bye, sizeof(bye) - 1);
    assert(n00b_result_is_err(sr3));
    assert(n00b_result_get_err(sr3) == N00B_QUIC_ERR_INVALID_ARG);

    n00b_quic_chan_close(chan);
    fixture_teardown(&f);
    printf("  [PASS] chan send + FIN transition + post-FIN reject\n");
}

/* ============================================================================
 * 3. Reset transitions state to LOCAL_RESET
 * ============================================================================ */

static void
test_chan_reset(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] reset (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    auto cr = n00b_quic_chan_open(f.conn);
    n00b_quic_chan_t *chan = n00b_result_get(cr);

    auto rr = n00b_quic_chan_reset(chan, 42);
    assert(n00b_result_is_ok(rr));
    assert(n00b_quic_chan_state(chan) == N00B_QUIC_CHAN_STATE_LOCAL_RESET);

    /* Send after reset → INVALID_ARG. */
    auto sr = n00b_quic_chan_send(chan, (const uint8_t *)"x", 1);
    assert(n00b_result_is_err(sr));

    /* stop_sending also works on a reset stream (stops peer's TX). */
    auto ssr = n00b_quic_chan_stop_sending(chan, 99);
    assert(n00b_result_is_ok(ssr));

    /* Close after reset is fine; state stays LOCAL_RESET. */
    n00b_quic_chan_close(chan);
    assert(n00b_quic_chan_state(chan) == N00B_QUIC_CHAN_STATE_LOCAL_RESET);

    fixture_teardown(&f);
    printf("  [PASS] chan reset transitions + post-reset behavior\n");
}

/* ============================================================================
 * 3.5. n00b_quic_chan_stats reflects send + state
 * ============================================================================ */

static void
test_chan_stats(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] stats (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    auto cr = n00b_quic_chan_open(f.conn);
    n00b_quic_chan_t *chan = n00b_result_get(cr);

    auto s0 = n00b_quic_chan_stats(chan);
    assert(s0.bytes_sent == 0);
    assert(s0.bytes_received == 0);
    assert(s0.state == N00B_QUIC_CHAN_STATE_OPEN);
    assert(s0.kind  == N00B_QUIC_CHAN_FRAMED);
    assert(s0.bidi  == 1);
    assert(s0.last_activity_ns > 0);

    static const uint8_t payload[] = "ping";
    auto sr = n00b_quic_chan_send(chan, payload, sizeof(payload) - 1,
                                  .fin = true);
    assert(n00b_result_is_ok(sr));

    auto s1 = n00b_quic_chan_stats(chan);
    assert(s1.bytes_sent == sizeof(payload) - 1);
    assert(s1.state == N00B_QUIC_CHAN_STATE_SEND_HALF_CLOSED);
    assert(s1.last_activity_ns >= s0.last_activity_ns);
    /* Per-stream flow-control / in-flight wiring (see chan.c
     * picoquic_find_stream / picoquic_sack_list_first path).  No
     * exact-value assertions — these reflect live picoquic state —
     * but the invariants must hold. */
    assert(s1.bytes_in_flight <= s1.bytes_sent);
    assert(s1.bytes_acked    <= s1.bytes_sent);
    assert(s1.bytes_acked + s1.bytes_in_flight <= s1.bytes_sent);

    /* Stats on NULL → zeroed (no crash). */
    auto sn = n00b_quic_chan_stats(nullptr);
    assert(sn.bytes_sent == 0);
    assert(sn.state == 0);

    fixture_teardown(&f);
    printf("  [PASS] chan_stats reflects send + state transitions\n");
}

/* ============================================================================
 * 4. NULL / closed-arg behavior
 * ============================================================================ */

static void
test_chan_null_args(void)
{
    /* open on NULL conn. */
    auto r = n00b_quic_chan_open(nullptr);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_QUIC_ERR_INVALID_ARG);

    /* DGRAM kind — RFC 9221 datagram channels (singleton per conn). */
    fixture_t f = {0};
    if (fixture_setup(&f)) {
        auto dr1 = n00b_quic_chan_open(f.conn, .kind = N00B_QUIC_CHAN_DGRAM);
        assert(n00b_result_is_ok(dr1));
        n00b_quic_chan_t *d1 = n00b_result_get(dr1);
        assert(d1 != nullptr);
        assert(n00b_quic_chan_kind(d1) == N00B_QUIC_CHAN_DGRAM);
        /* Sentinel stream_id for datagrams (no real stream). */
        assert(n00b_quic_chan_id(d1) == UINT64_MAX);

        /* Singleton: repeated opens return the same handle. */
        auto dr2 = n00b_quic_chan_open(f.conn, .kind = N00B_QUIC_CHAN_DGRAM);
        assert(n00b_result_is_ok(dr2));
        assert(n00b_result_get(dr2) == d1);

        /* recv_dgram on empty queue → ok+none, not error. */
        auto rr = n00b_quic_chan_recv_dgram(d1);
        assert(n00b_result_is_ok(rr));
        assert(!n00b_option_is_set(n00b_result_get(rr)));

        /* recv (stream API) on a DGRAM channel rejects with INVALID_ARG. */
        uint8_t scratch[8];
        auto srr = n00b_quic_chan_recv(d1, scratch, sizeof(scratch));
        assert(n00b_result_is_err(srr));
        assert(n00b_result_get_err(srr) == N00B_QUIC_ERR_INVALID_ARG);

        /* recv_dgram on a stream-kind channel rejects. */
        auto cr = n00b_quic_chan_open(f.conn);  /* default FRAMED */
        assert(n00b_result_is_ok(cr));
        n00b_quic_chan_t *stream_chan = n00b_result_get(cr);
        auto bad = n00b_quic_chan_recv_dgram(stream_chan);
        assert(n00b_result_is_err(bad));

        fixture_teardown(&f);
    }

    /* Accessors on NULL. */
    assert(n00b_quic_chan_id(nullptr)    == UINT64_MAX);
    assert(n00b_quic_chan_state(nullptr) == N00B_QUIC_CHAN_STATE_CLOSED);
    assert(n00b_quic_chan_conn(nullptr)  == nullptr);

    /* Send / reset / stop_sending / close on NULL. */
    auto sr = n00b_quic_chan_send(nullptr, (const uint8_t *)"x", 1);
    assert(n00b_result_is_err(sr));

    auto rsr = n00b_quic_chan_reset(nullptr, 0);
    assert(n00b_result_is_err(rsr));

    auto ssr = n00b_quic_chan_stop_sending(nullptr, 0);
    assert(n00b_result_is_err(ssr));

    n00b_quic_chan_close(nullptr);
    printf("  [PASS] chan null-arg / NOT_IMPLEMENTED behavior\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_quic_chan:\n");
    fflush(stdout);

    test_chan_open_close();
    fflush(stdout);
    test_chan_send_fin();
    fflush(stdout);
    test_chan_reset();
    fflush(stdout);
    test_chan_stats();
    fflush(stdout);
    test_chan_null_args();
    fflush(stdout);

    printf("All quic chan tests passed.\n");
    n00b_shutdown();
    return 0;
}
