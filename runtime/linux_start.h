#pragma once

// What Linux programs get instead of libc's startup and environment (see
// linux_start.c). Only in the program runtime, built with N00B_LIBC_FREE.

#include <stdint.h>

// The process environment, as the kernel passed it.
extern char **n00b_linux_environ;

// An auxiliary vector entry (AT_PAGESZ, AT_HWCAP, ...), or 0.
uintptr_t n00b_linux_auxv(uintptr_t type);

// The bytes a thread's TLS area needs, and how to fill one: returns the
// thread pointer for the area.
uintptr_t n00b_linux_tls_bytes(void);
uintptr_t n00b_linux_tls_init(void *area);

// Starts a thread that calls entry(argument) and then exits. Returns 0, or
// -1 if the thread could not be made.
int32_t n00b_linux_thread_start(void (*entry)(void *), void *argument);

// Stand-ins for the libc calls the compile-time runtime makes.
#include <stddef.h>
void *n00b_linux_calloc(size_t count, size_t size);
void *n00b_linux_realloc(void *memory, size_t size);
void n00b_linux_free(void *memory);
int n00b_linux_open(const char *path, int flags, int mode);
int n00b_linux_close(int fd);
long n00b_linux_write(int fd, const void *bytes, size_t count);
[[noreturn]] void n00b_linux_exit(int status);
