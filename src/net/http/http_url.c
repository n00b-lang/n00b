/*
 * http_url.c — HTTPS URL parser for the n00b HTTP client.
 *
 * Phase 6, chunk 1 (~/dd/quic_6.md § 7).  Splits an absolute HTTPS
 * URL into the structured form `n00b_http_url_t` so the dispatcher
 * (chunk 5) and transport paths (chunks 2 + 3) don't each re-tokenize
 * the input.
 *
 * Conformance:
 *   - RFC 3986 § 3 for the syntax skeleton (scheme + authority + path
 *     + query); IPv6 literals via § 3.2.2's `[…]` form.
 *   - RFC 9110 § 4.2.4 — userinfo MUST NOT appear in HTTP URIs;
 *     reject explicitly.
 *   - HTTPS only: § 2.2 of the Phase 6 plan rejects `http://` here
 *     so blunders never reach the transport.
 *
 * Anything past tokenization (percent-decoding hosts, IDN ToASCII,
 * canonical path normalization) is deferred to the transport layer
 * where the input is actually about to be transmitted.
 */

#define N00B_USE_INTERNAL_API
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "n00b.h"
#include "core/string.h"
#include "core/buffer.h"
#include "adt/result.h"
#include "internal/net/http/http_url.h"

/* ----------------------------------------------------------------- */
/* Local helpers                                                     */
/* ----------------------------------------------------------------- */

static inline bool
is_digit_byte(unsigned char b)
{
    return b >= '0' && b <= '9';
}

static inline bool
is_ipv6_byte(unsigned char b)
{
    /* IPv6 literal payload between [ and ]: hex digits, ':', '.'
     * (for IPv4-mapped), and '%' for zone IDs (RFC 6874).  Anything
     * else means a malformed authority. */
    if (is_digit_byte(b)) return true;
    if (b >= 'a' && b <= 'f') return true;
    if (b >= 'A' && b <= 'F') return true;
    return b == ':' || b == '.' || b == '%';
}

static n00b_string_t *
copy_slice(const char *p, size_t len, n00b_allocator_t *allocator)
{
    return n00b_string_from_raw(p, (int64_t)len, .allocator = allocator);
}

