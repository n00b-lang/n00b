/**
 * @file assert.h
 * @brief Debug-only `n00b_assert`, always-on `n00b_require`, and
 *        `n00b_unreachable` macros.
 *
 * `n00b_assert` and `n00b_require` evaluate their condition and, on
 * failure, write a diagnostic to stderr via `n00b_eprintf` and call
 * `n00b_abort()`.
 *
 * - `n00b_assert(expr)` is a no-op unless `N00B_DEBUG` is defined.
 * - `n00b_require(cond, msg)` is always active and includes the caller's
 *   message in the diagnostic.
 * - `n00b_unreachable()` is a thin wrapper around `__builtin_unreachable()`;
 *   it tells the compiler the location is dead code.  Do not use it for
 *   reachable error paths — call `n00b_panic` or `n00b_require` instead.
 */
#pragma once

#define n00b_unreachable() __builtin_unreachable()

[[noreturn]] extern void
_n00b_assert_failed(const char *expr,
                    const char *func,
                    const char *file,
                    int         line);

[[noreturn]] extern void
_n00b_require_failed(const char *cond,
                     const char *msg,
                     const char *func,
                     const char *file,
                     int         line);

#ifdef N00B_DEBUG
#define n00b_assert(expr)                       \
    do {                                        \
        if (!(expr)) {                          \
            _n00b_assert_failed(#expr,          \
                                __func__,       \
                                __FILE__,       \
                                __LINE__);      \
        }                                       \
    } while (0)
#else
#define n00b_assert(expr) ((void)0)
#endif

#define n00b_require(cond, msg)                 \
    do {                                        \
        if (!(cond)) {                          \
            _n00b_require_failed(#cond,         \
                                 (msg),         \
                                 __func__,      \
                                 __FILE__,      \
                                 __LINE__);     \
        }                                       \
    } while (0)
