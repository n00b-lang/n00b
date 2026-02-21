/**
 * @file platform.h
 * @brief Cross-platform abstractions for the base library.
 *
 * Centralizes platform detection and provides portable types/functions
 * for threads, TLS, sleep, clock, and sockets.
 */
#pragma once

#include <stdint.h>
// ============================================================================
// Platform detection
// ============================================================================

#if defined(_WIN32) || defined(_WIN64)
#define BASE_PLATFORM_WINDOWS 1  /**< Defined when targeting Windows. */
#elif defined(__APPLE__) && defined(__MACH__)
#define BASE_PLATFORM_MACOS   1  /**< Defined when targeting macOS. */
#elifdef __linux__
#define BASE_PLATFORM_LINUX   1  /**< Defined when targeting Linux. */
#else
#error "Unsupported platform"
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define BASE_ARCH_ARM64  1  /**< Defined when targeting ARM64 (AArch64). */
#elif defined(__x86_64__) || defined(_M_X64)
#define BASE_ARCH_X86_64 1  /**< Defined when targeting x86-64. */
#else
#error "Unsupported architecture"
#endif

/** @brief Default memory alignment (16 bytes). */
#define BASE_ALIGN 16

#ifdef BASE_PLATFORM_WINDOWS

// ============================================================================
// Windows
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602  // Windows 8 (for WaitOnAddress)
#endif
#include <windows.h>

// Missing POSIX types
typedef intptr_t ssize_t;

/** @brief Thread identifier type (Windows). */
typedef DWORD  base_thread_id_t;

/** @brief Thread handle type (Windows). */
typedef HANDLE base_thread_handle_t;

/** @brief Thread-local storage key type (Windows FLS). */
typedef DWORD base_tls_key_t;

/**
 * @brief Retrieve the value stored in a TLS slot.
 *
 * @param key  TLS key created by the platform TLS API.
 * @return Pointer to the thread-local value, or @c nullptr if not set.
 */
static inline void *
base_tls_get(base_tls_key_t key)
{
    return FlsGetValue(key);
}

/**
 * @brief Store a value in a TLS slot.
 *
 * @param key    TLS key created by the platform TLS API.
 * @param value  Pointer to store as the thread-local value.
 */
static inline void
base_tls_set(base_tls_key_t key, void *value)
{
    FlsSetValue(key, value);
}

/** @brief One-time initialization type (Windows). */
typedef INIT_ONCE base_once_t;

/** @brief Static initializer for @ref base_once_t. */
#define BASE_ONCE_INIT INIT_ONCE_STATIC_INIT

/** @brief Process identifier type (Windows). */
typedef DWORD base_pid_t;

/**
 * @brief Sleep for the specified number of nanoseconds.
 *
 * On Windows the actual granularity is milliseconds.
 *
 * @param ns  Duration in nanoseconds.
 */
static inline void
base_nanosleep_ns(uint64_t ns)
{
    Sleep((DWORD)(ns / 1000000));
}

/**
 * @brief Get a monotonic timestamp in milliseconds.
 *
 * @return Monotonic time in milliseconds.
 */
static inline uint64_t
base_monotonic_ms(void)
{
    return GetTickCount64();
}

/**
 * @brief Get a monotonic timestamp in nanoseconds.
 *
 * @return Monotonic time in nanoseconds.
 */
static inline uint64_t
base_monotonic_ns(void)
{
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)((double)count.QuadPart / freq.QuadPart * 1000000000.0);
}

/** @brief Socket descriptor type (matches Windows SOCKET = UINT_PTR). */
typedef UINT_PTR base_socket_t;

/** @brief Invalid socket sentinel value (matches INVALID_SOCKET). */
#define BASE_INVALID_SOCKET ((base_socket_t)~0)

/**
 * @brief Close a socket descriptor.
 *
 * Calls the Winsock closesocket() function. Callers using sockets
 * must link against ws2_32.
 *
 * @param s  Socket to close.
 * @return 0 on success, or a socket error code.
 */