static n00b_string_t *
lowercase_ascii_slice(const char *p, size_t len, n00b_allocator_t *allocator)
{
    /* RFC 3986 § 3.2.2: scheme + host comparisons are case-insensitive
     * on ASCII.  Hosts with non-ASCII bytes are passed through verbatim
     * (IDN handling is the transport layer's problem).
     *
     * We lowercase into a scratch buffer and then construct the
     * (immutable) n00b_string_t — never mutate a string after init. */
    char  stack[128];
    char *tmp;
    if (len < sizeof(stack)) {
        tmp = stack;
    } else {
        tmp = n00b_alloc_array(char, len, .allocator = allocator);
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        tmp[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : (char)c;
    }
    return n00b_string_from_raw(tmp, (int64_t)len, .allocator = allocator);
}

static n00b_string_t *
build_origin(n00b_string_t    *host,
             bool              is_ipv6_literal,
             uint16_t          port,
             bool              has_explicit_port,
             n00b_allocator_t *allocator)
{
    /* "https://" + maybe-bracketed host + optional :port — kept as
     * one buffer + final string copy; cheaper than n00b_string_concat
     * for a hot path the dispatcher will hit on every request. */
    char   scratch[128];
    char  *out;
    bool   omit_port = !has_explicit_port || port == 443;
    size_t brackets  = is_ipv6_literal ? 2 : 0;
    size_t port_len  = 0;
    char   port_buf[8];
    if (!omit_port) {
        int n = snprintf(port_buf, sizeof(port_buf), ":%u",
                         (unsigned)port);
        port_len = (n > 0) ? (size_t)n : 0;
    }
    size_t need = 8 /* "https://" */
                + brackets
                + host->u8_bytes
                + port_len;

    if (need < sizeof(scratch)) {
        out = scratch;
    } else {
        out = n00b_alloc_array(char, need, .allocator = allocator);
    }
    size_t off = 0;
    memcpy(out + off, "https://", 8);
    off += 8;
    if (is_ipv6_literal) {
        out[off++] = '[';
    }
    memcpy(out + off, host->data, host->u8_bytes);
    off += host->u8_bytes;
    if (is_ipv6_literal) {
        out[off++] = ']';
    }
    if (port_len) {
        memcpy(out + off, port_buf, port_len);
        off += port_len;
    }
    return n00b_string_from_raw(out, (int64_t)off, .allocator = allocator);
}

/* ----------------------------------------------------------------- */
/* Parser                                                             */
/* ----------------------------------------------------------------- */

n00b_result_t(n00b_http_url_t *)
n00b_http_url_parse(n00b_string_t *url)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
    }
{
    if (!url || !url->data) {
        return n00b_result_err(n00b_http_url_t *,
                               N00B_HTTP_ERR_NULL_ARG);
    }

    const char *p   = url->data;
    const char *end = p + url->u8_bytes;

    /* Scheme: "https://" (case-insensitive). */
    static const char SCHEME[] = "https://";
    const size_t      slen     = sizeof(SCHEME) - 1;
    if ((size_t)(end - p) < slen) {
        return n00b_result_err(n00b_http_url_t *,
                               N00B_HTTP_ERR_UNSUPPORTED_SCHEME);
    }
    for (size_t i = 0; i < slen; i++) {
        char want = SCHEME[i];
        char have = p[i];
        if (have >= 'A' && have <= 'Z') have = (char)(have + ('a' - 'A'));
        if (have != want) {
            return n00b_result_err(n00b_http_url_t *,
                                   N00B_HTTP_ERR_UNSUPPORTED_SCHEME);
        }
    }
    p += slen;

    /* Strip fragment up front: HTTP requests never send `#…` and we
     * do not want it appearing in path/query bytes. */
    const char *frag = memchr(p, '#', (size_t)(end - p));
    if (frag) {
        end = frag;
    }

    /* Authority extends to first `/`, `?`, or end (we already
     * trimmed `#`). */
    const char *auth_end = end;
    for (const char *q = p; q < end; q++) {
        if (*q == '/' || *q == '?') {
            auth_end = q;
            break;
        }
    }

    /* Userinfo check.  RFC 3986 syntactically allows `user@[::1]/`
     * but RFC 9110 § 4.2.4 forbids userinfo for HTTP URIs, so we
     * reject any `@` that appears BEFORE the optional `[` of an IPv6
     * literal.  An `@` *inside* `[...]` is a malformed host instead,
     * caught by the IPv6 validator below. */
    const char *userinfo_search_end = auth_end;
    if (p < auth_end && *p == '[') {
        userinfo_search_end = p;
    }
    if (memchr(p, '@', (size_t)(userinfo_search_end - p))) {
        return n00b_result_err(n00b_http_url_t *,
                               N00B_HTTP_ERR_USERINFO_REJECTED);
    }

    /* Host: bracketed IPv6 literal or bare. */
    bool        is_ipv6 = false;
    const char *host_start;
    const char *host_end;
    if (p < auth_end && *p == '[') {
        is_ipv6  = true;
        p++;                     /* consume `[` */
        host_start = p;
        const char *rb = memchr(p, ']', (size_t)(auth_end - p));
        if (!rb) {
            return n00b_result_err(n00b_http_url_t *,
                                   N00B_HTTP_ERR_HOST_INVALID);
        }
        host_end = rb;
        for (const char *q = host_start; q < host_end; q++) {
            if (!is_ipv6_byte((unsigned char)*q)) {
                return n00b_result_err(n00b_http_url_t *,
                                       N00B_HTTP_ERR_HOST_INVALID);
            }
        }
        p = rb + 1;              /* past `]` */
    } else {
        host_start = p;
        while (p < auth_end && *p != ':') {
            p++;
        }
        host_end = p;
    }
    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0) {
        return n00b_result_err(n00b_http_url_t *,
                               N00B_HTTP_ERR_HOST_EMPTY);
    }

    /* Optional :PORT. */
    uint16_t port              = 443;
    bool     has_explicit_port = false;
    if (p < auth_end && *p == ':') {
        p++;                     /* consume `:` */
        const char *port_start = p;
        unsigned long pv       = 0;
        while (p < auth_end && is_digit_byte((unsigned char)*p)) {
            pv = pv * 10 + (unsigned long)(*p - '0');
            if (pv > 65535) {
                return n00b_result_err(n00b_http_url_t *,
                                       N00B_HTTP_ERR_PORT_INVALID);
            }
            p++;
        }
        if (p == port_start || p != auth_end || pv == 0) {
            return n00b_result_err(n00b_http_url_t *,
                                   N00B_HTTP_ERR_PORT_INVALID);
        }
        port              = (uint16_t)pv;
        has_explicit_port = true;
    } else if (p != auth_end) {
        /* Garbage in the authority that wasn't a port. */
        return n00b_result_err(n00b_http_url_t *,
                               N00B_HTTP_ERR_INVALID_URL);
    }

    /* Path + query. */
    const char *path_start;
    size_t      path_len;
    const char *query_start = NULL;
    size_t      query_len   = 0;
    const char *q_marker    = memchr(auth_end, '?',
                                      (size_t)(end - auth_end));
    if (auth_end == end) {
        path_start = "/";
        path_len   = 1;
    } else if (*auth_end == '?') {
        path_start  = "/";
        path_len    = 1;
        query_start = auth_end + 1;
        query_len   = (size_t)(end - query_start);
    } else {
        path_start = auth_end;          /* points at the leading `/` */
        if (q_marker) {
            path_len    = (size_t)(q_marker - path_start);
            query_start = q_marker + 1;
            query_len   = (size_t)(end - query_start);
        } else {
            path_len = (size_t)(end - path_start);
        }
    }

    /* Build result. */
    n00b_http_url_t *u = n00b_alloc(n00b_http_url_t,
                                    .allocator = allocator);
    u->scheme            = lowercase_ascii_slice("https", 5, allocator);
    u->host              = lowercase_ascii_slice(host_start, host_len,
                                                 allocator);
    u->is_ipv6_literal   = is_ipv6;
    u->port              = port;
    u->has_explicit_port = has_explicit_port;
    u->path              = copy_slice(path_start, path_len, allocator);
    u->query             = query_start
                              ? copy_slice(query_start, query_len, allocator)
                              : n00b_string_empty(.allocator = allocator);
    u->origin            = build_origin(u->host,
                                        is_ipv6,
                                        port,
                                        has_explicit_port,
                                        allocator);
    return n00b_result_ok(n00b_http_url_t *, u);
}

const char *
n00b_http_err_str(n00b_http_err_t err)
{
    switch (err) {
    case N00B_HTTP_OK:                     return "ok";
    case N00B_HTTP_ERR_NULL_ARG:           return "null argument";
    case N00B_HTTP_ERR_INVALID_URL:        return "invalid URL";
    case N00B_HTTP_ERR_UNSUPPORTED_SCHEME: return "unsupported scheme (https only)";
    case N00B_HTTP_ERR_USERINFO_REJECTED:  return "userinfo not allowed in HTTP URI";
    case N00B_HTTP_ERR_HOST_EMPTY:         return "empty host";
    case N00B_HTTP_ERR_HOST_INVALID:       return "invalid host";
    case N00B_HTTP_ERR_PORT_INVALID:       return "invalid port";
    case N00B_HTTP_ERR_BAD_RESPONSE:       return "malformed HTTP response";
    }
    return "unknown error";
}
