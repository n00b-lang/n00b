/**
 * @file metrics.h
 * @brief Prometheus-compatible metrics for the QUIC subsystem.
 *
 * Phase 5 § 5.1.  Provides a small registry + counter / gauge /
 * histogram primitives + a `/metrics` listener that emits the
 * Prometheus 0.0.4 text exposition format.  The listener rides on
 * the existing conduit TCP socket primitives (`include/conduit/socket.h`);
 * no new socket code lives under `src/quic/`.
 *
 * **Registries are explicit per-process** — there is no singleton.
 * An application that wants metrics constructs one registry and
 * threads it into endpoint / H3 / RPC subsystems via the
 * `.metrics_registry` `_kargs` parameter on each constructor.
 * Subsystems given a non-NULL registry register their counters
 * against it; a NULL registry skips instrumentation entirely.
 *
 * Tests can build a registry, exercise it synchronously via
 * `n00b_quic_metrics_encode`, and never bind a listener.
 *
 * @par Reference metric set (registered by the owning subsystem):
 *   - `n00b_quic_chan_opens_total{kind}`       (counter)
 *   - `n00b_quic_chan_active{kind}`            (gauge)
 *   - `n00b_quic_audit_events_total{decision}` (counter)
 *   - `n00b_quic_cert_expiry_seconds{endpoint}` (gauge)
 *   - `n00b_quic_rpc_calls_total{service,method,status}` (counter)
 *   - `n00b_quic_rpc_call_duration_us`         (histogram)
 *
 * @see ~/dd/quic_5.md § 5.1, § 6
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "n00b.h"
#include "adt/list.h"
#include "adt/result.h"
#include "core/buffer.h"
#include "conduit/conduit.h"
#include "conduit/socket.h"

/* ===========================================================================
 * Opaque types
 * =========================================================================== */

typedef struct n00b_quic_metric_registry n00b_quic_metric_registry_t;
typedef struct n00b_quic_metric_counter  n00b_quic_metric_counter_t;
typedef struct n00b_quic_metric_gauge    n00b_quic_metric_gauge_t;
typedef struct n00b_quic_metric_hist     n00b_quic_metric_hist_t;
typedef struct n00b_quic_metric_listener n00b_quic_metric_listener_t;

/* ===========================================================================
 * Registry
 * =========================================================================== */

/**
 * @brief Allocate an empty metric registry.
 *
 * @param allocator  Allocator for the registry; nullptr means the
 *                   conduit pool.  Long-lived state should use the
 *                   conduit pool.
 *
 * @return  Owned registry pointer.
 */
extern n00b_quic_metric_registry_t *
n00b_quic_metrics_registry_new(n00b_allocator_t *allocator);

/**
 * @brief Encode the current registry snapshot in the Prometheus
 *        0.0.4 text exposition format.
 *
 * Safe to call concurrently with counter / gauge / histogram
 * updates.  Each metric is rendered with its `# HELP` + `# TYPE`
 * lines followed by sample lines.
 *
 * @param r  Registry; may be nullptr (returns an empty buffer).
 * @return   New buffer (registry's allocator) holding the rendered text.
 */
extern n00b_buffer_t *
n00b_quic_metrics_encode(n00b_quic_metric_registry_t *r);

/* ===========================================================================
 * Counters
 * =========================================================================== */

/**
 * @brief Register or fetch a counter on the registry.
 *
 * @param r     Registry.
 * @param name  Metric name (Prom-compatible: `[a-zA-Z_][a-zA-Z0-9_]*`).
 * @param help  One-line human-readable description.
 *
 * @kw labels   Optional list of label names (ordered).  When present,
 *              each `_inc` must supply a matching `label_values` list.
 *
 * @return Ok(counter) on success; Err(@c N00B_QUIC_ERR_INVALID_ARG)
 *         if @p name violates Prom naming or duplicates an existing
 *         metric of a different type.
 *
 * @pre    @p r and @p name are non-nullptr.
 */
extern n00b_result_t(n00b_quic_metric_counter_t *)
n00b_quic_metric_counter(n00b_quic_metric_registry_t *r,
                         const char                  *name,
                         const char                  *help)
    _kargs {
        n00b_list_t(n00b_buffer_t *) *labels = nullptr;
    };

