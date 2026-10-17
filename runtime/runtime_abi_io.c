#include "n00b-abi-v1.h"

#include <limits.h>
#include <stdint.h>

#if defined(__linux__)
#include "builtin_os_syscall.h"

static int32_t write_bytes(int32_t fd, const uint8_t *bytes, uint32_t count)
{
    long result = n00b_linux_syscall6(__NR_write, fd, (long)bytes,
                                      count, 0, 0, 0);
    return result < 0 || result > INT32_MAX ? -1 : (int32_t)result;
}
#elif defined(__APPLE__)
#include <unistd.h>

static int32_t write_bytes(int32_t fd, const uint8_t *bytes, uint32_t count)
{
    ssize_t result = write(fd, bytes, count);
    return result < 0 || result > INT32_MAX ? -1 : (int32_t)result;
}
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum { FIRST_DYNAMIC_FD = 3 };

static SRWLOCK fd_map_lock = SRWLOCK_INIT;
static HANDLE *fd_handles;
static size_t fd_handle_count;
static size_t fd_handle_capacity;

int32_t n00b_abi_v1_fd_register_handle(void *native_handle)
{
    HANDLE handle = native_handle;
    if (!handle || handle == INVALID_HANDLE_VALUE) return -1;
    AcquireSRWLockExclusive(&fd_map_lock);
    size_t slot = 0;
    while (slot < fd_handle_count && fd_handles[slot]) slot++;
    if (slot == fd_handle_count) {
        size_t limit = (size_t)INT32_MAX - FIRST_DYNAMIC_FD + 1;
        if (slot == limit) {
            ReleaseSRWLockExclusive(&fd_map_lock);
            return -1;
        }
        if (slot == fd_handle_capacity) {
            size_t next = fd_handle_capacity ? fd_handle_capacity * 2 : 1;
            if (next > limit || next < fd_handle_capacity) next = limit;
            HANDLE *grown = fd_handle_capacity
                ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, fd_handles,
                              next * sizeof(*fd_handles))
                : HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                            next * sizeof(*fd_handles));
            if (!grown) {
                ReleaseSRWLockExclusive(&fd_map_lock);
                return -1;
            }
            fd_handles = grown;
            fd_handle_capacity = next;
        }
        fd_handle_count++;
    }
    fd_handles[slot] = handle;
    ReleaseSRWLockExclusive(&fd_map_lock);
    return (int32_t)(slot + FIRST_DYNAMIC_FD);
}

int32_t n00b_abi_v1_fd_unregister_handle(int32_t fd)
{
    if (fd < FIRST_DYNAMIC_FD) return -1;
    size_t slot = (size_t)(fd - FIRST_DYNAMIC_FD);
    AcquireSRWLockExclusive(&fd_map_lock);
    int32_t result = slot < fd_handle_count && fd_handles[slot] ? 1 : 0;
    if (result) fd_handles[slot] = NULL;
    ReleaseSRWLockExclusive(&fd_map_lock);
    return result;
}

static int32_t write_bytes(int32_t fd, const uint8_t *bytes, uint32_t count)
{
    HANDLE handle;
    bool mapped = fd >= FIRST_DYNAMIC_FD;
    if (mapped) {
        size_t slot = (size_t)(fd - FIRST_DYNAMIC_FD);
        AcquireSRWLockShared(&fd_map_lock);
        handle = slot < fd_handle_count ? fd_handles[slot] : NULL;
    }
    else {
        DWORD standard;
        switch (fd) {
        case 0: standard = STD_INPUT_HANDLE; break;
        case 1: standard = STD_OUTPUT_HANDLE; break;
        case 2: standard = STD_ERROR_HANDLE; break;
        default: return -1;
        }
        handle = GetStdHandle(standard);
    }
    DWORD written = 0;
    int32_t result = handle && handle != INVALID_HANDLE_VALUE
        && WriteFile(handle, bytes, count, &written, NULL)
            ? (int32_t)written : -1;
    if (mapped) ReleaseSRWLockShared(&fd_map_lock);
    return result;
}
#else
#error "raw_fd_write needs an implementation for this target"
#endif

int32_t n00b_abi_v1_raw_fd_write(int32_t fd, const uint8_t *bytes,
                                 uint64_t count)
{
    if (!count) return 0;
    if (fd < 0 || !bytes) return -1;
    return write_bytes(fd, bytes,
                       count > INT32_MAX ? INT32_MAX : (uint32_t)count);
}

int32_t n00bb_runtime_write_bytes(int32_t fd, const uint8_t *bytes,
                                  uint32_t count)
{
    if (fd < 0 || !bytes) return -1;
    return count ? write_bytes(fd, bytes, count) : 0;
}
