/**
 * @file syscall.h
 * @brief Libc-free raw syscalls for async-signal-safe / TSD-independent code.
 *
 * Why this exists: a bare syscall instruction takes no lock and touches no
 * errno-TLS, so it is async-signal-safe AND safe on a thread whose TSD is torn
 * down or wrecked.  Neither is true of libc's `write()` / `_exit()` wrappers —
 * which are also exactly the libc symbols the no-libc effort removes (NCC.md
 * "NO LIBC ALLOWED").  Use these in the crash handler and any other fault- or
 * teardown-context output / process-exit path.
 *
 * Platform status: macOS/arm64 is fully raw (`svc #0x80`, x16 = BSD syscall
 * number — the same sequence `_n00b_darwin_syscall` (thread.c) and
 * `core/futex.h` already use).  Linux arm64/x86-64 are also raw for the small
 * syscall arities needed by crash/worker-thread code; raw-clone workers do not
 * have a full pthread TCB, so libc's cancellation/errno wrappers are unsafe
 * there. Other POSIX still routes through libc's thin `syscall()` trampoline.
 * No-op-free on Windows: not provided (no POSIX fault-handler path).
 */
#pragma once

#if !defined(_WIN32)

#include <stdint.h>      // uintptr_t
#include <sys/syscall.h> // SYS_write / SYS_exit / SYS_exit_group

#if defined(__APPLE__) && defined(__aarch64__)

// Raw BSD syscall via the arm64 unix trap (x16 = number, svc #0x80), returning
// x0.  Mirrors _n00b_darwin_syscall (thread.c); duplicated here so the crash
// handler does not depend on a static-inline buried in thread.c.
static inline long
_n00b_raw_bsd_syscall3(long n, long a0, long a1, long a2)
{
    register long x16 __asm__("x16") = n;
    register long x0 __asm__("x0")   = a0;
    register long x1 __asm__("x1")   = a1;
    register long x2 __asm__("x2")   = a2;
    __asm__ volatile("svc #0x80"
                     : "+r"(x0)
                     : "r"(x16), "r"(x1), "r"(x2)
                     : "cc", "memory");
    return x0;
}

static inline long
_n00b_raw_bsd_syscall5(long n, long a0, long a1, long a2, long a3, long a4)
{
    register long x16 __asm__("x16") = n;
    register long x0 __asm__("x0")   = a0;
    register long x1 __asm__("x1")   = a1;
    register long x2 __asm__("x2")   = a2;
    register long x3 __asm__("x3")   = a3;
    register long x4 __asm__("x4")   = a4;
    __asm__ volatile("svc #0x80"
                     : "+r"(x0)
                     : "r"(x16), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
                     : "cc", "memory");
    return x0;
}

#if !defined(SYS_getentropy)
#error "macOS arm64 requires SYS_getentropy."
#endif

/// Libc-free getentropy(2); macOS limits each request to 256 bytes.
static inline long
n00b_raw_getentropy(void *buf, unsigned long len)
{
    return _n00b_raw_bsd_syscall3(SYS_getentropy,
                                  (long)(uintptr_t)buf,
                                  (long)len,
                                  0);
}

/// Async-signal-safe, libc-free write to @p fd.  Best-effort (return ignored).
static inline void
n00b_raw_write(int fd, const void *buf, unsigned long len)
{
    (void)_n00b_raw_bsd_syscall3(SYS_write,
                                 (long)fd,
                                 (long)(uintptr_t)buf,
                                 (long)len);
}

/// Libc-free immediate whole-process exit (kernel `exit`, no atexit handlers).
[[noreturn]] static inline void
n00b_raw_exit(int code)
{
    (void)_n00b_raw_bsd_syscall3(SYS_exit, (long)code, 0, 0);
    __builtin_unreachable();
}

#elif defined(__linux__)

static inline long
_n00b_raw_linux_syscall1(long n, long a0)
{
#if defined(__aarch64__)
    register long x0 __asm__("x0") = a0;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8)
                     : "cc", "memory");
    return x0;
#elif defined(__x86_64__)
    register long rax __asm__("rax") = n;
    register long rdi __asm__("rdi") = a0;
    __asm__ volatile("syscall"
                     : "+r"(rax)
                     : "r"(rdi)
                     : "rcx", "r11", "cc", "memory");
    return rax;
#else
    return syscall(n, a0);
#endif
}

static inline long
_n00b_raw_linux_syscall2(long n, long a0, long a1)
{
#if defined(__aarch64__)
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x8)
                     : "cc", "memory");
    return x0;
#elif defined(__x86_64__)
    register long rax __asm__("rax") = n;
    register long rdi __asm__("rdi") = a0;
    register long rsi __asm__("rsi") = a1;
    __asm__ volatile("syscall"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi)
                     : "rcx", "r11", "cc", "memory");
    return rax;
#else
    return syscall(n, a0, a1);