/**
 * @brief Increment a counter by @p by.
 *
 * @kw label_values  Required when the counter was declared with
 *                   labels; must match in length.  Each value is
 *                   compared byte-for-byte with previously-seen
 *                   values to identify the per-label-tuple slot.
 */
extern void
n00b_quic_metric_counter_inc(n00b_quic_metric_counter_t *c, uint64_t by)
    _kargs {
        n00b_list_t(n00b_buffer_t *) *label_values = nullptr;
    };

/* ===========================================================================
 * Gauges
 * =========================================================================== */

extern n00b_result_t(n00b_quic_metric_gauge_t *)
n00b_quic_metric_gauge(n00b_quic_metric_registry_t *r,
                       const char                  *name,
                       const char                  *help)
    _kargs {
        n00b_list_t(n00b_buffer_t *) *labels = nullptr;
    };

extern void
n00b_quic_metric_gauge_set(n00b_quic_metric_gauge_t *g, double v)
    _kargs {
        n00b_list_t(n00b_buffer_t *) *label_values = nullptr;
    };

/* ===========================================================================
 * Histograms
 * =========================================================================== */

/**
 * @brief Register a histogram with explicit bucket upper-bounds.
 *
 * @param buckets    Upper bounds in ascending order.  An implicit
 *                   `+Inf` bucket is added by the encoder.
 * @param n_buckets  Length of @p buckets.
 */
extern n00b_result_t(n00b_quic_metric_hist_t *)
n00b_quic_metric_hist(n00b_quic_metric_registry_t *r,
                      const char                  *name,
                      const char                  *help,
                      const double                *buckets,
                      size_t                       n_buckets)
    _kargs {
        n00b_list_t(n00b_buffer_t *) *labels = nullptr;
    };

extern void
n00b_quic_metric_hist_observe(n00b_quic_metric_hist_t *h, double v)
    _kargs {
        n00b_list_t(n00b_buffer_t *) *label_values = nullptr;
    };

/* ===========================================================================
 * Listener (conduit-driven)
 * =========================================================================== */

/**
 * @brief Open a `/metrics` HTTP listener atop the conduit TCP
 *        primitives.
 *
 * Reuses the conduit + IO backend supplied by the caller — no
 * separate IO loop or socket bookkeeping.  Each accepted TCP
 * connection is read up to a small bound, matched against
 * `GET <path> HTTP/1.x`, and answered with the Prom exposition
 * encoding (200) or `404` / `400` for non-matching requests.
 *
 * Requires a periodic drive call from the application
 * (`n00b_quic_metrics_listener_run_once`); the listener does not
 * spawn its own thread.
 *
 * @param r   Registry to scrape.
 * @param c   Conduit instance hosting the IO loop.
 * @param io  IO backend.
 *
 * @kw bind_host  Bind address; default `::1`.
 * @kw bind_port  Bind port; default 9100.
 * @kw path       Request path to match; default `/metrics`.
 *
 * @return Ok(handle) on success.  Caller closes via
 *         `n00b_quic_metrics_listener_close`.
 */
extern n00b_result_t(n00b_quic_metric_listener_t *)
n00b_quic_metrics_listener_open(n00b_quic_metric_registry_t *r,
                                n00b_conduit_t              *c,
                                n00b_conduit_io_backend_t   *io)
    _kargs {
        n00b_buffer_t *bind_host = nullptr;
        uint16_t       bind_port = 9100;
        n00b_buffer_t *path      = nullptr;
    };

/**
 * @brief Drain the accept inbox + service one request per accepted
 *        connection.  Non-blocking; intended to be called from the
 *        application's drive loop alongside
 *        `n00b_quic_endpoint_run_once`.
 *
 * @param l  Listener; nullptr is a no-op.
 *
 * @return Number of connections serviced this call (0 if nothing was
 *         pending).
 */
extern size_t
n00b_quic_metrics_listener_run_once(n00b_quic_metric_listener_t *l);

/** @brief Close the listener and the underlying conduit listener. */
extern void
n00b_quic_metrics_listener_close(n00b_quic_metric_listener_t *l);

/**
 * @brief Return the port the listener is bound to.
 *
 * Useful when the caller passed `bind_port = 0` to request an
 * ephemeral port and now needs to advertise it (or, in tests,
 * connect to it).
 *
 * @return  Bound TCP port; 0 if the listener is closed or
 *          getsockname fails.
 */
extern uint16_t
n00b_quic_metrics_listener_port(n00b_quic_metric_listener_t *l);
