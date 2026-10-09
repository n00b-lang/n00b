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

#if defined(__APPLE__)

#include <mach/mach.h>
#include <mach/thread_act.h>
#include <pthread.h>

uint64_t n00b_os_thread_control_self(void)
{
    return (uint64_t)pthread_mach_thread_np(pthread_self());
}

int32_t n00b_os_thread_suspend(uint64_t control,
                               n00b_os_thread_context *context,
                               uint64_t timeout_ns)
{
    (void)timeout_ns;
    if (!control || !context) return -1;
    mach_port_t port = (mach_port_t)control;
    if (thread_suspend(port) != KERN_SUCCESS) return -1;

#if defined(__aarch64__)
    arm_thread_state64_t state;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(port, ARM_THREAD_STATE64,
                         (thread_state_t)&state, &count) != KERN_SUCCESS) {
        (void)thread_resume(port);
        return -1;
    }
    context->sp = (void *)(uintptr_t)arm_thread_state64_get_sp(state);
    context->fp = (void *)(uintptr_t)arm_thread_state64_get_fp(state);
    context->pc = (void *)(uintptr_t)arm_thread_state64_get_pc(state);
    context->nregs = 31;
    for (size_t i = 0; i < 29; i++) context->regs[i] = state.__x[i];
    context->regs[29] = (uintptr_t)arm_thread_state64_get_fp(state);
    context->regs[30] = (uintptr_t)arm_thread_state64_get_lr(state);
#elif defined(__x86_64__)
    x86_thread_state64_t state;
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    if (thread_get_state(port, x86_THREAD_STATE64,
                         (thread_state_t)&state, &count) != KERN_SUCCESS) {
        (void)thread_resume(port);
        return -1;
    }
    context->sp = (void *)(uintptr_t)state.__rsp;
    context->fp = (void *)(uintptr_t)state.__rbp;
    context->pc = (void *)(uintptr_t)state.__rip;
    const uint64_t regs[] = {
        state.__rax, state.__rbx, state.__rcx, state.__rdx,
        state.__rdi, state.__rsi, state.__rbp, state.__r8,
        state.__r9, state.__r10, state.__r11, state.__r12,
        state.__r13, state.__r14, state.__r15,
    };
    context->nregs = sizeof(regs) / sizeof(regs[0]);
    for (size_t i = 0; i < context->nregs; i++) context->regs[i] = regs[i];
#else
#error "unsupported macOS thread context architecture"
#endif
    context->handle = (uintptr_t)port;
    context->thread_id = 0;
    atomic_store_explicit(&context->parked, 1, memory_order_release);
    return 0;
}

int32_t n00b_os_thread_resume(uint64_t control,
                              n00b_os_thread_context *context)
{
    if (!context || !atomic_load_explicit(&context->parked,
                                          memory_order_acquire)
        || context->handle != (uintptr_t)control) return -1;
    if (thread_resume((mach_port_t)control) != KERN_SUCCESS) return -1;
    atomic_store_explicit(&context->parked, 0, memory_order_release);
    context->nregs = 0;
    context->handle = 0;
    return 0;
}

#elif defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint64_t n00b_os_thread_control_self(void)
{
    return (uint64_t)GetCurrentThreadId();
}

int32_t n00b_os_thread_suspend(uint64_t control,
                               n00b_os_thread_context *context,
                               uint64_t timeout_ns)
{
    (void)timeout_ns;
    if (!control || !context) return -1;
    HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                               FALSE, (DWORD)control);
    if (!handle) return -1;
    if (SuspendThread(handle) == (DWORD)-1) {
        CloseHandle(handle);
        return -1;
    }
    CONTEXT state = {0};
    state.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(handle, &state)) {
        (void)ResumeThread(handle);
        CloseHandle(handle);
        return -1;
    }
#if defined(_M_X64) || defined(__x86_64__)
    context->sp = (void *)(uintptr_t)state.Rsp;
    context->fp = (void *)(uintptr_t)state.Rbp;
    context->pc = (void *)(uintptr_t)state.Rip;
    const uint64_t regs[] = {
        state.Rax, state.Rbx, state.Rcx, state.Rdx,
        state.Rsi, state.Rdi, state.Rbp, state.R8,
        state.R9, state.R10, state.R11, state.R12,
        state.R13, state.R14, state.R15,
    };
    context->nregs = sizeof(regs) / sizeof(regs[0]);
    for (size_t i = 0; i < context->nregs; i++) context->regs[i] = regs[i];
