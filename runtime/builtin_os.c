#include "builtin_os.h"
#include "runtime_panic.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#if defined(__linux__)
#include "builtin_os_syscall.h"
#include <linux/fcntl.h>
#if defined(N00B_LIBC_FREE)
#include "linux_start.h"
#endif

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

[[noreturn]] static void os_exit_raw(int64_t code)
{
#if defined(N00B_LIBC_FREE)
    n00b_linux_exit_program((int)(int32_t)code);
#else
    for (;;)
        (void)n00b_linux_syscall6(__NR_exit_group, (long)(int32_t)code,
                                  0, 0, 0, 0, 0);
#endif
}

static bool os_map_file_raw(const char *path, const uint8_t **bytes,
                            uint64_t *length)
{
    long fd = n00b_linux_syscall6(__NR_openat, AT_FDCWD, (long)path, 0, 0, 0,
                                  0);
    if (fd < 0) return false;
    long size = n00b_linux_syscall6(__NR_lseek, fd, 0, 2, 0, 0, 0);
    long mapped = size > 0
        ? n00b_linux_syscall6(__NR_mmap, 0, size, 1, 2, fd, 0) : -1;
    n00b_linux_syscall6(__NR_close, fd, 0, 0, 0, 0, 0);
    if (size <= 0 || (mapped < 0 && mapped > -4096)) return false;
    *bytes = (const uint8_t *)mapped;
    *length = (uint64_t)size;
    return true;
}

static uint64_t os_executable_path_raw(uint8_t *buffer, uint64_t capacity)
{
    long used = n00b_linux_syscall6(__NR_readlinkat, AT_FDCWD,
        (long)"/proc/self/exe", (long)buffer, (long)capacity, 0, 0);
    if (used <= 0 || (uint64_t)used >= capacity) return 0;
    buffer[used] = 0;
    return (uint64_t)used;
}

static uint64_t os_current_directory_raw(uint8_t *buffer, uint64_t capacity)
{
    long used = n00b_linux_syscall6(__NR_getcwd, (long)buffer,
                                    (long)capacity, 0, 0, 0, 0);
    if (used <= 0) return 0;
    uint64_t length = 0;
    while (length < capacity && buffer[length]) length++;
    return length < capacity ? length : 0;
}

static bool os_on_stack_raw(const void *address, uint64_t bytes)
{
    (void)bytes;
    return address != NULL;
}

#elif defined(__APPLE__)

#include <mach-o/dyld.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
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

[[noreturn]] static void os_exit_raw(int64_t code)
{
    _exit((int)code);
}

static bool os_map_file_raw(const char *path, const uint8_t **bytes,
                            uint64_t *length)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    off_t size = lseek(fd, 0, SEEK_END);
    void *mapped = size > 0
        ? mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (mapped == MAP_FAILED) return false;
    *bytes = mapped;
    *length = (uint64_t)size;
    return true;
}

static uint64_t os_text_length(const uint8_t *buffer, uint64_t capacity)
{
    uint64_t length = 0;
    while (length < capacity && buffer[length]) length++;
    return length < capacity ? length : 0;
}

static uint64_t os_executable_path_raw(uint8_t *buffer, uint64_t capacity)
{
    uint32_t size = capacity > UINT32_MAX ? UINT32_MAX : (uint32_t)capacity;
    if (_NSGetExecutablePath((char *)buffer, &size)) return 0;
    return os_text_length(buffer, capacity);
}

static uint64_t os_current_directory_raw(uint8_t *buffer, uint64_t capacity)
{
    if (!getcwd((char *)buffer, (size_t)capacity)) return 0;
    return os_text_length(buffer, capacity);
}

static bool os_on_stack_raw(const void *address, uint64_t bytes)
{
    (void)bytes;
    return address != NULL;
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

static DWORD os_main_thread;

__attribute__((constructor)) static void os_record_main_thread(void)
{
    os_main_thread = GetCurrentThreadId();
}

static bool os_thread_is_main(void)
{
    return GetCurrentThreadId() == os_main_thread;
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
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(address, &info, sizeof info) || info.State == MEM_FREE)
        return -1;
    uint8_t *base = info.AllocationBase;
    if (!VirtualFree(address, bytes, MEM_DECOMMIT)) return -1;
    uint8_t *cursor = base;
    while (VirtualQuery(cursor, &info, sizeof info)
           && info.AllocationBase == base) {
        if (info.State == MEM_COMMIT) return 0;
        cursor = (uint8_t *)info.BaseAddress + info.RegionSize;
    }
    return VirtualFree(base, 0, MEM_RELEASE) ? 0 : -1;
}

