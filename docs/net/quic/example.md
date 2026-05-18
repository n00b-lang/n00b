# Walkthrough: `quic_echo` example

`examples/quic_echo/main.c` is a working end-to-end QUIC demo that
exercises everything the Phase 1 transport ships.  This document
walks through what it does, the n00b API patterns it demonstrates,
and what to look at when you're writing your own QUIC code on top
of n00b.

## What the example is

A single binary with three modes:

| Mode | Invocation | Use case |
|---|---|---|
| Loopback smoke | `./build_debug/quic_echo` | One-process round-trip; quick "did the build work?" check |
| Server | `./build_debug/quic_echo server [PORT] [flags]` | Long-lived listener, handles many clients |
| Client | `./build_debug/quic_echo client HOST PORT [MSG] [flags]` | One-shot or multi-message client; exits when done |

The server installs **no application logic of its own** beyond the
echo callback structure: it subscribes to the endpoint's accept
topic, walks each accepted conn's channel list, and copies received
bytes back via `chan_send`.  Same surface a real RPC server would
use.

## Server-side flow (the API patterns)

```c
/* 1. Bring up an endpoint in listen mode with cert+key. */
auto er = n00b_quic_endpoint_new(c, io,
    .listen         = true,
    .bind_host      = "0.0.0.0",
    .bind_port      = 4433,
    .alpn           = "n00b-echo/1",
    .cert_der_bytes = cert_der,
    .cert_der_len   = cert_der_len,
    .key_pem_path   = key_path);
n00b_quic_endpoint_t *server = n00b_result_get(er);

/* 2. Subscribe an inbox to the accept topic.  Each new accepted
 *    picoquic_cnx_t is wrapped as n00b_quic_conn_t and published
 *    once.  Application code never sees raw picoquic. */
n00b_conduit_topic_base_t *atopic = n00b_quic_endpoint_accept_topic(server);
n00b_quic_accept_inbox_t *ainbox  = n00b_quic_accept_inbox_new(c);
n00b_quic_accept_subscribe(atopic, ainbox, .operations = N00B_CONDUIT_OP_ALL);

/* 3. Run loop.  Three things every iteration:
 *      a. n00b_quic_endpoint_run_once drives picoquic + UDP.
 *      b. drain accept inbox → record new conns.
 *      c. for each live conn, walk channels and echo bytes. */
while (!stop) {
    n00b_quic_endpoint_run_once(server, 100);

    /* a. New connections arrive here. */
    while (n00b_quic_accept_inbox_has_messages(ainbox)) {
        n00b_quic_accept_msg_t *m = n00b_quic_accept_inbox_pop(ainbox);
        track(m->payload.conn);   /* application keeps the pointer */
    }

    /* b. Echo on every conn we know about. */
    for (each conn we're tracking) {
        for (n00b_quic_chan_t *ch = n00b_quic_conn_first_chan(conn);
             ch != NULL;
             ch = n00b_quic_chan_next_in_conn(ch)) {
            if (n00b_quic_chan_has_data(ch) || n00b_quic_chan_recv_fin(ch)) {
                uint8_t buf[1500];
                size_t n = n00b_result_get(n00b_quic_chan_recv(ch, buf, sizeof(buf)));
                bool fin = n00b_quic_chan_recv_fin(ch);
                n00b_quic_chan_send(ch, buf, n, .fin = fin);
            }
        }
    }
}
```

Things worth pausing on:

- **No raw picoquic.**  Earlier versions of this example installed
  an echo callback via `picoquic_set_default_callback` because we
  hadn't shipped the accept-topic API yet.  Now everything goes
  through n00b's surface: `chan_recv` / `chan_send` /
  `chan_recv_fin`.  This is the surface a real application uses.
- **Server channels appear automatically.**  When the peer opens
  a stream, our per-cnx callback (in `src/net/quic/conn.c`) detects
  the unknown stream-ID, calls `_n00b_quic_chan_accept_internal`
  to wrap it, and adds it to `conn->channels`.  Application code
  sees a new channel via `conn_first_chan` / `chan_next_in_conn`.
- **Multi-client concurrency is free.**  picoquic dispatches per-
  cnx events through the same callback; our default callback
  routes per-cnx via the conn's per-cnx callback.  The application
  loop walks every conn each iteration; thread count = 1.

## Client-side flow

```c
/* 1. Endpoint without listen/cert (initiate-only). */
auto er = n00b_quic_endpoint_new(c, io,
    .bind_host = "0.0.0.0",
    .alpn      = "n00b-echo/1");
n00b_quic_endpoint_t *client = n00b_result_get(er);

/* 2. Connect.  Returns immediately — handshake bytes flow on the
 *    next run_once. */
auto cr = n00b_quic_connect(client, dst_sockaddr, sni_string);
n00b_quic_conn_t *conn = n00b_result_get(cr);

/* 3. Drive until conn_state == CONNECTED. */
while (n00b_quic_conn_state(conn) != N00B_QUIC_CONN_STATE_CONNECTED) {
    n00b_quic_endpoint_run_once(client, 5);
}

/* 4. Open a channel and send. */
n00b_quic_chan_t *chan = n00b_result_get(n00b_quic_chan_open(conn));
n00b_quic_chan_send(chan, payload, len, .fin = true);

/* 5. Drive until echo arrives. */
while (need_more_bytes) {
    n00b_quic_endpoint_run_once(client, 5);
    if (n00b_quic_chan_has_data(chan)) {
        n00b_quic_chan_recv(chan, buf, sizeof(buf));
    }
}
```

Things worth pausing on:

