/*
 * examples/quic_echo/main.c — End-to-end QUIC echo demo.
 *
 * Three modes:
 *
 *   * `quic_echo`                       — single-process loopback
 *                                          (smoke test: client + server
 *                                          in one program, prints
 *                                          a single echo round-trip).
 *
 *   * `quic_echo server [PORT]`         — long-lived listener.
 *                                          Default port: 4433.  Echoes
 *                                          every received stream byte
 *                                          back on the same stream.
 *                                          Handles many concurrent
 *                                          clients.  Ctrl-C to exit.
 *
 *   * `quic_echo client HOST PORT [MSG]` — one-shot client.  Connects,
 *                                          sends MSG (default
 *                                          "hello, n00b/quic!"),
 *                                          prints the echo, exits 0.
 *
 * Multi-client concurrency lives in `picoquic_quic_t`: a single
 * server context dispatches per-connection events through one
 * registered callback.  That callback (here: `echo_callback`) is
 * stateless across connections; picoquic handles the per-cnx state
 * for us.  Each client gets its own picoquic_cnx_t under the hood.
 *
 * Cert wiring is currently the test fixture in
 * `test/fixtures/quic_test_pki.h` — self-signed; the client opts out
 * of cert verification via `picoquic_set_null_verifier`.  Production
 * goes through real PKI (`docs/quic/dev_pki.md`); the verifier
 * bypass goes away when the trust_t-to-picotls bridge ships.
 *
 * Build & run:
 *
 *     bash build.sh
 *     ./build_debug/quic_echo server 4433 &
 *     ./build_debug/quic_echo client 127.0.0.1 4433 "msg from client 1"
 *     ./build_debug/quic_echo client 127.0.0.1 4433 "msg from client 2"
 *     ./build_debug/quic_echo client 127.0.0.1 4433 "msg from client 3"
 *     # …in parallel:
 *     for i in 1 2 3 4 5; do
 *         ./build_debug/quic_echo client 127.0.0.1 4433 "msg $i" &
 *     done; wait
 *     # …eventually:
 *     kill %1
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "n00b.h"
#include "util/assert.h"
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

#include "picoquic.h"

#include "../../test/fixtures/quic_test_pki.h"

/* ===========================================================================
 * Echo loop helpers — pure n00b API.
 *
 * For each accepted connection, walk every channel.  If a channel has
 * pending recv bytes, copy them out and send them back.  If the peer
 * has FIN'd the channel and we've drained, FIN our send direction.
 *
 * Picoquic's per-cnx callback (installed by our accept path) does the
 * heavy lifting of fanning stream events into per-channel recv
 * buffers and auto-creating channels for peer-initiated streams.
 * The server doesn't need to know any of that — it just polls the
 * channels it sees.
 * =========================================================================== */

static void
echo_one_chan(n00b_quic_chan_t *chan)
{
    if (!n00b_quic_chan_has_data(chan) && !n00b_quic_chan_recv_fin(chan)) {
        return;
    }

    uint8_t buf[1500];
    size_t  pulled = 0;
    while (n00b_quic_chan_has_data(chan)) {
        auto rr = n00b_quic_chan_recv(chan, buf + pulled,
                                      sizeof(buf) - pulled);
        if (n00b_result_is_err(rr)) break;
        size_t n = n00b_result_get(rr);
        if (n == 0) break;
        pulled += n;
        if (pulled == sizeof(buf)) break;
    }

    bool send_fin =
        n00b_quic_chan_recv_fin(chan)
        && n00b_quic_chan_state(chan) != N00B_QUIC_CHAN_STATE_SEND_HALF_CLOSED
        && n00b_quic_chan_state(chan) != N00B_QUIC_CHAN_STATE_CLOSED;

    if (pulled > 0 || send_fin) {
        (void)n00b_quic_chan_send(chan, buf, pulled, .fin = send_fin);
    }
}

static void
echo_walk(n00b_quic_conn_t *conn)
{
    n00b_quic_chan_t *c;
    for (c = n00b_quic_conn_first_chan(conn);
         c != NULL;
         c = n00b_quic_chan_next_in_conn(c)) {
        echo_one_chan(c);
    }
}

