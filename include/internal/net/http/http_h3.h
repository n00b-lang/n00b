/**
 * @file http_h3.h
 * @internal
 * @brief HTTP/3 transport for the n00b HTTP client (Phase 6, chunk 3).
 *
 * Pulls the existing QUIC + H3 stack into a single round-trip
 * primitive parallel to `http_h1.h`'s `n00b_http_h1_round_trip`:
 *
 *     URL → DNS resolve → endpoint_new(ALPN=h3) → connect →
 *     wait for CONNECTED → h3_client_new → drive(SETTINGS) →
 *     h3_client_request → h3_request_await → response.
 *
 * Optional connection pooling via the `pool` kwarg: hits skip
 * everything up through `h3_client_new` and reuse the cached
 * (conduit, io, endpoint, conn, h3_client) tuple; misses run the
 * full sequence above and release the tuple back to the pool when
 * the round-trip finishes cleanly.
 *
 * @see ~/dd/quic_6.md § 7 chunk 3.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/result.h"
#include "net/quic/quic_types.h"
#include "net/quic/h3.h"
#include "internal/net/http/http_url.h"
#include "internal/net/http/http_h1.h"

/* Forward decl — pool struct lives in include/internal/net/http/http_pool.h. */
typedef struct n00b_http_connection_pool n00b_http_connection_pool_t;

/**
 * @brief One synchronous HTTP/3 request → response round trip.
 *
 * Constructs a fresh QUIC endpoint + outbound connection for each
 * call unless the optional `pool` kwarg points at a connection
 * pool with an idle entry for `url->origin` under the H3
 * transport.  ALPN is hardcoded to `"h3"`; the caller supplies an
 * HTTPS URL (the parser rejects every other scheme).
 *
 * Trust-store plumbing IS wired through picotls via
 * `n00b_quic_picotls_verify_install` (see endpoint.c).  Passing
 * `.trust = nullptr` defaults to the OS-native verifier
 * (`n00b_quic_trust_system()`); pass an explicit pinned-fingerprint
 * trust handle for self-signed test fixtures.
 *
 * @param url  Parsed URL.  Must be HTTPS.  Required.
 *
 * @kw method        HTTP method.  Default `"GET"`.
 * @kw body          Optional request body.
 * @kw content_type  Required if @p body is non-NULL.
 * @kw extra         Optional caller-supplied headers (re-uses the
 *                   h1 bag — internal type, not public).
 * @kw handshake_ms  Per-call deadline for the QUIC handshake.
 *                   Default 1500 — matches the chunk-5 race-fallback
 *                   default so a slow path falls through to h1
 *                   quickly.
 * @kw await_ms      Deadline for the response-await phase only.
 *                   Default 30000 (matches h1).
 * @kw trust         Optional trust handle (currently a no-op; see
 *                   note above).
 * @kw pool          Optional connection pool.  When non-NULL, the
 *                   round-trip tries `acquire(origin, H3)` before
 *                   building a fresh tuple, and `release`s the tuple
 *                   back on clean completion (CONNECTED-state conn
 *                   only).  When NULL the round-trip is fully
 *                   stateless — every call pays the QUIC handshake.
 * @kw allocator     Default per-runtime conduit pool.
 *
 * @return  Result with the populated `n00b_h3_response_t *` on
 *          success.  Errors carry `n00b_quic_err_t` codes
 *          (negative ints) — DNS failures map to
 *          `N00B_QUIC_ERR_BIND_FAILED`, handshake timeout to
 *          `N00B_QUIC_ERR_HANDSHAKE`, etc.  HTTP 4xx/5xx are
 *          **ok**, not err — the caller interprets `:status`.
 */
/* Forward decl — public auth helper from net/http/http_auth.h. */
typedef struct n00b_http_auth n00b_http_auth_t;

extern n00b_result_t(n00b_h3_response_t *)
n00b_http_h3_round_trip(n00b_http_url_t *url)
    _kargs {
        const char                  *method       = "GET";
        n00b_buffer_t               *body         = nullptr;
        const char                  *content_type = nullptr;
        n00b_http_h1_headers_t      *extra        = nullptr;
        int32_t                      handshake_ms = 1500;
        int32_t                      await_ms     = 30000;
        n00b_quic_trust_t           *trust        = nullptr;
        n00b_http_connection_pool_t *pool         = nullptr;
        /** Optional auth helper for mTLS handshake-time client cert
         *  presentation.  See `http_h1.h` for the matching wiring on
         *  the h1 path; same shape applies here. */
        n00b_http_auth_t            *auth         = nullptr;
        n00b_allocator_t            *allocator    = nullptr;
    };
