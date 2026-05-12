/**
 * @file http_cookies.h
 * @internal
 * @brief Cookie jar (RFC 6265-bis) for the n00b HTTP client.
 *
 * Phase 6 chunk 9 (in-memory subset).  Ships:
 *
 *   - `Set-Cookie:` parser with all common attributes
 *     (Domain, Path, Expires, Max-Age, Secure, HttpOnly, SameSite).
 *   - Per-origin/path/name jar with insertion-order tracking.
 *   - Cookie filtering for outgoing requests (domain match,
 *     path prefix, scheme/secure, expiry, host-only).
 *
 * Deferred to follow-up:
 *   - Public-suffix-list (PSL) matching to forbid super-cookies.
 *     Today's domain match treats every Domain= as if the registry
 *     allowed it; a follow-up bundles libpsl or a PSL snapshot.
 *   - SameSite=Strict / Lax / None enforcement at request time
 *     (parsed and stored; not yet consulted by the filter).
 *   - On-disk persistence — the spec's persist-by-default doesn't
 *     apply to non-browser clients, so we ship an in-memory-only
 *     jar.  An on-disk variant could be a thin wrapper.
 *
 * @see ~/dd/quic_6.md § 7 chunk 9.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "n00b.h"
#include "core/string.h"
#include "internal/net/http/http_url.h"

typedef enum : uint8_t {
    N00B_COOKIE_SAMESITE_UNSET  = 0,
    N00B_COOKIE_SAMESITE_LAX    = 1,
    N00B_COOKIE_SAMESITE_STRICT = 2,
    N00B_COOKIE_SAMESITE_NONE   = 3,
} n00b_http_cookie_samesite_t;

/**
 * @brief One parsed cookie.  Owned by a jar after insertion; the
 * parser returns these as detached values for tests + dispatcher
 * use.
 *
 * @c expires_ms  Unix-epoch milliseconds.  0 = session cookie
 *                (lives until jar is dropped).  Negative = already
 *                expired (causes deletion on insert).
 * @c host_only   True when Set-Cookie omitted Domain= — the cookie
 *                is restricted to the exact host that issued it.
 *                When false, the Domain= attribute permits
 *                subdomains.
 */
typedef struct {
    n00b_string_t              *name;
    n00b_string_t              *value;
    n00b_string_t              *domain;
    n00b_string_t              *path;
    int64_t                     expires_ms;
    bool                        secure;
    bool                        http_only;
    n00b_http_cookie_samesite_t samesite;
    bool                        host_only;
    int64_t                     creation_ms;
} n00b_http_cookie_t;

/**
 * @brief Parse one `Set-Cookie:` header value.
 *
 * @param header   Header value (may include surrounding OWS).
 * @param origin   URL of the response that delivered the header
 *                 (provides defaults for Domain + Path; required
 *                 for security-conscious scoping).
 *
 * @kw allocator Default per-runtime conduit pool.
 *
 * @return  Heap-allocated cookie on success.  nullptr if the
 *          header is unparseable, missing `name=value`, or
 *          violates a hard scoping rule (e.g. Domain attribute
 *          for an unrelated host).
 */
extern n00b_http_cookie_t *
n00b_http_cookie_parse(const char      *header,
                        n00b_http_url_t *origin)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
        /* Wall-clock now-ms for `creation_ms` + Max-Age expiry
         * derivation.  Default 0 → use real wall-clock.  Tests
         * inject a fixed value so the jar's fake-clock test hook
         * works deterministically. */
        int64_t           now_ms    = 0;
    };

typedef struct n00b_http_cookie_jar n00b_http_cookie_jar_t;

/**
 * @brief Create an empty cookie jar.
 *
 * @kw max_cookies  Total cap; oldest evicted on overflow.  Default 600.
 * @kw allocator    Default per-runtime conduit pool.
 */
extern n00b_http_cookie_jar_t *
n00b_http_cookie_jar_new()
    _kargs {
        size_t            max_cookies = 600;
        n00b_allocator_t *allocator   = nullptr;
    };

/** @brief Drop all cookies.  Idempotent on @p jar nullptr. */
extern void n00b_http_cookie_jar_close(n00b_http_cookie_jar_t *jar);

/**
 * @brief Update the jar from a response's `Set-Cookie:` headers.
 *
 * Caller iterates the response's headers; for each `Set-Cookie:`
 * line, calls this function once with the value bytes.
 */
extern void
n00b_http_cookie_jar_set_from_response(n00b_http_cookie_jar_t *jar,
                                       n00b_http_url_t        *origin,
                                       const char             *header_value);

/**
 * @brief Compose the `Cookie:` header value for an outgoing request.
 *
 * Selects all live, non-expired cookies whose (domain, path, secure)
 * match @p url, formats them as a single
 * `name1=value1; name2=value2` string.  Returns nullptr if no
 * cookies apply.
 */
extern n00b_string_t *
n00b_http_cookie_jar_header_for(n00b_http_cookie_jar_t *jar,
                                n00b_http_url_t        *url)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
    };

/** @brief Live cookie count. */
extern size_t
n00b_http_cookie_jar_size(n00b_http_cookie_jar_t *jar);

/** @brief Test hook: inject a fake clock (unix ms). */
extern void
n00b_http_cookie_jar_set_now_for_test(n00b_http_cookie_jar_t *jar,
                                      int64_t                 now_ms);