/* ===========================================================================
 * Common helpers
 * =========================================================================== */

static n00b_conduit_t            *g_conduit;
static n00b_conduit_io_backend_t *g_io;

static bool
common_setup(void)
{
    auto cr = n00b_conduit_new();
    if (n00b_result_is_err(cr)) {
        fprintf(stderr, "n00b_conduit_new failed\n");
        return false;
    }
    g_conduit = n00b_result_get(cr);

    auto ir = n00b_conduit_io_new_default(g_conduit);
    if (n00b_result_is_err(ir)) {
        fprintf(stderr, "n00b_conduit_io_new_default failed\n");
        return false;
    }
    g_io = n00b_result_get(ir);
    return true;
}

static void
common_teardown(void)
{
    if (g_io)      { n00b_conduit_io_destroy(g_io);   g_io = NULL; }
    if (g_conduit) { n00b_conduit_destroy(g_conduit); g_conduit = NULL; }
}

/* ===========================================================================
 * Server mode
 * =========================================================================== */

static atomic_bool g_server_stop = false;

static void
sigint_handler(int sig)
{
    (void)sig;
    atomic_store(&g_server_stop, true);
}

/* Read a whole PEM/DER file into a freshly-allocated buffer.  Caller
 * frees `*out`.  Returns 0 on success. */
static int
slurp_file(const char *path, uint8_t **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return -1; }
    *out = buf;
    *out_len = (size_t)sz;
    return 0;
}

/* Convert a PEM cert (one BEGIN CERTIFICATE block) to DER.  Returns a
 * malloc'd buffer in *out + length in *out_len; caller frees. */
static int
pem_cert_to_der(const uint8_t *pem, size_t pem_len,
                uint8_t **out, size_t *out_len)
{
    const char *begin = "-----BEGIN CERTIFICATE-----";
    const char *end   = "-----END CERTIFICATE-----";
    const char *pem_s = (const char *)pem;
    const char *b = strstr(pem_s, begin);
    if (!b) return -1;
    b += strlen(begin);
    while (b < pem_s + pem_len && (*b == '\n' || *b == '\r')) b++;
    const char *e = strstr(b, end);
    if (!e) return -1;

    /* base64-decode (b..e), skipping whitespace.  Use OpenSSL via
     * shelling out to `openssl base64 -d` to avoid pulling in a base64
     * library here.  Simpler: call out to `openssl x509 -outform DER`. */
    char tmpl_pem[] = "/tmp/n00b_dev_pem_XXXXXX";
    int  fd_pem    = mkstemp(tmpl_pem);
    if (fd_pem < 0) return -1;
    write(fd_pem, pem, pem_len);
    close(fd_pem);

    char tmpl_der[] = "/tmp/n00b_dev_der_XXXXXX";
    int  fd_der    = mkstemp(tmpl_der);
    if (fd_der < 0) { unlink(tmpl_pem); return -1; }
    close(fd_der);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "/opt/homebrew/opt/openssl@3.3/bin/openssl x509 -in '%s' "
             "-outform DER -out '%s' 2>/dev/null || "
             "openssl x509 -in '%s' -outform DER -out '%s' 2>/dev/null",
             tmpl_pem, tmpl_der, tmpl_pem, tmpl_der);
    int rc = system(cmd);
    unlink(tmpl_pem);
    if (rc != 0) { unlink(tmpl_der); return -1; }

    int sr = slurp_file(tmpl_der, out, out_len);
    unlink(tmpl_der);
    return sr;
}

typedef struct {
    uint16_t    port;
    const char *cert_pem_path;   /* nullptr → use embedded test fixture */
    const char *key_pem_path;
    const char *qlog_dir;        /* nullptr → off */
} server_opts_t;

