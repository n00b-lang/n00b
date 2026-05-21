/*
 * http_client.c — Public HTTP client dispatcher.
 *
 * Phase 6 chunk 5 (sub-chunk 5a — synchronous form).  Wires the
 * URL parser (chunk 1), h1 transport (chunk 2), h3 transport
 * (chunk 3), and the future connection pool (chunk 4) into a
 * single public entry point.
 *
 * Layout:
 *   §1   Per-runtime state (loss cache only at this revision)
 *   §2   Internal response struct + accessors
 *   §3   Race + fallback dispatcher
 *   §4   Synchronous public entry point
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/time.h"
#include "core/thread.h"
#include "adt/result.h"
#include "conduit/conduit.h"
#include "conduit/topic.h"
#include "conduit/service.h"
#include "net/quic/quic_types.h"
#include "net/quic/h3.h"
#include "net/http/http_client.h"
#include "net/http/http_auth.h"
#include "internal/net/http/http_url.h"
#include "internal/net/http/http_h1.h"
#include "internal/net/http/http_h3.h"
#include "internal/net/http/http_pool.h"
#include "internal/net/http/http_cookies.h"
#include "internal/net/http/http_compression.h"
#include "internal/net/http/http_client.h"
#include "text/unicode/idna.h"

/* ===========================================================================
 * §1   Per-runtime state
 *
 * Loss cache: per-origin sticky note that says "h3 didn't work here
 * recently, skip it for `LOSS_CACHE_TTL_MS` and go straight to h1."
 * Stored as a small open-addressed table — typical operator workload
 * has < 100 distinct origins.
 *
 * The cache is process-global state, but we store it via a static
 * pointer that's allocated lazily from the runtime's conduit pool.
 * That keeps the user's "no globals; per-runtime only" rule honest:
 * the bookkeeping is owned by the runtime's allocator, not by libc
 * static memory.
 * =========================================================================== */

#define LOSS_CACHE_TTL_MS  (5 * 60 * 1000ULL)   /* 5 minutes */
#define LOSS_CACHE_SLOTS   64

typedef struct {
    n00b_string_t *origin;        /* nullptr = empty slot */
    uint64_t       expires_at_ms;
} loss_slot_t;

typedef struct {
    loss_slot_t slots[LOSS_CACHE_SLOTS];
} loss_cache_t;

static _Atomic(loss_cache_t *) g_loss_cache = nullptr;

static loss_cache_t *
loss_cache_get(void)
{
    loss_cache_t *cur = atomic_load_explicit(&g_loss_cache,
                                              memory_order_acquire);
    if (cur) return cur;
    n00b_allocator_t *cp =
        (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
    loss_cache_t *fresh = n00b_alloc_with_opts(
        loss_cache_t,
        &(n00b_alloc_opts_t){.allocator = cp});
    loss_cache_t *expected = nullptr;
    if (!atomic_compare_exchange_strong_explicit(
            &g_loss_cache, &expected, fresh,
            memory_order_acq_rel, memory_order_acquire)) {
        return expected;
    }
    return fresh;
}

static uint32_t
loss_hash(n00b_string_t *origin)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < origin->u8_bytes; i++) {
        h ^= (unsigned char)origin->data[i];
        h *= 16777619u;
    }
    return h;
}

static bool
loss_cache_h3_blocked(n00b_string_t *origin)
{
    loss_cache_t *cache = atomic_load_explicit(&g_loss_cache,
                                                memory_order_acquire);
    if (!cache) return false;
    uint64_t now = n00b_ns_timestamp() / 1000000ULL;
    uint32_t idx = loss_hash(origin) & (LOSS_CACHE_SLOTS - 1);
    for (uint32_t probe = 0; probe < LOSS_CACHE_SLOTS; probe++) {
        loss_slot_t *s = &cache->slots[(idx + probe) & (LOSS_CACHE_SLOTS - 1)];
        if (!s->origin) return false;
        if (s->origin->u8_bytes == origin->u8_bytes
            && memcmp(s->origin->data, origin->data,
                      origin->u8_bytes) == 0) {
            return now < s->expires_at_ms;
        }
    }
    return false;
}

static void
loss_cache_record(n00b_string_t *origin)
{
    loss_cache_t *cache = loss_cache_get();
    uint64_t now    = n00b_ns_timestamp() / 1000000ULL;
    uint64_t expire = now + LOSS_CACHE_TTL_MS;
    uint32_t idx    = loss_hash(origin) & (LOSS_CACHE_SLOTS - 1);
    /* Find existing entry or the first expired slot. */
    int32_t free_slot = -1;
    for (uint32_t probe = 0; probe < LOSS_CACHE_SLOTS; probe++) {
        uint32_t at = (idx + probe) & (LOSS_CACHE_SLOTS - 1);
        loss_slot_t *s = &cache->slots[at];
        if (!s->origin) {
            if (free_slot < 0) free_slot = (int32_t)at;
            break;
        }
        if (s->origin->u8_bytes == origin->u8_bytes
            && memcmp(s->origin->data, origin->data,
                      origin->u8_bytes) == 0) {
            s->expires_at_ms = expire;
            return;
        }
        if (s->expires_at_ms < now && free_slot < 0) {
            free_slot = (int32_t)at;
        }
    }
    if (free_slot < 0) {
        /* Cache full of live entries — clobber the bucket head.  Some
         * collateral damage on a tiny cache is fine; entries are
         * advisory. */
        free_slot = (int32_t)idx;
    }
    cache->slots[free_slot].origin       = origin;
    cache->slots[free_slot].expires_at_ms = expire;
}

void
n00b_http_loss_cache_reset(void)
{
    loss_cache_t *cache = atomic_load_explicit(&g_loss_cache,
                                                memory_order_acquire);
    if (!cache) return;
    memset(cache->slots, 0, sizeof(cache->slots));
}

/* ===========================================================================
 * §2   Response struct + accessors
 * =========================================================================== */

typedef struct {
    n00b_string_t *name;
    n00b_buffer_t *value;
} resp_header_t;

struct n00b_http_response {
    int                    status;
    n00b_buffer_t         *body;
    resp_header_t         *headers;
    size_t                 n_headers;
    n00b_http_transport_t  transport;
    int32_t                error;     /**< Transport-level err; 0 on success. */
    n00b_allocator_t      *allocator;
};

int
n00b_http_response_status(n00b_http_response_t *resp)
{
    return resp ? resp->status : 0;
}

