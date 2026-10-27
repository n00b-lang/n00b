#include <stdint.h>

#if defined(__aarch64__)
__attribute__((naked)) int32_t
n00b_ffi_unwind_save(uint64_t *buffer)
{
    __asm__ volatile(
        "stp x19, x20, [x0, #0]\n"
        "stp x21, x22, [x0, #16]\n"
        "stp x23, x24, [x0, #32]\n"
        "stp x25, x26, [x0, #48]\n"
        "stp x27, x28, [x0, #64]\n"
        "stp x29, x30, [x0, #80]\n"
        "mov x2, sp\n"
        "str x2, [x0, #96]\n"
        "stp d8, d9, [x0, #104]\n"
        "stp d10, d11, [x0, #120]\n"
        "stp d12, d13, [x0, #136]\n"
        "stp d14, d15, [x0, #152]\n"
        "mov w0, #0\n"
        "ret\n");
}

__attribute__((naked, noreturn)) void
n00b_ffi_unwind_jump(uint64_t *buffer)
{
    __asm__ volatile(
        "ldp x19, x20, [x0, #0]\n"
        "ldp x21, x22, [x0, #16]\n"
        "ldp x23, x24, [x0, #32]\n"
        "ldp x25, x26, [x0, #48]\n"
        "ldp x27, x28, [x0, #64]\n"
        "ldp x29, x30, [x0, #80]\n"
        "ldr x2, [x0, #96]\n"
        "mov sp, x2\n"
        "ldp d8, d9, [x0, #104]\n"
        "ldp d10, d11, [x0, #120]\n"
        "ldp d12, d13, [x0, #136]\n"
        "ldp d14, d15, [x0, #152]\n"
        "mov w0, #1\n"
        "ret\n");
}
#elif defined(__x86_64__) && defined(_WIN32)
__attribute__((naked)) int32_t
n00b_ffi_unwind_save(uint64_t *buffer)
{
    __asm__ volatile(
        "movq %rbx, 0(%rcx)\n"
        "movq %rbp, 8(%rcx)\n"
        "movq %r12, 16(%rcx)\n"
        "movq %r13, 24(%rcx)\n"
        "movq %r14, 32(%rcx)\n"
        "movq %r15, 40(%rcx)\n"
        "leaq 8(%rsp), %rdx\n"
        "movq %rdx, 48(%rcx)\n"
        "movq (%rsp), %rdx\n"
        "movq %rdx, 56(%rcx)\n"
        "movq %rdi, 64(%rcx)\n"
        "movq %rsi, 72(%rcx)\n"
        "movdqu %xmm6, 80(%rcx)\n"
        "movdqu %xmm7, 96(%rcx)\n"
        "movdqu %xmm8, 112(%rcx)\n"
        "movdqu %xmm9, 128(%rcx)\n"
        "movdqu %xmm10, 144(%rcx)\n"
        "movdqu %xmm11, 160(%rcx)\n"
        "movdqu %xmm12, 176(%rcx)\n"
        "movdqu %xmm13, 192(%rcx)\n"
        "movdqu %xmm14, 208(%rcx)\n"
        "movdqu %xmm15, 224(%rcx)\n"
        "xorl %eax, %eax\n"
        "ret\n");
}

__attribute__((naked, noreturn)) void
n00b_ffi_unwind_jump(uint64_t *buffer)
{
    __asm__ volatile(
        "movq 0(%rcx), %rbx\n"
        "movq 8(%rcx), %rbp\n"
        "movq 16(%rcx), %r12\n"
        "movq 24(%rcx), %r13\n"
        "movq 32(%rcx), %r14\n"
        "movq 40(%rcx), %r15\n"
        "movq 64(%rcx), %rdi\n"
        "movq 72(%rcx), %rsi\n"
        "movdqu 80(%rcx), %xmm6\n"
        "movdqu 96(%rcx), %xmm7\n"
        "movdqu 112(%rcx), %xmm8\n"
        "movdqu 128(%rcx), %xmm9\n"
        "movdqu 144(%rcx), %xmm10\n"
        "movdqu 160(%rcx), %xmm11\n"
        "movdqu 176(%rcx), %xmm12\n"
        "movdqu 192(%rcx), %xmm13\n"
        "movdqu 208(%rcx), %xmm14\n"
        "movdqu 224(%rcx), %xmm15\n"
        "movq 48(%rcx), %rsp\n"
        "movl $1, %eax\n"
        "jmpq *56(%rcx)\n");
}
#elif defined(__x86_64__)
__attribute__((naked)) int32_t
n00b_ffi_unwind_save(uint64_t *buffer)
{
    __asm__ volatile(
        "movq %rbx, 0(%rdi)\n"
        "movq %rbp, 8(%rdi)\n"
        "movq %r12, 16(%rdi)\n"
        "movq %r13, 24(%rdi)\n"
        "movq %r14, 32(%rdi)\n"
        "movq %r15, 40(%rdi)\n"
        "leaq 8(%rsp), %rdx\n"
        "movq %rdx, 48(%rdi)\n"
        "movq (%rsp), %rdx\n"
        "movq %rdx, 56(%rdi)\n"
        "xorl %eax, %eax\n"
        "ret\n");
}

__attribute__((naked, noreturn)) void
n00b_ffi_unwind_jump(uint64_t *buffer)
{
    __asm__ volatile(
        "movq 0(%rdi), %rbx\n"
        "movq 8(%rdi), %rbp\n"
        "movq 16(%rdi), %r12\n"
        "movq 24(%rdi), %r13\n"
        "movq 32(%rdi), %r14\n"
        "movq 40(%rdi), %r15\n"
        "movq 48(%rdi), %rsp\n"
        "movl $1, %eax\n"
        "jmpq *56(%rdi)\n");
}
#endif