static int
run_server_with(server_opts_t opts)
{
    uint8_t *cert_der_owned    = NULL;
    size_t   cert_der_owned_len= 0;
    char    *key_pem_owned     = NULL;   /* tempfile we write+unlink */
    const uint8_t *cert_der    = NULL;
    size_t         cert_der_len = 0;
    const char    *key_path     = NULL;

    if (opts.cert_pem_path && opts.key_pem_path) {
        /* Real PKI path: parse the PEM cert into DER, pass the key
         * file path straight through (picotls's minicrypto loader is
         * file-based). */
        uint8_t *pem; size_t pem_len;
        if (slurp_file(opts.cert_pem_path, &pem, &pem_len) != 0) {
            fprintf(stderr, "[server] cannot read %s\n", opts.cert_pem_path);
            return 1;
        }
        if (pem_cert_to_der(pem, pem_len,
                            &cert_der_owned, &cert_der_owned_len) != 0) {
            fprintf(stderr, "[server] cannot parse cert PEM at %s\n",
                    opts.cert_pem_path);
            free(pem);
            return 1;
        }
        free(pem);
        cert_der     = cert_der_owned;
        cert_der_len = cert_der_owned_len;
        key_path     = opts.key_pem_path;
        fprintf(stderr, "[server] using real PKI: cert=%s key=%s\n",
                opts.cert_pem_path, opts.key_pem_path);
    } else {
        /* Test-fixture path. */
        key_pem_owned = n00b_quic_test_write_key_pem();
        if (!key_pem_owned) {
            fprintf(stderr, "[server] cannot write key tempfile\n");
            return 1;
        }
        cert_der     = n00b_quic_test_cert_der;
        cert_der_len = n00b_quic_test_cert_der_len;
        key_path     = key_pem_owned;
    }

    n00b_string_t *qlog_str =
        opts.qlog_dir ? n00b_string_from_cstr(opts.qlog_dir) : nullptr;
    auto er = n00b_quic_endpoint_new(g_conduit, g_io,
                                     .listen         = true,
                                     .bind_host      = "0.0.0.0",
                                     .bind_port      = opts.port,
                                     .alpn           = "n00b-echo/1",
                                     .cert_der_bytes = cert_der,
                                     .cert_der_len   = cert_der_len,
                                     .key_pem_path   = key_path,
                                     .qlog_dir       = qlog_str);
    if (n00b_result_is_err(er)) {
        fprintf(stderr, "[server] endpoint_new err=%d\n",
                n00b_result_get_err(er));
        if (key_pem_owned) { unlink(key_pem_owned); free(key_pem_owned); }
        free(cert_der_owned);
        return 2;
    }
    n00b_quic_endpoint_t *server = n00b_result_get(er);

    /* (cert_der_owned is held by picoquic now via its own copy; we
     * can free our parse buffer.  The endpoint copied the bytes
     * inside set_tls_certificate_chain.) */
    free(cert_der_owned);

    /* Subscribe an inbox to the endpoint's accept topic.  The
     * endpoint's accept-default callback wraps each new picoquic_cnx
     * as `n00b_quic_conn_t` and publishes here.  Subscribers (us)
     * keep the conn pointers and walk their channels each iteration. */
    n00b_option_t(n00b_conduit_topic_base_t *) atopic_opt =
        n00b_quic_endpoint_accept_topic(server);
    n00b_require(n00b_option_is_set(atopic_opt),
                 "server accept topic must be set");
    n00b_conduit_topic_base_t *atopic = n00b_option_get(atopic_opt);
    n00b_quic_accept_inbox_t *ainbox = n00b_quic_accept_inbox_new(g_conduit);
    n00b_quic_accept_subscribe(atopic, ainbox,
                               .operations = N00B_CONDUIT_OP_ALL);

    uint16_t bound = n00b_quic_endpoint_local_port(server);
    fprintf(stderr, "[server] listening on 0.0.0.0:%u (Ctrl-C to stop)\n",
            bound);

    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* Bookkeeping: every accepted conn gets walked each loop iteration
     * to find newly-recv'd bytes and echo them back.  picoquic and
     * our conn callback handle the dirty work; the application sees
     * channels appear / get bytes / receive FIN through the same
     * `chan_recv` API a client uses. */
    enum { MAX_LIVE = 256 };
    n00b_quic_conn_t *live[MAX_LIVE];
    size_t           n_live = 0;

    while (!atomic_load(&g_server_stop)) {
        n00b_quic_endpoint_run_once(server, 100);

        /* Drain newly accepted conns. */
        while (n00b_quic_accept_inbox_has_messages(ainbox)) {
            n00b_quic_accept_msg_t *m = n00b_quic_accept_inbox_pop(ainbox);
            if (m && m->payload.conn && n_live < MAX_LIVE) {
                live[n_live++] = m->payload.conn;
                fprintf(stderr, "[server] accepted conn %p (live=%zu)\n",
                        (void *)m->payload.conn, n_live);
            }
        }

        /* Walk live conns, echoing on each.  Compact closed conns out. */
        size_t kept = 0;
        for (size_t i = 0; i < n_live; i++) {
            n00b_quic_conn_t *c = live[i];
            n00b_quic_conn_state_t st = n00b_quic_conn_state(c);
            if (st == N00B_QUIC_CONN_STATE_CLOSED ||
                st == N00B_QUIC_CONN_STATE_FAILED) {
                fprintf(stderr, "[server] conn %p closed (state=%d)\n",
                        (void *)c, (int)st);
                continue;
            }
            echo_walk(c);
            live[kept++] = c;
        }
        n_live = kept;
    }

    fprintf(stderr, "[server] shutting down (%zu live conns at exit)\n",
            n_live);
    n00b_quic_endpoint_close(server);
    if (key_pem_owned) { unlink(key_pem_owned); free(key_pem_owned); }
    return 0;
}

