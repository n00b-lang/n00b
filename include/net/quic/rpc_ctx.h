/**
 * @file rpc_ctx.h
 * @brief gRPC-style cancellation + deadline context for n00b RPC.
 *
 * Phase 4 § 4.8.  An `n00b_rpc_ctx_t` is a small handle threaded
 * through every RPC call (client and server side).  It carries:
 *
 *   - A **sticky cancel** flag (atomic; once set, never cleared).
 *   - An optional **monotonic deadline** in nanoseconds (sentinel
 *     `-1` means "no deadline").  When the wall-clock crosses the
 *     deadline, the context becomes implicitly cancelled.
 *   - A **parent pointer** + a child list, so cancelling a parent
 *     cascades to every descendant.
 *   - A condition variable that waiters block on; cancel (and the
 *     close path) broadcasts to wake them.
 *
 * ### Lifetime + ownership
 *
 * Contexts are allocated from the runtime's `conduit_pool` — the
 * same allocator used by every other QUIC primitive.  No explicit
 * free is required for memory; `n00b_rpc_ctx_close` exists to release
 * waiters and detach from the parent's child list (so a long-lived
 * parent doesn't accumulate dead descendants).  Close is idempotent.
 *
 * ### Thread-safety
 *
 * `n00b_rpc_ctx_is_cancelled` is lock-free (atomic load).  Cancel,
 * close, and the child-tracking operations are mutex-protected per
 * ctx.  Cascading cancellation walks the tree depth-first and locks
 * each node in turn; a single contiguous lock is intentionally
 * avoided to prevent priority-inversion across the tree.
 *
 * ### Wire signaling
 *
 * This module is **local-only**.  The over-the-wire signal
 * (STOP_SENDING / RESET_STREAM with H3 error codes) lands in the RPC
 * unary code path (sub-phase 4.6), which observes a context cancel
 * via `is_cancelled` and emits the stream-level signal.  The ctx
 * primitive itself has no dependency on H3 / QUIC / RPC runtime —
 * tests can build it standalone.
 *
 * @see ~/dd/quic_4.md § 4.8 + § 8
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "n00b.h"

/**
 * @brief Cancellation/deadline context handle.
 *
 * Opaque from the public API; the implementation lives in
 * `src/quic/rpc_ctx.c`.
 */
typedef struct n00b_rpc_ctx n00b_rpc_ctx_t;

/**
 * @brief Build a root context: no parent, no deadline, cancellable.
 *
 * Roots are typically allocated at the top of an RPC call site (or
 * a handler entry point).  They live until explicitly closed; closing
 * a root automatically cancels and releases waiters on every
 * descendant that is still attached.
 *
 * @return Fresh ctx allocated from `conduit_pool`.  Never nullptr.
 */
extern n00b_rpc_ctx_t *n00b_rpc_ctx_new(void);

/**
 * @brief Build a child context with a wall-clock deadline.
 *
 * Deadline is given in **monotonic nanoseconds** (the same epoch as
 * `n00b_ns_timestamp()`) — typically computed as `now + duration`.
 *
 * **Inheritance**: if @p parent has its own deadline, the child's
 * effective deadline is `min(parent_deadline, deadline_ns)`.  This
 * matches gRPC's "deadline propagation": a child can never extend a
 * parent's clock.
 *
 * @param parent       Optional parent ctx.  When non-null the child
 *                     inherits cancel propagation from the parent.
 * @param deadline_ns  Absolute monotonic-ns deadline.
 *
 * @return Fresh ctx; cancel cascades from parent (if any).
 */
extern n00b_rpc_ctx_t *
n00b_rpc_ctx_with_deadline(n00b_rpc_ctx_t *parent, int64_t deadline_ns);

/**
 * @brief Build a child context that is cancellable independent of
 *        any deadline; inherits parent's deadline (if any).
 *
 * @param parent  Optional parent ctx.
 *
 * @return Fresh ctx.
 */
extern n00b_rpc_ctx_t *
n00b_rpc_ctx_with_cancel(n00b_rpc_ctx_t *parent);

/**
 * @brief Cancel a context.  Sticky: once set, never cleared.
 *
 * Cascades to every attached descendant.  Safe to call concurrently
 * with reads; safe to call multiple times (subsequent calls are no-
 * ops).
 *
 * @param ctx  Context to cancel; nullptr is a no-op.
 */
extern void n00b_rpc_ctx_cancel(n00b_rpc_ctx_t *ctx);

/**
 * @brief Has the context been cancelled (or its deadline expired)?
 *
 * Lock-free.  When the context has a deadline and the deadline has
 * elapsed, this call also performs a one-shot cancel transition (so
 * waiters get woken on the next `_cancel` / `_close` boundary, and
 * subsequent reads observe the sticky cancel flag directly).
 *
 * @param ctx  Context to query.  nullptr returns false.
 */
extern bool n00b_rpc_ctx_is_cancelled(n00b_rpc_ctx_t *ctx);

/**
 * @brief Nanoseconds remaining until the deadline.
 *
 * @param ctx  Context to query.
 *
 * @return  - `-1` if @p ctx is null or has no deadline.
 *          - `0` if the deadline has passed (or the context is
 *            already cancelled).
 *          - The remaining ns otherwise.
 */
extern int64_t n00b_rpc_ctx_remaining_ns(n00b_rpc_ctx_t *ctx);

/**
 * @brief Block until the context is cancelled (or its deadline
 *        elapses).
 *
 * Returns immediately if the context is already cancelled.  Spurious
 * wakeups are filtered internally; the call only returns when the
 * cancelled flag is observably true.
 *
 * @param ctx  Context to wait on.  nullptr returns immediately.
 */
extern void n00b_rpc_ctx_wait(n00b_rpc_ctx_t *ctx);

/**
 * @brief Block until cancellation, or until the wall-clock cap.
 *
 * @param ctx          Context to wait on.  nullptr ⇒ returns false.
 * @param deadline_ns  Absolute monotonic-ns cap on the wait.
 *
 * @return  true  if the wait returned because the ctx was cancelled.
 *          false if the wait returned because @p deadline_ns passed
 *                first (the ctx may still be live).
 */
extern bool n00b_rpc_ctx_wait_until(n00b_rpc_ctx_t *ctx, int64_t deadline_ns);

/**
 * @brief Release waiters and detach from the parent's child list.
 *
 * Idempotent.  After close, `n00b_rpc_ctx_is_cancelled` continues to
 * report the last observed sticky state (so close does NOT clear a
 * cancel).  Close also broadcasts the condition variable so any
 * pending waiters return promptly.
 *
 * Note that close does NOT free the context (memory belongs to the
 * conduit pool); it just unwires the context from the live tree.
 *
 * @param ctx  Context to close; nullptr is a no-op.
 */
extern void n00b_rpc_ctx_close(n00b_rpc_ctx_t *ctx);