#elif defined(_M_ARM64) || defined(__aarch64__)
    context->sp = (void *)(uintptr_t)state.Sp;
    context->fp = (void *)(uintptr_t)state.Fp;
    context->pc = (void *)(uintptr_t)state.Pc;
    context->nregs = 31;
    for (size_t i = 0; i < 29; i++) context->regs[i] = state.X[i];
    context->regs[29] = state.Fp;
    context->regs[30] = state.Lr;
#else
#error "unsupported Windows thread context architecture"
#endif
    context->handle = (uintptr_t)handle;
    context->thread_id = control;
    atomic_store_explicit(&context->parked, 1, memory_order_release);
    return 0;
}

int32_t n00b_os_thread_resume(uint64_t control,
                              n00b_os_thread_context *context)
{
    if (!context || !atomic_load_explicit(&context->parked,
                                          memory_order_acquire)
        || context->thread_id != control || !context->handle) return -1;
    HANDLE handle = (HANDLE)context->handle;
    if (ResumeThread(handle) == (DWORD)-1) return -1;
    atomic_store_explicit(&context->parked, 0, memory_order_release);
    context->nregs = 0;
    context->handle = 0;
    CloseHandle(handle);
    return 0;
}

#elif defined(__linux__)

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

enum { STW_SIGNAL = 40, STW_CLOCK_MONOTONIC = 1 };

typedef struct {
    void (*handler)(int, void *, void *);
    unsigned long flags;
    void (*restorer)(void);
    unsigned long mask;
} stw_kernel_sigaction;

static atomic_uint signal_state;
static _Atomic(n00b_os_thread_context *) pending_context;

static void stw_suspend_handler(int signal, void *info, void *raw_context)
{
    (void)signal;
    (void)info;
    n00b_os_thread_context *context = atomic_load_explicit(
        &pending_context, memory_order_acquire);
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

static int stw_install_handler(void)
{
    unsigned expected = 0;
    if (atomic_compare_exchange_strong_explicit(&signal_state, &expected,
            1, memory_order_acq_rel, memory_order_acquire)) {
        stw_kernel_sigaction action = {
            .handler = stw_suspend_handler,
            .flags = SA_SIGINFO | SA_RESTART | SA_RESTORER,
            .restorer = stw_sigreturn,
        };
        long result = n00b_linux_syscall6(__NR_rt_sigaction,
                                           STW_SIGNAL, (long)&action, 0,
                                           sizeof(action.mask), 0, 0);
        atomic_store_explicit(&signal_state, result == 0 ? 2 : 3,
                              memory_order_release);
    }
    while (atomic_load_explicit(&signal_state, memory_order_acquire) == 1)
        n00b_os_cpu_relax();
    return atomic_load_explicit(&signal_state, memory_order_acquire) == 2
        ? 0 : -1;
}

uint64_t n00b_os_thread_control_self(void)
{
    return n00b_os_thread_id();
}

int32_t n00b_os_thread_suspend(uint64_t control,
                               n00b_os_thread_context *context,
                               uint64_t timeout_ns)
{
    if (!control || !context || stw_install_handler() != 0) return -1;
    n00b_os_thread_context *empty = NULL;
    while (!atomic_compare_exchange_weak_explicit(&pending_context, &empty,
            context, memory_order_acq_rel, memory_order_acquire)) {
        empty = NULL;
        n00b_os_cpu_relax();
    }
    context->thread_id = control;
    context->nregs = 0;
    atomic_store_explicit(&context->parked, 0, memory_order_release);
    long pid = n00b_linux_syscall6(__NR_getpid, 0, 0, 0, 0, 0, 0);
    long sent = n00b_linux_syscall6(__NR_tgkill, pid, (long)control,
                                    STW_SIGNAL, 0, 0, 0);
    if (sent != 0) {
        atomic_store_explicit(&pending_context, NULL, memory_order_release);
        return -1;
    }
    uint64_t start = n00b_os_now_ns();
    while (!atomic_load_explicit(&context->parked, memory_order_acquire)) {
        if (timeout_ns && n00b_os_now_ns() - start > timeout_ns)
            __builtin_trap();
        n00b_os_cpu_relax();
    }
    atomic_store_explicit(&pending_context, NULL, memory_order_release);
    return 0;
}

int32_t n00b_os_thread_resume(uint64_t control,
                              n00b_os_thread_context *context)
{
    if (!context || context->thread_id != control
        || !atomic_load_explicit(&context->parked, memory_order_acquire))
        return -1;
    context->nregs = 0;
    atomic_store_explicit(&context->parked, 0, memory_order_release);
    return 0;
}

#else
#error "builtin/os requires macOS, Linux, or Windows"
#endif