/* Compatibility wrapper: old single-port entry point.  Argv parsing
 * sets cert_pem_path / key_pem_path when --cert-pem= / --key-pem= are
 * given; that goes through here. */
static int
run_server(uint16_t port)
{
    server_opts_t o = { .port = port };
    return run_server_with(o);
}

/* ===========================================================================
 * Client mode
 * =========================================================================== */

typedef struct {
    const char *host;
    uint16_t    port;
    const char *msg;
    int         repeat;       /* number of msgs on one channel; 1 = single */
    const char *qlog_dir;     /* nullptr = off */
} client_opts_t;

static int
run_client_with(client_opts_t opts)
{
    n00b_string_t *qlog_str =
        opts.qlog_dir ? n00b_string_from_cstr(opts.qlog_dir) : nullptr;
    auto er = n00b_quic_endpoint_new(g_conduit, g_io,
                                     .bind_host = "0.0.0.0",
                                     .alpn      = "n00b-echo/1",
                                     .qlog_dir  = qlog_str);
    if (n00b_result_is_err(er)) {
        fprintf(stderr, "[client] endpoint_new err=%d\n",
                n00b_result_get_err(er));
        return 1;
    }
    n00b_quic_endpoint_t *client = n00b_result_get(er);

    /* Test-only: bypass picotls's cert verifier.  Replaced by
     * trust_t-driven verification when that bridge ships. */
    picoquic_set_null_verifier(client->quic);

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(opts.port);
    if (inet_pton(AF_INET, opts.host, &dst.sin_addr) != 1) {
        fprintf(stderr, "[client] bad host '%s'\n", opts.host);
        n00b_quic_endpoint_close(client);
        return 2;
    }

    auto cr = n00b_quic_connect(client,
                                (const struct sockaddr *)&dst,
                                n00b_string_from_cstr("quic-test.n00b.local"));
    if (n00b_result_is_err(cr)) {
        fprintf(stderr, "[client] connect err=%d\n",
                n00b_result_get_err(cr));
        n00b_quic_endpoint_close(client);
        return 3;
    }
    n00b_quic_conn_t *conn = n00b_result_get(cr);

    /* Drive until handshake completes.  Across a real network this
     * is dominated by RTT × 3 (one ClientHello + two Handshake round
     * trips for TLS 1.3). */
    uint64_t handshake_start_us = (uint64_t)n00b_us_timestamp();
    for (int i = 0; i < 600; i++) {
        n00b_quic_endpoint_run_once(client, 5);
        if (n00b_quic_conn_state(conn) == N00B_QUIC_CONN_STATE_CONNECTED) {
            break;
        }
        if (n00b_quic_conn_state(conn) == N00B_QUIC_CONN_STATE_FAILED ||
            n00b_quic_conn_state(conn) == N00B_QUIC_CONN_STATE_CLOSED) {
            fprintf(stderr, "[client] handshake failed (state=%d)\n",
                    (int)n00b_quic_conn_state(conn));
            n00b_quic_endpoint_close(client);
            return 4;
        }
    }
    if (n00b_quic_conn_state(conn) != N00B_QUIC_CONN_STATE_CONNECTED) {
        fprintf(stderr, "[client] handshake timeout\n");
        n00b_quic_endpoint_close(client);
        return 5;
    }
    uint64_t handshake_us =
        (uint64_t)n00b_us_timestamp() - handshake_start_us;
    fprintf(stderr, "[client] connected in %llu ms\n",
            (unsigned long long)(handshake_us / 1000));

    /* For repeat=1: send one message + FIN, observe peer FIN, exit.
     * For repeat>N: open ONE channel; send N messages without FIN
     * (waiting for each echo before sending the next).  After the
     * Nth echo, FIN.  This is the duplex / pipelined-message demo
     * — the handshake amortizes across all N messages. */
    int repeat = opts.repeat > 0 ? opts.repeat : 1;
    size_t base_msg_len = strlen(opts.msg);

    auto chr = n00b_quic_chan_open(conn);
    n00b_quic_chan_t *chan = n00b_result_get(chr);

    uint64_t total_echo_us = 0;
    for (int round = 0; round < repeat; round++) {
        /* Build a per-round message so the echo prints distinguishably. */
        char round_msg[512];
        int  rlen = snprintf(round_msg, sizeof(round_msg),
                             "%s [round %d/%d]", opts.msg, round + 1, repeat);
        size_t mlen = (size_t)rlen;
        bool   fin  = (round == repeat - 1);

        auto sr = n00b_quic_chan_send(chan, (const uint8_t *)round_msg, mlen,
                                      .fin = fin);
        if (n00b_result_is_err(sr)) {
            fprintf(stderr, "[client] chan_send err=%d\n",
                    n00b_result_get_err(sr));
            n00b_quic_endpoint_close(client);
            return 6;
        }
        fprintf(stderr, "[client] sent %zu bytes (round %d/%d, fin=%d)\n",
                mlen, round + 1, repeat, fin ? 1 : 0);

        /* Drive until we've recv'd at least mlen more bytes for this
         * round.  Server echoes our exact bytes back, so we count
         * by length. */
        char    recv_buf[1024] = {0};
        size_t  got            = 0;
        uint64_t round_start_us = (uint64_t)n00b_us_timestamp();
        for (int i = 0; i < 2000; i++) {
            n00b_quic_endpoint_run_once(client, 5);
            if (n00b_quic_chan_has_data(chan)) {
                auto rr = n00b_quic_chan_recv(chan,
                                              (uint8_t *)recv_buf + got,
                                              sizeof(recv_buf) - got);
                if (n00b_result_is_ok(rr)) got += n00b_result_get(rr);
            }
            if (got >= mlen) {
                /* If FIN'd, also wait for peer FIN. */
                if (!fin || n00b_quic_chan_recv_fin(chan)) break;
            }
        }
        uint64_t round_us = (uint64_t)n00b_us_timestamp() - round_start_us;
        total_echo_us += round_us;

        if (got < mlen || memcmp(recv_buf, round_msg, mlen) != 0) {
            fprintf(stderr,
                    "[client] round %d: echo mismatch — got %zu bytes\n",
                    round + 1, got);
            n00b_quic_endpoint_close(client);
            return 7;
        }
        printf("%.*s\n", (int)got, recv_buf);
    }

    fprintf(stderr, "[client] %d round(s) echoed; total %llu ms (%llu ms/round)\n",
            repeat,
            (unsigned long long)(total_echo_us / 1000),
            (unsigned long long)(total_echo_us / 1000 / (uint64_t)repeat));

    n00b_quic_chan_close(chan);
    n00b_quic_close(conn, 0);
    /* One more run to flush the close frame. */
    n00b_quic_endpoint_run_once(client, 5);
    n00b_quic_endpoint_close(client);
    return 0;
}

