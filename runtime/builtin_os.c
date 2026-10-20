#include "builtin_os.h"

#include <stdatomic.h>
#include <stdbool.h>

#if defined(__linux__)
#include "builtin_os_syscall.h"
#include <linux/fcntl.h>

enum {
    LINUX_PROT_READ = 1, LINUX_PROT_WRITE = 2,
    LINUX_MAP_PRIVATE_ANON = 0x22,
};

static size_t os_page_size(void)
{
    static atomic_size_t cached;
    size_t page = atomic_load_explicit(&cached, memory_order_acquire);
    if (page) return page;

    long fd = n00b_linux_syscall6(__NR_openat, AT_FDCWD,
                                 (long)"/proc/self/auxv",
                                 O_RDONLY | O_CLOEXEC, 0, 0, 0);
    if (fd < 0) return 0;
    for (;;) {
        uintptr_t entry[2];
        size_t done = 0;
        while (done < sizeof entry) {
            long got = n00b_linux_syscall6(__NR_read, fd, (long)((char *)entry + done),
                                (long)(sizeof entry - done), 0, 0, 0);
            if (got <= 0) {
                n00b_linux_syscall6(__NR_close, fd, 0, 0, 0, 0, 0);
                return 0;
            }
            done += (size_t)got;
        }
        if (entry[0] == 6) {
            page = (size_t)entry[1];
            break;
        }
        if (!entry[0]) break;
    }
    n00b_linux_syscall6(__NR_close, fd, 0, 0, 0, 0, 0);
    if (!page || (page & (page - 1))) return 0;
    atomic_store_explicit(&cached, page, memory_order_release);
    return page;
}

static void os_thread_yield(void)
{
    (void)n00b_linux_syscall6(__NR_sched_yield, 0, 0, 0, 0, 0, 0);
}

static uint64_t os_thread_id(void)
{
    return (uint64_t)n00b_linux_syscall6(__NR_gettid, 0, 0, 0, 0, 0, 0);
}

static bool os_thread_is_main(void)
{
    return n00b_linux_syscall6(__NR_gettid, 0, 0, 0, 0, 0, 0)
        == n00b_linux_syscall6(__NR_getpid, 0, 0, 0, 0, 0, 0);
}

static bool maps_hex(char **cursor, char *end, uintptr_t *value)
{
    uintptr_t result = 0;
    bool saw = false;
    while (*cursor < end) {
        char c = **cursor;
        unsigned digit;
        if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
        else break;
        if (result > (UINTPTR_MAX >> 4)) return false;
        result = (result << 4) | digit;
        (*cursor)++;
        saw = true;
    }
    *value = result;
    return saw;
}

static bool maps_stack_high(char *line, char *end, uintptr_t target,
                            uintptr_t *high)
{
    uintptr_t low;
    uintptr_t limit;
    if (!maps_hex(&line, end, &low) || line >= end || *line++ != '-'
        || !maps_hex(&line, end, &limit)) return false;
    if (target < low || target >= limit) return false;
    *high = limit;
    return true;
}

static uintptr_t os_thread_stack_high_self(void)
{
    static const char path[] = "/proc/self/maps";
    char anchor;
    const uintptr_t target = (uintptr_t)&anchor;
    long fd = n00b_linux_syscall6(__NR_openat, AT_FDCWD, (long)path,
                                   O_RDONLY | O_CLOEXEC, 0, 0, 0);
    if (fd < 0) return 0;

    char buffer[32768];
    size_t used = 0;
    uintptr_t high = 0;
    for (;;) {
        if (used == sizeof(buffer)) break;
        long n = n00b_linux_syscall6(__NR_read, fd,
                                     (long)(buffer + used),
                                     (long)(sizeof(buffer) - used), 0, 0, 0);
        if (n < 0) break;
        used += (size_t)n;
        char *line = buffer;
        char *end = buffer + used;
        while (line < end) {
            char *newline = line;
            while (newline < end && *newline != '\n') newline++;
            if (newline == end) break;
            if (maps_stack_high(line, newline, target, &high)) break;
            line = newline + 1;
        }
        if (high) break;
        if (n == 0) {
            if (line < end)
                (void)maps_stack_high(line, end, target, &high);
            break;
        }
        size_t remaining = (size_t)(end - line);
        for (size_t i = 0; i < remaining; i++) buffer[i] = line[i];
        used = remaining;
    }
    (void)n00b_linux_syscall6(__NR_close, fd, 0, 0, 0, 0, 0);
    return high;
}

