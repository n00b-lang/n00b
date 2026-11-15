// Program startup for Linux without libc.
//
// n00b programs on Linux link no C library: this file provides what the C
// startup files and libc would, for the runtime and for compiled n00b code:
//
//   - `_start`, which reads argc, argv, the environment and the auxiliary
//     vector from the initial stack, sets up thread-local storage for the
//     main thread, runs the program and exits;
//   - the saved environment and auxiliary vector;
//   - thread-local storage blocks, built from the program's PT_TLS segment;
//   - memcpy, memmove, memset, memcmp and bcmp, which compiled code calls;
//   - malloc, calloc, realloc and free over the n00b pool allocator, plus
//     strlen, strcmp and memchr, for FFI shims.
//
// It is built only into the program runtime, with N00B_LIBC_FREE defined.
//
// A program whose FFI contracts link "c" links libc dynamically, but still
// starts here: the dynamic loader sets libc up, main thread included, and
// then jumps to _start. These definitions interpose libc's, so C code
// allocates from the n00b pool too. Threads the runtime starts carry n00b's
// thread-local storage only, not libc's, so a shim for a contract that
// links "c" refuses to run on one (n00b_ffi_libc_thread_check).

#if defined(__linux__) && defined(N00B_LIBC_FREE)

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "builtin_os_syscall.h"
#include "linux_start.h"
#include "runtime_panic.h"

#include <elf.h>

// The program's main: runtime_main.c's, which runs the n00b program, or a
// compile-time program's own.
int main(int argc, char **argv);

char **n00b_linux_environ;
static const uintptr_t *auxiliary_vector;

uintptr_t
n00b_linux_auxv(uintptr_t type)
{
    for (const uintptr_t *entry = auxiliary_vector; entry && entry[0];
         entry += 2)
        if (entry[0] == type) return entry[1];
    return 0;
}

// ---------------------------------------------------------------------------
// Memory functions. `no_builtin` keeps the compiler from turning these loops
// back into calls to themselves.

__attribute__((no_builtin)) void *
memcpy(void *restrict destination, const void *restrict source, size_t count)
{
    unsigned char *to = destination;
    const unsigned char *from = source;
    while (count--) *to++ = *from++;
    return destination;
}

__attribute__((no_builtin)) void *
memmove(void *destination, const void *source, size_t count)
{
    unsigned char *to = destination;
    const unsigned char *from = source;
    if (to == from || !count) return destination;
    if (to < from || to >= from + count) {
        while (count--) *to++ = *from++;
    }
    else {
        to += count;
        from += count;
        while (count--) *--to = *--from;
    }
    return destination;
}

__attribute__((no_builtin)) void *
memset(void *destination, int value, size_t count)
{
    unsigned char *to = destination;
    while (count--) *to++ = (unsigned char)value;
    return destination;
}

__attribute__((no_builtin)) int
memcmp(const void *left, const void *right, size_t count)
{
    const unsigned char *a = left;
    const unsigned char *b = right;
    for (; count; count--, a++, b++)
        if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}

__attribute__((no_builtin)) int
bcmp(const void *left, const void *right, size_t count)
{
    return memcmp(left, right, count);
}

__attribute__((no_builtin)) size_t
strlen(const char *text)
{
    size_t length = 0;
    while (text[length]) length++;
    return length;
}

// ---------------------------------------------------------------------------
// Thread-local storage.
//
// The static linker resolves every thread-local access to a fixed offset
// from the thread pointer, in the layout the architecture's ABI defines:
//
//   x86_64 (variant II): the TLS block ends at the thread pointer, which
//     points to a thread control block whose first word points to itself.
//   aarch64 (variant I): the thread pointer points to a 16-byte control
//     block, and the TLS block follows it, aligned.

typedef struct {
    const unsigned char *image;
    uintptr_t image_bytes;
    uintptr_t total_bytes;
    uintptr_t alignment;
} tls_template_t;