/* Compatibility wrapper for the old positional-arg path. */
static int
run_client(const char *host, uint16_t port, const char *msg)
{
    client_opts_t o = { .host = host, .port = port, .msg = msg, .repeat = 1 };
    return run_client_with(o);
}

/* ===========================================================================
 * Loopback smoke (default mode — kept so `quic_echo` with no args
 * still does the original demo).
 * =========================================================================== */

static int
run_loopback(void)
{
    char *key_pem_path = n00b_quic_test_write_key_pem();
    if (!key_pem_path) return 1;

    auto sr = n00b_quic_endpoint_new(g_conduit, g_io,
                                     .listen         = true,
                                     .bind_host      = "127.0.0.1",
                                     .alpn           = "n00b-echo/1",
                                     .cert_der_bytes = n00b_quic_test_cert_der,
                                     .cert_der_len   = n00b_quic_test_cert_der_len,
                                     .key_pem_path   = key_pem_path);
    n00b_quic_endpoint_t *server = n00b_result_get(sr);

    n00b_option_t(n00b_conduit_topic_base_t *) atopic_opt =
        n00b_quic_endpoint_accept_topic(server);
    n00b_require(n00b_option_is_set(atopic_opt),
                 "server accept topic must be set");
    n00b_conduit_topic_base_t *atopic = n00b_option_get(atopic_opt);
    n00b_quic_accept_inbox_t *ainbox = n00b_quic_accept_inbox_new(g_conduit);
    n00b_quic_accept_subscribe(atopic, ainbox,
                               .operations = N00B_CONDUIT_OP_ALL);

    auto cur = n00b_quic_endpoint_new(g_conduit, g_io,
                                      .bind_host = "127.0.0.1",
                                      .alpn      = "n00b-echo/1");
    n00b_quic_endpoint_t *client = n00b_result_get(cur);
    picoquic_set_null_verifier(client->quic);

    uint16_t           sport = n00b_quic_endpoint_local_port(server);
    struct sockaddr_in dst   = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(sport);
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    n00b_quic_conn_t *conn = n00b_result_get(
        n00b_quic_connect(client,
                          (const struct sockaddr *)&dst,
                          n00b_string_from_cstr("quic-test.n00b.local")));

    for (int i = 0; i < 200; i++) {
        n00b_quic_endpoint_run_once(client, 5);
        n00b_quic_endpoint_run_once(server, 5);
        if (n00b_quic_conn_state(conn) == N00B_QUIC_CONN_STATE_CONNECTED) break;
    }

    n00b_quic_chan_t *chan =
        n00b_result_get(n00b_quic_chan_open(conn));

    static const char *msg = "hello, n00b/quic!";
    size_t             mlen = strlen(msg);
    n00b_quic_chan_send(chan, (const uint8_t *)msg, mlen, .fin = true);

    char  buf[256] = {0};
    size_t got = 0;
    n00b_quic_conn_t *server_conn = NULL;
    for (int i = 0; i < 500; i++) {
        n00b_quic_endpoint_run_once(client, 5);
        n00b_quic_endpoint_run_once(server, 5);

        /* Drain new server-side accepts. */
        while (n00b_quic_accept_inbox_has_messages(ainbox)) {
            n00b_quic_accept_msg_t *m = n00b_quic_accept_inbox_pop(ainbox);
            if (m && m->payload.conn) server_conn = m->payload.conn;
        }
        if (server_conn) {
            echo_walk(server_conn);
        }

        if (n00b_quic_chan_has_data(chan)) {
            auto rr = n00b_quic_chan_recv(chan, (uint8_t *)buf + got,
                                          sizeof(buf) - got);
            if (n00b_result_is_ok(rr)) got += n00b_result_get(rr);
        }
        if (got >= mlen && n00b_quic_chan_recv_fin(chan)) break;
    }

    if (got != mlen || memcmp(buf, msg, mlen) != 0) {
        fprintf(stderr, "[loopback] mismatch (got %zu bytes)\n", got);
        return 1;
    }
    printf("[loopback OK] %s -> %.*s\n", msg, (int)got, buf);

    n00b_quic_chan_close(chan);
    n00b_quic_close(conn, 0);
    n00b_quic_endpoint_close(client);
    n00b_quic_endpoint_close(server);
    unlink(key_pem_path);
    free(key_pem_path);
    return 0;
}

