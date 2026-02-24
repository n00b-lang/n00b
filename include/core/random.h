/**
 * @file random.h
 * @brief Cryptographic-quality pseudo-random number generation.
 *
 * On Linux, uses the getrandom(2) system call.  On macOS/BSD, uses
 * arc4random_buf().  Convenience typed wrappers (n00b_rand8 .. n00b_rand64)
 * are generated via macro.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include("n00b_build_config.h")
#include "n00b_build_config.h"
#endif
#endif

#if !defined(N00B_HAVE_GETRANDOM)
#if defined(__linux__)
#define N00B_HAVE_GETRANDOM 1
#else
#define N00B_HAVE_GETRANDOM 0
#endif
#endif

#if !defined(N00B_HAVE_ARC4RANDOM_BUF)
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)  \
    || defined(__DragonFly__)
#define N00B_HAVE_ARC4RANDOM_BUF 1
#else
#define N00B_HAVE_ARC4RANDOM_BUF 0
#endif
#endif

#if !defined(N00B_HAVE_BCRYPT_GEN_RANDOM)
#if defined(_WIN32)
#define N00B_HAVE_BCRYPT_GEN_RANDOM 1
#else
#define N00B_HAVE_BCRYPT_GEN_RANDOM 0
#endif
#endif

// On Linux we are best off using the low-level interface to the
// system PRNG.  On other Unix systems, we fall back to
// `arc4random_buf()` which is widely available.

#if N00B_HAVE_GETRANDOM
#include <sys/random.h>
#include <stdlib.h> // for abort

static inline void
n00b_random_bytes(char *bufptr, size_t len)
{
    if (getrandom(bufptr, len, GRND_NONBLOCK) == -1) {
        abort();
    }
}
#elif N00B_HAVE_ARC4RANDOM_BUF

// IWYU Picks this up on the mac, but isn't wanted.
// IWYU pragma: no_include <_stdlib.h>
// for arc4random_buf
#include <stdlib.h> // IWYU pragma: keep
#define n00b_random_bytes(bufptr, len) arc4random_buf(bufptr, len)
#elif N00B_HAVE_BCRYPT_GEN_RANDOM
#include "n00b_windows_compat.h"
#include <stdlib.h> // for abort

static inline void
n00b_random_bytes(char *bufptr, size_t len)
{
    while (len) {
        ULONG chunk = len > UINT32_MAX ? UINT32_MAX : (ULONG)len;
        NTSTATUS rc = BCryptGenRandom(nullptr,
                                      (PUCHAR)bufptr,
                                      chunk,
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);

        if (rc != 0) {
            abort();
        }

        bufptr += chunk;
        len -= chunk;
    }
}
#else
#error "No supported random source available."
#endif

// clang-format off
#define N00B_DECLARE_RAND_FN(type_name, designator)        \
                                                           \
static inline type_name                                    \
n00b_rand##designator(void)                                \
{                                                          \
    type_name result;                                      \
                                                           \
    n00b_random_bytes((char *)&result, sizeof(type_name)); \
                                                           \
    return result;                                         \
}
// clang-format on

// Explicitly add the prototypes for the sake of grep, etc.
static inline uint64_t n00b_rand64(void);
static inline uint32_t n00b_rand32(void);
static inline uint16_t n00b_rand16(void);
static inline uint8_t  n00b_rand8(void);
static inline int64_t  n00b_rand_i64(void);
static inline int32_t  n00b_rand_i32(void);
static inline int16_t  n00b_rand_i16(void);
static inline int8_t   n00b_rand_i8(void);

N00B_DECLARE_RAND_FN(uint64_t, 64);
N00B_DECLARE_RAND_FN(int64_t, _i64);
N00B_DECLARE_RAND_FN(uint32_t, 32);
N00B_DECLARE_RAND_FN(int32_t, _i32);
N00B_DECLARE_RAND_FN(uint16_t, 16);
N00B_DECLARE_RAND_FN(int16_t, _i16);
N00B_DECLARE_RAND_FN(uint8_t, 8);
N00B_DECLARE_RAND_FN(int8_t, _i8);