static wchar_t *os_wide(const uint8_t *path);

[[noreturn]] static void os_exit_raw(int64_t code)
{
    ExitProcess((UINT)code);
}

static wchar_t *os_wide(const uint8_t *path)
{
    int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    (const char *)path, -1, NULL, 0);
    wchar_t *wide = units > 0 ? malloc((size_t)units * sizeof(wchar_t))
                              : NULL;
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     (const char *)path, -1, wide, units)) {
        free(wide);
        wide = NULL;
    }
    return wide;
}

static uint64_t os_utf8_path(const wchar_t *wide, DWORD units,
                             uint8_t *buffer, uint64_t capacity)
{
    int limit = capacity > INT_MAX ? INT_MAX : (int)capacity;
    int length = units ? WideCharToMultiByte(CP_UTF8, 0, wide, (int)units,
        (char *)buffer, limit, NULL, NULL) : 0;
    if (length <= 0 || (uint64_t)length >= capacity) return 0;
    for (int i = 0; i < length; i++)
        if (buffer[i] == '\\') buffer[i] = '/';
    buffer[length] = 0;
    return (uint64_t)length;
}

static bool os_map_file_raw(const char *path, const uint8_t **bytes,
                            uint64_t *length)
{
    wchar_t *wide = os_wide((const uint8_t *)path);
    HANDLE file = wide ? CreateFileW(wide, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL)
        : INVALID_HANDLE_VALUE;
    free(wide);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {0};
    HANDLE section = GetFileSizeEx(file, &size) && size.QuadPart > 0
        ? CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL) : NULL;
    CloseHandle(file);
    const void *view = section
        ? MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (section) CloseHandle(section);
    if (!view) return false;
    *bytes = view;
    *length = (uint64_t)size.QuadPart;
    return true;
}

static wchar_t os_path_units[32768];

static uint64_t os_executable_path_raw(uint8_t *buffer, uint64_t capacity)
{
    DWORD limit = sizeof os_path_units / sizeof os_path_units[0];
    DWORD units = GetModuleFileNameW(NULL, os_path_units, limit);
    return units && units < limit
        ? os_utf8_path(os_path_units, units, buffer, capacity) : 0;
}

static uint64_t os_current_directory_raw(uint8_t *buffer, uint64_t capacity)
{
    DWORD limit = sizeof os_path_units / sizeof os_path_units[0];
    DWORD units = GetCurrentDirectoryW(limit, os_path_units);
    return units && units < limit
        ? os_utf8_path(os_path_units, units, buffer, capacity) : 0;
}

static bool os_on_stack_raw(const void *address, uint64_t bytes)
{
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    uintptr_t at = (uintptr_t)address;
    return at >= low && at < high && high - at >= bytes;
}

#else
#error "builtin/os requires macOS, Linux, or Windows"
#endif

typedef struct {
    n00b_os_thread_entry entry;
    void *argument;
    void *seed;
} os_thread_packet_t;

static void (*_Atomic thread_start_hook)(void);
static void *(*_Atomic thread_spawn_hook)(void);
static void (*_Atomic thread_seed_hook)(void *seed);

void n00b_os_set_thread_start_hook(void (*hook)(void))
{
    atomic_store(&thread_start_hook, hook);
}

void n00b_os_set_thread_seed_hooks(void *(*spawn)(void),
                                   void (*seed)(void *seed))
{
    atomic_store(&thread_spawn_hook, spawn);
    atomic_store(&thread_seed_hook, seed);
}

static void (*_Atomic thread_exit_hook)(void);

void n00b_os_set_thread_exit_hook(void (*hook)(void))
{
    atomic_store(&thread_exit_hook, hook);
}

#if defined(__linux__) && defined(N00B_LIBC_FREE)
// Without malloc, a thread's start packet gets a page of its own.
static os_thread_packet_t *packet_new(void)
{
    long area = n00b_linux_syscall6(__NR_mmap, 0, (long)os_page_size(),
        LINUX_PROT_READ | LINUX_PROT_WRITE, LINUX_MAP_PRIVATE_ANON, -1, 0);
    return area < 0 && area > -4096 ? NULL : (os_thread_packet_t *)area;
}

static void packet_free(os_thread_packet_t *packet)
{
    (void)n00b_linux_syscall6(__NR_munmap, (long)packet, (long)os_page_size(),
                              0, 0, 0, 0);
}
#else
static os_thread_packet_t *packet_new(void)
{
    return malloc(sizeof(os_thread_packet_t));
}

static void packet_free(os_thread_packet_t *packet)
{
    free(packet);
}
#endif