n00b_buffer_t *
n00b_http_response_body(n00b_http_response_t *resp)
{
    return resp ? resp->body : nullptr;
}

n00b_http_transport_t
n00b_http_response_transport(n00b_http_response_t *resp)
{
    return resp ? resp->transport : N00B_HTTP_TRANSPORT_UNKNOWN;
}

int32_t
n00b_http_response_error(n00b_http_response_t *resp)
{
    return resp ? resp->error : N00B_HTTP_ERR_NULL_ARG;
}

size_t
n00b_http_response_n_headers(n00b_http_response_t *resp)
{
    return resp ? resp->n_headers : 0;
}

bool
n00b_http_response_header_at(n00b_http_response_t *resp,
                              size_t                idx,
                              n00b_string_t       **name_out,
                              n00b_buffer_t       **value_out)
{
    if (!resp || idx >= resp->n_headers) return false;
    if (name_out)  *name_out  = resp->headers[idx].name;
    if (value_out) *value_out = resp->headers[idx].value;
    return true;
}

n00b_buffer_t *
n00b_http_response_header(n00b_http_response_t *resp, n00b_string_t *name)
{
    if (!resp || !name) return nullptr;
    for (size_t i = 0; i < resp->n_headers; i++) {
        n00b_string_t *n = resp->headers[i].name;
        if (n->u8_bytes == name->u8_bytes
            && strncasecmp(n->data, name->data, n->u8_bytes) == 0) {
            return resp->headers[i].value;
        }
    }
    return nullptr;
}

const char *
n00b_http_response_header_cstr(n00b_http_response_t *resp, const char *name)
{
    if (!resp || !name) return nullptr;
    size_t nlen = strlen(name);
    for (size_t i = 0; i < resp->n_headers; i++) {
        n00b_string_t *hn = resp->headers[i].name;
        if (hn->u8_bytes != nlen) continue;
        if (strncasecmp(hn->data, name, nlen) != 0) continue;
        n00b_buffer_t *val = resp->headers[i].value;
        if (!val) return nullptr;
        size_t vlen = (size_t)val->byte_len;
        char  *cstr = n00b_alloc_array(char, vlen + 1,
                                        .allocator = resp->allocator);
        memcpy(cstr, val->data, vlen);
        cstr[vlen] = '\0';
        return cstr;
    }
    return nullptr;
}

/* ===========================================================================
 * §3   Response building
 *
 * Adapts h1 + h3 internal response shapes to the public
 * n00b_http_response_t.
 * =========================================================================== */

static n00b_http_response_t *
build_from_h1(n00b_http_h1_response_t *h1,
              n00b_allocator_t        *a)
{
    n00b_http_response_t *r = n00b_alloc_with_opts(
        n00b_http_response_t,
        &(n00b_alloc_opts_t){.allocator = a});
    r->status    = h1->status;
    r->body      = h1->body;
    r->transport = N00B_HTTP_TRANSPORT_H1;
    r->error     = 0;
    r->allocator = a;

    size_t n = n00b_http_h1_headers_len(h1->headers);
    r->headers = (n > 0)
        ? n00b_alloc_array(resp_header_t, n, .allocator = a)
        : nullptr;
    r->n_headers = n;
    for (size_t i = 0; i < n; i++) {
        const char *name;
        const char *value;
        if (!n00b_http_h1_headers_at(h1->headers, i, &name, &value)) {
            continue;
        }
        r->headers[i].name  = n00b_string_from_cstr((char *)name,
                                                     .allocator = a);
        r->headers[i].value = n00b_buffer_from_bytes((char *)value,
                                                      (int64_t)strlen(value),
                                                      .allocator = a);
    }
    return r;
}

static n00b_http_response_t *
build_from_h3(n00b_h3_response_t *h3,
              n00b_allocator_t   *a)
{
    n00b_http_response_t *r = n00b_alloc_with_opts(
        n00b_http_response_t,
        &(n00b_alloc_opts_t){.allocator = a});
    r->status    = (int)h3->status;
    r->body      = h3->body;
    r->transport = N00B_HTTP_TRANSPORT_H3;
    r->error     = 0;
    r->allocator = a;
    r->n_headers = h3->n_headers;
    r->headers   = (h3->n_headers > 0)
        ? n00b_alloc_array(resp_header_t, h3->n_headers, .allocator = a)
        : nullptr;
    for (size_t i = 0; i < h3->n_headers; i++) {
        r->headers[i].name = n00b_string_from_raw(
            (const char *)h3->headers[i].name,
            (int64_t)h3->headers[i].name_len, .allocator = a);
        r->headers[i].value = n00b_buffer_from_bytes(
            (char *)h3->headers[i].value,
            (int64_t)h3->headers[i].value_len, .allocator = a);
    }
    return r;
}

/* ===========================================================================
 * §4   Race + fallback dispatcher
 *
 * Sequential per ~/dd/quic_6.md § 5.  H3 is attempted first when
 * `prefer_h3` is set AND the loss cache says the origin's h3 path
 * is healthy; on h3 failure we fall through to h1 and record the
 * loss.  The plan's deferred Happy-Eyeballs-style parallel race is
 * a Phase-7 candidate.
 * =========================================================================== */

static const char *
method_cstr(n00b_string_t *method)
{
    return (method && method->u8_bytes) ? method->data : "GET";
}

static const char *
content_type_cstr(n00b_string_t *ct)
{
    return (ct && ct->u8_bytes) ? ct->data : nullptr;
}

/* RFC 9110 § 15.4: 301/302/303 collapse the method to GET (drop the
 * body), 307/308 preserve the original method + body. */
bool
n00b_http_status_is_redirect(int s)
{
    return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
}

bool
n00b_http_status_preserves_method(int s)
{
    return s == 307 || s == 308;
}

/* Local convenience aliases — the public names are too long for the
 * tight loop body. */
#define status_is_redirect       n00b_http_status_is_redirect
#define status_preserves_method  n00b_http_status_preserves_method

/* Resolve a Location header against the current URL per RFC 3986
 * § 5 (transform-references algorithm).  Handles:
 *
 *   - Absolute URI    `https://...`            (we still require https)
 *   - Network-path    `//host/foo`             (scheme inherited)
 *   - Absolute-path   `/foo`                   (origin inherited)
 *   - Relative-path   `foo` / `../bar`         (path merged + dot-removed)
 *   - Empty / fragment-only `#frag` / `?q=1`   (path inherited)
 *
 * The dot-segment removal (§ 5.2.4) collapses `./` and `../`
 * segments deterministically; `..` past the root is clamped (RFC
 * § 5.2.4 step 2C/D). */