#endif
}

static inline long
_n00b_raw_linux_syscall3(long n, long a0, long a1, long a2)
{
#if defined(__aarch64__)
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x8)
                     : "cc", "memory");
    return x0;
#elif defined(__x86_64__)
    register long rax __asm__("rax") = n;
    register long rdi __asm__("rdi") = a0;
    register long rsi __asm__("rsi") = a1;
    register long rdx __asm__("rdx") = a2;
    __asm__ volatile("syscall"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx)
                     : "rcx", "r11", "cc", "memory");
    return rax;
#else
    return syscall(n, a0, a1, a2);
#endif
}

static inline long
_n00b_raw_linux_syscall4(long n, long a0, long a1, long a2, long a3)
{
#if defined(__aarch64__)
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                     : "cc", "memory");
    return x0;
#elif defined(__x86_64__)
    register long rax __asm__("rax") = n;
    register long rdi __asm__("rdi") = a0;
    register long rsi __asm__("rsi") = a1;
    register long rdx __asm__("rdx") = a2;
    register long r10 __asm__("r10") = a3;
    __asm__ volatile("syscall"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                     : "rcx", "r11", "cc", "memory");
    return rax;
#else
    return syscall(n, a0, a1, a2, a3);
#endif
}

static inline long
_n00b_raw_linux_syscall5(long n, long a0, long a1, long a2, long a3, long a4)
{
#if defined(__aarch64__)
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
                     : "cc", "memory");
    return x0;
#elif defined(__x86_64__)
    register long rax __asm__("rax") = n;
    register long rdi __asm__("rdi") = a0;
    register long rsi __asm__("rsi") = a1;
    register long rdx __asm__("rdx") = a2;
    register long r10 __asm__("r10") = a3;
    register long r8 __asm__("r8")   = a4;
    __asm__ volatile("syscall"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8)
                     : "rcx", "r11", "cc", "memory");
    return rax;
#else
    return syscall(n, a0, a1, a2, a3, a4);
#endif
}

static inline long
_n00b_raw_linux_syscall6(long n,
                         long a0,
                         long a1,
                         long a2,
                         long a3,
                         long a4,
                         long a5)
{
#if defined(__aarch64__)
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "cc", "memory");
    return x0;
#elif defined(__x86_64__)
    register long rax __asm__("rax") = n;
    register long rdi __asm__("rdi") = a0;
    register long rsi __asm__("rsi") = a1;
    register long rdx __asm__("rdx") = a2;
    register long r10 __asm__("r10") = a3;
    register long r8 __asm__("r8")   = a4;
    register long r9 __asm__("r9")   = a5;
    __asm__ volatile("syscall"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "cc", "memory");
    return rax;
#else
    return syscall(n, a0, a1, a2, a3, a4, a5);
#endif
}

static inline void
n00b_raw_write(int fd, const void *buf, unsigned long len)
{
    (void)_n00b_raw_linux_syscall3(SYS_write,
                                   (long)fd,
                                   (long)(uintptr_t)buf,
                                   (long)len);
}

[[noreturn]] static inline void
n00b_raw_exit(int code)
{
#if defined(SYS_exit_group)
    (void)_n00b_raw_linux_syscall1(SYS_exit_group, (long)code);
#else
    (void)_n00b_raw_linux_syscall1(SYS_exit, (long)code);
#endif
    __builtin_unreachable();
}

#else // other POSIX: thin syscall() trampoline

#include <unistd.h> // syscall

static inline void
n00b_raw_write(int fd, const void *buf, unsigned long len)
{
    (void)syscall(SYS_write, fd, buf, len);
}

[[noreturn]] static inline void
n00b_raw_exit(int code)
{
#if defined(SYS_exit_group)
    (void)syscall(SYS_exit_group, code); // terminate the whole process
#else
    (void)syscall(SYS_exit, code);
#endif
    __builtin_unreachable();
}

#endif // __APPLE__ && __aarch64__

#else // _WIN32

// Windows has no raw-syscall path available to us. The toolchain targets the
// MSVC ABI (windows-component.yml passes --target=x86_64-pc-windows-msvc), so
// there is no <unistd.h> write() and no syscall() trampoline to wrap.
//
// WriteFile on a raw HANDLE is the closest equivalent to the POSIX raw writes
// above: a thin wrapper over NtWriteFile that does not allocate, does not touch
// the CRT, and takes no user-mode lock this process holds. That last property
// is the whole point -- callers reach for this primitive precisely when they
// cannot use anything richer: a reader that has decided a bucket mutex is
// stranded (#296), and the crash paths.
//
// Deliberately NOT _write()/_get_osfhandle(): both route through the CRT's fd
// table, which allocates on first use and takes a per-file lock. A CRT-backed
// implementation would compile, link, pass CI, and reintroduce exactly the
// deadlock class this primitive exists to avoid -- the worst possible outcome,
// because nothing here would catch it.

