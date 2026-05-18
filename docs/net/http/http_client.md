# `n00b_http_request` — Operator guide

A single public entry point for issuing HTTPS requests, available
in two shapes:

- `n00b_http_request_sync(url, …)` — blocks the caller until the
  response is in hand.  Use this for scripts, CLIs, and code that
  doesn't already have a conduit IO loop running.

- `n00b_http_request(c, url, …)` — returns a typed conduit topic
  immediately; the caller reads with `n00b_conduit_read(...)`
  (sync) or subscribes (`n00b_conduit_subscribe(...)`) to receive
  the response asynchronously.

Both forms share the same semantics: the same kwargs, the same
transport selection, the same response struct.

## 30-second tour (sync)

```c
#include "net/http/http_client.h"

auto rr = n00b_http_request_sync(
    n00b_string_from_cstr("https://api.example.com/v1/widgets"),
    .timeout_ms       = 5000,
    .follow_redirects = true);
if (n00b_result_is_err(rr)) {
    int err = (int)n00b_result_get_err(rr);
    /* err is a negative n00b_quic_err_t or n00b_http_err_t */
    return err;
}
n00b_http_response_t *resp = n00b_result_get(rr);
int status = n00b_http_response_status(resp);
n00b_buffer_t *body = n00b_http_response_body(resp);
```

## Async — fire and handle

```c
auto tr = n00b_http_request(c, url, .auth = my_auth);
n00b_conduit_topic_t(n00b_http_response_t *) *t = n00b_result_get(tr);

/* Subscribe — your handler fires when the response lands. */
n00b_conduit_subscribe(n00b_http_response_t *, t, my_inbox,
                       .operations = N00B_CONDUIT_OP_ALL);

/* Drive the conduit; the worker thread the dispatcher spawned will
 * publish exactly once.  After publish, the topic closes; the
 * subscription's done-topic fires for callers that want to know
 * the round trip is complete. */
```

The dispatcher uses an `on_first_subscribe` hook — the worker that
performs the round trip doesn't run until at least one subscriber
attaches to the returned topic, so a cold-start race between
publish and read can't drop the message.

## Transport selection

The dispatcher races h3 first by default, falling back to h1 on
failure (`prefer_h3=true` default).  Failures are remembered in a
per-runtime "loss cache" with a 5-minute TTL — repeated calls to a
known h3-blocked origin go straight to h1.

To force one transport:

```c
n00b_http_request_sync(url, .prefer_h3 = false);  /* h1 only */
```

To accelerate fallback on slow networks, lower `h3_handshake_ms`
(default 1500):

```c
n00b_http_request_sync(url, .h3_handshake_ms = 500);
```

### Why h1 + h3, why no h2

Servers that speak h3 also speak h2 (the Alt-Svc upgrade requires
it).  Servers that speak h2 also speak h1 (h2 negotiation falls
back).  So h1 + h3 covers approximately every reachable HTTPS
server.  Implementing h2 (HPACK + frame layer + flow control +
stream priorities) is multi-month scope; the operational benefit
is "slightly faster large-payload throughput on a vanishing
population of h2-only servers."  Skipped.

## Body + content type

```c
n00b_buffer_t *body = n00b_buffer_from_cstr("{\"k\":1}");
n00b_http_request_sync(
    n00b_string_from_cstr("https://api.example.com/v1/widgets"),
    .method       = n00b_string_from_cstr("POST"),
    .body         = body,
    .content_type = n00b_string_from_cstr("application/json"));
```

`content_type` has no default — when you supply a body, you supply
the type.

## Redirects

Disabled by default for safety.  Enable with `follow_redirects =
true`:

```c
n00b_http_request_sync(url,
    .follow_redirects = true,
    .max_redirects    = 5);   /* default */
```

Method preservation per RFC 9110 § 15.4:
- 301 / 302 / 303 → method collapses to GET, body is dropped
- 307 / 308 → method + body preserved

`Location` is resolved from the response.  Supported shapes:
- Absolute `https://...` — pass-through to the URL parser.
- Absolute path (`/foo`) — combined with the current origin.

Protocol-relative (`//host/path`) and full RFC 3986 § 5 relative
resolution aren't supported yet; if you hit a server that uses
those, file a bug.

## Compression

Decompression is **on by default** (`auto_decompress = true`).  The
client advertises `Accept-Encoding: gzip, deflate` always, and adds
`br` and `zstd` opportunistically — at startup, the dispatcher
tries to `dlopen` `libbrotlidec` and `libzstd` and narrows the
header to whatever's available.

To disable decompression and receive the raw bytes:

```c
n00b_http_request_sync(url, .auto_decompress = false);
```

Request-body compression (`body_encoding` kwarg) lands in a
follow-up; today the body is sent as-is.

