#ifndef N00B_BUILTIN_OS_SYSCALL_H
#define N00B_BUILTIN_OS_SYSCALL_H

#if !defined(__linux__)
#error "raw Linux syscalls are only available on Linux"
#endif

#include <asm/unistd.h>

static inline long n00b_linux_syscall6(long number, long a, long b, long c,
                                        long d, long e, long f)
{
#if defined(__x86_64__)
    register long rax __asm__("rax") = number;
    register long rdi __asm__("rdi") = a;
    register long rsi __asm__("rsi") = b;
    register long rdx __asm__("rdx") = c;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "cc", "memory");
    return rax;
#elif defined(__aarch64__)
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    register long x8 __asm__("x8") = number;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3),
                       "r"(x4), "r"(x5), "r"(x8)
                     : "cc", "memory");
    return x0;
#else
#error "unsupported Linux syscall architecture"
#endif
}

#endif
