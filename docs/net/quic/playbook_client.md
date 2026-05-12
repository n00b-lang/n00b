# HTTP/3 client / connect-only playbook

How to use n00b's QUIC + H3 surface to connect to *someone else's*
HTTP/3 server: a SaaS API endpoint, an internal microservice, an
ACME directory, an OIDC provider.  The flip side of
[`playbook_k8s.md`](playbook_k8s.md) and
[`playbook_vm.md`](playbook_vm.md), which cover running n00b *as*
a server.

The library surface for client-only deployments is small:

- [`n00b_quic_endpoint_new`](../../../include/net/quic/endpoint.h) with
  `listen=false` (default) — owns the local UDP socket + picoquic
  context.
- [`n00b_quic_connect`](../../../include/net/quic/conn.h) — opens one QUIC
  connection to a peer address.
- [`n00b_h3_client_new`](../../../include/net/quic/h3.h) — wraps the
  connection in HTTP/3 framing + QPACK.
- [`n00b_h3_client_request`](../../../include/net/quic/h3.h) — issues
  one HTTP/3 request.

`examples/quic_echo/main.c` is the minimal shape: open a QUIC
connection, exchange a message, close.  `examples/quic_phase5_demo/main.c`
is the production-shape: real cert-pinned trust, multiple RPCs,
DPoP / mTLS-bound tokens.

