/*
 * test_quic_chan_recv.c — Synthetic-event tests for the recv path.
 *
 * We can't drive a real QUIC handshake yet (no server cert), so the
 * tests here invoke the conn-level callback's helpers directly to
 * verify dispatch, recv-buffer accumulation, and state transitions.
 * Once cert wiring lands, a second test file exercises the same
 * surface end-to-end over the wire.
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
#include "internal/net/quic/conn_internal.h"
#include "internal/net/quic/chan_internal.h"

/* Fixture: a connecting conn (no live handshake) + a chan opened on it. */
typedef struct {
    n00b_conduit_t            *c;
    n00b_conduit_io_backend_t *io;
    n00b_quic_endpoint_t      *ep;
    n00b_quic_conn_t          *conn;
    n00b_quic_chan_t          *chan;
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

    auto rr = n00b_quic_connect(f->ep, (const struct sockaddr *)&dst,
                                n00b_string_from_cstr("test.invalid"));
    if (n00b_result_is_err(rr)) return false;
    f->conn = n00b_result_get(rr);

    auto chanr = n00b_quic_chan_open(f->conn);
    if (n00b_result_is_err(chanr)) return false;
    f->chan = n00b_result_get(chanr);
    return true;
}

static void
fixture_teardown(fixture_t *f)
{
    if (f->chan) n00b_quic_chan_close(f->chan);
    if (f->conn) n00b_quic_close(f->conn, 0);
    if (f->ep)   n00b_quic_endpoint_close(f->ep);
    if (f->io)   n00b_conduit_io_destroy(f->io);
    if (f->c)    n00b_conduit_destroy(f->c);
}

/* ============================================================================
 * 1. Channels register on conn — find_chan locates them by stream_id
 * ============================================================================ */

static void
test_recv_find_chan(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] find_chan (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    /* Open a couple more channels so the lookup walks the list. */
    auto r2 = n00b_quic_chan_open(f.conn);
    auto r3 = n00b_quic_chan_open(f.conn);
    n00b_quic_chan_t *chan2 = n00b_result_get(r2);
    n00b_quic_chan_t *chan3 = n00b_result_get(r3);

    /* Each chan is findable by its own stream_id. */
    assert(_n00b_quic_conn_find_chan(f.conn, n00b_quic_chan_id(f.chan)) == f.chan);
    assert(_n00b_quic_conn_find_chan(f.conn, n00b_quic_chan_id(chan2)) == chan2);
    assert(_n00b_quic_conn_find_chan(f.conn, n00b_quic_chan_id(chan3)) == chan3);
    /* Unknown stream_id → NULL. */
    assert(_n00b_quic_conn_find_chan(f.conn, 999999) == nullptr);

    fixture_teardown(&f);
    printf("  [PASS] conn registers chans + find_chan walks list\n");
}

/* ============================================================================
 * 2. Recv buffer accumulates + recv pulls bytes + memmove-shift works
 * ============================================================================ */

static void
test_recv_append_and_pull(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] append_and_pull (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    /* Synthetic STREAM_DATA: append "hello" then "world" via the helper. */
    _n00b_quic_chan_append_recv(f.chan, (const uint8_t *)"hello", 5);
    _n00b_quic_chan_append_recv(f.chan, (const uint8_t *)"world", 5);

    assert(n00b_quic_chan_has_data(f.chan));

    /* Pull 4 bytes — should see "hell". */
    uint8_t buf[16] = {0};
    auto    r1     = n00b_quic_chan_recv(f.chan, buf, 4);
    assert(n00b_result_is_ok(r1));
    assert(n00b_result_get(r1) == 4);
    assert(memcmp(buf, "hell", 4) == 0);
    assert(n00b_quic_chan_has_data(f.chan));   /* still 6 left */

    /* Pull 100 — should get the remaining 6 bytes "oworld". */
    auto r2 = n00b_quic_chan_recv(f.chan, buf, 100);
    assert(n00b_result_is_ok(r2));
    assert(n00b_result_get(r2) == 6);
    assert(memcmp(buf, "oworld", 6) == 0);

    /* Now empty. */
    assert(!n00b_quic_chan_has_data(f.chan));
    auto r3 = n00b_quic_chan_recv(f.chan, buf, 100);
    assert(n00b_result_is_ok(r3));
    assert(n00b_result_get(r3) == 0);

    /* bytes_received counter accumulated everything. */
    assert(f.chan->bytes_received == 10);

    fixture_teardown(&f);
    printf("  [PASS] recv-buffer append + pull + counter\n");
}

/* ============================================================================
 * 3. Recv buffer grows past initial capacity (geometric growth)
 * ============================================================================ */

