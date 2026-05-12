/**
 * @file http_pool.h
 * @internal
 * @brief Per-runtime connection pool for the n00b HTTP client.
 *
 * Phase 6 chunk 4.  Standalone bookkeeping container — the pool
 * stores opaque entries with a destructor callback so the same data
 * structure can hold both h1 (TCP+TLS socket) and h3 (endpoint +
 * QUIC conn + h3 client) idle connections.  Chunk 5's dispatcher
 * owns one pool **per runtime** and consults it before constructing
 * a fresh round-trip.
 *
 * Caps + timers (defaults match curl's reference behavior):
 *   - per-origin idle cap        4
 *   - global idle cap           16
 *   - idle timeout              30 s
 *   - lifetime cap              10 m
 *
 * Concurrency: this revision is single-threaded.  The dispatcher
 * either runs on the conduit IO thread or holds an external mutex
 * — chunk 5 wires whichever pattern the dispatcher picks.  No
 * internal locking yet; doc'd here so call sites don't assume MT
 * safety.
 *
 * @see ~/dd/quic_6.md § 6 + § 7 chunk 4.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "n00b.h"
#include "core/string.h"

/* ----------------------------------------------------------------- */
/* Types                                                             */
/* ----------------------------------------------------------------- */

typedef struct n00b_http_connection_pool n00b_http_connection_pool_t;

typedef enum : uint8_t {
    N00B_HTTP_CONNECTION_POOL_TRANSPORT_H1 = 1,
    N00B_HTTP_CONNECTION_POOL_TRANSPORT_H3 = 3,
} n00b_http_connection_pool_transport_t;

/**
 * @brief Destructor invoked when the pool drops an entry.
 *
 * Called from `n00b_http_connection_pool_release` (when an admit would exceed a
 * cap), from `n00b_http_connection_pool_reap` (when the entry has expired), and
 * from `n00b_http_connection_pool_close` (when the pool itself is torn down).
 * The callback is responsible for closing fds / endpoints / conns
 * referenced by @p user_data.
 *
 * @param user_data  Caller-supplied pointer set in `release()`.
 */
typedef void (*n00b_http_connection_pool_close_fn)(void *user_data);

/**
 * @brief Snapshot pool statistics for testing + observability.
 *
 * All counters are monotonic (never decrease) except `idle_count`
 * which is the live count.
 */
typedef struct {
    size_t idle_count;             /**< Live idle entries (all origins). */
    size_t acquire_hits;           /**< acquire() returned an entry. */
    size_t acquire_misses;         /**< acquire() returned nullptr. */
    size_t evict_idle_timeout;     /**< Reaped past idle_timeout_ms. */
    size_t evict_lifetime;         /**< Reaped past lifetime_ms. */
    size_t evict_cap_total;        /**< Evicted on global-cap admit. */
    size_t evict_cap_per_origin;   /**< Evicted on per-origin-cap admit. */
} n00b_http_connection_pool_stats_t;

/* ----------------------------------------------------------------- */
/* Lifecycle                                                         */
/* ----------------------------------------------------------------- */

/**
 * @brief Allocate a fresh HTTP connection pool.
 *
 * @kw max_total_idle  Global cap on idle entries.  Default 16.
 * @kw max_per_origin  Cap per (origin, transport) bucket.  Default 4.
 * @kw idle_timeout_ms Reaping threshold for last-use age.  Default
 *                     30000.  Set to 0 to disable idle reaping
 *                     (lifetime cap still applies).
 * @kw lifetime_ms     Hard cap on total entry lifetime regardless of
 *                     activity.  Default 600000.  Set to 0 to disable.
 * @kw allocator       Allocator for pool internals.  Default
 *                     per-runtime conduit pool — picked because
 *                     pool entries may be dispatched across the IO
 *                     thread.
 */
extern n00b_http_connection_pool_t *
n00b_http_connection_pool_new()
    _kargs {
        size_t            max_total_idle  = 16;
        size_t            max_per_origin  = 4;
        int32_t           idle_timeout_ms = 30000;
        int32_t           lifetime_ms     = 600000;
        n00b_allocator_t *allocator       = nullptr;
    };

