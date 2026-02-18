/**
 * @file result.h
 * @brief Type-safe result values using typeid().
 *
 * @c n00b_result_t(T) represents a value of type @p T on success,
 * or an @c n00b_err_t on failure.
 */
#pragma once

typedef int n00b_err_t;

#define n00b_result_tid(T) typeid("result", T)
#define n00b_result_t(T)   struct n00b_result_tid(T)

#define n00b_result_decl(T)                                                                    \
    struct n00b_result_tid(T) {                                                                \
        bool is_ok;                                                                            \
        union {                                                                                \
            T          ok;                                                                     \
            n00b_err_t err;                                                                    \
        };                                                                                     \
    }

#define n00b_result_ok(T, x)                                                                   \
    ((n00b_result_t(T)){                                                                       \
        .is_ok = true,                                                                         \
        .ok    = (x),                                                                          \
    })

#define n00b_result_err(T, e)                                                                  \
    ((n00b_result_t(T)){                                                                       \
        .is_ok = false,                                                                        \
        .err   = (e),                                                                          \
    })

#define n00b_result_is_ok(x)  ((x).is_ok)
#define n00b_result_is_err(x) (!(x).is_ok)

/** @brief Extract the value.  Crashes (null deref) if the result is an error. */
#define n00b_result_get(x)                                                                     \
    ({                                                                                         \
        auto _bl_r = (x);                                                                      \
        *(_bl_r.is_ok ? &_bl_r.ok : (void *)0);                                                \
    })

/** @brief Extract the error.  Crashes (null deref) if the result is ok. */
#define n00b_result_get_err(x)                                                                 \
    ({                                                                                         \
        auto _bl_r = (x);                                                                      \
        *(_bl_r.is_ok ? (void *)0 : &_bl_r.err);                                               \
    })

#define n00b_result_get_or_else(x, y)                                                          \
    ({                                                                                         \
        auto _bl_r = (x);                                                                      \
        _bl_r.is_ok ? _bl_r.ok : (y);                                                          \
    })

#define n00b_result_match(x, ok_expr, err_expr) ((x).is_ok ? (ok_expr) : (err_expr))

// ============================================================================
// Common result type declarations
// ============================================================================

n00b_result_decl(int);
n00b_result_decl(void *);
n00b_result_decl(uint64_t);

// ============================================================================
// Stdlib wrapper macros — produce n00b_result_t values from system calls
// ============================================================================

/**
 * @brief Wrap a POSIX call that returns 0 on success, nonzero on error.
 * Usage: n00b_result_t(int) r = n00b_check_posix(getrlimit(...));
 */
#define n00b_check_posix(call)                                                                 \
    ({                                                                                         \
        int _rc = (call);                                                                      \
        _rc ? n00b_result_err(int, errno)                                                      \
            : n00b_result_ok(int, 0);                                                          \
    })

/**
 * @brief Wrap an mmap() call — returns MAP_FAILED on error.
 * Usage: n00b_result_t(void *) r = n00b_check_mmap(NULL, sz, ...);
 */
#define n00b_check_mmap(...)                                                                   \
    ({                                                                                         \
        void *_p = mmap(__VA_ARGS__);                                                          \
        _p == MAP_FAILED ? n00b_result_err(void *, errno)                                      \
                         : n00b_result_ok(void *, _p);                                         \
    })

/**
 * @brief Wrap a sysconf() call — returns -1 on error.
 * Usage: n00b_result_t(int) r = n00b_check_sysconf(_SC_PAGESIZE);
 */
#define n00b_check_sysconf(name)                                                               \
    ({                                                                                         \
        long _v = sysconf(name);                                                               \
        _v == -1 ? n00b_result_err(int, errno)                                                 \
                 : n00b_result_ok(int, (int)_v);                                               \
    })
