#include "builtin_os.h"

#if defined(__APPLE__)
#define N00B_ASM_SYMBOL(name) "\"_" name "\""
#else
#define N00B_ASM_SYMBOL(name) "\"" name "\""
#endif

#define N00B_BLOCKING_HANDLER N00B_ASM_SYMBOL("n00b_gc_enter_blocking_from")
#define N00B_CAPTURE_SELF N00B_ASM_SYMBOL("n00b_os_thread_capture_self")

_Static_assert(sizeof(n00b_os_thread_context) <= 448,
               "blocking capture buffer");

#if defined(__aarch64__)
__attribute__((naked)) void n00b_os_gc_enter_blocking(void)
{
    __asm__ volatile(
        "stp x29, x30, [sp, #-16]!\n\t"
        "mov x29, sp\n\t"
        "sub sp, sp, #448\n\t"
        "mov x0, sp\n\t"
        "bl " N00B_CAPTURE_SELF "\n\t"
        "ldp x9, x10, [x29]\n\t"
        "add x11, x29, #16\n\t"
        "str x11, [sp]\n\t"
        "str x9, [sp, #8]\n\t"
        "stp x9, x10, [sp, #256]\n\t"
        "str x10, [sp, #432]\n\t"
        "mov x0, sp\n\t"
        "bl " N00B_BLOCKING_HANDLER "\n\t"
        "mov sp, x29\n\t"
        "ldp x29, x30, [sp], #16\n\t"
        "ret");
}
#elif defined(__x86_64__) && defined(_WIN32)
__attribute__((naked)) void n00b_os_gc_enter_blocking(void)
{
    __asm__ volatile(
        "push %rbp\n\t"
        "mov %rsp, %rbp\n\t"
        "sub $480, %rsp\n\t"
        "lea 32(%rsp), %rcx\n\t"
        "call " N00B_CAPTURE_SELF "\n\t"
        "mov (%rbp), %rax\n\t"
        "mov %rax, 40(%rsp)\n\t"
        "mov %rax, 104(%rsp)\n\t"
        "lea 16(%rbp), %rax\n\t"
        "mov %rax, 32(%rsp)\n\t"
        "mov %rax, 112(%rsp)\n\t"
        "mov 8(%rbp), %rax\n\t"
        "mov %rax, 464(%rsp)\n\t"
        "lea 32(%rsp), %rcx\n\t"
        "call " N00B_BLOCKING_HANDLER "\n\t"
        "mov %rbp, %rsp\n\t"
        "pop %rbp\n\t"
        "ret");
}
#elif defined(__x86_64__)
__attribute__((naked)) void n00b_os_gc_enter_blocking(void)
{
    __asm__ volatile(
        "push %rbp\n\t"
        "mov %rsp, %rbp\n\t"
        "sub $448, %rsp\n\t"
        "mov %rsp, %rdi\n\t"
        "call " N00B_CAPTURE_SELF "\n\t"
        "mov (%rbp), %rax\n\t"
        "mov %rax, 8(%rsp)\n\t"
        "mov %rax, 72(%rsp)\n\t"
        "lea 16(%rbp), %rax\n\t"
        "mov %rax, 0(%rsp)\n\t"
        "mov %rax, 80(%rsp)\n\t"
        "mov 8(%rbp), %rax\n\t"
        "mov %rax, 432(%rsp)\n\t"
        "mov %rsp, %rdi\n\t"
        "call " N00B_BLOCKING_HANDLER "\n\t"
        "mov %rbp, %rsp\n\t"
        "pop %rbp\n\t"
        "ret");
}
#endif
