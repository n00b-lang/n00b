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
#define n00b_result_t(T)                                                                       \
    _generic_struct n00b_result_tid(T) {                                                       \
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

/** @brief Extract the value.  Asserts (aborts) if the result is an error. */
#define n00b_result_get(x)                                                                     \
    ({                                                                                         \
        auto _bl_r = (x);                                                                      \
        assert(_bl_r.is_ok);                                                                   \
        *(_bl_r.is_ok ? &_bl_r.ok : (typeof(_bl_r.ok) *)0);                                    \
    })

/** @brief Extract the error.  Asserts (aborts) if the result is ok. */
#define n00b_result_get_err(x)                                                                 \
    ({                                                                                         \
        auto _bl_r = (x);                                                                      \
        assert(!_bl_r.is_ok);                                                                  \
        *(_bl_r.is_ok ? (n00b_err_t *)0 : &_bl_r.err);                                         \
    })

#define n00b_result_get_or_else(x, y)                                                          \
    ({                                                                                         \
        auto _bl_r = (x);                                                                      \
        _bl_r.is_ok ? _bl_r.ok : (y);                                                          \
    })

#define n00b_result_match(x, ok_expr, err_expr) ((x).is_ok ? (ok_expr) : (err_expr))

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
 * Usage: n00b_result_t(void *) r = n00b_check_mmap(nullptr, sz, ...);
 */
#ifdef _WIN32
#define n00b_check_mmap(addr, sz, prot, flags, fd, offset)                                     \
    ({                                                                                         \
        (void)(prot); (void)(flags); (void)(fd); (void)(offset);                               \
        void *_p = VirtualAlloc((addr), (sz),                                                  \
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);                                \
        _p == NULL ? n00b_result_err(void *, ENOMEM)                                           \
                   : n00b_result_ok(void *, _p);                                               \
    })
#else
#define n00b_check_mmap(...)                                                                   \
    ({                                                                                         \
        void *_p = mmap(__VA_ARGS__);                                                          \
        _p == MAP_FAILED ? n00b_result_err(void *, errno)                                      \
                         : n00b_result_ok(void *, _p);                                         \
    })
#endif

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