static uint64_t os_now_ns(void)
{
    struct { long sec, nsec; } stamp = {0};
    if (n00b_linux_syscall6(__NR_clock_gettime, 1, (long)&stamp, 0, 0, 0, 0) < 0)
        return 0;
    return (uint64_t)stamp.sec * UINT64_C(1000000000)
        + (uint64_t)stamp.nsec;
}

static void *os_map_raw(size_t bytes)
{
    long result = n00b_linux_syscall6(__NR_mmap, 0, (long)bytes,
                           LINUX_PROT_READ | LINUX_PROT_WRITE,
                           LINUX_MAP_PRIVATE_ANON, -1, 0);
    return (unsigned long)result >= (unsigned long)-4095 ? NULL
                                                          : (void *)result;
}

static int32_t os_unmap_raw(void *address, size_t bytes)
{
    long result = n00b_linux_syscall6(__NR_munmap, (long)address, (long)bytes, 0, 0, 0, 0);
    return result < 0 ? -1 : 0;
}

static int32_t os_protect_raw(void *address, size_t bytes, uint32_t protection)
{
    long result = n00b_linux_syscall6(__NR_mprotect, (long)address, (long)bytes,
                           protection, 0, 0, 0);
    return result < 0 ? -1 : 0;
}

static int64_t os_open_raw(const uint8_t *path, int64_t flags, uint32_t mode)
{
    return n00b_linux_syscall6(__NR_openat, AT_FDCWD, (long)path, (long)flags,
                               (long)mode, 0, 0);
}

static int64_t os_read_raw(int64_t fd, uint8_t *buffer, uint64_t count)
{
    return n00b_linux_syscall6(__NR_read, (long)fd, (long)buffer, (long)count,
                               0, 0, 0);
}

static int64_t os_close_raw(int64_t fd)
{
    return n00b_linux_syscall6(__NR_close, (long)fd, 0, 0, 0, 0, 0);
}

static int64_t os_seek_raw(int64_t fd, int64_t offset, int32_t whence)
{
    return n00b_linux_syscall6(__NR_lseek, (long)fd, (long)offset,
                               (long)whence, 0, 0, 0);
}

enum { LINUX_TCGETS = 0x5401, LINUX_TIOCGWINSZ = 0x5413, LINUX_ENOTTY = 25 };

static int64_t os_fd_is_tty_raw(int64_t fd)
{
    uint64_t termios[8];
    long result = n00b_linux_syscall6(__NR_ioctl, (long)fd, LINUX_TCGETS,
                                      (long)termios, 0, 0, 0);
    if (result >= 0) return 1;
    return result == -LINUX_ENOTTY ? 0 : result;
}

static int64_t os_terminal_size_raw(int64_t fd, uint16_t winsize[4])
{
    return n00b_linux_syscall6(__NR_ioctl, (long)fd, LINUX_TIOCGWINSZ,
                               (long)winsize, 0, 0, 0);
}

static int64_t os_pipe_raw(int32_t fds[2])
{
    return n00b_linux_syscall6(__NR_pipe2, (long)fds, O_CLOEXEC, 0, 0, 0, 0);
}

[[noreturn]] static void os_exit_raw(int64_t code)
{
    for (;;)
        (void)n00b_linux_syscall6(__NR_exit_group, (long)(int32_t)code,
                                  0, 0, 0, 0, 0);
}

#elif defined(__APPLE__)

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

static void os_thread_yield(void)
{
    (void)sched_yield();
}

static uint64_t os_thread_id(void)
{
    uint64_t id = 0;
    return pthread_threadid_np(NULL, &id) == 0 ? id : 0;
}

static bool os_thread_is_main(void)
{
    return pthread_main_np() == 1;
}

static uintptr_t os_thread_stack_high_self(void)
{
    return (uintptr_t)pthread_get_stackaddr_np(pthread_self());
}

