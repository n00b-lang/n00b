#ifndef N00B_BUILTIN_OS_H
#define N00B_BUILTIN_OS_H

#include <stdbool.h>
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

typedef void *(*n00b_os_thread_entry)(void *);

void n00b_os_thread_yield(void);
uint64_t n00b_os_thread_id(void);
uint64_t n00b_os_thread_is_main(void);
int32_t n00b_os_thread_start(n00b_os_thread_entry entry, void *argument);
void n00b_os_set_thread_start_hook(void (*hook)(void));
void n00b_os_set_thread_exit_hook(void (*hook)(void));
void n00b_os_set_thread_seed_hooks(void *(*spawn)(void),
                                   void (*seed)(void *seed));
uint64_t n00b_os_now_ns(void);
void *n00b_os_map_zeroed(size_t bytes, size_t alignment);
int32_t n00b_os_unmap(void *address, size_t bytes);
[[noreturn]] void n00b_os_exit(int64_t code);
uint64_t n00b_os_env_count(void);
const uint8_t *n00b_os_env_at(uint64_t index);
void n00b_os_save_args(int32_t argc, char **argv);
uint64_t n00b_os_arg_count(void);
const uint8_t *n00b_os_arg_at(uint64_t index);
void n00b_os_thread_capture_self(n00b_os_thread_context *context);
void n00b_os_gc_enter_blocking(void);
#if defined(__linux__)
uintptr_t n00b_linux_stw_handler(void);
uintptr_t n00b_linux_stw_restorer(void);
#endif
bool n00b_os_map_file(const char *path, const uint8_t **bytes,
                      uint64_t *length);
uint64_t n00b_os_executable_path(uint8_t *buffer, uint64_t capacity);
uint64_t n00b_os_current_directory(uint8_t *buffer, uint64_t capacity);
bool n00b_os_on_stack(const void *address, uint64_t bytes);
#endif
