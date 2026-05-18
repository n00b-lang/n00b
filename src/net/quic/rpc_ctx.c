/*
 * rpc_ctx.c — gRPC-style cancellation + deadline context.
 *
 * The implementation: each ctx is a struct with
 *   - sticky cancel flag (atomic uint32 used as a futex word: 0 = live,
 *     1 = cancelled).
 *   - monotonic deadline_ns (-1 = no deadline).
 *   - parent ptr + child intrusive list.
 *   - mutex covering the child list.
 *
 * Concurrency:
 *   - is_cancelled: atomic load + deadline compare (lock-free).
 *   - cancel: atomic CAS on the flag; if flipped 0→1, futex_wake_all;
 *     then walk the child list (lock per ctx) and recurse.
 *   - close: detach from parent list (under parent lock); broadcast.
 *   - wait: read flag, futex_wait if 0, repeat until observed-cancelled
 *     or timeout.
 *
 * Cascading cancellation is depth-first.  Each ctx's mutex is held
 * only briefly per node; we don't take a parent + child lock at the
 * same time (no inversion risk).
 */

#define N00B_USE_INTERNAL_API
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/atomic.h"
#include "core/data_lock.h"
#include "core/futex.h"
#include "core/time.h"
#include "net/quic/rpc_ctx.h"

/* ===========================================================================
 * Layout
 * =========================================================================== */

#define CTX_LIVE       0u
#define CTX_CANCELLED  1u

struct n00b_rpc_ctx {
    n00b_futex_t       cancelled;     /* 0 = live, 1 = cancelled */
    int64_t            deadline_ns;   /* -1 = none */

    n00b_rwlock_t     *lock;          /* covers parent + child list + closed */
    bool               closed;

    /* Tree links. */
    struct n00b_rpc_ctx *parent;          /* nullable */
    struct n00b_rpc_ctx *first_child;
    struct n00b_rpc_ctx *next_sibling;
    struct n00b_rpc_ctx *prev_sibling;
};

static n00b_allocator_t *
ctx_alloc(void)
{
    return (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
}

/* ===========================================================================
 * Construction
 * =========================================================================== */

static n00b_rpc_ctx_t *
ctx_new_inner(n00b_rpc_ctx_t *parent, int64_t deadline_ns)
{
    n00b_rpc_ctx_t *c = n00b_alloc_with_opts(n00b_rpc_ctx_t,
        &(n00b_alloc_opts_t){.allocator = ctx_alloc()});
    n00b_futex_init(&c->cancelled);
    c->deadline_ns  = deadline_ns;
    c->closed       = false;
    c->parent       = nullptr;
    c->first_child  = nullptr;
    c->next_sibling = nullptr;
    c->prev_sibling = nullptr;
    c->lock = n00b_data_lock_new();

    if (parent) {
        n00b_data_write_lock(parent->lock);
        if (parent->closed) {
            /* Parent already closed: child starts cancelled. */
            n00b_data_unlock(parent->lock);
            n00b_atomic_store(&c->cancelled, CTX_CANCELLED);
            return c;
        }
        /* Inherit parent's cancel: if parent is already cancelled,
         * child is born cancelled. */
        bool parent_cancelled =
            (n00b_atomic_load(&parent->cancelled) == CTX_CANCELLED);
        if (parent_cancelled) {
            n00b_data_unlock(parent->lock);
            n00b_atomic_store(&c->cancelled, CTX_CANCELLED);
            return c;
        }
        /* Inherit parent's deadline (min). */
        if (parent->deadline_ns >= 0) {
            if (c->deadline_ns < 0
                || parent->deadline_ns < c->deadline_ns) {
                c->deadline_ns = parent->deadline_ns;
            }
        }
        /* Link into parent's child list (head insert). */
        c->parent = parent;
        c->next_sibling = parent->first_child;
        if (parent->first_child) {
            parent->first_child->prev_sibling = c;
        }
        parent->first_child = c;
        n00b_data_unlock(parent->lock);
    }
    return c;
}

n00b_rpc_ctx_t *
n00b_rpc_ctx_new(void)
{
    return ctx_new_inner(nullptr, -1);
}

n00b_rpc_ctx_t *
n00b_rpc_ctx_with_deadline(n00b_rpc_ctx_t *parent, int64_t deadline_ns)
{
    return ctx_new_inner(parent, deadline_ns);
}

n00b_rpc_ctx_t *
n00b_rpc_ctx_with_cancel(n00b_rpc_ctx_t *parent)
{
    return ctx_new_inner(parent, -1);
}

/* ===========================================================================
 * Cancel
 * =========================================================================== */

/* Cancel a single ctx and wake its waiters; returns true if this call
 * was the one that flipped the state (so the caller knows to recurse
 * into children). */
static bool
ctx_cancel_self(n00b_rpc_ctx_t *ctx)
{
    uint32_t expected = CTX_LIVE;
    bool flipped =
        atomic_compare_exchange_strong((_Atomic uint32_t *)&ctx->cancelled,
                                       &expected, CTX_CANCELLED);
    if (flipped) {
        n00b_futex_wake_all(&ctx->cancelled);
    }
    return flipped;
}

static void
cancel_descendants(n00b_rpc_ctx_t *ctx)
{
    /* Snapshot the child pointer under the lock so children added
     * concurrently after this point are born-cancelled (handled in
     * ctx_new_inner). */
    n00b_data_write_lock(ctx->lock);
    n00b_rpc_ctx_t *child = ctx->first_child;
    n00b_data_unlock(ctx->lock);
    while (child) {
        if (ctx_cancel_self(child)) {
            cancel_descendants(child);
        }
        /* next_sibling is stable: detachment writes parent + sibling
         * links under the parent's lock; we're walking children of
         * `ctx` and reading their next_sibling without locks.  Race
         * tolerated: a detaching child observed mid-walk gets cancelled
         * once via the snapshot read, which is fine (cancel is sticky). */
        n00b_data_write_lock(child->lock);
        n00b_rpc_ctx_t *next = child->next_sibling;
        n00b_data_unlock(child->lock);
        child = next;
    }
}

void
n00b_rpc_ctx_cancel(n00b_rpc_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx_cancel_self(ctx)) {
        cancel_descendants(ctx);
    }
}