#include <stdint.h> // intptr_t

// Declares GetStdHandle and WriteFile rather than including a Windows header.
// Five revisions established the constraints; all are recorded because none is
// obvious and the next person to touch this will hit one.
//
//  1. <windows.h> here breaks the platform.h path. platform.h includes it at
//     :53 and this header at :420, so a second entry through a different chain
//     re-opens winnt.h under different conditions:
//
//       winnt.h:4304: error: type 'struct _CONTEXT' has incompatible definitions
//
//     Matching platform.h's WIN32_LEAN_AND_MEAN/NOMINMAX guards does NOT help:
//     an include guard no-ops the top-level header, not the nested ones reached
//     by a different path.
//
//  2. <processenv.h> + <fileapi.h>, the two headers that actually declare
//     these, cannot be included standalone -- they assume the architecture
//     macro windows.h defines:
//
//       winnt.h:169: error: "No Target Architecture"
//
//  3. Declaring nothing and relying on platform.h fails on the direct-include
//     path. TEN files include "core/syscall.h" directly -- exit.c, thread.c,
//     stw.c, signals.c, crash.c, crash_capture.c, memory_info.c, file.c,
//     io_epoll.c, quic/metrics.c -- and there nothing has provided them.
//
//  4. The final parameter must be `void *`, NOT `struct _OVERLAPPED *`.
//     include/internal/win32_sockets.h:478 ALREADY declares WriteFile for this
//     tree, with `void *overlapped`, and a differing declaration collides:
//
//       win32_sockets.h:478: error: conflicting types for 'WriteFile'
//
//     That header also declares GetStdHandle (:455). These declarations
//     therefore deliberately MATCH it rather than the SDK's spelling --
//     `void *` and `struct _OVERLAPPED *` are compatible against the SDK
//     (both are pointer-to-object), but not against each other, and this tree
//     had already chosen `void *`.
//
//  5. No `__declspec(dllimport)`. win32_sockets.h declares these without it
//     (it uses dllimport nowhere), and mixing the two spellings of the same
//     function is itself a redeclaration conflict whose winner depends on
//     include order. Matching its plain form makes this order-independent.
//     Omitting dllimport costs only an indirection thunk.
//
//  6. Spelled with the underlying types (`void *`, `unsigned long`, `int`)
//     rather than HANDLE/DWORD/BOOL. platform.h defines those typedefs at
//     :66-81, before it includes this header at :420 -- but on the
//     direct-include path of failure 3 they are not in scope. The raw types
//     are identical after expansion (platform.h:66,72,75) and need nothing
//     declared first, so one spelling works on both paths.
//
// Not including win32_sockets.h instead: it is an internal header that pulls
// winsock2/ws2tcpip/afunix/windows.h on the _WINDOWS path, which is failure
// mode 1 again.
void *__attribute__((__stdcall__)) GetStdHandle(unsigned long std_handle);
int __attribute__((__stdcall__)) WriteFile(void          *file,
                                           const void    *buffer,
                                           unsigned long  to_write,
                                           unsigned long *written,
                                           void          *overlapped);

// From winbase.h / handleapi.h, which we are not including.
#define N00B_STD_OUTPUT_HANDLE ((unsigned long)-11)
#define N00B_STD_ERROR_HANDLE  ((unsigned long)-12)
#define N00B_INVALID_HANDLE    ((void *)(intptr_t)-1)

/// Best-effort, CRT-free write to @p fd. Matches the POSIX contract above:
/// one write, return ignored, no retry on a short write.
///
/// Windows resolves only fds 1 and 2, through GetStdHandle -- which honours
/// SetStdHandle redirection. Any other fd is a silent no-op: resolving an
/// arbitrary fd needs _get_osfhandle, i.e. the CRT, which this primitive must
/// not touch. Every caller that passes a non-standard fd today
/// (crash_capture.c, crash.c's log_fd) is itself inside #if !defined(_WIN32),
/// so nothing currently relies on the general case.
static inline void
n00b_raw_write(int fd, const void *buf, unsigned long len)
{
    unsigned long which;

    switch (fd) {
    case 1:
        which = N00B_STD_OUTPUT_HANDLE;
        break;
    case 2:
        which = N00B_STD_ERROR_HANDLE;
        break;
    default:
        return;
    }

    void *h = GetStdHandle(which);

    // A service started with no console gets NULL; a failed lookup gets
    // INVALID_HANDLE_VALUE. Neither is an error worth reacting to from a
    // best-effort diagnostic, and faulting here would turn the message
    // describing a problem into a second, worse one.
    if (h == nullptr || h == N00B_INVALID_HANDLE) {
        return;
    }

    unsigned long written = 0;
    (void)WriteFile(h, buf, (unsigned long)len, &written, nullptr);
}

#endif // !_WIN32 / _WIN32
