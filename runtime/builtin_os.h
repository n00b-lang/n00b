#ifndef N00B_BUILTIN_OS_H
#define N00B_BUILTIN_OS_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

enum { N00B_OS_CONTEXT_REGS = 48 };

typedef struct {
    void *sp;
    void *fp;
    size_t nregs;
    uintptr_t regs[N00B_OS_CONTEXT_REGS];
    atomic_uint parked;
    uintptr_t handle;
    uint64_t thread_id;
    void *pc;
} n00b_os_thread_context;

enum {
    N00B_OS_PROT_NONE = 0,
    N00B_OS_PROT_READ = 1,
    N00B_OS_PROT_WRITE = 2,
};

size_t n00b_os_page_size(void);
void n00b_os_thread_yield(void);
void n00b_os_thread_fence(void);
uint64_t n00b_os_thread_id(void);
uint64_t n00b_os_thread_is_main(void);
void *n00b_os_thread_stack_high_self(void);
size_t n00b_os_stack_red_zone_bytes(void);
uint64_t n00b_os_now_ns(void);
void n00b_os_cpu_relax(void);
void *n00b_os_map_zeroed(size_t bytes, size_t alignment);
int32_t n00b_os_unmap(void *address, size_t bytes);
int32_t n00b_os_protect(void *address, size_t bytes, uint32_t protection);
int64_t n00b_os_open(const uint8_t *path, int64_t flags, uint32_t mode);
int64_t n00b_os_read(int64_t fd, uint8_t *buffer, uint64_t count);
int64_t n00b_os_close(int64_t fd);
int64_t n00b_os_seek(int64_t fd, int64_t offset, int32_t whence);
uint64_t n00b_os_thread_control_self(void);
void n00b_os_thread_capture_self(n00b_os_thread_context *context);
int32_t n00b_os_thread_suspend(uint64_t control,
                               n00b_os_thread_context *context,
                               uint64_t timeout_ns);
int32_t n00b_os_thread_resume(uint64_t control,
                              n00b_os_thread_context *context);
#endif