// The collector's detach (sys.internal.gc.alloc.detach_runtime_mutator).
// Programs define it; this weak stand-in serves C programs that link the
// runtime without a collector.
__attribute__((weak)) void n00b_gc_detach_thread(void)
{
}

static void *os_thread_begin(void *raw)
{
    os_thread_packet_t packet = *(os_thread_packet_t *)raw;
    packet_free(raw);
    n00bb_runtime_install_failure_handlers();
    void (*hook)(void) = atomic_load(&thread_start_hook);
    if (hook) hook();
    void (*seed)(void *) = atomic_load(&thread_seed_hook);
    if (seed) seed(packet.seed);
    void *result = packet.entry(packet.argument);
    // A thread that ran n00b code is attached to the collector; it must
    // leave before it ends, or a collection would wait for it forever.
    n00b_gc_detach_thread();
    void (*exit_hook)(void) = atomic_load(&thread_exit_hook);
    if (exit_hook) exit_hook();
    return result;
}

#if defined(_WIN32)

static DWORD WINAPI os_thread_begin_windows(LPVOID raw)
{
    os_thread_begin(raw);
    return 0;
}

static int32_t os_thread_start_raw(os_thread_packet_t *packet)
{
    HANDLE thread = CreateThread(NULL, 0, os_thread_begin_windows, packet, 0,
                                 NULL);
    if (!thread) return -1;
    CloseHandle(thread);
    return 0;
}

#elif defined(__linux__) && defined(N00B_LIBC_FREE)

// Threads without libc: linux_start.c clones them.
static void os_thread_begin_linux(void *raw)
{
    (void)os_thread_begin(raw);
}

static int32_t os_thread_start_raw(os_thread_packet_t *packet)
{
    return n00b_linux_thread_start(os_thread_begin_linux, packet);
}

#else

#include <pthread.h>

static int32_t os_thread_start_raw(os_thread_packet_t *packet)
{
    pthread_attr_t attributes;
    pthread_t thread;
    if (pthread_attr_init(&attributes)) return -1;
    int result = pthread_attr_setdetachstate(&attributes,
                                             PTHREAD_CREATE_DETACHED);
    if (!result)
        result = pthread_create(&thread, &attributes, os_thread_begin,
                                packet);
    pthread_attr_destroy(&attributes);
    return result ? -1 : 0;
}

#endif

int32_t n00b_os_thread_start(n00b_os_thread_entry entry, void *argument)
{
    os_thread_packet_t *packet = entry ? packet_new() : NULL;
    if (!packet) return -1;
    void *(*spawn)(void) = atomic_load(&thread_spawn_hook);
    *packet = (os_thread_packet_t){.entry = entry, .argument = argument,
                                   .seed = spawn ? spawn() : NULL};
    if (!os_thread_start_raw(packet)) return 0;
    packet_free(packet);
    return -1;
}

extern void (*n00b_comptime_exit)(int64_t code);

void n00b_os_exit(int64_t code)
{
    if (n00b_comptime_exit) n00b_comptime_exit(code);
    os_exit_raw(code);
}

#if defined(__linux__) && defined(N00B_LIBC_FREE)
#include "linux_start.h"
#define environ n00b_linux_environ
#else
extern char **environ;
#endif

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
    n00bb_runtime_install_failure_handlers();
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

void n00b_os_thread_yield(void)
{
    os_thread_yield();
}

uint64_t n00b_os_thread_is_main(void)
{
    return os_thread_is_main() ? 1 : 0;
}

uint64_t n00b_os_thread_id(void)
{
    return os_thread_id();
}

uint64_t n00b_os_now_ns(void)
{
    return os_now_ns();
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

bool n00b_os_map_file(const char *path, const uint8_t **bytes,
                      uint64_t *length)
{
    return path && bytes && length && os_map_file_raw(path, bytes, length);
}

uint64_t n00b_os_executable_path(uint8_t *buffer, uint64_t capacity)
{
    return buffer && capacity ? os_executable_path_raw(buffer, capacity) : 0;
}

uint64_t n00b_os_current_directory(uint8_t *buffer, uint64_t capacity)
{
    return buffer && capacity ? os_current_directory_raw(buffer, capacity)
                              : 0;
}

bool n00b_os_on_stack(const void *address, uint64_t bytes)
{
    return os_on_stack_raw(address, bytes);
}


#if !(defined(__linux__) && defined(N00B_LIBC_FREE))
// Shims for contracts that link "c" call this first. Only Linux's runtime
// starts threads without libc's thread state (runtime/linux_start.c); here
// every thread is a libc thread.
void
n00b_ffi_libc_thread_check(void)
{
}
#endif