static uint64_t os_now_ns(void)
{
    struct timespec stamp;
    if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) return 0;
    return (uint64_t)stamp.tv_sec * UINT64_C(1000000000)
        + (uint64_t)stamp.tv_nsec;
}

static size_t os_page_size(void)
{
    long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? (size_t)size : 0;
}

static void *os_map_raw(size_t bytes)
{
    void *result = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    return result == MAP_FAILED ? NULL : result;
}

static int32_t os_unmap_raw(void *address, size_t bytes)
{
    return munmap(address, bytes) == 0 ? 0 : -1;
}

static int32_t os_protect_raw(void *address, size_t bytes, uint32_t protection)
{
    return mprotect(address, bytes, (int)protection) == 0 ? 0 : -1;
}

static int64_t os_errno_result(int64_t result)
{
    return result < 0 ? -(int64_t)errno : result;
}

static int64_t os_open_raw(const uint8_t *path, int64_t flags, uint32_t mode)
{
    return os_errno_result(open((const char *)path, (int)flags, (int)mode));
}

static int64_t os_read_raw(int64_t fd, uint8_t *buffer, uint64_t count)
{
    return os_errno_result(read((int)fd, buffer, (size_t)count));
}

static int64_t os_close_raw(int64_t fd)
{
    return os_errno_result(close((int)fd));
}

[[noreturn]] static void os_exit_raw(int64_t code)
{
    _exit((int)code);
}

static int64_t os_seek_raw(int64_t fd, int64_t offset, int32_t whence)
{
    return os_errno_result(lseek((int)fd, (off_t)offset, whence));
}

static int64_t os_fd_is_tty_raw(int64_t fd)
{
    struct termios settings;
    int64_t result = os_errno_result(ioctl((int)fd, TIOCGETA, &settings));
    if (result >= 0) return 1;
    return result == -ENOTTY ? 0 : result;
}

static int64_t os_terminal_size_raw(int64_t fd, uint16_t winsize[4])
{
    return os_errno_result(ioctl((int)fd, TIOCGWINSZ, winsize));
}

static int64_t os_pipe_raw(int32_t fds[2])
{
    int pair[2];
    int64_t result = os_errno_result(pipe(pair));
    if (result < 0) return result;
    (void)fcntl(pair[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pair[1], F_SETFD, FD_CLOEXEC);
    fds[0] = pair[0];
    fds[1] = pair[1];
    return 0;
}

#elif defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>

static void os_thread_yield(void)
{
    (void)SwitchToThread();
}

static uint64_t os_thread_id(void)
{
    return (uint64_t)GetCurrentThreadId();
}

static bool os_thread_is_main(void)
{
    return false;
}

static uintptr_t os_thread_stack_high_self(void)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    return (uintptr_t)tib->StackBase;
}

static uint64_t os_now_ns(void)
{
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceCounter(&counter)
        || !QueryPerformanceFrequency(&frequency)
        || frequency.QuadPart <= 0) return 0;
    uint64_t ticks = (uint64_t)counter.QuadPart;
    uint64_t rate = (uint64_t)frequency.QuadPart;
    return (ticks / rate) * UINT64_C(1000000000)
        + ((ticks % rate) * UINT64_C(1000000000)) / rate;
}

static size_t os_page_size(void)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (size_t)info.dwPageSize;
}

static size_t os_allocation_granularity(void)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (size_t)info.dwAllocationGranularity;
}

static void *os_map_raw(size_t bytes)
{
    return VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT,
                        PAGE_READWRITE);
}

static int32_t os_unmap_raw(void *address, size_t bytes)
{
    (void)bytes;
    return VirtualFree(address, 0, MEM_RELEASE) ? 0 : -1;
}

static int32_t os_protect_raw(void *address, size_t bytes, uint32_t protection)
{
    DWORD old;
    DWORD mode = protection == N00B_OS_PROT_NONE ? PAGE_NOACCESS
        : protection == N00B_OS_PROT_READ ? PAGE_READONLY : PAGE_READWRITE;
    return VirtualProtect(address, bytes, mode, &old) ? 0 : -1;
}