static tls_template_t tls_template;

static uintptr_t
round_up(uintptr_t value, uintptr_t alignment)
{
    return alignment > 1 ? (value + alignment - 1) & ~(alignment - 1)
                         : value;
}

static void
find_tls_template(void)
{
    const Elf64_Phdr *headers = (const Elf64_Phdr *)n00b_linux_auxv(AT_PHDR);
    uintptr_t count = n00b_linux_auxv(AT_PHNUM);
    uintptr_t bias = 0;
    for (uintptr_t i = 0; headers && i < count; i++)
        if (headers[i].p_type == PT_PHDR)
            bias = (uintptr_t)headers - headers[i].p_vaddr;
    for (uintptr_t i = 0; headers && i < count; i++) {
        if (headers[i].p_type != PT_TLS) continue;
        tls_template = (tls_template_t){
            .image = (const unsigned char *)(bias + headers[i].p_vaddr),
            .image_bytes = headers[i].p_filesz,
            .total_bytes = headers[i].p_memsz,
            .alignment = headers[i].p_align ? headers[i].p_align : 1,
        };
    }
}

enum {
    LINUX_PROT_READ = 1,
    LINUX_PROT_WRITE = 2,
    LINUX_MAP_PRIVATE_ANONYMOUS = 0x22,
    THREAD_CONTROL_BYTES = 16,
};

uintptr_t
n00b_linux_tls_bytes(void)
{
    uintptr_t alignment = tls_template.alignment > 16
        ? tls_template.alignment : 16;
    return round_up(tls_template.total_bytes, alignment)
        + THREAD_CONTROL_BYTES + 2 * alignment;
}

uintptr_t
n00b_linux_tls_init(void *area)
{
    uintptr_t alignment = tls_template.alignment > 16
        ? tls_template.alignment : 16;
    uintptr_t base = (uintptr_t)area;
#if defined(__x86_64__)
    uintptr_t block_bytes = round_up(tls_template.total_bytes,
                                     tls_template.alignment);
    uintptr_t pointer = round_up(base + block_bytes, alignment);
    unsigned char *block = (unsigned char *)(pointer - block_bytes);
    *(uintptr_t *)pointer = pointer;
#elif defined(__aarch64__)
    uintptr_t pointer = round_up(base, alignment);
    unsigned char *block = (unsigned char *)(pointer + round_up(
        THREAD_CONTROL_BYTES, tls_template.alignment));
#else
#error "linux_start.c supports x86_64 and aarch64"
#endif
    memcpy(block, tls_template.image, tls_template.image_bytes);
    memset(block + tls_template.image_bytes, 0,
           tls_template.total_bytes - tls_template.image_bytes);
    return pointer;
}

static void
set_thread_pointer(uintptr_t pointer)
{
#if defined(__x86_64__)
    enum { ARCH_SET_FS = 0x1002 };
    (void)n00b_linux_syscall6(__NR_arch_prctl, ARCH_SET_FS, (long)pointer,
                              0, 0, 0, 0);
#else
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(pointer) : "memory");
#endif
}

// ---------------------------------------------------------------------------
// A minimal stand-in for the few libc calls the compile-time runtime
// makes. Allocations map whole pages, with two words in front of the
// block: the mapping's size, then where the mapping starts (an aligned
// block need not start right after the header). They are few and
// short-lived, so that is enough.

enum { ALLOCATION_HEADER = 16 };

static uintptr_t
page_bytes(void)
{
    uintptr_t page = n00b_linux_auxv(AT_PAGESZ);
    return page ? page : 4096;
}