## Cookies

Off by default.  To use a cookie jar:

```c
n00b_http_cookie_jar_t *jar = n00b_http_cookie_jar_new();
n00b_http_request_sync(url, .cookie_jar = jar);
/* …jar is updated from `Set-Cookie:` headers; subsequent calls
 *  send the matching cookies via `Cookie:` */
```

The jar is in-memory only; if you need persistence across runs,
serialize the jar yourself (the helpers ship in chunk 12).

`SameSite` is parsed and stored but not yet enforced at request
time.  Public-suffix-list domain checks are also a follow-up; the
current jar treats every Domain= attribute as if the registry
allowed it.

## Auth

`n00b_http_auth_t` bundles three orthogonal mechanisms:

```c
n00b_http_auth_t auth = {
    .bearer_token = my_jwt,                /* Authorization: Bearer …  */
    .dpop_signer  = my_dpop_secret_handle, /* DPoP: …                  */
    .mtls_cert    = my_mtls_secret_handle, /* TLS client cert          */
};
n00b_http_request_sync(url, .auth = &auth);
```

DPoP proofs are computed per-request from `(method, URL)` — you
don't call `n00b_dpop_create()` yourself.

mTLS storage works in chunk 10; the cert is captured on the auth
struct.  Threading it through to the TCP+TLS path is a chunk-11/12
follow-up, so today the field is plumbed but not yet honored at
the transport layer.

For response-side validation (e.g. checking a server-signed JWS),
set `response_verifier` + `response_verifier_ctx`.  The dispatcher
calls the verifier after decompressing the response; returning
false rejects the response and surfaces an error to the caller.

## Trust

`trust` defaults to the platform's system trust store (the same
path Phase 1 ships).  To pin a self-signed CA for an internal
endpoint, pass an explicit handle:

```c
n00b_http_request_sync(url, .trust = my_trust_handle);
```

## Connection pooling

Both transports (h1 over TCP+TLS, h3 over QUIC) reuse connections
through a per-runtime pool stored on
`n00b_runtime_t::http_connection_pool` and exposed via
`n00b_http_get_connection_pool()`.  The dispatcher consults this
slot when the caller does not pass an explicit `.pool=` kwarg, so
plain `n00b_http_request_sync(url)` calls already benefit from
keep-alive against the same origin.

For h1, reuse is gated on the server's `Connection: keep-alive`
header (RFC 9112 § 9.6) AND a clean message boundary (Content-Length
fully consumed or chunked-body terminator seen) AND the peer not
having FIN'd.  For h3 the gate is the QUIC connection still being in
the `CONNECTED` state at the end of the round-trip.  Failed requests,
half-shut connections, and unexpected EOFs all bypass the pool and
close the underlying transport.

Caps + lifetimes default to 16 idle global / 4 per origin / 30s
idle / 10m lifetime; pass `.pool = n00b_http_connection_pool_new(
.max_total_idle = …, .max_per_origin = …, .idle_timeout_ms = …,
.lifetime_ms = …)` to override.

Pool stats (hits, misses, evictions) are observable through
`n00b_http_connection_pool_stats(pool)` for production tuning.

## Errors

The result error is a negative integer, taken from one of two
domains:

- `n00b_http_err_t` — URL / wire-format failures
  (`N00B_HTTP_ERR_UNSUPPORTED_SCHEME`, `N00B_HTTP_ERR_BAD_RESPONSE`,
  …).
- `n00b_quic_err_t` — transport failures (DNS, TCP, TLS, QUIC
  handshake, timeout).  Negative ints from `quic_types.h`.

Both ranges don't overlap, so a `switch` on the int value works.

HTTP status codes (4xx, 5xx) are **not** errors — they're returned
on the success path as `n00b_http_response_t` with the status code
in `n00b_http_response_status(resp)`.  The caller decides what
constitutes an error at the application layer.

In the topic-shaped path, transport errors come back as a response
with `status = 0` and a non-zero
`n00b_http_response_error()` — same semantics, different signal.

## What's not here yet

- True async (worker pool driven by the conduit IO loop, not a
  thread block).  Today the topic-shaped path submits work to the
  per-conduit `conduit_service` threadpool (lazy-spawned on first
  use), with a per-request thread fallback when the service can't
  accept the submit.  Latency is still bounded by socket / QUIC
  blocking — only fewer thread creates per request.
- HTTP/2 (deliberately skipped — see "Why h1 + h3" above).
- Plain HTTP (`http://`).  HTTPS only.
- WebSockets, Server-Sent Events, streaming response bodies — all
  require an event-driven response surface that's a different
  shape from `n00b_http_response_t`.
- HTTP/3 streaming responses (same reason).
- Multipart-form bodies — caller serializes for now.