static int64_t os_errno_result(int64_t result)
{
    return result < 0 ? -(int64_t)errno : result;
}

static int64_t os_open_raw(const uint8_t *path, int64_t flags, uint32_t mode)
{
    return os_errno_result(_open((const char *)path, (int)flags | _O_BINARY,
                                 (int)mode));
}

static int64_t os_read_raw(int64_t fd, uint8_t *buffer, uint64_t count)
{
    unsigned chunk = count > INT_MAX ? INT_MAX : (unsigned)count;
    return os_errno_result(_read((int)fd, buffer, chunk));
}

static int64_t os_close_raw(int64_t fd)
{
    return os_errno_result(_close((int)fd));
}

[[noreturn]] static void os_exit_raw(int64_t code)
{
    ExitProcess((UINT)code);
}

static int64_t os_seek_raw(int64_t fd, int64_t offset, int32_t whence)
{
    return os_errno_result(_lseeki64((int)fd, offset, whence));
}

static int64_t os_fd_is_tty_raw(int64_t fd)
{
    return _isatty((int)fd) ? 1 : 0;
}

static int64_t os_terminal_size_raw(int64_t fd, uint16_t winsize[4])
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE handle = (HANDLE)_get_osfhandle((int)fd);
    if (handle == INVALID_HANDLE_VALUE
        || !GetConsoleScreenBufferInfo(handle, &info)) return -25;
    winsize[0] = (uint16_t)(info.srWindow.Bottom - info.srWindow.Top + 1);
    winsize[1] = (uint16_t)(info.srWindow.Right - info.srWindow.Left + 1);
    return 0;
}

static int64_t os_pipe_raw(int32_t fds[2])
{
    int pair[2];
    int64_t result = os_errno_result(_pipe(pair, 65536, _O_BINARY));
    if (result < 0) return result;
    fds[0] = pair[0];
    fds[1] = pair[1];
    return 0;
}

#else
#error "builtin/os requires macOS, Linux, or Windows"
#endif

#if defined(_WIN32)

static int32_t os_thread_start_raw(n00b_os_thread_entry entry, void *argument)
{
    HANDLE thread = CreateThread(NULL, 0,
                                 (LPTHREAD_START_ROUTINE)(void (*)(void))entry,
                                 argument, 0, NULL);
    if (!thread) return -1;
    CloseHandle(thread);
    return 0;
}

#else

#include <pthread.h>

static int32_t os_thread_start_raw(n00b_os_thread_entry entry, void *argument)
{
    pthread_attr_t attributes;
    pthread_t thread;
    if (pthread_attr_init(&attributes)) return -1;
    int result = pthread_attr_setdetachstate(&attributes,
                                             PTHREAD_CREATE_DETACHED);
    if (!result) result = pthread_create(&thread, &attributes, entry, argument);
    pthread_attr_destroy(&attributes);
    return result ? -1 : 0;
}

#endif

int32_t n00b_os_thread_start(n00b_os_thread_entry entry, void *argument)
{
    return entry ? os_thread_start_raw(entry, argument) : -1;
}

int64_t n00b_os_open(const uint8_t *path, int64_t flags, uint32_t mode)
{
    return path ? os_open_raw(path, flags, mode) : -14;
}

int64_t n00b_os_read(int64_t fd, uint8_t *buffer, uint64_t count)
{
    if (!count) return 0;
    return buffer ? os_read_raw(fd, buffer, count) : -14;
}

int64_t n00b_os_close(int64_t fd)
{
    return os_close_raw(fd);
}

void n00b_os_exit(int64_t code)
{
    os_exit_raw(code);
}

int64_t n00b_os_seek(int64_t fd, int64_t offset, int32_t whence)
{
    return os_seek_raw(fd, offset, whence);
}

extern char **environ;

uint64_t n00b_os_env_count(void)
{
    uint64_t count = 0;
    while (environ && environ[count]) count++;
    return count;
}

const uint8_t *n00b_os_env_at(uint64_t index)
{
    return index < n00b_os_env_count()
        ? (const uint8_t *)environ[index] : NULL;
}

static int32_t saved_argc = 0;
static char **saved_argv = NULL;