static char *
remove_dot_segments(const char *path,
                    size_t      len,
                    n00b_allocator_t *a,
                    size_t          *out_len)
{
    /* Per RFC 3986 § 5.2.4: scratch buffer at most as long as input. */
    char  *out = n00b_alloc_array(char, len + 1, .allocator = a);
    size_t in  = 0;
    size_t op  = 0;

    while (in < len) {
        /* "../" / "./" prefix → strip. */
        if (in + 2 < len + 1 && path[in] == '.' && path[in + 1] == '.'
            && in + 2 < len && path[in + 2] == '/') {
            in += 3;
            continue;
        }
        if (in + 1 < len + 1 && path[in] == '.'
            && in + 1 < len && path[in + 1] == '/') {
            in += 2;
            continue;
        }
        /* "/./" → "/". */
        if (in + 2 < len && path[in] == '/' && path[in + 1] == '.'
            && path[in + 2] == '/') {
            in += 2;
            continue;
        }
        /* "/." at end → "/". */
        if (in + 1 < len + 1 && path[in] == '/'
            && in + 1 < len && path[in + 1] == '.'
            && (in + 2 == len)) {
            out[op++] = '/';
            in        = len;
            continue;
        }
        /* "/../" — pop last segment from out, then "/" remains. */
        if (in + 3 < len && path[in] == '/' && path[in + 1] == '.'
            && path[in + 2] == '.' && path[in + 3] == '/') {
            in += 3;
            /* Pop last segment from out (back up to last '/'). */
            while (op > 0 && out[op - 1] != '/') op--;
            if (op > 0) op--;       /* drop trailing '/' too */
            continue;
        }
        /* "/.." at end → push '/'. */
        if (in + 2 < len + 1 && path[in] == '/' && path[in + 1] == '.'
            && in + 2 < len && path[in + 2] == '.'
            && (in + 3 == len)) {
            in += 2;
            while (op > 0 && out[op - 1] != '/') op--;
            if (op > 0) op--;
            out[op++] = '/';
            continue;
        }
        /* Standalone "." or ".." input — drop entirely. */
        if (len - in == 1 && path[in] == '.') break;
        if (len - in == 2 && path[in] == '.' && path[in + 1] == '.') break;

        /* Copy the next path segment up to (and including) the next '/'. */
        if (path[in] == '/') {
            out[op++] = '/';
            in++;
        }
        while (in < len && path[in] != '/') {
            out[op++] = path[in++];
        }
    }
    out[op] = '\0';
    *out_len = op;
    return out;
}

static n00b_string_t *
resolve_location(n00b_http_url_t  *current,
                 n00b_buffer_t    *location,
                 n00b_allocator_t *a)
{
    if (!location || location->byte_len == 0) return nullptr;
    const char *raw = location->data;
    size_t      len = (size_t)location->byte_len;
    /* Strip trailing CR/LF if any made it into the buffer. */
    while (len > 0 && (raw[len - 1] == '\r' || raw[len - 1] == '\n')) {
        len--;
    }
    if (len == 0) return nullptr;

    /* Strip a fragment (`#…`) — RFC 3986 § 5.3 says the resolver's
     * output preserves it, but HTTP requests never carry one so we
     * drop it. */
    const char *frag = memchr(raw, '#', len);
    if (frag) len = (size_t)(frag - raw);
    if (len == 0) {
        /* Fragment-only ref: target = current URL. */
        return n00b_string_from_raw(current->origin->data,
                                     (int64_t)current->origin->u8_bytes,
                                     .allocator = a);
    }

    /* (a) Absolute URI: starts with "scheme:".  We only honor https. */
    bool has_scheme = false;
    for (size_t i = 0; i < len; i++) {
        char c = raw[i];
        if (c == ':') { has_scheme = (i > 0); break; }
        if (i == 0 && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            break;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) {
            break;
        }
    }
    if (has_scheme) {
        /* Reject if not https — the dispatcher only speaks https. */
        if (len < 8
            || (raw[0] != 'h' && raw[0] != 'H')
            || (raw[1] != 't' && raw[1] != 'T')
            || (raw[2] != 't' && raw[2] != 'T')
            || (raw[3] != 'p' && raw[3] != 'P')
            || (raw[4] != 's' && raw[4] != 'S')
            || raw[5] != ':' || raw[6] != '/' || raw[7] != '/') {
            return nullptr;
        }
        return n00b_string_from_raw(raw, (int64_t)len, .allocator = a);
    }

    /* (b) Network-path: starts with "//".  Inherit scheme (https). */
    if (len >= 2 && raw[0] == '/' && raw[1] == '/') {
        size_t total = 6 + len;          /* "https:" + // + rest */
        char  *buf   = n00b_alloc_array(char, total + 1, .allocator = a);
        memcpy(buf, "https:", 6);
        memcpy(buf + 6, raw, len);
        buf[total] = '\0';
        return n00b_string_from_raw(buf, (int64_t)total, .allocator = a);
    }

    /* (c) Absolute path: starts with "/".  Inherit origin. */
    if (raw[0] == '/') {
        size_t plen;
        char  *cleaned = remove_dot_segments(raw, len, a, &plen);
        size_t total   = current->origin->u8_bytes + plen;
        char  *buf     = n00b_alloc_array(char, total + 1, .allocator = a);
        memcpy(buf, current->origin->data, current->origin->u8_bytes);
        memcpy(buf + current->origin->u8_bytes, cleaned, plen);
        buf[total] = '\0';
        return n00b_string_from_raw(buf, (int64_t)total, .allocator = a);
    }

    /* (d) Relative path: merge with current's path's directory. */
    const char *cur_path = (current->path && current->path->u8_bytes)
                                ? current->path->data : "/";
    size_t      cur_len  = (current->path && current->path->u8_bytes)
                                ? (size_t)current->path->u8_bytes : 1;
    /* Find last '/' in current path; everything up to and including
     * that becomes the merge prefix.  RFC 3986 § 5.2.3. */
    size_t last_slash = 0;
    for (size_t i = 0; i < cur_len; i++) {
        if (cur_path[i] == '/') last_slash = i + 1;
    }
    if (last_slash == 0) last_slash = 1;   /* at least "/" */
    size_t merged_len = last_slash + len;
    char  *merged     = n00b_alloc_array(char, merged_len + 1,
                                          .allocator = a);
    memcpy(merged, cur_path, last_slash);
    memcpy(merged + last_slash, raw, len);
    merged[merged_len] = '\0';

    size_t plen;
    char  *cleaned = remove_dot_segments(merged, merged_len, a, &plen);
    size_t total   = current->origin->u8_bytes + plen;
    char  *buf     = n00b_alloc_array(char, total + 1, .allocator = a);
    memcpy(buf, current->origin->data, current->origin->u8_bytes);
    memcpy(buf + current->origin->u8_bytes, cleaned, plen);
    buf[total] = '\0';
    return n00b_string_from_raw(buf, (int64_t)total, .allocator = a);
}

