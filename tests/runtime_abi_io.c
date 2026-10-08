#include "n00b-abi-v1.h"

#include <assert.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

static n00b_abi_string_v1 short_string(const uint8_t *bytes, uint8_t count)
{
    n00b_abi_string_v1 value = {0};
    assert(count <= 14);
    memcpy(value.short_value.bytes, bytes, count);
    value.short_value.selector = count;
    return value;
}

static n00b_abi_string_v1 long_string(uint8_t *bytes, uint64_t count,
                                      uint32_t tag)
{
    n00b_abi_string_v1 value = {0};
    value.long_value.data = bytes;
    value.long_value.selector = ((uint64_t)tag << N00B_ABI_STRING_TAG_SHIFT)
                                | count;
    return value;
}

int main(void)
{
    n00b_abi_string_v1 empty = short_string((const uint8_t *)"", 0);
    assert(n00b_abi_v1_raw_fd_write(1, empty) == 0);
    n00b_abi_string_v1 bad = empty;
    bad.short_value.selector = 15;
    assert(n00b_abi_v1_raw_fd_write(1, bad) == -1);
    assert(n00b_abi_v1_raw_fd_write(-1,
        short_string((const uint8_t *)"x", 1)) == -1);
    uint8_t long_bytes[] = "a longer\0binary string";
    n00b_abi_string_v1 rich = long_string(long_bytes, 0, N00B_ABI_STRING_RICH);
    assert(n00b_abi_v1_raw_fd_write(1, rich) == -1);
#if defined(_WIN32)
    HANDLE read_handle;
    HANDLE write_handle;
    assert(CreatePipe(&read_handle, &write_handle, NULL, 0));
    int32_t fd = n00b_abi_v1_fd_register_handle(write_handle);
    assert(fd >= 3);
    const uint8_t short_bytes[] = {'a', 0, 'b'};
    n00b_abi_string_v1 short_value = short_string(short_bytes, 3);
    assert(n00b_abi_v1_raw_fd_write(fd, short_value) == 3);
    uint8_t actual[sizeof short_bytes];
    DWORD read_count = 0;
    assert(ReadFile(read_handle, actual, sizeof actual, &read_count, NULL));
    assert(read_count == sizeof actual);
    assert(memcmp(actual, short_bytes, sizeof actual) == 0);
    assert(n00b_abi_v1_fd_unregister_handle(fd) == 1);
    assert(n00b_abi_v1_raw_fd_write(fd, short_value) == -1);
    assert(n00b_abi_v1_fd_unregister_handle(fd) == 0);
    assert(n00b_abi_v1_fd_unregister_handle(1) == -1);
    assert(n00b_abi_v1_fd_register_handle(INVALID_HANDLE_VALUE) == -1);
    assert(n00b_abi_v1_fd_register_handle(write_handle) == fd);
    assert(n00b_abi_v1_fd_unregister_handle(fd) == 1);
    assert(CloseHandle(write_handle));
    assert(CloseHandle(read_handle));
#else
    int fds[2];
    assert(pipe(fds) == 0);
    const uint8_t short_bytes[] = {'a', 0, 'b'};
    n00b_abi_string_v1 short_value = short_string(short_bytes, 3);
    assert(n00b_abi_v1_raw_fd_write(fds[1], short_value) == 3);
    uint8_t actual[sizeof long_bytes];
    assert(read(fds[0], actual, 3) == 3);
    assert(memcmp(actual, short_bytes, 3) == 0);
    n00b_abi_string_v1 long_value =
        long_string(long_bytes, sizeof long_bytes - 1, N00B_ABI_STRING_LONG);
    assert(n00b_abi_v1_raw_fd_write(fds[1], long_value) ==
           sizeof long_bytes - 1);
    assert(read(fds[0], actual, sizeof long_bytes - 1) ==
           sizeof long_bytes - 1);
    assert(memcmp(actual, long_bytes, sizeof long_bytes - 1) == 0);
    n00b_abi_string_v1 span_value =
        long_string(long_bytes + 2, 5, N00B_ABI_STRING_SPAN);
    assert(n00b_abi_v1_raw_fd_write(fds[1], span_value) == 5);
    assert(read(fds[0], actual, 5) == 5);
    assert(memcmp(actual, long_bytes + 2, 5) == 0);
    close(fds[0]);
    close(fds[1]);
#endif
    return 0;
}