void n00b_os_save_args(int32_t argc, char **argv)
{
    saved_argc = argc;
    saved_argv = argv;
}

uint64_t n00b_os_arg_count(void)
{
    return saved_argc < 0 ? 0 : (uint64_t)saved_argc;
}

const uint8_t *n00b_os_arg_at(uint64_t index)
{
    return saved_argv && index < n00b_os_arg_count()
        ? (const uint8_t *)saved_argv[index] : NULL;
}

int64_t n00b_os_fd_is_tty(int64_t fd)
{
    return os_fd_is_tty_raw(fd);
}

int64_t n00b_os_terminal_size(int64_t fd, uint16_t *rows, uint16_t *cols)
{
    uint16_t winsize[4] = {0};
    if (!rows || !cols) return -14;
    int64_t result = os_terminal_size_raw(fd, winsize);
    if (result < 0) return result;
    *rows = winsize[0];
    *cols = winsize[1];
    return 0;
}

int64_t n00b_os_pipe(int64_t *read_fd, int64_t *write_fd)
{
    int32_t fds[2] = {-1, -1};
    if (!read_fd || !write_fd) return -14;
    int64_t result = os_pipe_raw(fds);
    if (result < 0) return result;
    *read_fd = fds[0];
    *write_fd = fds[1];
    return 0;
}

size_t n00b_os_page_size(void)
{
    return os_page_size();
}

void n00b_os_thread_yield(void)
{
    os_thread_yield();
}

void n00b_os_thread_fence(void)
{
    atomic_thread_fence(memory_order_seq_cst);
}

uint64_t n00b_os_thread_is_main(void)
{
    return os_thread_is_main() ? 1 : 0;
}

uint64_t n00b_os_thread_id(void)
{
    return os_thread_id();
}

void *n00b_os_thread_stack_high_self(void)
{
    return (void *)os_thread_stack_high_self();
}

size_t n00b_os_stack_red_zone_bytes(void)
{
#if defined(__x86_64__) || (defined(__APPLE__) && defined(__aarch64__))
    return 128;
#else
    return 0;
#endif
}

uint64_t n00b_os_now_ns(void)
{
    return os_now_ns();
}

void n00b_os_cpu_relax(void)
{
#if defined(_WIN32)
    YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("yield");
#else
    atomic_signal_fence(memory_order_seq_cst);
#endif
}

void *n00b_os_map_zeroed(size_t bytes, size_t alignment)
{
    if (!bytes || (alignment && (alignment & (alignment - 1)))) return NULL;
    if (!alignment) return os_map_raw(bytes);
    size_t page = os_page_size();
    if (!page) return NULL;
    if (!alignment || alignment <= page) return os_map_raw(bytes);
#if defined(_WIN32)
    if (alignment > os_allocation_granularity()) return NULL;
    return os_map_raw(bytes);
#else
    if (bytes > SIZE_MAX - alignment || bytes > SIZE_MAX - page + 1)
        return NULL;
    size_t rounded = (bytes + page - 1) & ~(page - 1);
    if (rounded > SIZE_MAX - alignment) return NULL;
    void *mapping = os_map_raw(rounded + alignment);
    if (!mapping) return NULL;
    uintptr_t base = (uintptr_t)mapping;
    uintptr_t aligned = (base + alignment - 1) & ~(uintptr_t)(alignment - 1);
    size_t head = (size_t)(aligned - base);
    size_t tail = alignment - head;
    if (head && os_unmap_raw(mapping, head) != 0) {
        os_unmap_raw(mapping, rounded + alignment);
        return NULL;
    }
    if (tail && os_unmap_raw((void *)(aligned + rounded), tail) != 0) {
        os_unmap_raw((void *)aligned, rounded + tail);
        return NULL;
    }
    return (void *)aligned;
#endif
}

int32_t n00b_os_unmap(void *address, size_t bytes)
{
    return address && bytes ? os_unmap_raw(address, bytes) : -1;
}

int32_t n00b_os_protect(void *address, size_t bytes, uint32_t protection)
{
    if (!address || !bytes || (protection & ~UINT32_C(3))) return -1;
    return os_protect_raw(address, bytes, protection);
}