// `size` zeroed bytes aligned to `alignment`, a power of two of at least 16.
static void *
page_allocate(size_t size, size_t alignment)
{
    if (size > SIZE_MAX - ALLOCATION_HEADER - alignment - page_bytes())
        return NULL;
    uintptr_t bytes = round_up(size + ALLOCATION_HEADER + alignment - 16,
                               page_bytes());
    long area = n00b_linux_syscall6(__NR_mmap, 0, (long)bytes,
        LINUX_PROT_READ | LINUX_PROT_WRITE, LINUX_MAP_PRIVATE_ANONYMOUS, -1, 0);
    if (area < 0 && area > -4096) return NULL;
    uintptr_t *block = (uintptr_t *)round_up((uintptr_t)area
                                             + ALLOCATION_HEADER, alignment);
    block[-2] = bytes;
    block[-1] = (uintptr_t)area;
    return block;
}

// The bytes from `memory` to the end of its mapping.
static uintptr_t
page_usable(void *memory)
{
    uintptr_t *block = memory;
    return block[-1] + block[-2] - (uintptr_t)memory;
}

void *
n00b_linux_calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) return NULL;
    return page_allocate(count * size, 16);
}

void
n00b_linux_free(void *memory)
{
    if (!memory) return;
    uintptr_t *block = memory;
    (void)n00b_linux_syscall6(__NR_munmap, (long)block[-1], (long)block[-2],
                              0, 0, 0, 0);
}

void *
n00b_linux_realloc(void *memory, size_t size)
{
    if (!memory) return n00b_linux_calloc(1, size);
    uintptr_t usable = page_usable(memory);
    if (size <= usable) return memory;
    void *grown = n00b_linux_calloc(1, size);
    if (!grown) return NULL;
    memcpy(grown, memory, usable);
    n00b_linux_free(memory);
    return grown;
}

// ---------------------------------------------------------------------------
// The C allocator, for FFI shims and C libraries linked into a program: the
// n00b pool allocator (lib/sys/internal/pool.n), which every program links.
// A program without it, such as a compile-time one, falls back to the page
// allocator above, and so does a program before _start: with libc linked,
// the loader and libc's initializers may allocate before the n00b program
// (and the pool) is set up.
//
// free and realloc tell the two apart by the block's first header word: a
// pool block's is a size class below 16 or a mapping size with its top bit
// set; a page allocation's is its mapping size, a whole number of pages.

static bool started;

static bool
page_allocated(void *memory)
{
    uintptr_t header = *(uintptr_t *)((unsigned char *)memory
                                      - ALLOCATION_HEADER);
    return header >= 4096 && !(header >> 63);
}

extern void *n00b_pool_allocate(uint64_t bytes) __attribute__((weak));
extern void *n00b_pool_resize(void *memory, uint64_t bytes)
    __attribute__((weak));
extern void n00b_pool_release(void *memory) __attribute__((weak));

void *
malloc(size_t size)
{
    return n00b_pool_allocate && started ? n00b_pool_allocate(size)
                                         : n00b_linux_calloc(1, size);
}

void *
calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) return NULL;
    return malloc(count * size);
}

void *
realloc(void *memory, size_t size)
{
    if (!memory) return malloc(size);
    if (!n00b_pool_resize || page_allocated(memory))
        return n00b_linux_realloc(memory, size);
    return n00b_pool_resize(memory, size);
}

void
free(void *memory)
{
    if (!memory) return;
    if (n00b_pool_release && !page_allocated(memory))
        n00b_pool_release(memory);
    else n00b_linux_free(memory);
}

// The rest of the allocator libc's own would provide, so no block from
// libc's allocator reaches the free above. Alignments past the pool's 16
// bytes come from the page allocator.

void *
aligned_alloc(size_t alignment, size_t size)
{
    if (!alignment || (alignment & (alignment - 1))) return NULL;
    return alignment <= 16 ? malloc(size) : page_allocate(size, alignment);
}

void *
memalign(size_t alignment, size_t size)
{
    return aligned_alloc(alignment, size);
}