int __attribute__((__stdcall__)) closesocket(base_socket_t);

static inline int
base_closesocket(base_socket_t s)
{
    return closesocket(s);
}

#else

// ============================================================================
// POSIX (Linux + macOS)
// ============================================================================

#include <pthread.h>
#include <time.h>
#include <unistd.h>

/** @brief Thread identifier type (POSIX). */
typedef pthread_t base_thread_id_t;

/** @brief Thread handle type (POSIX). */
typedef pthread_t base_thread_handle_t;

/** @brief Thread-local storage key type (POSIX). */
typedef pthread_key_t base_tls_key_t;

/**
 * @brief Retrieve the value stored in a TLS slot.
 *
 * @param key  TLS key created via @c pthread_key_create.
 * @return Pointer to the thread-local value, or @c nullptr if not set.
 */
static inline void *
base_tls_get(base_tls_key_t key)
{
    return pthread_getspecific(key);
}

/**
 * @brief Store a value in a TLS slot.
 *
 * @param key    TLS key created via @c pthread_key_create.
 * @param value  Pointer to store as the thread-local value.
 */
static inline void
base_tls_set(base_tls_key_t key, void *value)
{
    pthread_setspecific(key, value);
}

/** @brief One-time initialization type (POSIX). */
typedef pthread_once_t base_once_t;

/** @brief Static initializer for @ref base_once_t. */
#define BASE_ONCE_INIT PTHREAD_ONCE_INIT

/** @brief Process identifier type (POSIX). */
typedef pid_t base_pid_t;

/**
 * @brief Sleep for the specified number of nanoseconds.
 *
 * @param ns  Duration in nanoseconds.
 */
static inline void
base_nanosleep_ns(uint64_t ns)
{
    struct timespec ts = {
        .tv_sec  = (time_t)(ns / 1000000000ULL),
        .tv_nsec = (long)(ns % 1000000000ULL),
    };
    nanosleep(&ts, nullptr);
}

/**
 * @brief Get a monotonic timestamp in milliseconds.
 *
 * @return Monotonic time in milliseconds.
 */
static inline uint64_t
base_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/**
 * @brief Get a monotonic timestamp in nanoseconds.
 *
 * @return Monotonic time in nanoseconds.
 */
static inline uint64_t
base_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/** @brief Socket descriptor type (POSIX file descriptor). */
typedef int base_socket_t;

/** @brief Invalid socket sentinel value. */
#define BASE_INVALID_SOCKET (-1)

/**
 * @brief Close a socket descriptor.
 *
 * @param s  Socket file descriptor to close.
 * @return 0 on success, -1 on error.
 */
static inline int
base_closesocket(base_socket_t s)
{
    return close(s);
}

#endif // _WIN32

// ============================================================================
// Common thread helpers
// ============================================================================

/**
 * @brief Get the current thread's identifier.
 *
 * @return The calling thread's ID.
 */
static inline base_thread_id_t
base_current_thread_id(void)
{
#ifdef BASE_PLATFORM_WINDOWS
    return GetCurrentThreadId();
#else
    return pthread_self();
#endif
}

/**
 * @brief Compare two thread identifiers for equality.
 *
 * @param a  First thread ID.
 * @param b  Second thread ID.
 * @return @c true if the IDs refer to the same thread.
 */
static inline bool
base_thread_id_equal(base_thread_id_t a, base_thread_id_t b)
{
#ifdef BASE_PLATFORM_WINDOWS
    return a == b;
#else
    return pthread_equal(a, b) != 0;
#endif
}

// ============================================================================
// Page size
// ============================================================================

/**
 * @brief Get the system memory page size.
 *
 * @return Page size in bytes.
 */
static inline size_t
base_page_size(void)
{
#ifdef BASE_PLATFORM_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    return (size_t)sysconf(_SC_PAGESIZE);
#endif
}
