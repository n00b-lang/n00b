#include "builtin_os.h"

_Static_assert(sizeof(void *) == 8, "thread capture requires 64-bit pointers");
_Static_assert(offsetof(n00b_os_thread_context, sp) == 0, "context sp offset");
_Static_assert(offsetof(n00b_os_thread_context, fp) == 8, "context fp offset");
_Static_assert(offsetof(n00b_os_thread_context, nregs) == 16, "context register count offset");
_Static_assert(offsetof(n00b_os_thread_context, regs) == 24, "context register offset");
_Static_assert(offsetof(n00b_os_thread_context, pc) == 432, "context pc offset");

#if defined(__aarch64__)
__attribute__((naked)) void n00b_os_thread_capture_self(n00b_os_thread_context *context)
{
    __asm__ volatile(
        "stp x0, x1, [x0, #24]\n\t"
        "stp x2, x3, [x0, #40]\n\t"
        "stp x4, x5, [x0, #56]\n\t"
        "stp x6, x7, [x0, #72]\n\t"
        "stp x8, x9, [x0, #88]\n\t"
        "stp x10, x11, [x0, #104]\n\t"
        "stp x12, x13, [x0, #120]\n\t"
        "stp x14, x15, [x0, #136]\n\t"
        "stp x16, x17, [x0, #152]\n\t"
        "stp x18, x19, [x0, #168]\n\t"
        "stp x20, x21, [x0, #184]\n\t"
        "stp x22, x23, [x0, #200]\n\t"
        "stp x24, x25, [x0, #216]\n\t"
        "stp x26, x27, [x0, #232]\n\t"
        "stp x28, x29, [x0, #248]\n\t"
        "str x30, [x0, #264]\n\t"
        "mov x9, sp\n\t"
        "str x9, [x0]\n\t"
        "str x29, [x0, #8]\n\t"
        "str x30, [x0, #432]\n\t"
        "mov x9, #31\n\t"
        "str x9, [x0, #16]\n\t"
        "ret");
}
#elif defined(__x86_64__)
__attribute__((naked)) void n00b_os_thread_capture_self(n00b_os_thread_context *context)
{
#if defined(_WIN32)
    __asm__ volatile(
        "mov %rsp, 0(%rcx)\n\t"
        "mov %rbp, 8(%rcx)\n\t"
        "movq $16, 16(%rcx)\n\t"
        "mov %rax, 24(%rcx)\n\t"
        "mov %rbx, 32(%rcx)\n\t"
        "mov %rcx, 40(%rcx)\n\t"
        "mov %rdx, 48(%rcx)\n\t"
        "mov %rsi, 56(%rcx)\n\t"
        "mov %rdi, 64(%rcx)\n\t"
        "mov %rbp, 72(%rcx)\n\t"
        "mov %rsp, 80(%rcx)\n\t"
        "mov %r8, 88(%rcx)\n\t"
        "mov %r9, 96(%rcx)\n\t"
        "mov %r10, 104(%rcx)\n\t"
        "mov %r11, 112(%rcx)\n\t"
        "mov %r12, 120(%rcx)\n\t"
        "mov %r13, 128(%rcx)\n\t"
        "mov %r14, 136(%rcx)\n\t"
        "mov %r15, 144(%rcx)\n\t"
        "mov (%rsp), %rax\n\t"
        "mov %rax, 432(%rcx)\n\t"
        "ret");
#else
    __asm__ volatile(
        "mov %rsp, 0(%rdi)\n\t"
        "mov %rbp, 8(%rdi)\n\t"
        "movq $16, 16(%rdi)\n\t"
        "mov %rax, 24(%rdi)\n\t"
        "mov %rbx, 32(%rdi)\n\t"
        "mov %rcx, 40(%rdi)\n\t"
        "mov %rdx, 48(%rdi)\n\t"
        "mov %rsi, 56(%rdi)\n\t"
        "mov %rdi, 64(%rdi)\n\t"
        "mov %rbp, 72(%rdi)\n\t"
        "mov %rsp, 80(%rdi)\n\t"
        "mov %r8, 88(%rdi)\n\t"
        "mov %r9, 96(%rdi)\n\t"
        "mov %r10, 104(%rdi)\n\t"
        "mov %r11, 112(%rdi)\n\t"
        "mov %r12, 120(%rdi)\n\t"
        "mov %r13, 128(%rdi)\n\t"
        "mov %r14, 136(%rdi)\n\t"
        "mov %r15, 144(%rdi)\n\t"
        "mov (%rsp), %rax\n\t"
        "mov %rax, 432(%rdi)\n\t"
        "ret");
#endif
}
#else
#error "unsupported self-capture architecture"
#endif

#if defined(__linux__)

#include <asm/signal.h>
#include <asm/sigcontext.h>
#include <asm/ucontext.h>
#include "builtin_os_syscall.h"

#if defined(__x86_64__)
__attribute__((naked)) static void stw_sigreturn(void)
{
    __asm__ volatile("mov $15, %rax\n\tsyscall");
}
#elif defined(__aarch64__)
__attribute__((naked)) static void stw_sigreturn(void)
{
    __asm__ volatile("mov x8, #139\n\tsvc #0");
}
#else
#error "unsupported Linux thread context architecture"
#endif

enum { STW_CLOCK_MONOTONIC = 1 };

typedef struct {
    int signo;
    int error;
    int code;
    int padding;
    int pid;
    unsigned uid;
    n00b_os_thread_context *context;
} stw_siginfo_t;

static void stw_suspend_handler(int signal, void *info, void *raw_context)
{
    (void)signal;
    n00b_os_thread_context *context = info
        ? ((const stw_siginfo_t *)info)->context : NULL;
    if (!context || context->thread_id != n00b_os_thread_id()) return;

    struct ucontext *state = raw_context;
#if defined(__x86_64__)
    const struct sigcontext *saved = &state->uc_mcontext;
    context->sp = (void *)(uintptr_t)saved->rsp;
    context->fp = (void *)(uintptr_t)saved->rbp;
    context->pc = (void *)(uintptr_t)saved->rip;
    const uintptr_t regs[] = {
        saved->r8, saved->r9, saved->r10, saved->r11,
        saved->r12, saved->r13, saved->r14, saved->r15,
        saved->rdi, saved->rsi, saved->rbp, saved->rbx,
        saved->rdx, saved->rax, saved->rcx, saved->rsp,
    };
    context->nregs = sizeof(regs) / sizeof(regs[0]);
    for (size_t i = 0; i < context->nregs; i++)
        context->regs[i] = regs[i];
#else
    context->sp = (void *)(uintptr_t)state->uc_mcontext.sp;
    context->fp = (void *)(uintptr_t)state->uc_mcontext.regs[29];
    context->pc = (void *)(uintptr_t)state->uc_mcontext.pc;
    context->nregs = 31;
    for (size_t i = 0; i < 31; i++)
        context->regs[i] = (uintptr_t)state->uc_mcontext.regs[i];
#endif
    atomic_store_explicit(&context->parked, 1, memory_order_release);
    while (atomic_load_explicit(&context->parked, memory_order_acquire)) {
        struct { long sec, nsec; } wait = {0, 1000000};
        (void)n00b_linux_syscall6(__NR_clock_nanosleep,
                                   STW_CLOCK_MONOTONIC, 0,
                                   (long)&wait, 0, 0, 0);
    }
}

uintptr_t n00b_linux_stw_handler(void)
{
    return (uintptr_t)stw_suspend_handler;
}

uintptr_t n00b_linux_stw_restorer(void)
{
    return (uintptr_t)stw_sigreturn;
}

#endif
