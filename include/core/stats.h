/**
 * @file stats.h
 * @brief Online running statistics.
 *
 * Two small primitives that show up everywhere we observe a stream of
 * numbers:
 *
 *   - @ref n00b_running_stats_t — Welford's algorithm for the running
 *     mean and (sample) variance of a sequence, in O(1) space and one
 *     pass per observation.  Numerically stable: it avoids the classic
 *     `Σx²` cancellation when the values cluster far from zero.
 *
 *   - @ref n00b_ewma_t — exponentially-weighted moving average with
 *     smoothing factor `α ∈ (0, 1]`.  The first observation primes the
 *     average; subsequent observations update via
 *     `value ← α·x + (1−α)·value`.
 *
 * Both are header-only — they compile down to a few lines per call and
 * don't need an out-of-line implementation.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

// ============================================================================
// Welford running mean / variance
// ============================================================================

typedef struct {
    uint64_t count; /**< Number of observations seen so far. */
    double   mean;  /**< Running arithmetic mean. */
    double   m2;    /**< Σ(x − mean)² accumulated for variance. */
} n00b_running_stats_t;

static inline void
n00b_running_stats_init(n00b_running_stats_t *s)
{
    s->count = 0;
    s->mean  = 0.0;
    s->m2    = 0.0;
}

static inline void
n00b_running_stats_observe(n00b_running_stats_t *s, double x)
{
    s->count++;
    double delta  = x - s->mean;
    s->mean      += delta / (double)s->count;
    double delta2 = x - s->mean;
    s->m2        += delta * delta2;
}

static inline uint64_t
n00b_running_stats_count(const n00b_running_stats_t *s)
{
    return s->count;
}

static inline double
n00b_running_stats_mean(const n00b_running_stats_t *s)
{
    return s->mean;
}

/**
 * @brief Sample variance (Bessel's correction, divides by n-1).
 * @return 0 when fewer than two observations have been seen.
 */
static inline double
n00b_running_stats_variance(const n00b_running_stats_t *s)
{
    if (s->count < 2) return 0.0;
    return s->m2 / (double)(s->count - 1);
}

static inline double
n00b_running_stats_stddev(const n00b_running_stats_t *s)
{
    return sqrt(n00b_running_stats_variance(s));
}

// ============================================================================
// Exponentially weighted moving average
// ============================================================================

typedef struct {
    double value;  /**< Current EWMA value (undefined until primed). */
    double alpha;  /**< Smoothing factor in (0, 1]. */
    bool   primed; /**< False until the first @ref n00b_ewma_observe. */
} n00b_ewma_t;

/**
 * @brief Initialize an EWMA with smoothing factor @p alpha.
 *
 * Higher alpha means the EWMA reacts faster but is noisier; alpha=1
 * collapses to "last value seen", alpha→0 to "long-term average".
 */
static inline void
n00b_ewma_init(n00b_ewma_t *e, double alpha)
{
    e->value  = 0.0;
    e->alpha  = alpha;
    e->primed = false;
}

static inline void
n00b_ewma_observe(n00b_ewma_t *e, double x)
{
    if (!e->primed) {
        e->value  = x;
        e->primed = true;
    } else {
        e->value = e->alpha * x + (1.0 - e->alpha) * e->value;
    }
}

static inline double
n00b_ewma_value(const n00b_ewma_t *e)
{
    return e->value;
}