/**
 * @brief Tear down a pool, evicting every remaining idle entry
 *        through its registered close callback.
 *
 * Idempotent on @p pool nullptr.
 */
extern void n00b_http_connection_pool_close(n00b_http_connection_pool_t *pool);

/* ----------------------------------------------------------------- */
/* Acquire / release                                                 */
/* ----------------------------------------------------------------- */

/**
 * @brief Try to grab an idle entry for @p (origin, transport).
 *
 * Origin matching is byte-exact on the lowercased canonical form
 * produced by `n00b_http_url_t::origin`.
 *
 * @return  user_data of an entry on hit, removed from the idle list.
 *          The caller now "owns" the entry — call release() to
 *          return it after use, or invoke its close-fn directly if
 *          the connection has gone bad.
 *
 *          nullptr on miss.
 */
extern void *
n00b_http_connection_pool_acquire(n00b_http_connection_pool_t           *pool,
                       n00b_string_t              *origin,
                       n00b_http_connection_pool_transport_t  transport);

/**
 * @brief Return @p user_data to the idle list under @p (origin,
 *        transport).
 *
 * If admitting would exceed the per-origin cap, the LRU entry of
 * that bucket is evicted via its close-fn.  If admitting would also
 * exceed the global cap, the LRU entry across all buckets is
 * evicted (may be in another bucket).
 *
 * @param close_fn  Required; never nullptr.  Stored alongside the
 *                  entry and invoked when the pool drops it.
 */
extern void
n00b_http_connection_pool_release(n00b_http_connection_pool_t           *pool,
                       n00b_string_t              *origin,
                       n00b_http_connection_pool_transport_t  transport,
                       void                       *user_data,
                       n00b_http_connection_pool_close_fn     close_fn);

/**
 * @brief Sweep the pool for expired entries.
 *
 * Tests pass an explicit @p now_ms so the clock can be controlled.
 * Production callers pass the current value of
 * `n00b_ns_timestamp() / 1000000`.
 */
extern void
n00b_http_connection_pool_reap(n00b_http_connection_pool_t *pool, uint64_t now_ms);

/* ----------------------------------------------------------------- */
/* Stats                                                             */
/* ----------------------------------------------------------------- */

extern n00b_http_connection_pool_stats_t
n00b_http_connection_pool_stats(n00b_http_connection_pool_t *pool);

/**
 * @brief Test hook: inject a fake clock.
 *
 * Once called, future `release()` calls timestamp entries with
 * @p now_ms instead of the OS monotonic clock.  Tests use this
 * together with `n00b_http_connection_pool_reap(pool, fake_now)` to drive the
 * timer logic deterministically.  The fake clock value is sticky;
 * it is set on first call and continues to be used (the function
 * cannot un-set it — pools are short-lived in tests).
 */
extern void
n00b_http_connection_pool_set_now_for_test(n00b_http_connection_pool_t *pool, uint64_t now_ms);

/* ----------------------------------------------------------------- */
/* Per-runtime singleton accessor                                    */
/* ----------------------------------------------------------------- */

/**
 * @brief Return the runtime's HTTP connection pool, lazy-creating it
 *        on first call.
 *
 * The pool is stored on `n00b_runtime_t::http_connection_pool` so it
 * has natural per-runtime lifetime — drained when the runtime tears
 * down (the dispatcher invokes
 * `n00b_http_connection_pool_close()` on shutdown).
 *
 * Callers can override the per-runtime singleton by passing an
 * explicit `pool=` kwarg to the dispatcher (e.g. a CLI that wants
 * a fresh pool per invocation regardless of runtime state).  When
 * the kwarg is nullptr (default) the dispatcher consults this
 * accessor.
 *
 * Thread-safe: uses an atomic CAS on the runtime slot to avoid two
 * threads racing on lazy-init.
 */
extern n00b_http_connection_pool_t *
n00b_http_get_connection_pool(void);
