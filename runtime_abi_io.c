#include "n00b-abi-v1.h"

#include <limits.h>

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

static int32_t write_bytes(int32_t fd, const uint8_t *bytes, uint32_t count)
{
    DWORD standard;
    switch (fd) {
    case 0: standard = STD_INPUT_HANDLE; break;
    case 1: standard = STD_OUTPUT_HANDLE; break;
    case 2: standard = STD_ERROR_HANDLE; break;
    default: return -1;
    }
    HANDLE handle = GetStdHandle(standard);
    if (!handle || handle == INVALID_HANDLE_VALUE) return -1;
    DWORD written = 0;
    return WriteFile(handle, bytes, count, &written, NULL)
               ? (int32_t)written : -1;
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