/* ===========================================================================
 * Argv dispatch
 * =========================================================================== */

static void
usage(const char *argv0)
{
    fprintf(stderr,
            "usage:\n"
            "  %s                              # loopback smoke test\n"
            "  %s server [PORT] [flags]\n"
            "      --cert-pem=PATH --key-pem=PATH   real PKI cert+key\n"
            "      --qlog=DIR                       write per-cnx qlog files\n"
            "  %s client HOST PORT [MESSAGE] [flags]\n"
            "      --multi=N    send N messages on one channel (default 1)\n"
            "      --qlog=DIR   write per-cnx qlog files\n",
            argv0, argv0, argv0);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    if (!common_setup()) return 1;

    int rc = 0;
    if (argc <= 1) {
        rc = run_loopback();
    } else if (strcmp(argv[1], "server") == 0) {
        server_opts_t opts = { .port = 4433 };
        for (int i = 2; i < argc; i++) {
            const char *a = argv[i];
            if (a[0] == '-' && a[1] == '-') {
                if (strncmp(a, "--cert-pem=", 11) == 0) {
                    opts.cert_pem_path = a + 11;
                } else if (strncmp(a, "--key-pem=", 10) == 0) {
                    opts.key_pem_path = a + 10;
                } else if (strncmp(a, "--qlog=", 7) == 0) {
                    opts.qlog_dir = a + 7;
                } else {
                    fprintf(stderr, "unknown server flag: %s\n", a);
                    usage(argv[0]); return 1;
                }
            } else {
                /* Positional: port. */
                opts.port = (uint16_t)atoi(a);
            }
        }
        if ((opts.cert_pem_path && !opts.key_pem_path) ||
            (!opts.cert_pem_path && opts.key_pem_path)) {
            fprintf(stderr, "--cert-pem and --key-pem must come together\n");
            return 1;
        }
        rc = run_server_with(opts);
    } else if (strcmp(argv[1], "client") == 0) {
        client_opts_t copts = { .repeat = 1, .msg = "hello, n00b/quic!" };
        int positional = 0;
        for (int i = 2; i < argc; i++) {
            const char *a = argv[i];
            if (a[0] == '-' && a[1] == '-') {
                if (strncmp(a, "--multi=", 8) == 0) {
                    copts.repeat = atoi(a + 8);
                    if (copts.repeat <= 0) copts.repeat = 1;
                } else if (strncmp(a, "--qlog=", 7) == 0) {
                    copts.qlog_dir = a + 7;
                } else {
                    fprintf(stderr, "unknown client flag: %s\n", a);
                    usage(argv[0]); return 1;
                }
            } else {
                /* Positional: HOST PORT [MESSAGE] */
                switch (positional++) {
                case 0: copts.host = a; break;
                case 1: copts.port = (uint16_t)atoi(a); break;
                case 2: copts.msg  = a; break;
                default:
                    fprintf(stderr, "extra positional arg: %s\n", a);
                    usage(argv[0]); return 1;
                }
            }
        }
        if (positional < 2) { usage(argv[0]); return 1; }
        rc = run_client_with(copts);
    } else {
        usage(argv[0]);
        rc = 1;
    }

    common_teardown();
    n00b_shutdown();
    return rc;
}