- **`n00b_quic_connect` does not block on handshake.**  It returns
  the conn handle immediately; the handshake bytes go out on the
  next `run_once`.  Caller waits for `conn_state ==
  N00B_QUIC_CONN_STATE_CONNECTED` to know when the conn is ready.
- **Channel state machine reflects FIN propagation.**  When you
  `chan_send` with `.fin = true`, your channel transitions to
  SEND_HALF_CLOSED.  When the peer FINs back, you also see
  RECV_HALF_CLOSED → CLOSED.  Half-close is observable; we don't
  paper it over.
- **`run_once` is the only IO driver.**  Application code is in
  the application's run loop; `run_once` does one
  poll-IO + drain-recv + drain-send pass.  No hidden threads, no
  async surprise.

## The three modes, in detail

### Loopback (no args)

Two endpoints in one process; client and server both run on
`127.0.0.1`.  Sends `"hello, n00b/quic!"` once with FIN, expects
the same bytes back.  Useful as a smoke test that the build, the
test cert fixture, and the picoquic linkage are all working.

### Server mode

```bash
./build_debug/quic_echo server [PORT] [flags]
```

Flags:

| Flag | Effect |
|---|---|
| `--cert-pem=PATH` | Real-PKI cert (PEM x509).  Pair with `--key-pem`. |
| `--key-pem=PATH`  | Real-PKI key (PEM PKCS#8).  Pair with `--cert-pem`. |
| `--qlog=DIR`      | Write per-cnx `<conn-id>.qlog` files into DIR. |

Without `--cert-pem` / `--key-pem`, the server uses the embedded
test fixture (`test/fixtures/quic_test_pki.h`) — fine for local
dev, never used in production.

Server installs `SIGINT` and `SIGTERM` handlers; `Ctrl-C` shuts
down cleanly.  Logs to stderr, so redirect with `> server.log
2>&1 &` if backgrounding.

### Client mode

```bash
./build_debug/quic_echo client HOST PORT [MESSAGE] [flags]
```

Flags:

| Flag | Effect |
|---|---|
| `--multi=N`  | Send N messages on **one** channel without FIN between them; FIN at the end.  Demos handshake amortization. |
| `--qlog=DIR` | Write per-cnx qlog file into DIR. |

Without `--multi`, default behaviour is one message + FIN.  The
client always uses `picoquic_set_null_verifier(client->quic)`
today — that bypass goes away when the trust→picotls bridge ships
in Phase 3.

## Reading the timings the example prints

```
[client] connected in 56 ms
[client] sent 16 bytes (round 1/5, fin=0)
[client] sent 16 bytes (round 2/5, fin=0)
...
[client] 5 round(s) echoed; total 438 ms (87 ms/round)
```

- **`connected in N ms`** — wall-clock time from `n00b_quic_connect`
  to `conn_state == CONNECTED`.  Across loopback this is ~3 RTTs
  + setup overhead, typically 50–80 ms.
- **`87 ms/round`** — per-message round-trip time.  Note the
  single-message case is faster (~3 ms) because picoquic's pacing
  / probe timer fires later when there's nothing to send.  For
  small messages on loopback the loop's 5 ms `run_once` timeout
  is the dominant factor; production code using a tighter
  `run_once` cadence + picoquic's wake-time hints will see
  microsecond-scale latencies.

## Multi-client demo

```bash
./build_debug/quic_echo server 4433 &
for i in $(seq 1 5); do
    ./build_debug/quic_echo client 127.0.0.1 4433 "msg from client $i" &
done; wait
```

The server handles all 5 concurrently from one thread — picoquic
maintains state per `picoquic_cnx_t`, our `n00b_quic_conn_t`
wraps that, the accept-default callback wires each new cnx into
the accept topic.  Server log:

```
[server] accepted conn 0x... (live=5)
[server] conn 0x... closed (state=3)
... × 5
```

`live=N` is the high-water-mark.  Each conn cleanly transitions to
CLOSING (state 3) when the client closes, and gets unlinked from
`endpoint->accepted` so long-lived servers don't leak.

## Real PKI demo

```bash
./examples/quic_echo/setup_dev_pki.sh    # sudo prompt for cert install
./build_debug/quic_echo server 4433 \
    --cert-pem=$HOME/.n00b-dev-pki/server.crt.pem \
    --key-pem=$HOME/.n00b-dev-pki/server.key.pem \
    --qlog=/tmp/n00b-qlog &
./build_debug/quic_echo client 127.0.0.1 4433 "real PKI"
./examples/quic_echo/teardown_dev_pki.sh
```

See `docs/net/quic/dev_pki.md` for what's happening under the hood.
The cert is real, ACME-issued, OS-trusted — the only piece that's
still test-grade is the client-side `picoquic_set_null_verifier`,
which goes away in Phase 3.

## What this example doesn't do

- **0-RTT.**  Channels declare `.zero_rtt` as a kwarg, but the
  echo example never opts in.  When the channel API ships 0-RTT,
  add a `--zero-rtt` flag.
- **Datagram channels (RFC 9221).**  `N00B_QUIC_CHAN_DGRAM` is in
  the API surface but returns `NOT_IMPLEMENTED`; pending design
  work on per-connection vs per-channel semantics.
- **Authentication (token verification, JWT, mTLS uplift).**  All
  Phase 3.

## Cross-references

- `examples/quic_echo/main.c` — source.
- `examples/quic_echo/setup_dev_pki.sh` — dev PKI bootstrap.
- `examples/quic_echo/teardown_dev_pki.sh` — dev PKI cleanup.
- `docs/net/quic/overview.md` — full API map.
- `docs/net/quic/dev_pki.md` — step-ca workflow.
- `~/dd/quic_1.md § 14.4`, `§ 15` — design source.
