#include "n00b-abi-v1.h"

#include <limits.h>
#include <stdint.h>

#include "abi_string.h"

#if defined(__linux__)
static int32_t write_bytes(int32_t fd, const uint8_t *bytes, uint32_t count)
{
    long result;
#if defined(__x86_64__)
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(1L), "D"((long)fd), "S"(bytes), "d"((long)count)
                     : "rcx", "r11", "memory");
#elif defined(__aarch64__)
    register long x0 __asm__("x0") = fd;
    register const uint8_t *x1 __asm__("x1") = bytes;
    register long x2 __asm__("x2") = count;
    register long x8 __asm__("x8") = 64;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x8)
                     : "memory");
    result = x0;
#else
#error "raw_fd_write needs a Linux syscall implementation for this target"
#endif
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

int32_t n00b_abi_v1_raw_fd_write(int32_t fd, n00b_abi_string_v1 text)
{
    n00bb_abi_string_header_t header;
    if (!n00bb_abi_string_decode((const uint8_t *)&text, sizeof text,
                                 &header) ||
        header.tag == N00BB_ABI_STRING_RICH) return -1;
    const uint8_t *bytes = header.tag == N00BB_ABI_STRING_SHORT
                               ? text.short_value.bytes
                               : text.long_value.data;
    if (!header.length) return 0;
    if (fd < 0 || !bytes) return -1;
    uint32_t count = header.length > INT32_MAX
                         ? INT32_MAX : (uint32_t)header.length;
    return write_bytes(fd, bytes, count);
}

static bool write_all(int32_t fd, const uint8_t *bytes, uint64_t count)
{
    while (count) {
        uint32_t chunk = count > INT32_MAX ? INT32_MAX : (uint32_t)count;
        int32_t written = write_bytes(fd, bytes, chunk);
        if (written <= 0) return false;
        bytes += written;
        count -= (uint32_t)written;
    }
    return true;
}

int32_t n00bb_comptime_emit_array(
    int32_t fd, const void *data,
    const n00b_abi_array_descriptor_header_v1 *descriptor,
    uint32_t stride)
{
    if (fd < 0 || !data || !descriptor || !stride
        || descriptor->rank != 1 || descriptor->flat_rank != 1
        || descriptor->flags || descriptor->reserved_zero
        || descriptor->total_count != ((const uint64_t *)(descriptor + 1))[0]
        || descriptor->total_count > UINT64_MAX / stride)
        return -1;
    uint64_t count = descriptor->total_count;
    if (!write_all(fd, (const uint8_t *)&count, sizeof(count))
        || !write_all(fd, data, count * stride)) return -1;
    return 0;
}

int32_t n00bb_comptime_emit_string(int32_t fd, uint64_t low, uint64_t high)
{
    n00b_abi_string_v1 value = {.words = {low, high}};
    n00bb_abi_string_header_t header;
    if (fd < 0 || !n00bb_abi_string_decode((const uint8_t *)&value,
            sizeof(value), &header) || header.tag == N00BB_ABI_STRING_RICH)
        return -1;
    const uint8_t *bytes = header.tag == N00BB_ABI_STRING_SHORT
        ? value.short_value.bytes : value.long_value.data;
    uint8_t encoding = (uint8_t)header.encoding;
    if (header.length && !bytes) return -1;
    if (!write_all(fd, (const uint8_t *)&header.length,
            sizeof(header.length))
        || !write_all(fd, &encoding, sizeof(encoding))
        || !write_all(fd, bytes, header.length)) return -1;
    return 0;
}

int32_t n00bb_comptime_emit_failure(int32_t fd, uint64_t low, uint64_t high)
{
    const uint64_t marker = UINT64_MAX;
    if (fd < 0 || !write_all(fd, (const uint8_t *)&marker,
                            sizeof(marker))) return -1;
    return n00bb_comptime_emit_string(fd, low, high);
}

int32_t n00bb_comptime_emit_effect(int32_t fd)
{
    const uint64_t marker = 0;
    return fd >= 0 && write_all(fd, (const uint8_t *)&marker,
                                sizeof(marker)) ? 0 : -1;
}
