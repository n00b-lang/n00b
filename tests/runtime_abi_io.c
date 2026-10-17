#include "n00b-abi-v1.h"

#include <assert.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

int main(void)
{
    const uint8_t short_bytes[] = {'a', 0, 'b'};
    uint8_t long_bytes[] = "a longer\0binary string";
    assert(n00b_abi_v1_raw_fd_write(1, short_bytes, 0) == 0);
    assert(n00b_abi_v1_raw_fd_write(1, NULL, 3) == -1);
    assert(n00b_abi_v1_raw_fd_write(-1, short_bytes, 1) == -1);
#if defined(_WIN32)
    HANDLE read_handle;
    HANDLE write_handle;
    assert(CreatePipe(&read_handle, &write_handle, NULL, 0));
    int32_t fd = n00b_abi_v1_fd_register_handle(write_handle);
    assert(fd >= 3);
    assert(n00b_abi_v1_raw_fd_write(fd, short_bytes, 3) == 3);
    uint8_t actual[sizeof long_bytes];
    DWORD read_count = 0;
    assert(ReadFile(read_handle, actual, 3, &read_count, NULL));
    assert(read_count == 3);
    assert(memcmp(actual, short_bytes, 3) == 0);
    HANDLE second_read;
    HANDLE second_write;
    assert(CreatePipe(&second_read, &second_write, NULL, 0));
    int32_t second_fd = n00b_abi_v1_fd_register_handle(second_write);
    assert(second_fd > fd);
    assert(n00b_abi_v1_raw_fd_write(second_fd, long_bytes,
                                    sizeof long_bytes - 1)
           == sizeof long_bytes - 1);
    assert(ReadFile(second_read, actual, sizeof long_bytes - 1,
                    &read_count, NULL));
    assert(read_count == sizeof long_bytes - 1);
    assert(memcmp(actual, long_bytes, read_count) == 0);
    assert(n00b_abi_v1_fd_unregister_handle(fd) == 1);
    assert(n00b_abi_v1_raw_fd_write(fd, short_bytes, 3) == -1);
    assert(n00b_abi_v1_fd_unregister_handle(fd) == 0);
    assert(n00b_abi_v1_fd_unregister_handle(1) == -1);
    assert(n00b_abi_v1_fd_register_handle(INVALID_HANDLE_VALUE) == -1);
    assert(n00b_abi_v1_fd_register_handle(write_handle) == fd);
    assert(n00b_abi_v1_fd_unregister_handle(fd) == 1);
    assert(n00b_abi_v1_fd_unregister_handle(second_fd) == 1);
    assert(CloseHandle(write_handle));
    assert(CloseHandle(read_handle));
    assert(CloseHandle(second_write));
    assert(CloseHandle(second_read));
#else
    int fds[2];
    assert(pipe(fds) == 0);
    assert(n00b_abi_v1_raw_fd_write(fds[1], short_bytes, 3) == 3);
    uint8_t actual[sizeof long_bytes];
    assert(read(fds[0], actual, 3) == 3);
    assert(memcmp(actual, short_bytes, 3) == 0);
    assert(n00b_abi_v1_raw_fd_write(fds[1], long_bytes,
                                    sizeof long_bytes - 1)
           == sizeof long_bytes - 1);
    assert(read(fds[0], actual, sizeof long_bytes - 1)
           == sizeof long_bytes - 1);
    assert(memcmp(actual, long_bytes, sizeof long_bytes - 1) == 0);
    close(fds[0]);
    close(fds[1]);
#endif
    return 0;
}