/* ASCII-case-insensitive byte compare of two host strings.  Host
 * comparison for the redirect allowlist is intentionally
 * structural — we compare against the URL's authority host (no
 * port, no path, no fragment) so callers don't have to encode the
 * port to permit a host running on a non-default port. */
static bool
host_eq_ascii_ci(const char *a, size_t alen,
                 const char *b, size_t blen)
{
    if (alen != blen) return false;
    for (size_t i = 0; i < alen; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

/* IDNA / UTS-46 canonicalization of a domain string for allowlist
 * matching.  Collapses every failure mode into a single "skip"
 * signal: returns `nullptr` iff
 *
 *   - input is null or empty (callers don't pass empty here; this is
 *     defensive against pathological allowlist entries), OR
 *   - `n00b_unicode_idna_to_ascii` reports any non-OK error code
 *     (`_DISALLOWED`, `_BIDI_ERROR`, `_PUNYCODE_ERROR`, `_LABEL_TOO_LONG`,
 *     `_DOMAIN_TOO_LONG`, `_CONTEXTJ_ERROR`, `_CONTEXTO_ERROR`,
 *     `_LEADING_COMBINING`, `_EMPTY_LABEL`, `_INVALID_ACE`,
 *     `_PROCESSING_ERROR`).
 *
 * On success returns the ACE form (pure-ASCII Punycode where
 * needed, byte-identical for already-ASCII inputs modulo
 * UTS-46 case folding).
 *
 * The result is transient — allocated on the runtime default
 * allocator, immediately consumed by a byte compare, then dropped
 * to the GC.  The matcher's documented "side-effect-free" contract
 * is preserved at the API boundary; transient internal allocations
 * are within scope per § 4.4 (the GC reclaims them).
 *
 * Contract note: as of DF-Y, `n00b_unicode_idna_to_ascii` returns
 * `_PROCESSING_ERROR` (not OK + empty) on invalid-UTF-8 input, so
 * the matcher no longer needs to second-guess an OK return with an
 * empty `u8_bytes`. */
static n00b_string_t *
idna_canonicalize_or_null(const char *src, size_t src_len)
{
    if (!src || src_len == 0) return nullptr;

    n00b_string_t *in = n00b_string_from_raw(src, (int64_t)src_len);
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(in);
    if (r.error != N00B_UNICODE_IDNA_OK) return nullptr;
    if (!r.value) return nullptr;
    return r.value;
}

/* Returns true iff @p host (already lowercased by the URL parser
 * per RFC 3986 § 3.2.2, but NOT yet IDNA-canonicalized) is matched
 * by some entry in @p allowlist.  Both sides are run through UTS-46
 * `to_ascii` so Unicode and Punycode forms compare equal in either
 * direction.
 *
 * Two entry shapes are recognized:
 *
 *   - Exact entries (no `*` anywhere): canonicalize the whole
 *     entry, then ASCII-CI byte-compare against the canonicalized
 *     host.  Pure-ASCII entries land at the same bytes they would
 *     have hit pre-IDNA (ASCII is a fixed point of `to_ascii`
 *     modulo case folding), preserving the cand-#6 contract
 *     byte-identically.
 *
 *   - Wildcard entries (`*.DOMAIN` with at least one label after
 *     the dot, e.g. `*.example.com` or `*.例え.com`): classify on
 *     the RAW bytes (the `*.` prefix is literal ASCII and must NOT
 *     be fed to the IDNA pipeline — it would reject `*` as
 *     disallowed), then canonicalize only the `DOMAIN` portion
 *     after the `*.`.  Match any host of the form `X.DOMAIN` for
 *     some non-empty `X` after both sides are canonicalized.  The
 *     apex `DOMAIN` itself is NOT matched.
 *
 *   - Any other `*` placement (`foo.*.com`, `**.example.com`,
 *     `*example.com`, bare `*`) is malformed and silently skipped.
 *
 * IDNA-error handling:
 *
 *   - Host canonicalization failure → return false immediately.  A
 *     host that cannot be IDNA-canonicalized cannot belong to any
 *     allowlist.  In practice the URL parser will not produce one;
 *     this is defensive.
 *   - Entry canonicalization failure (exact-side or DOMAIN-portion)
 *     → skip that entry, continue scanning the rest of the list.
 *     Same skip-as-malformed semantics as cand #6.
 *
 * An empty allowlist (0 entries) returns false — "no hosts
 * permitted".  A nullptr allowlist is *not* passed here; the
 * caller checks `redirect_host_allowlist != nullptr` before
 * calling this. */
bool
host_in_allowlist(n00b_string_t                *host,
                  n00b_list_t(n00b_string_t *) *allowlist)
{
    if (!host || !allowlist) return false;

    /* Canonicalize the host once.  Any failure → no allowlist
     * entry can match. */
    n00b_string_t *chost = idna_canonicalize_or_null(host->data,
                                                     host->u8_bytes);
    if (!chost) return false;
    const char *hdat = chost->data;
    size_t      hlen = chost->u8_bytes;

    /* Lists are value types; the kwarg is a pointer to the
     * caller's lvalue.  The list macros expect lvalue access so we
     * dereference at every call site. */
    size_t n = n00b_list_len(*allowlist);
    for (size_t i = 0; i < n; i++) {
        n00b_string_t *entry = n00b_list_get(*allowlist, i);
        if (!entry) continue;
        size_t      elen = entry->u8_bytes;
        const char *edat = entry->data;

        /* Classify on the raw entry bytes: scan for any `*` byte. */
        bool   has_star      = false;
        size_t first_star_at = 0;
        for (size_t k = 0; k < elen; k++) {
            if (edat[k] == '*') {
                has_star      = true;
                first_star_at = k;
                break;
            }
        }

        if (!has_star) {
            /* Exact path — canonicalize the whole entry, then
             * ASCII-CI compare against the canonical host. */
            n00b_string_t *centry = idna_canonicalize_or_null(edat, elen);
            if (!centry) continue;
            if (host_eq_ascii_ci(hdat, hlen,
                                  centry->data, centry->u8_bytes)) {
                return true;
            }
            continue;
        }

        /* Wildcard candidate.  Accept iff:
         *   - the first (and only) `*` is at offset 0,
         *   - byte 1 is `.`,
         *   - there is at least one more byte after the dot
         *     (the DOMAIN part is non-empty),
         *   - no further `*` appears in the entry. */
        if (first_star_at != 0) continue;          /* e.g. `foo.*.com` */
        if (elen < 3)            continue;          /* `*` or `*.`   */
        if (edat[1] != '.')      continue;          /* e.g. `*example.com` */

        bool further_star = false;
        for (size_t k = 1; k < elen; k++) {
            if (edat[k] == '*') {
                further_star = true;
                break;
            }
        }
        if (further_star) continue;                 /* e.g. `**.example.com` */

        /* Canonicalize the DOMAIN portion only — the `*.` prefix
         * is literal ASCII and would be rejected by the IDNA
         * pipeline.  `domain` is everything after the `.`. */
        const char *domain     = edat + 2;
        size_t      domain_len = elen - 2;
        n00b_string_t *cdomain = idna_canonicalize_or_null(domain,
                                                           domain_len);
        if (!cdomain) continue;

        /* Form the canonical suffix `.<canonical-DOMAIN>` in a
         * tiny stack buffer.  Canonical ASCII domains cap at 253
         * bytes per IDNA (`_DOMAIN_TOO_LONG` rejects above that),
         * so 256 is sufficient. */
        char   suffix_buf[260];
        size_t cdomain_len = cdomain->u8_bytes;
        if (cdomain_len + 1 > sizeof(suffix_buf)) continue;
        suffix_buf[0] = '.';
        memcpy(suffix_buf + 1, cdomain->data, cdomain_len);
        const char *suffix     = suffix_buf;
        size_t      suffix_len = cdomain_len + 1;

        /* Strict-greater length + ASCII-CI tail match.  The strict
         * inequality guarantees at least one wildcarded-label byte
         * sits before the leading `.` in `suffix`, so the `.` acts
         * as the label boundary without an explicit leading-byte
         * check.  Punycode-encoded labels are pure ASCII, so the
         * invariant survives canonicalization unchanged. */
        if (hlen <= suffix_len) continue;
        const char *tail = hdat + (hlen - suffix_len);
        if (host_eq_ascii_ci(tail, suffix_len, suffix, suffix_len)) {
            return true;
        }
    }
    return false;
}

/* Single-shot dispatch (no redirect follow).  Factored out so the
 * redirect loop can call it repeatedly with adjusted args.
 *
 * `max_body_size` is threaded into both transports so the caller's
 * per-call body cap (DF-014) is enforced before the body
 * materializes past the limit.  0 = no cap, preserving existing
 * behavior for callers who don't pass the kwarg. */
static n00b_result_t(n00b_http_response_t *)
dispatch_once(n00b_http_url_t             *u,
              const char                  *method_str,
              n00b_buffer_t               *body,
              const char                  *ct_str,
              n00b_http_h1_headers_t      *extra,
              bool                         prefer_h3,
              int32_t                      h3_handshake_ms,
              int32_t                      timeout_ms,
              n00b_quic_trust_t           *trust,
              n00b_http_connection_pool_t *pool,
              n00b_http_auth_t            *auth,
              uint64_t                     max_body_size,
              n00b_allocator_t            *a)
{
    if (prefer_h3 && !loss_cache_h3_blocked(u->origin)) {
        auto rr = n00b_http_h3_round_trip(
            u,
            .method        = method_str,
            .body          = body,
            .content_type  = ct_str,
            .extra         = extra,
            .handshake_ms  = h3_handshake_ms,
            .await_ms      = timeout_ms,
            .trust         = trust,
            .pool          = pool,
            .auth          = auth,
            .max_body_size = max_body_size,
            .allocator     = a);
        if (n00b_result_is_ok(rr)) {
            return n00b_result_ok(n00b_http_response_t *,
                                   build_from_h3(n00b_result_get(rr), a));
        }
        /* An over-cap response on the h3 path is a policy verdict,
         * not an h3-transport failure — don't poison the loss cache
         * (the h1 path would hit the same cap).  Surface the error
         * directly so callers see a single, distinguishable code. */
        int32_t h3_err = (int32_t)n00b_result_get_err(rr);
        if (h3_err == N00B_HTTP_ERR_RESPONSE_TOO_LARGE) {
            return n00b_result_err(n00b_http_response_t *, h3_err);
        }
        loss_cache_record(u->origin);
    }
    auto rr1 = n00b_http_h1_round_trip(
        u,
        .method        = method_str,
        .body          = body,
        .content_type  = ct_str,
        .extra         = extra,
        .timeout_ms    = timeout_ms,
        .pool          = pool,
        .auth          = auth,
        .trust         = trust,
        .max_body_size = max_body_size,
        .allocator     = a);
    if (n00b_result_is_err(rr1)) {
        return n00b_result_err(n00b_http_response_t *,
                                n00b_result_get_err(rr1));
    }
    return n00b_result_ok(n00b_http_response_t *,
                           build_from_h1(n00b_result_get(rr1), a));
}

n00b_result_t(n00b_http_response_t *)
n00b_http_request_sync(n00b_string_t *url)
    _kargs {
        n00b_string_t          *method            = nullptr;
        n00b_buffer_t          *body              = nullptr;
        n00b_string_t          *content_type      = nullptr;
        n00b_http_h1_headers_t *extra             = nullptr;
        bool                    prefer_h3         = true;
        int32_t                 h3_handshake_ms   = 1500;
        int32_t                 timeout_ms        = 30000;
        n00b_quic_trust_t      *trust             = nullptr;
        bool                    follow_redirects  = false;
        int32_t                 max_redirects     = 5;
        bool                    auto_decompress   = true;
        n00b_string_t          *body_encoding     = nullptr;
        n00b_http_cookie_jar_t *cookie_jar        = nullptr;
        n00b_http_auth_t       *auth              = nullptr;
        n00b_http_connection_pool_t *pool         = nullptr;
        uint64_t                max_body_size     = 0;
        n00b_list_t(n00b_string_t *) *redirect_host_allowlist = nullptr;
        n00b_allocator_t       *allocator         = nullptr;
    }
{
    if (!url) {
        return n00b_result_err(n00b_http_response_t *,
                               N00B_HTTP_ERR_NULL_ARG);
    }
    n00b_allocator_t *a = allocator
        ? allocator
        : (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;

    /* Default to the per-runtime connection pool when the caller
     * did not pass an explicit one. */
    if (!pool) {
        pool = n00b_http_get_connection_pool();
    }

    n00b_string_t *cur_url     = url;
    n00b_string_t *cur_method  = method;
    n00b_buffer_t *cur_body    = body;
    n00b_string_t *cur_ct      = content_type;

    /* Apply request-body compression up front when requested.  We
     * compress once before any redirect loop iterations so that
     * 307/308 redirects send the same compressed bytes. */
    if (body_encoding && body_encoding->u8_bytes > 0 && cur_body) {
        auto cr = n00b_http_compress(cur_body, body_encoding->data,
                                       .allocator = a);
        if (n00b_result_is_err(cr)) {
            return n00b_result_err(n00b_http_response_t *,
                                    n00b_result_get_err(cr));
        }
        cur_body = n00b_result_get(cr);
    }

    int32_t hops = 0;
    while (true) {
        auto ur = n00b_http_url_parse(cur_url, .allocator = a);
        if (n00b_result_is_err(ur)) {
            return n00b_result_err(n00b_http_response_t *,
                                    n00b_result_get_err(ur));
        }
        n00b_http_url_t *u = n00b_result_get(ur);

        /* Build the outgoing-headers bag.  Order matters:
         *   1. Auth helper drops Bearer + DPoP onto a fresh bag.
         *   2. Cookie jar's Cookie: header.
         *   3. Accept-Encoding (when auto_decompress).
         *   4. Caller's `extra` overlays last so explicit values win.
         */
        n00b_http_h1_headers_t *headers = n00b_http_auth_apply(
            auth, nullptr, method_cstr(cur_method), u, .allocator = a);

        if (cookie_jar) {
            n00b_string_t *cookie = n00b_http_cookie_jar_header_for(
                cookie_jar, u, .allocator = a);
            if (cookie) {
                n00b_http_h1_headers_set(headers, "Cookie", cookie->data);
            }
        }
        if (auto_decompress) {
            n00b_string_t *ae = n00b_http_accept_encoding_header(
                .allocator = a);
            if (ae) {
                n00b_http_h1_headers_set(headers, "Accept-Encoding",
                                          ae->data);
            }
        }
        if (body_encoding && body_encoding->u8_bytes > 0 && cur_body) {
            /* Compressed body — advertise the encoding so the server
             * decodes correctly. */
            n00b_http_h1_headers_set(headers, "Content-Encoding",
                                      body_encoding->data);
        }
        if (extra) {
            size_t n = n00b_http_h1_headers_len(extra);
            for (size_t i = 0; i < n; i++) {
                const char *name;
                const char *value;
                if (n00b_http_h1_headers_at(extra, i, &name, &value)) {
                    n00b_http_h1_headers_set(headers, name, value);
                }
            }
        }

        auto rr = dispatch_once(u,
                                 method_cstr(cur_method),
                                 cur_body,
                                 content_type_cstr(cur_ct),
                                 headers,
                                 prefer_h3,
                                 h3_handshake_ms,
                                 timeout_ms,
                                 trust,
                                 pool,
                                 auth,
                                 max_body_size,
                                 a);
        if (n00b_result_is_err(rr)) return rr;
        n00b_http_response_t *resp = n00b_result_get(rr);

        /* Update the cookie jar from any Set-Cookie headers.
         * We iterate every response header (case-insensitive name
         * match) so we capture all Set-Cookie lines, not just the
         * last one. */
        if (cookie_jar) {
            for (size_t i = 0; i < resp->n_headers; i++) {
                n00b_string_t *name  = resp->headers[i].name;
                n00b_buffer_t *value = resp->headers[i].value;
                if (!name || !value) continue;
                if (name->u8_bytes != 10) continue;
                if (strncasecmp(name->data, "set-cookie", 10) != 0) {
                    continue;
                }
                /* value is a buffer; copy to NUL-terminated for the
                 * parser. */
                char *cstr = n00b_alloc_array(char,
                                               (size_t)value->byte_len + 1,
                                               .allocator = a);
                memcpy(cstr, value->data, (size_t)value->byte_len);
                cstr[value->byte_len] = '\0';
                n00b_http_cookie_jar_set_from_response(cookie_jar, u, cstr);
            }
        }

        /* Auto-decompress when the response advertises a
         * Content-Encoding the client supports. */
        if (auto_decompress && resp->body && resp->body->byte_len > 0) {
            n00b_buffer_t *enc = n00b_http_response_header(
                resp, n00b_string_from_cstr("content-encoding"));
            if (enc && enc->byte_len > 0) {
                /* Build a NUL-terminated copy so the codec switch works. */
                size_t el = (size_t)enc->byte_len;
                char  *etmp = n00b_alloc_array(char, el + 1, .allocator = a);
                memcpy(etmp, enc->data, el);
                etmp[el] = '\0';
                auto dr = n00b_http_decompress(resp->body, etmp,
                                                .allocator = a);
                if (n00b_result_is_ok(dr)) {
                    resp->body = n00b_result_get(dr);
                }
                /* On decode failure we leave the body as-is; the
                 * caller can still inspect via the existing body
                 * accessor. */
            }
        }

        /* Optional response verifier hook. */
        if (auth && !n00b_http_auth_verify_response(auth, resp)) {
            return n00b_result_err(n00b_http_response_t *,
                                    N00B_HTTP_ERR_BAD_RESPONSE);
        }

        if (!follow_redirects || !status_is_redirect(resp->status)
            || hops >= max_redirects) {
            return n00b_result_ok(n00b_http_response_t *, resp);
        }

        /* Look for Location.  If absent, surface the redirect to the
         * caller as-is (some servers issue 3xx without Location). */
        n00b_buffer_t *loc =
            n00b_http_response_header(resp, n00b_string_from_cstr("location"));
        if (!loc) {
            return n00b_result_ok(n00b_http_response_t *, resp);
        }
        n00b_string_t *next_url = resolve_location(u, loc, a);
        if (!next_url) {
            /* Unresolvable Location — surface the original response. */
            return n00b_result_ok(n00b_http_response_t *, resp);
        }

        /* Host-allowlist enforcement (DF-015): when the caller
         * supplied a non-null `redirect_host_allowlist`, the next
         * hop's authority host must be in the list.  We parse the
         * resolved URL to extract the canonical host (already
         * lowercased by the parser per RFC 3986 § 3.2.2) so the
         * comparison is structural and not bytewise-string. */
        if (redirect_host_allowlist) {
            auto next_ur = n00b_http_url_parse(next_url, .allocator = a);
            if (n00b_result_is_err(next_ur)) {
                return n00b_result_err(
                    n00b_http_response_t *,
                    n00b_result_get_err(next_ur));
            }
            n00b_http_url_t *next_u = n00b_result_get(next_ur);
            if (!host_in_allowlist(next_u->host,
                                    redirect_host_allowlist)) {
                return n00b_result_err(
                    n00b_http_response_t *,
                    N00B_HTTP_ERR_HOST_REDIRECT_NOT_ALLOWED);
            }
        }

        /* Method preservation per RFC 9110 § 15.4. */
        if (!status_preserves_method(resp->status)) {
            /* 301/302/303 → GET, drop body. */
            cur_method = n00b_string_from_cstr("GET", .allocator = a);
            cur_body   = nullptr;
            cur_ct     = nullptr;
        }
        cur_url = next_url;
        hops++;
    }
}

/* ===========================================================================
 * §5   Topic-shaped dispatcher (chunk 5b)
 *
 * Wraps `n00b_http_request_sync` in a worker thread whose result is
 * delivered on a one-shot conduit topic.  Sync callers
 * `n00b_conduit_read(...)` it; async callers
 * `n00b_conduit_subscribe(...)`.  Either way the worker publishes
 * exactly one response and closes the topic.
 * =========================================================================== */

static n00b_http_response_t *
make_error_response(int32_t err, n00b_allocator_t *a)
{
    n00b_http_response_t *r = n00b_alloc_with_opts(
        n00b_http_response_t,
        &(n00b_alloc_opts_t){.allocator = a});
    r->status    = 0;
    r->body      = n00b_buffer_empty(.allocator = a);
    r->headers   = nullptr;
    r->n_headers = 0;
    r->transport = N00B_HTTP_TRANSPORT_UNKNOWN;
    r->error     = err;
    r->allocator = a;
    return r;
}

typedef struct {
    /* Owned heap copies — caller may drop refs after request returns. */
    n00b_conduit_t                                       *c;
    n00b_conduit_topic_t(n00b_http_response_t *)         *topic;
    n00b_string_t                                        *url;
    n00b_string_t                                        *method;
    n00b_buffer_t                                        *body;
    n00b_string_t                                        *content_type;
    n00b_http_h1_headers_t                               *extra;
    bool                                                  prefer_h3;
    int32_t                                               h3_handshake_ms;
    int32_t                                               timeout_ms;
    n00b_quic_trust_t                                    *trust;
    bool                                                  follow_redirects;
    int32_t                                               max_redirects;
    bool                                                  auto_decompress;
    n00b_string_t                                        *body_encoding;
    n00b_http_cookie_jar_t                               *cookie_jar;
    n00b_http_auth_t                                     *auth;
    n00b_http_connection_pool_t                          *pool;
    uint64_t                                              max_body_size;
    n00b_list_t(n00b_string_t *)                         *redirect_host_allowlist;
    n00b_allocator_t                                     *allocator;
} http_worker_args_t;

static void
publish_response(http_worker_args_t          *a,
                 n00b_http_response_t        *resp)
{
    /* Claim the publisher (we are the only producer). */
    n00b_result_t(n00b_conduit_publisher_t *) pres =
        n00b_conduit_publish_try_claim(
            (n00b_conduit_topic_base_t *)a->topic);
    if (n00b_result_is_err(pres)) {
        /* No subscriber will ever see this; close and return. */
        n00b_conduit_topic_close((n00b_conduit_topic_base_t *)a->topic);
        return;
    }
    n00b_conduit_publisher_t *pub = n00b_result_get(pres);

    n00b_conduit_message_t(n00b_http_response_t *) *msg =
        n00b_alloc_with_opts(
            n00b_conduit_message_t(n00b_http_response_t *),
            &(n00b_alloc_opts_t){.allocator = a->allocator});
    msg->header.type       = N00B_CONDUIT_MSG_USER;
    msg->header.topic      = (n00b_conduit_topic_base_t *)a->topic;
    msg->header.generation =
        n00b_conduit_topic_generation((n00b_conduit_topic_base_t *)a->topic);
    msg->header.epoch      =
        n00b_conduit_topic_epoch((n00b_conduit_topic_base_t *)a->topic);
    msg->header.next       = nullptr;
    msg->payload           = resp;

    n00b_conduit_topic_deliver_msg(
        n00b_http_response_t *,
        a->topic, msg, N00B_CONDUIT_OP_ALL);

    n00b_conduit_publish_yield(pub);

    /* One-shot topic: close after the publish so subscribers waiting
     * on the done-topic receive the closure signal and any further
     * read attempts fail fast. */
    n00b_conduit_topic_close((n00b_conduit_topic_base_t *)a->topic);
}

static void *
http_worker_main(void *arg)
{
    http_worker_args_t *a = (http_worker_args_t *)arg;

    auto rr = n00b_http_request_sync(
        a->url,
        .method                  = a->method,
        .body                    = a->body,
        .content_type            = a->content_type,
        .extra                   = a->extra,
        .prefer_h3               = a->prefer_h3,
        .h3_handshake_ms         = a->h3_handshake_ms,
        .timeout_ms              = a->timeout_ms,
        .trust                   = a->trust,
        .follow_redirects        = a->follow_redirects,
        .max_redirects           = a->max_redirects,
        .auto_decompress         = a->auto_decompress,
        .body_encoding           = a->body_encoding,
        .cookie_jar              = a->cookie_jar,
        .auth                    = a->auth,
        .pool                    = a->pool,
        .max_body_size           = a->max_body_size,
        .redirect_host_allowlist = a->redirect_host_allowlist,
        .allocator               = a->allocator);

    n00b_http_response_t *resp;
    if (n00b_result_is_ok(rr)) {
        resp = n00b_result_get(rr);
    } else {
        resp = make_error_response((int32_t)n00b_result_get_err(rr),
                                   a->allocator);
    }
    publish_response(a, resp);
    return nullptr;
}

/* Adapter: the conduit_service work-fn signature is `void(void*)`,
 * but http_worker_main is `void *(*)(void *)` (n00b_thread_spawn
 * shape).  This adapter just calls through. */
static void
http_worker_submit_adapter(void *arg)
{
    (void)http_worker_main(arg);
}

/* on_first_subscribe hook: submits the worker job once the first
 * subscriber attaches.  This avoids the publish-before-subscribe
 * race where a fast worker fans out to zero subscribers and the
 * caller's read() never wakes.
 *
 * We submit via the conduit's service threadpool (lazy-spawned on
 * first call) so concurrent requests share a small set of worker
 * threads instead of paying a thread-create per call. */
static void
http_request_first_sub_cb(n00b_conduit_topic_base_t *topic, void *ctx)
{
    (void)topic;
    http_worker_args_t *wargs = (http_worker_args_t *)ctx;

    /* Get-or-create the conduit's service. */
    n00b_conduit_t *c = wargs->c;
    n00b_conduit_service_t *svc = c->service;
    if (!svc) {
        auto sr = n00b_conduit_service_new(c);
        if (n00b_result_is_ok(sr)) {
            svc = n00b_result_get(sr);
        }
    }

    if (svc) {
        auto sub = n00b_conduit_service_submit(
            svc, http_worker_submit_adapter, wargs);
        if (n00b_result_is_ok(sub)) return;
    }

    /* Service submit unavailable — fall back to per-request
     * thread spawn rather than fail the request entirely. */
    auto tr = n00b_thread_spawn(http_worker_main, wargs);
    if (n00b_result_is_err(tr)) {
        /* Last-resort: synthesize and publish inline (caller is
         * already subscribed, so this delivers cleanly). */
        publish_response(wargs,
                         make_error_response(N00B_HTTP_ERR_INVALID_URL,
                                              wargs->allocator));
        return;
    }
    (void)n00b_result_get(tr);
}

n00b_result_t(n00b_conduit_topic_t(n00b_http_response_t *) *)
n00b_http_request(n00b_conduit_t *c, n00b_string_t *url)
    _kargs {
        n00b_string_t          *method            = nullptr;
        n00b_buffer_t          *body              = nullptr;
        n00b_string_t          *content_type      = nullptr;
        n00b_http_h1_headers_t *extra             = nullptr;
        bool                    prefer_h3         = true;
        int32_t                 h3_handshake_ms   = 1500;
        int32_t                 timeout_ms        = 30000;
        n00b_quic_trust_t      *trust             = nullptr;
        bool                    follow_redirects  = false;
        int32_t                 max_redirects     = 5;
        bool                    auto_decompress   = true;
        n00b_string_t          *body_encoding     = nullptr;
        n00b_http_cookie_jar_t *cookie_jar        = nullptr;
        n00b_http_auth_t       *auth              = nullptr;
        n00b_http_connection_pool_t *pool         = nullptr;
        uint64_t                max_body_size     = 0;
        n00b_list_t(n00b_string_t *) *redirect_host_allowlist = nullptr;
        n00b_allocator_t       *allocator         = nullptr;
    }
{
    if (!c) {
        return n00b_result_err(
            n00b_conduit_topic_t(n00b_http_response_t *) *,
            N00B_HTTP_ERR_NULL_ARG);
    }
    if (!url) {
        return n00b_result_err(
            n00b_conduit_topic_t(n00b_http_response_t *) *,
            N00B_HTTP_ERR_NULL_ARG);
    }

    n00b_allocator_t *a = allocator
        ? allocator
        : (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;

    /* Allocate a fresh topic.  USER_EVENT tag + atomic id keeps
     * URIs unique per request. */
    static _Atomic(uint64_t) next_req_id = 1;
    uint64_t           id  = atomic_fetch_add(&next_req_id, 1);
    n00b_conduit_uri_t uri =
        n00b_conduit_int_uri(N00B_CONDUIT_TAG_USER_EVENT, id);

    n00b_result_t(n00b_conduit_topic_base_t *) tres =
        n00b_conduit_topic_get(
            c, uri,
            sizeof(n00b_conduit_topic_t(n00b_http_response_t *)));
    if (n00b_result_is_err(tres)) {
        return n00b_result_err(
            n00b_conduit_topic_t(n00b_http_response_t *) *,
            n00b_result_get_err(tres));
    }
    n00b_conduit_topic_t(n00b_http_response_t *) *topic =
        (n00b_conduit_topic_t(n00b_http_response_t *) *)
            n00b_result_get(tres);

    /* The TOPIC_IMPL macro generated `_N00B_TOPIC_FN(init, T)` which
     * sets up subscriptions list + done-topic.  Calling it idempotent
     * for fresh topics. */
    topic->subscriptions =
        n00b_list_new(n00b_conduit_subscription_t(n00b_http_response_t *) *,
                      c->allocator);
    topic->inbox = nullptr;

    /* Stash request kwargs in a heap struct the worker owns. */
    http_worker_args_t *wargs = n00b_alloc_with_opts(
        http_worker_args_t,
        &(n00b_alloc_opts_t){.allocator = a});
    wargs->c                       = c;
    wargs->topic                   = topic;
    wargs->url                     = url;
    wargs->method                  = method;
    wargs->body                    = body;
    wargs->content_type            = content_type;
    wargs->extra                   = extra;
    wargs->prefer_h3               = prefer_h3;
    wargs->h3_handshake_ms         = h3_handshake_ms;
    wargs->timeout_ms              = timeout_ms;
    wargs->trust                   = trust;
    wargs->follow_redirects        = follow_redirects;
    wargs->max_redirects           = max_redirects;
    wargs->auto_decompress         = auto_decompress;
    wargs->body_encoding           = body_encoding;
    wargs->cookie_jar              = cookie_jar;
    wargs->auth                    = auth;
    wargs->pool                    = pool;
    wargs->max_body_size           = max_body_size;
    wargs->redirect_host_allowlist = redirect_host_allowlist;
    wargs->allocator               = a;

    /* Defer the worker spawn until the first subscriber attaches.
     * The conduit topic's on_first_subscribe hook fires from inside
     * the subscribe() path, so by the time the worker publishes, the
     * subscriber's inbox is in place. */
    n00b_conduit_topic_base_t *base =
        (n00b_conduit_topic_base_t *)topic;
    base->on_first_subscribe_ctx = wargs;
    base->on_first_subscribe     = http_request_first_sub_cb;

    return n00b_result_ok(
        n00b_conduit_topic_t(n00b_http_response_t *) *, topic);
}
