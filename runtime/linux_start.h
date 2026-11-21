#pragma once

#include <stdint.h>

extern char **n00b_linux_environ;

uintptr_t n00b_linux_auxv(uintptr_t type);

uintptr_t n00b_linux_tls_bytes(void);
uintptr_t n00b_linux_tls_init(void *area);

int32_t n00b_linux_thread_start(void (*entry)(void *), void *argument);

uintptr_t n00b_linux_stack_high(uintptr_t sp);

#include <stddef.h>
void *n00b_linux_calloc(size_t count, size_t size);
void *n00b_linux_realloc(void *memory, size_t size);
void n00b_linux_free(void *memory);
int n00b_linux_open(const char *path, int flags, int mode);
int n00b_linux_close(int fd);
long n00b_linux_write(int fd, const void *bytes, size_t count);
[[noreturn]] void n00b_linux_exit(int status);
[[noreturn]] void n00b_linux_exit_program(int status);