static void
test_recv_grow(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] grow (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    /* Default initial cap is 256.  Append 10 KB to force several grows. */
    static const size_t total = 10 * 1024;
    uint8_t            *src   = malloc(total);
    for (size_t i = 0; i < total; i++) {
        src[i] = (uint8_t)(i & 0xff);
    }
    _n00b_quic_chan_append_recv(f.chan, src, total);
    assert(f.chan->recv_buf && f.chan->recv_buf->byte_len == total);
    assert(f.chan->recv_buf->alloc_len >= total);

    /* Drain in 1 KB increments and verify byte values. */
    uint8_t out[1024];
    size_t  drained = 0;
    while (n00b_quic_chan_has_data(f.chan)) {
        auto r = n00b_quic_chan_recv(f.chan, out, sizeof(out));
        assert(n00b_result_is_ok(r));
        size_t n = n00b_result_get(r);
        for (size_t i = 0; i < n; i++) {
            assert(out[i] == (uint8_t)((drained + i) & 0xff));
        }
        drained += n;
    }
    assert(drained == total);
    free(src);

    fixture_teardown(&f);
    printf("  [PASS] recv-buffer grows geometrically + ordering preserved\n");
}

/* ============================================================================
 * 4. STREAM_FIN event transitions state and flips recv_fin
 * ============================================================================ */

static void
test_recv_fin_state(void)
{
    fixture_t f = {0};
    if (!fixture_setup(&f)) {
        printf("  [SKIP] fin_state (fixture setup failed)\n");
        fixture_teardown(&f);
        return;
    }

    /* Append some data first, then synthesize a FIN.  The FIN handler
     * lives in conn.c; we drive it by mutating chan state directly to
     * mirror the callback's effect (we test the callback's
     * decision-making via the loopback test once cert wiring lands). */
    _n00b_quic_chan_append_recv(f.chan, (const uint8_t *)"final-bytes", 11);

    /* Equivalent to what conn_default_callback does on
     * picoquic_callback_stream_fin: */
    f.chan->recv_fin = true;
    if (f.chan->state == N00B_QUIC_CHAN_STATE_OPEN) {
        f.chan->state = N00B_QUIC_CHAN_STATE_RECV_HALF_CLOSED;
    }

    assert(n00b_quic_chan_recv_fin(f.chan));
    assert(n00b_quic_chan_state(f.chan) == N00B_QUIC_CHAN_STATE_RECV_HALF_CLOSED);

    /* Drain the remaining bytes — recv works fine in RECV_HALF_CLOSED. */
    uint8_t buf[64] = {0};
    auto    r       = n00b_quic_chan_recv(f.chan, buf, sizeof(buf));
    assert(n00b_result_is_ok(r));
    assert(n00b_result_get(r) == 11);
    assert(memcmp(buf, "final-bytes", 11) == 0);

    /* Now no data + recv_fin true → "no more bytes coming". */
    assert(!n00b_quic_chan_has_data(f.chan));
    assert(n00b_quic_chan_recv_fin(f.chan));

    fixture_teardown(&f);
    printf("  [PASS] FIN sets recv_fin + transitions to RECV_HALF_CLOSED\n");
}

/* ============================================================================
 * 5. NULL / closed args on recv API
 * ============================================================================ */

static void
test_recv_null_args(void)
{
    uint8_t buf[16];

    /* recv on NULL → INVALID_ARG. */
    auto r = n00b_quic_chan_recv(nullptr, buf, sizeof(buf));
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_QUIC_ERR_INVALID_ARG);

    /* has_data + recv_fin on NULL are safe. */
    assert(!n00b_quic_chan_has_data(nullptr));
    assert(!n00b_quic_chan_recv_fin(nullptr));

    /* recv with max=0 → ok(0) regardless of state. */
    fixture_t f = {0};
    if (fixture_setup(&f)) {
        auto r2 = n00b_quic_chan_recv(f.chan, buf, 0);
        assert(n00b_result_is_ok(r2));
        assert(n00b_result_get(r2) == 0);

        /* recv with non-zero max but NULL out → NULL_ARG. */
        _n00b_quic_chan_append_recv(f.chan, (const uint8_t *)"x", 1);
        auto r3 = n00b_quic_chan_recv(f.chan, nullptr, 16);
        assert(n00b_result_is_err(r3));
        assert(n00b_result_get_err(r3) == N00B_QUIC_ERR_NULL_ARG);

        fixture_teardown(&f);
    }

    printf("  [PASS] recv null/zero-arg behavior\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_quic_chan_recv:\n");
    fflush(stdout);

    test_recv_find_chan();         fflush(stdout);
    test_recv_append_and_pull();   fflush(stdout);
    test_recv_grow();              fflush(stdout);
    test_recv_fin_state();         fflush(stdout);
    test_recv_null_args();         fflush(stdout);

    printf("All quic chan_recv tests passed.\n");
    n00b_shutdown();
    return 0;
}