/* ===========================================================================
 * is_cancelled / remaining_ns
 * =========================================================================== */

bool
n00b_rpc_ctx_is_cancelled(n00b_rpc_ctx_t *ctx)
{
    if (!ctx) return false;
    if (n00b_atomic_load(&ctx->cancelled) == CTX_CANCELLED) {
        return true;
    }
    if (ctx->deadline_ns >= 0
        && n00b_ns_timestamp() >= ctx->deadline_ns) {
        /* Deadline passed: trip the sticky flag, broadcast.  Do NOT
         * cascade — descendants see their own deadlines (or inherited
         * ones); a deadline-elapsed parent doesn't necessarily mean a
         * child's deadline has elapsed (child could have a tighter
         * deadline that's still in the future... wait, no — children
         * inherit `min`, so if parent is past deadline, child must be
         * too).  We DO cascade for symmetry with explicit cancel. */
        if (ctx_cancel_self(ctx)) {
            cancel_descendants(ctx);
        }
        return true;
    }
    return false;
}

int64_t
n00b_rpc_ctx_remaining_ns(n00b_rpc_ctx_t *ctx)
{
    if (!ctx) return -1;
    if (n00b_atomic_load(&ctx->cancelled) == CTX_CANCELLED) return 0;
    if (ctx->deadline_ns < 0) return -1;
    int64_t now = n00b_ns_timestamp();
    int64_t rem = ctx->deadline_ns - now;
    return rem < 0 ? 0 : rem;
}

/* ===========================================================================
 * Wait
 * =========================================================================== */

bool
n00b_rpc_ctx_wait_until(n00b_rpc_ctx_t *ctx, int64_t deadline_ns)
{
    if (!ctx) return false;
    while (1) {
        if (n00b_rpc_ctx_is_cancelled(ctx)) return true;

        int64_t now = n00b_ns_timestamp();
        if (deadline_ns >= 0 && now >= deadline_ns) return false;

        /* Compute how long to wait.  If the ctx has its own deadline,
         * choose the earliest of (caller deadline, ctx deadline). */
        int64_t target = deadline_ns;
        if (ctx->deadline_ns >= 0
            && (target < 0 || ctx->deadline_ns < target)) {
            target = ctx->deadline_ns;
        }
        uint64_t wait_ns;
        if (target < 0) {
            /* No deadline at all — wait "forever" (pick a long bound;
             * we'll re-check the cancel flag on wakeup). */
            wait_ns = (uint64_t)10 * 1000 * 1000 * 1000;  /* 10s */
        } else {
            int64_t r = target - now;
            wait_ns = r < 0 ? 0 : (uint64_t)r;
            if (wait_ns == 0) {
                /* Re-check cancel one more time (fall-through) and
                 * return. */
                if (n00b_rpc_ctx_is_cancelled(ctx)) return true;
                return false;
            }
        }
        /* Block on the futex.  Expected value = LIVE; futex_wake_all
         * after cancel will release us.  Spurious wakeups + timeout
         * are both handled by the loop. */
        (void)n00b_futex_wait(&ctx->cancelled, CTX_LIVE, wait_ns);
        /* Loop: re-check cancel + clock. */
    }
}

void
n00b_rpc_ctx_wait(n00b_rpc_ctx_t *ctx)
{
    /* No external deadline; wait until cancel or ctx's own deadline. */
    (void)n00b_rpc_ctx_wait_until(ctx, -1);
}

/* ===========================================================================
 * Close
 * =========================================================================== */

void
n00b_rpc_ctx_close(n00b_rpc_ctx_t *ctx)
{
    if (!ctx) return;
    n00b_data_write_lock(ctx->lock);
    if (ctx->closed) {
        n00b_data_unlock(ctx->lock);
        return;
    }
    ctx->closed = true;
    n00b_rpc_ctx_t *parent = ctx->parent;
    n00b_data_unlock(ctx->lock);

    /* Detach from parent's child list (must be under parent's lock). */
    if (parent) {
        n00b_data_write_lock(parent->lock);
        if (ctx->prev_sibling) {
            ctx->prev_sibling->next_sibling = ctx->next_sibling;
        } else if (parent->first_child == ctx) {
            parent->first_child = ctx->next_sibling;
        }
        if (ctx->next_sibling) {
            ctx->next_sibling->prev_sibling = ctx->prev_sibling;
        }
        ctx->parent       = nullptr;
        ctx->prev_sibling = nullptr;
        ctx->next_sibling = nullptr;
        n00b_data_unlock(parent->lock);
    }

    /* Wake any waiters; they'll re-check the cancel flag.  We
     * deliberately do NOT auto-cancel on close — close is "I'm done
     * with this handle"; the cancel state is sticky and reflects what
     * actually happened. */
    n00b_futex_wake_all(&ctx->cancelled);
}