int
posix_memalign(void **out, size_t alignment, size_t size)
{
    enum { LINUX_EINVAL = 22, LINUX_ENOMEM = 12 };
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)))
        return LINUX_EINVAL;
    void *memory = aligned_alloc(alignment, size);
    if (!memory) return LINUX_ENOMEM;
    *out = memory;
    return 0;
}

void *
valloc(size_t size)
{
    return page_allocate(size, page_bytes());
}

void *
pvalloc(size_t size)
{
    return page_allocate(round_up(size, page_bytes()), page_bytes());
}

size_t
malloc_usable_size(void *memory)
{
    if (!memory) return 0;
    if (page_allocated(memory)) return page_usable(memory);
    // The pool's header (lib/sys/internal/pool.n): a size class, or a large
    // block's mapping size with the top bit set.
    uint64_t tag = *(uint64_t *)((unsigned char *)memory - ALLOCATION_HEADER);
    return tag >> 63 ? (size_t)(tag & ~(1ull << 63)) - ALLOCATION_HEADER
                     : (size_t)16 << tag;
}

__attribute__((no_builtin)) void *
memchr(const void *memory, int value, size_t count)
{
    const unsigned char *at = memory;
    for (; count; count--, at++)
        if (*at == (unsigned char)value) return (void *)at;
    return NULL;
}

__attribute__((no_builtin)) int
strcmp(const char *left, const char *right)
{
    while (*left && *left == *right) left++, right++;
    return (unsigned char)*left - (unsigned char)*right;
}

int
n00b_linux_open(const char *path, int flags, int mode)
{
    enum { AT_WORKING_DIRECTORY = -100 };
    long fd = n00b_linux_syscall6(__NR_openat, AT_WORKING_DIRECTORY,
                                  (long)path, flags, mode, 0, 0);
    return fd < 0 ? -1 : (int)fd;
}

int
n00b_linux_close(int fd)
{
    return n00b_linux_syscall6(__NR_close, fd, 0, 0, 0, 0, 0) < 0 ? -1 : 0;
}

long
n00b_linux_write(int fd, const void *bytes, size_t count)
{
    long written;
    do {
        written = n00b_linux_syscall6(__NR_write, fd, (long)bytes,
                                      (long)count, 0, 0, 0);
    } while (written == -4);
    return written < 0 ? -1 : written;
}

// libc's, when the program links it; null otherwise.
extern void exit(int status) __attribute__((weak));
extern int __cxa_atexit(void (*function)(void *), void *argument,
                        void *dso) __attribute__((weak));

static bool runtime_thread_now(void);

[[noreturn]] void
n00b_linux_exit_program(int status)
{
    // With libc linked, exit as a C program does: libc runs its atexit
    // handlers, the shared libraries' destructors, and flushes stdio. That
    // takes libc's thread state, which a runtime thread lacks.
    if (exit && !runtime_thread_now()) exit(status);
    for (;;)
        (void)n00b_linux_syscall6(__NR_exit_group, status, 0, 0, 0, 0, 0);
}