(The ACME demos under `examples/quic_acme_demo/` /
`examples/quic_acme_http01_demo/` look like they should be in
this category but aren't — ACME directories are HTTP/1.1 over
TCP+TLS, not HTTP/3, so the n00b ACME client uses a separate
blocking-socket HTTPS shim — see
[`acme_http.h`](../../../include/internal/net/quic/acme_http.h) — and
doesn't go through the QUIC stack at all.)

## Can I use QUIC at all?

Three answers, in increasing order of how much work it is to find
out.

### 1. Network-path check

QUIC is UDP/443 (or whatever port the operator chooses; UDP/443 is
the convention).  The client environment must allow:

- Outbound UDP to that port.  Not always true.  Common blockers:
  - **Corporate firewalls** with explicit deny-UDP-except-DNS.
    Often paired with TLS MITM proxies that expect TCP+TLS.
  - **Hotel / public WiFi** with captive portals — UDP often
    works but is throttled.
  - **Mobile carrier NAT** — UDP works but the connection-id
    privacy / migration story matters more here than on a wired
    desktop (carrier NATs rebind aggressively).
  - **VPN tunnels** — wireguard / OpenVPN-UDP work; some legacy
    SSL VPNs only forward TCP.
- A path MTU of at least 1200 bytes.  RFC 9000 § 14.1 mandates
  this as the QUIC INITIAL packet floor.  PMTUD-blackholing
  middleboxes (older home routers, some satellite uplinks) can
  drop INITIAL packets silently.

A 5-second `nc -u <host> 443 < /dev/null` from the client
environment is a sufficient first check.  More thorough: stand up
[`picoquicdemo`](https://github.com/private-octopus/picoquic) on a
test endpoint and run `picoquicdemo <host> 443 -h <hostname>` from
the client side; failure modes (timeout vs handshake error vs
version-negotiation) tell you which layer is blocking.

### 2. Server discovery

You know the path is open; does the *server* speak QUIC?  Three
mechanisms are standard:

#### a. Pre-knowledge

You read the vendor's docs.  Docs say "h3 endpoint at
`<host>:443`."  You point n00b at it.  Done.

This is the path the Phase 5 fixtures take — they hardcode
Pebble's directory URL, Keycloak's discovery endpoint, etc.
Every public ACME server (Let's Encrypt, ZeroSSL, Buypass)
publishes its directory URL; every OIDC IdP publishes its
discovery endpoint.

Real production-shaped n00b clients almost always have an
operator-provided URL — the ACME directory, the IdP's discovery
URL, the API gateway's address.  The discovery problem only
exists when you're talking to *user-supplied* servers (web
browsers visiting random URLs).

#### b. Alt-Svc (RFC 7838)

Connect once over HTTP/2 / TCP+TLS.  Server response carries:

```http
Alt-Svc: h3=":443"; ma=86400
```

The header tells the client: "I also speak HTTP/3 on UDP/443;
remember this for the next 24 hours."  The client then uses
HTTP/3 for follow-up requests to the same origin.

Caveats:

- The first request *always* takes a TCP+TLS roundtrip to learn
  about Alt-Svc.  That's why §c (DNS) exists.
- The cache is per-origin.  Cross-origin redirects re-trigger
  discovery.
- Some servers send Alt-Svc *without actually supporting h3*
  (legacy config, broken upgrades).  Clients should fall back
  on h3 connection failure (see § 3 below).

n00b doesn't implement Alt-Svc parsing today — operators wire
this layer themselves.  If you're connecting to an HTTPS API
that announces h3 via Alt-Svc, the safe pattern is:

1. Make the first request via your existing HTTP/2 client
   (curl, libcurl, n00b's HTTP/1.1 shim from
   [`acme_http.h`](../../../include/internal/net/quic/acme_http.h)).
2. Parse Alt-Svc from the response.
3. For follow-up requests, open a QUIC connection to the
   advertised endpoint via `n00b_quic_connect`.
4. Cache the Alt-Svc validity window; re-resolve on expiry.

#### c. HTTPS / SVCB DNS records (RFC 9460)

The DNS-native answer.  An `HTTPS` record published alongside
the `A`/`AAAA` records announces the supported ALPNs *before*
any TCP handshake:

```
example.com. 60 IN HTTPS 1 . alpn="h3,h2" port=443
```

A client that supports HTTPS records can issue an HTTPS-record
query first, see `alpn=h3`, and go straight to QUIC.  This is
what modern browsers do; n00b doesn't ship an HTTPS-record
resolver today (DNS RR parsing is out of n00b's scope), but
operators can use a sidecar resolver (Unbound 1.18+, knot
3.x+, dnsdist 1.8+) and feed the result into n00b's
`n00b_quic_connect` call as the resolved IP + port.

#### d. Happy Eyeballs v2 (RFC 8305) racing

For latency-critical clients (browsers, mobile apps), the
state of the art is to race the connection establishment
itself: open a QUIC handshake and a TCP+TLS handshake in
parallel, take whichever completes first.  The IETF's
[draft-pauly-happy-eyeballs-v2](https://datatracker.ietf.org/doc/draft-pauly-happy-eyeballs-v2/)
extends Happy Eyeballs to include HTTP/3 racing with a
configurable head-start for QUIC (typical: 200–500 ms).

n00b doesn't ship a Happy-Eyeballs-v2 racer in this revision.
The pattern is well-suited to operator-side composition:
a thin wrapper that calls `n00b_quic_connect` and a TCP
client in two threads, returns the first one that succeeds.
If you're building one, the
[Cronet](https://chromium.googlesource.com/chromium/src/+/main/components/cronet/)
project's race logic is a reasonable reference.

### 3. Fallback when QUIC fails

For any client that wants to "use HTTP/3 if possible, fall back
otherwise," the test is the QUIC handshake itself.  The
n00b_quic_connect signal:

| Result                                  | Interpretation                                      |
|-----------------------------------------|-----------------------------------------------------|
| Returns `n00b_quic_conn_t *` in CONNECTED state | QUIC works on this path. |
| Returns `n00b_result_err(N00B_QUIC_ERR_HANDSHAKE)` | Server speaks QUIC but the handshake failed (cert verify, ALPN mismatch, version negotiation). |
| Returns `n00b_result_err(N00B_QUIC_ERR_TIMEOUT)` | UDP is plausibly blocked, server doesn't respond, or path MTU is too low. |

The conventional fallback recipe:

1. Set a tight handshake timeout (`.timeout_ms = 1500` or so) on
   the QUIC attempt.
2. On err / timeout, fall back to TCP+TLS+HTTP/2 (or /1.1).  n00b
   doesn't ship an HTTP/2 client; libcurl is the typical
   substrate.
3. Cache the success/failure decision per-origin for ~5 minutes
   so subsequent calls don't repeat the failed handshake.

This caching is the *opposite* of Alt-Svc's "remember the win"
semantics — you remember the loss too, so a path that's blocked
right now doesn't cost a 1.5 s handshake on every API call.

## Trust store

n00b's client uses the OS-native trust store via
[`n00b_quic_trust_system()`](../../../include/net/quic/trust.h):

- **macOS**: `SecTrustEvaluateWithError` against `SecTrustRef`.
- **Linux**: dlopens `libssl.so.3` + uses
  `X509_STORE_set_default_paths()` (which honours `SSL_CERT_FILE`
  / `SSL_CERT_DIR` env vars).

The default `n00b_quic_endpoint_new` call uses this trust store;
operators with private CAs / corporate trust requirements have
two paths:

1. **System-wide trust addition** — drop the CA into
   `/usr/local/share/ca-certificates/<your-ca>.crt` +
   `update-ca-certificates`.  This is what the Phase 5 K8s
   fixture image does for Pebble's Mini-CA (see
   [`runbook.md` § K8s.6](runbook.md)) so that n00b's ACME
   client trusts the in-cluster ACME server.
2. **Per-endpoint pinned trust** — pass
   `.trust = n00b_quic_trust_pinned(...)` with a SHA-256
   fingerprint.  This is the right call for mTLS-bound clients
   talking to a known-fingerprint endpoint, or for closed-fixture
   tests.

For ACME-style flows the n00b libraries route trust through
`n00b_quic_trust_system_verify_chain` (see
[`acme_trust_linux.c`](../../../src/net/quic/acme_trust_linux.c)); the
HTTPS shim used for ACME directory fetches honours the same
system trust store.  Operators don't need to thread trust
through the ACME APIs explicitly — it works out of the box once
the CA is in the system store.

## Minimal client skeleton

```c
n00b_runtime_t rt;
n00b_init(&rt, argc, argv);

auto cc = n00b_conduit_new();
n00b_conduit_t *c = n00b_result_get(cc);
auto ic = n00b_conduit_io_new_default(c);
n00b_conduit_io_backend_t *io = n00b_result_get(ic);

/* Client endpoint: listen=false (default), system trust store. */
auto er = n00b_quic_endpoint_new(c, io,
    .bind_host = "0.0.0.0",
    .alpn      = "h3");
n00b_quic_endpoint_t *ep = n00b_result_get(er);

/* Resolve <host>:443 to a sockaddr — operator's choice of resolver
 * (getaddrinfo for the simple case; HTTPS DNS RR for the modern
 * one). */
struct sockaddr_in6 peer = ...;

auto cr = n00b_quic_connect(ep, (struct sockaddr *)&peer, "example.com",
    .timeout_ms       = 1500);
if (n00b_result_is_err(cr)) {
    /* QUIC handshake failed.  Fall back to HTTP/2 over TCP. */
    fall_back_to_h2(...);
    return;
}
n00b_quic_conn_t *conn = n00b_result_get(cr);

auto h3r = n00b_h3_client_new(conn);
n00b_h3_client_t *h3 = n00b_result_get(h3r);

/* Drive request.  See examples/quic_echo/main.c for the minimal
 * end-to-end loopback, or examples/quic_phase5_demo/main.c for a
 * production-shaped flow with multiple RPCs and pinned trust. */
...

n00b_h3_client_close(h3);
n00b_quic_close(conn, 0);
n00b_quic_endpoint_close(ep);
n00b_conduit_io_destroy(io);
n00b_conduit_destroy(c);
n00b_shutdown();
```

## Connection migration considerations (client side)

QUIC's connection migration is what survives a client moving
between networks (WiFi → cellular, IPv4 → IPv6, NAT rebind) without
dropping the connection.  picoquic implements client-side
migration; the client code above gets it transparently when the
local IP changes.

If the *server* you're talking to is multi-replica behind an
LB-CID-aware load balancer (per
[`security.md` § QUIC LB-CID](security.md#quic-lb-cid)), migration
will Just Work — the LB decodes the wire CID and routes to the
right replica.  If the server is multi-replica behind a plain
random L4 LB, your post-migration packet flight may land on a
*different* replica that doesn't know about your connection;
you'll get a connection close.  Either:

1. Tolerate occasional reconnects (fine for stateless RPCs).
2. Pin the load balancer to source-IP-affinity (loses the
   migration benefit on the LB side; client side still
   migrates between paths cleanly when the affinity holds).
3. Lobby the server operator to deploy LB-CID.

## Operations + diagnostics

- **Did this connection use QUIC?** —
  `n00b_quic_conn_state(conn) == N00B_QUIC_CONN_STATE_CONNECTED`
  before any application traffic flows.  If false, you're not
  on QUIC yet (or you're on a fallback TCP path your wrapper
  set up).
- **What ALPN was negotiated?** — For now, n00b doesn't expose a
  post-handshake ALPN getter.  In practice, an `n00b_h3_client_new`
  call against a non-h3 endpoint fails fast, so by the time you
  have a working H3 client, ALPN was h3.  When n00b grows non-h3
  ALPN support, this will need a getter.
- **Why did handshake fail?** — Enable qlog
  (`.qlog_dir = "/tmp/n00b-qlog"`) on the endpoint;
  [qvis](https://qvis.quictools.info/) renders the per-connection
  trace.  For repeated handshake failures from the same client
  origin, watch the
  [`audit.h`](../../../include/net/quic/audit.h) sink — every reject
  decision is recorded.
- **Connection migration in flight** — qlog also records path
  changes and migration events.

## Where this fits in the docs

| You want to                                                         | See                                                  |
|---------------------------------------------------------------------|------------------------------------------------------|
| Run n00b *as* a server in K8s                                       | [`playbook_k8s.md`](playbook_k8s.md)                 |
| Run n00b *as* a server on a bare VM                                 | [`playbook_vm.md`](playbook_vm.md)                   |
| Connect to someone else's HTTP/3 endpoint                           | this file                                            |
| Acquire / renew a TLS cert via ACME                                 | [`cert_lifecycle.md`](cert_lifecycle.md), [`le_staging.md`](le_staging.md)        |
| Verify peer identity (token bindings, mTLS thumbprints)             | [`auth.md`](auth.md), [`security.md`](security.md)   |
