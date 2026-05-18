/**
 * @file audit.h
 * @brief Auth-decision audit log: structured events fanned out to
 *        subscribers, with a built-in JSONL sink.
 *
 * Phase 3 § 12 + § 10.2: every auth decision (allow / deny)
 * produces an event so the deployment playbook's monitoring
 * layer has something to watch.  The shape:
 *
 *   - **Topic-style fan-out**: callers subscribe a function +
 *     opaque ctx; each emit invokes every subscriber.  The
 *     subscriber list is process-global, mutex-protected.
 *   - **Optional JSONL sink**: a built-in sink that subscribes,
 *     opens a file, and appends one JSON event per line.
 *     Operators wire this via the manifest extension in 3.9.
 *
 * The "topic" choice from the user's earlier design poll —
 * "topic primary + optional JSONL sink" — maps cleanly onto
 * this subscriber-list shape.  Promoting to a conduit-topic-
 * based fan-out is a follow-up if cross-thread / cross-process
 * fan-out becomes a real use case; today's hook is in-process
 * and synchronous.
 *
 * @see ~/dd/quic_3.md § 10.2 + § 12
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "n00b.h"
#include "adt/result.h"
#include "core/string.h"
#include "net/quic/quic_types.h"
#include "net/quic/metrics.h"

typedef enum : uint8_t {
    N00B_QUIC_AUDIT_ALLOW = 0,
    N00B_QUIC_AUDIT_DENY  = 1,
} n00b_quic_audit_decision_t;

/**
 * @brief One auth decision event.
 *
 * String fields are **borrowed pointers** valid for the duration
 * of the emit call.  Subscribers that need to retain values must
 * copy them.
 */
typedef struct {
    int64_t                    timestamp_ms;  /**< now, in ms since epoch. */
    n00b_quic_audit_decision_t decision;
    n00b_quic_err_t            reason_code;   /**< OK on allow; AUTH_* on deny. */
    /* JWT claim fields (borrowed; may be null). */
    const char                *iss;
    const char                *sub;
    const char                *aud;
    const char                *jti;
    /* DPoP request shape (borrowed; may be null). */
    const char                *htm;
    const char                *htu;
    /* Policy identifier (optional; manifest references). */
    const char                *policy_id;
    /* Peer-address informational (e.g., "1.2.3.4:5432"; null if
     * not available at the call site). */
    const char                *peer_addr;
} n00b_quic_audit_event_t;

/**
 * @brief Subscriber callback signature.
 *
 * @param evt  Event payload (borrowed; do not retain past the
 *             call unless the subscriber copies fields).
 * @param ctx  The opaque ctx the subscriber registered with.
 */
typedef void (*n00b_quic_audit_subscriber_fn)(
    const n00b_quic_audit_event_t *evt,
    void                          *ctx);

/**
 * @brief Subscribe to all subsequent audit events.
 *
 * @return Subscription handle (>0).  Pass to
 *         @c n00b_quic_audit_unsubscribe to stop.
 *         Returns -1 on registration failure (cap reached).
 */
extern int
n00b_quic_audit_subscribe(n00b_quic_audit_subscriber_fn fn, void *ctx);

/** @brief Cancel a subscription. */
extern void
n00b_quic_audit_unsubscribe(int subscription_id);

/**
 * @brief Emit an event to all current subscribers (synchronous).
 *
 * Each subscriber is called in subscription order under a global
 * mutex.  Subscribers should not call back into
 * `n00b_quic_audit_subscribe` from the callback (recursive lock
 * behavior is undefined per n00b_mutex defaults).
 */
extern void
n00b_quic_audit_emit(const n00b_quic_audit_event_t *evt);

/**
 * @brief Render an event to a NUL-terminated JSON line.
 *
 * Lifetime: heap-allocated (conduit pool).  Same line shape the
 * built-in JSONL sink writes.
 */
extern char *
n00b_quic_audit_event_to_json(const n00b_quic_audit_event_t *evt);

/* ===========================================================================
 * JSONL sink
 * =========================================================================== */

typedef struct n00b_quic_audit_jsonl_sink n00b_quic_audit_jsonl_sink_t;

/**
 * @brief Open a JSONL sink at @p path.
 *
 * Subscribes a sink that appends one JSON event per line to the
 * file.  Existing files are appended to (no truncation).
 *
 * @return Result: ok with the sink handle on success; err on
 *         open failure.
 */
extern n00b_result_t(n00b_quic_audit_jsonl_sink_t *)
n00b_quic_audit_jsonl_sink_open(const char *path);

/**
 * @brief Close a JSONL sink: unsubscribe + flush + fclose.
 */
extern void
n00b_quic_audit_jsonl_sink_close(n00b_quic_audit_jsonl_sink_t *s);

/**
 * @brief Attach a Prometheus registry to the audit fan-out.
 *
 * Phase 5 § 5.1.  Registers `n00b_quic_audit_events_total{decision}`
 * on @p r and increments it on every subsequent
 * `n00b_quic_audit_emit`.  Idempotent: a second call against a
 * different registry is a no-op (audit's instrumentation is
 * single-registry-bound).  Pass nullptr to skip.
 */
extern void
n00b_quic_audit_attach_metrics(n00b_quic_metric_registry_t *r);