[[noreturn]] void
n00b_linux_exit(int status)
{
    for (;;)
        (void)n00b_linux_syscall6(__NR_exit_group, status, 0, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// Threads.
//
// Each thread gets one mapping:
//
//   [ control | TLS area | guard | stack ............ top | cap ]
//
// The guard and the cap are mapped with no access. Besides catching an
// overflow, they keep the stack a mapping of its own: the kernel merges
// neighbouring anonymous mappings with the same access, and a merged one
// would make /proc/self/maps, which the collector reads the stack top
// from, report the end of a neighbour instead.
// The kernel clears `tid` when the thread has exited (CLONE_CHILD_CLEARTID);
// a thread can't unmap the stack it runs on, so finished threads' mappings
// are released by the next thread_start.

typedef struct thread_area {
    _Atomic int32_t tid;
    struct thread_area *next;
    uintptr_t bytes;
    void (*entry)(void *);
    void *argument;
} thread_area_t;

// Set on the threads n00b_linux_thread_start starts: they have no libc
// thread state, even when the program links libc.
static _Thread_local bool runtime_thread;

static bool
runtime_thread_now(void)
{
    return runtime_thread;
}

static void
thread_begin(void *area)
{
    runtime_thread = true;
    thread_area_t *thread = area;
    thread->entry(thread->argument);
}

void
n00b_ffi_libc_thread_check(void)
{
    if (runtime_thread) n00bb_runtime_fail(N00BB_FAILURE_LIBC_THREAD, 0);
}

static thread_area_t *threads;
static atomic_flag threads_lock = ATOMIC_FLAG_INIT;

enum {
    THREAD_STACK_BYTES = 8u << 20,
    THREAD_GUARD_PAGES = 16,
    LINUX_PROT_NONE = 0,
    LINUX_CLONE_VM = 0x100,
    LINUX_CLONE_FS = 0x200,
    LINUX_CLONE_FILES = 0x400,
    LINUX_CLONE_SIGHAND = 0x800,
    LINUX_CLONE_THREAD = 0x10000,
    LINUX_CLONE_SYSVSEM = 0x40000,
    LINUX_CLONE_SETTLS = 0x80000,
    LINUX_CLONE_PARENT_SETTID = 0x100000,
    LINUX_CLONE_CHILD_CLEARTID = 0x200000,
};

// clone: the child starts on `stack`, calls entry(argument), and exits the
// thread. Returns the child's id in the parent, or a negated errno.
long n00b_linux_clone(unsigned long flags, void *stack, _Atomic int32_t *tid,
                      uintptr_t thread_pointer, void (*entry)(void *),
                      void *argument);

#if defined(__x86_64__)
__asm__(
    ".text\n"
    ".globl n00b_linux_clone\n"
    ".type n00b_linux_clone, @function\n"
    "n00b_linux_clone:\n"
    // rdi flags, rsi stack, rdx tid, rcx thread pointer, r8 entry,
    // r9 argument. The child pops entry and argument off its new stack.
    "    and $-16, %rsi\n"
    "    sub $16, %rsi\n"
    "    mov %r8, 0(%rsi)\n"
    "    mov %r9, 8(%rsi)\n"
    "    mov %rcx, %r8\n"            // tls: 5th syscall argument
    "    mov %rdx, %r10\n"           // child_tid: 4th
    "    mov $56, %eax\n"            // __NR_clone
    "    syscall\n"                  // clone(flags, stack, ptid, ctid, tls)
    "    test %rax, %rax\n"
    "    jnz 1f\n"
    "    xor %ebp, %ebp\n"
    "    pop %rax\n"
    "    pop %rdi\n"
    "    call *%rax\n"
    "    xor %edi, %edi\n"
    "    mov $60, %eax\n"            // __NR_exit: this thread only
    "    syscall\n"
    "    ud2\n"
    "1:  ret\n"
    ".size n00b_linux_clone, .-n00b_linux_clone\n");
#elif defined(__aarch64__)
__asm__(
    ".text\n"
    ".globl n00b_linux_clone\n"
    ".type n00b_linux_clone, %function\n"
    "n00b_linux_clone:\n"
    // x0 flags, x1 stack, x2 tid, x3 thread pointer, x4 entry, x5 argument.
    "    and x1, x1, #-16\n"
    "    stp x4, x5, [x1, #-16]!\n"
    "    mov x4, x2\n"               // child_tid: 5th syscall argument
    "    mov x8, #220\n"             // __NR_clone
    "    svc #0\n"                   // clone(flags, stack, ptid, tls, ctid)
    "    cbnz x0, 1f\n"
    "    ldp x4, x5, [sp], #16\n"
    "    mov x29, #0\n"
    "    mov x30, #0\n"
    "    mov x0, x5\n"
    "    blr x4\n"
    "    mov x0, #0\n"
    "    mov x8, #93\n"              // __NR_exit: this thread only
    "    svc #0\n"
    "    brk #0\n"
    "1:  ret\n"
    ".size n00b_linux_clone, .-n00b_linux_clone\n");
#endif

static void
lock_threads(void)
{
    while (atomic_flag_test_and_set_explicit(&threads_lock,
                                             memory_order_acquire)) {
    }
}

static void
unlock_threads(void)
{
    atomic_flag_clear_explicit(&threads_lock, memory_order_release);
}

// Releases the mappings of threads the kernel has finished with.
static void
release_finished_threads(void)
{
    lock_threads();
    thread_area_t **link = &threads;
    while (*link) {
        thread_area_t *area = *link;
        if (atomic_load_explicit(&area->tid, memory_order_acquire) != 0) {
            link = &area->next;
            continue;
        }
        *link = area->next;
        (void)n00b_linux_syscall6(__NR_munmap, (long)area, (long)area->bytes,
                                  0, 0, 0, 0);
    }
    unlock_threads();
}

int32_t
n00b_linux_thread_start(void (*entry)(void *), void *argument)
{
    release_finished_threads();
    uintptr_t page = page_bytes();
    uintptr_t control = round_up(sizeof(thread_area_t), 64);
    uintptr_t head = round_up(control + n00b_linux_tls_bytes(), page);
    uintptr_t guard = THREAD_GUARD_PAGES * page;
    uintptr_t bytes = head + guard + THREAD_STACK_BYTES + page;
    long mapped = n00b_linux_syscall6(__NR_mmap, 0, (long)bytes,
        LINUX_PROT_READ | LINUX_PROT_WRITE, LINUX_MAP_PRIVATE_ANONYMOUS, -1, 0);
    if (mapped < 0 && mapped > -4096) return -1;
    unsigned char *base = (unsigned char *)mapped;
    unsigned char *top = base + bytes - page;
    if (n00b_linux_syscall6(__NR_mprotect, (long)(base + head), (long)guard,
                            LINUX_PROT_NONE, 0, 0, 0) < 0
        || n00b_linux_syscall6(__NR_mprotect, (long)top, (long)page,
                               LINUX_PROT_NONE, 0, 0, 0) < 0) {
        (void)n00b_linux_syscall6(__NR_munmap, mapped, (long)bytes, 0, 0, 0,
                                  0);
        return -1;
    }
    thread_area_t *area = (thread_area_t *)base;
    area->bytes = bytes;
    area->entry = entry;
    area->argument = argument;
    uintptr_t thread_pointer = n00b_linux_tls_init(base + control);
    // The area joins the list before the thread starts, which may ask for
    // its stack's top right away. Its tid is nonzero until the kernel clears
    // it, so it isn't taken for a finished thread's meanwhile.
    atomic_store_explicit(&area->tid, -1, memory_order_relaxed);
    lock_threads();
    area->next = threads;
    threads = area;
    unlock_threads();
    unsigned long flags = LINUX_CLONE_VM | LINUX_CLONE_FS | LINUX_CLONE_FILES
        | LINUX_CLONE_SIGHAND | LINUX_CLONE_THREAD | LINUX_CLONE_SYSVSEM
        | LINUX_CLONE_SETTLS | LINUX_CLONE_PARENT_SETTID
        | LINUX_CLONE_CHILD_CLEARTID;
    long child = n00b_linux_clone(flags, top, &area->tid, thread_pointer,
                                  thread_begin, area);
    if (child < 0) {
        lock_threads();
        for (thread_area_t **link = &threads; *link; link = &(*link)->next)
            if (*link == area) {
                *link = area->next;
                break;
            }
        unlock_threads();
        (void)n00b_linux_syscall6(__NR_munmap, mapped, (long)bytes, 0, 0, 0,
                                  0);
        return -1;
    }
    return 0;
}

// The address just past the top of the main thread's stack.
static uintptr_t main_stack_high;

// The kernel copies the program's file name (AT_EXECFN) to the very top of
// the initial stack, with only a null word above it, so the stack's mapping
// ends at the page boundary after it.
static uintptr_t
initial_stack_high(uintptr_t *stack)
{
    const char *name = (const char *)n00b_linux_auxv(31);
    uintptr_t end = (uintptr_t)stack;
    if (name) {
        while (*name) name++;
        end = (uintptr_t)name + 1 + sizeof(uintptr_t);
    }
    return round_up(end, page_bytes());
}

uintptr_t
n00b_linux_stack_high(uintptr_t sp)
{
    // The main thread's is known without the thread list, whose lock a
    // thread stopped for the collector may hold.
    if (n00b_linux_syscall6(__NR_gettid, 0, 0, 0, 0, 0, 0)
        == n00b_linux_syscall6(__NR_getpid, 0, 0, 0, 0, 0, 0))
        return main_stack_high;
    uintptr_t high = 0;
    lock_threads();
    for (thread_area_t *area = threads; area && !high; area = area->next) {
        uintptr_t base = (uintptr_t)area;
        if (sp >= base && sp < base + area->bytes)
            high = base + area->bytes - page_bytes();
    }
    unlock_threads();
    return high;
}

// ---------------------------------------------------------------------------
// Startup.

static void
call_loader_finalizer(void *finalizer)
{
    ((void (*)(void))finalizer)();
}

// `finalizer` is what the dynamic loader passes for atexit (zero from the
// kernel): it runs the shared libraries' destructors.
[[noreturn]] void
n00b_linux_start(uintptr_t *stack, void (*finalizer)(void))
{
    int argc = (int)stack[0];
    char **argv = (char **)(stack + 1);
    char **environment = argv + argc + 1;
    char **end = environment;
    while (*end) end++;
    n00b_linux_environ = environment;
    auxiliary_vector = (const uintptr_t *)(end + 1);
    main_stack_high = initial_stack_high(stack);

    find_tls_template();
    // Under the dynamic loader (AT_BASE is where it is loaded), the main
    // thread's thread-local storage is set up already, libc's with it.
    if (!n00b_linux_auxv(AT_BASE)) {
        long area = n00b_linux_syscall6(__NR_mmap, 0,
            (long)n00b_linux_tls_bytes(), LINUX_PROT_READ | LINUX_PROT_WRITE,
            LINUX_MAP_PRIVATE_ANONYMOUS, -1, 0);
        if (area < 0 && area > -4096) {
            for (;;)
                (void)n00b_linux_syscall6(__NR_exit_group, 127, 0, 0, 0, 0, 0);
        }
        set_thread_pointer(n00b_linux_tls_init((void *)area));
    }
    started = true;
    if (finalizer && __cxa_atexit)
        (void)__cxa_atexit(call_loader_finalizer, (void *)finalizer, NULL);

    n00b_linux_exit_program(main(argc, argv));
}

// The kernel, or the dynamic loader, starts the process here, with the
// stack pointer at argc and the loader's finalizer in rdx (x0).
__attribute__((naked, noreturn)) void
_start(void)
{
#if defined(__x86_64__)
    __asm__ volatile(
        "xor %%ebp, %%ebp\n\t"
        "mov %%rdx, %%rsi\n\t"
        "mov %%rsp, %%rdi\n\t"
        "and $-16, %%rsp\n\t"
        "call n00b_linux_start\n\t"
        "ud2" ::: "memory");
#elif defined(__aarch64__)
    __asm__ volatile(
        "mov x29, #0\n\t"
        "mov x30, #0\n\t"
        "mov x1, x0\n\t"
        "mov x0, sp\n\t"
        "and sp, x0, #-16\n\t"
        "bl n00b_linux_start\n\t"
        "brk #0" ::: "memory");
#endif
}

#endif
