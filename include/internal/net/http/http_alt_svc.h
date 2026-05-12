/**
 * @file http_alt_svc.h
 * @internal
 * @brief Alt-Svc (RFC 7838) parser + per-origin cache for the
 *        n00b HTTP client.
 *
 * Phase 6 chunk 6.  An h1 response carrying
 *
 *     Alt-Svc: h3=":443"; ma=86400, h3=":8443"; ma=3600
 *
 * advertises that the origin also speaks h3 at the listed
 * alt-authorities for the given max-ages.  The dispatcher consults
 * this cache before falling back to h1 — when a live h3 entry
 * exists, the next call to that origin attempts h3 directly even
 * if a recent h1 fallback put the origin into the loss cache.
 *
 * Per the user's "no globals; per-runtime only" rule the cache is
 * a standalone data structure; the dispatcher (chunk 5) owns one
 * per runtime.
 *
 * @see ~/dd/quic_6.md § 6 + § 7 chunk 6.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "n00b.h"
#include "core/string.h"
#include "core/buffer.h"

/* ----------------------------------------------------------------- */
/* Parsed Alt-Svc entry                                              */
/* ----------------------------------------------------------------- */

/**
 * @brief One parsed Alt-Svc alternative.
 *
 * @c protocol_id  Wire identifier — `"h3"`, `"h2"`, `"h3-29"`, etc.
 *                 Lowercased per RFC 7838 § 3 (token comparison is
 *                 case-insensitive).
 * @c host         Alt-authority host.  Empty (zero u8_bytes) means
 *                 "same host as the origin"; otherwise an explicit
 *                 hostname / IP.
 * @c port         Alt-authority port.  0 means "same port as the
 *                 origin".  Note RFC 7838 disallows port 0 in the
 *                 wire form, so we use it as the sentinel.
 * @c ma_seconds   Max-age in seconds.  Default 24 * 3600 when the
 *                 header omits `ma=`.
 * @c persist      The `persist=1` parameter.  We don't yet persist
 *                 the cache across process restarts — flag is
 *                 captured for completeness so the future on-disk
 *                 cache can honor it without an API change.
 */
typedef struct {
    n00b_string_t *protocol_id;
    n00b_string_t *host;
    uint16_t       port;
    int32_t        ma_seconds;
    bool           persist;
} n00b_http_alt_svc_entry_t;

/* ----------------------------------------------------------------- */
/* Parser                                                            */
/* ----------------------------------------------------------------- */

/**
 * @brief Parse the value of an `Alt-Svc:` header.
 *
 * @param header  NUL-terminated header value (e.g.
 *                `h3=\":443\"; ma=86400, h3-29=\":443\"; ma=3600`).
 *                Required.
 * @param n_out   Output: number of entries.
 * @param clear_out Output: true if the value is the special `clear`
 *                  token (no entries; caller should clear its cache
 *                  for this origin).
 *
 * @kw allocator  Allocator for entry array + strings.  Default
 *                per-runtime conduit pool.
 *
 * @return  Heap-allocated entry array on success (may be empty if
 *          the header was malformed).  nullptr only on
 *          @p header nullptr or `clear`.  The caller frees indirectly
 *          via GC; no explicit free required.
 */
extern n00b_http_alt_svc_entry_t *
n00b_http_alt_svc_parse(const char *header,
                        size_t     *n_out,
                        bool       *clear_out)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
    };

/* ----------------------------------------------------------------- */
/* Cache                                                             */
/* ----------------------------------------------------------------- */

typedef struct n00b_http_alt_svc_cache n00b_http_alt_svc_cache_t;

/**
 * @brief Allocate a fresh per-origin Alt-Svc cache.
 *
 * @kw max_origins  Per-cache origin cap (oldest entry evicted on
 *                  overflow).  Default 64.
 * @kw allocator    Default per-runtime conduit pool.
 */
extern n00b_http_alt_svc_cache_t *
n00b_http_alt_svc_cache_new()
    _kargs {
        size_t            max_origins = 64;
        n00b_allocator_t *allocator   = nullptr;
    };

/** @brief Drop all cached entries.  Idempotent on @p cache nullptr. */
extern void
n00b_http_alt_svc_cache_close(n00b_http_alt_svc_cache_t *cache);

/**
 * @brief Replace this origin's cache entries with @p entries.
 *
 * Called after a response carries `Alt-Svc:`.  When the parsed
 * header is `clear`, the dispatcher calls
 * `n00b_http_alt_svc_cache_clear` instead.
 */
extern void
n00b_http_alt_svc_cache_set(n00b_http_alt_svc_cache_t       *cache,
                            n00b_string_t                   *origin,
                            const n00b_http_alt_svc_entry_t *entries,
                            size_t                           n_entries);

/** @brief Drop all entries for @p origin (RFC 7838 `clear`). */
extern void
n00b_http_alt_svc_cache_clear(n00b_http_alt_svc_cache_t *cache,
                              n00b_string_t             *origin);

/**
 * @brief Lookup the preferred live h3 alternate for @p origin.
 *
 * @param cache    Cache to consult.  nullptr → returns false.
 * @param origin   Canonical origin string from `n00b_http_url_t::origin`.
 * @param host_out Out: alt-authority host (may be empty meaning
 *                 "same as origin host").  Borrowed.
 * @param port_out Out: alt-authority port (0 means "same as origin").
 *
 * @return  true iff an unexpired h3 entry exists.  The dispatcher
 *          uses the (host, port) tuple to redirect the h3 attempt;
 *          when host is empty and port is 0 it stays on the original
 *          (host, port).
 */
extern bool
n00b_http_alt_svc_cache_lookup_h3(n00b_http_alt_svc_cache_t  *cache,
                                  n00b_string_t              *origin,
                                  n00b_string_t             **host_out,
                                  uint16_t                   *port_out);

/**
 * @brief Reap expired entries.  Caller invokes periodically or
 *        on-demand; the dispatcher will wire this into its idle
 *        path in chunk 12.
 */
extern void
n00b_http_alt_svc_cache_reap(n00b_http_alt_svc_cache_t *cache,
                             uint64_t                   now_ms);

/**
 * @brief Test hook: inject a fake clock.  Same shape as
 *        `n00b_http_connection_pool_set_now_for_test`.
 */
extern void
n00b_http_alt_svc_cache_set_now_for_test(n00b_http_alt_svc_cache_t *cache,
                                         uint64_t                   now_ms);

/** @brief Live-entry count.  Diagnostic. */
extern size_t
n00b_http_alt_svc_cache_size(n00b_http_alt_svc_cache_t *cache);
