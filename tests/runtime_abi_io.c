#include "n00b-abi-v1.h"

#include <assert.h>
#include <string.h>

int32_t n00bb_comptime_emit_reloc_array(
    int32_t fd, const void *data,
    const n00b_abi_array_descriptor_header_v1 *descriptor,
    uint32_t stride, const uint32_t *string_offsets, uint32_t offset_count,
    const uint8_t *byte_mask);

int32_t n00bb_comptime_emit_reloc_value(
    int32_t fd, const void *data, uint32_t stride,
    const uint32_t *string_offsets, uint32_t offset_count,
    const uint8_t *byte_mask);

int32_t n00bb_comptime_emit_nested_array(
    int32_t fd, const n00b_abi_array_value_v1 *data,
    const n00b_abi_array_descriptor_header_v1 *descriptor,
    uint32_t inner_stride, const uint8_t *byte_mask);

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
    HANDLE second_read;
    HANDLE second_write;
    assert(CreatePipe(&second_read, &second_write, NULL, 0));
    int32_t second_fd = n00b_abi_v1_fd_register_handle(second_write);
    assert(second_fd > fd);
    n00b_abi_string_v1 long_value =
        long_string(long_bytes, sizeof long_bytes - 1, N00B_ABI_STRING_LONG);
    assert(n00b_abi_v1_raw_fd_write(second_fd, long_value) ==
           sizeof long_bytes - 1);
    uint8_t long_actual[sizeof long_bytes];
    assert(ReadFile(second_read, long_actual, sizeof long_bytes - 1,
                    &read_count, NULL));
    assert(read_count == sizeof long_bytes - 1);
    assert(memcmp(long_actual, long_bytes, read_count) == 0);
    n00b_abi_string_v1 span_value =
        long_string(long_bytes + 2, 5, N00B_ABI_STRING_SPAN);
    assert(n00b_abi_v1_raw_fd_write(second_fd, span_value) == 5);
    assert(ReadFile(second_read, long_actual, 5, &read_count, NULL));
    assert(read_count == 5);
    assert(memcmp(long_actual, long_bytes + 2, 5) == 0);
    assert(n00b_abi_v1_fd_unregister_handle(fd) == 1);
    assert(n00b_abi_v1_raw_fd_write(fd, short_value) == -1);
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
    struct {
        n00b_abi_array_descriptor_header_v1 header;
        uint64_t extent;
    } descriptor = {
        .header = {.total_count = 1, .rank = 1, .flat_rank = 1},
        .extent = 1,
    };
    uint8_t element[48];
    uint8_t mask[sizeof(element)] = {0};
    memset(element, 0xa5, sizeof(element));
    n00b_abi_string_v1 left = short_string((const uint8_t *)"left", 4);
    n00b_abi_string_v1 right = short_string((const uint8_t *)"right", 5);
    memcpy(element, &left, sizeof(left));
    memcpy(element + 32, &right, sizeof(right));
    memset(mask, 1, sizeof(left));
    memset(mask + 20, 1, 4);
    memset(mask + 32, 1, sizeof(right));
    uint32_t offsets[] = {0, 32};
    assert(n00bb_comptime_emit_reloc_array(fds[1], element,
        &descriptor.header, sizeof(element), offsets, 2, mask) == 0);
    uint8_t serialized[8 + sizeof(element) + 9 + 4 + 9 + 5];
    assert(read(fds[0], serialized, sizeof(serialized))
        == sizeof(serialized));
    uint64_t count;
    memcpy(&count, serialized, sizeof(count));
    assert(count == 1);
    for (size_t i = 0; i < sizeof(element); i++) {
        uint8_t expected = i >= 20 && i < 24 ? 0xa5 : 0;
        assert(serialized[8 + i] == expected);
    }
    size_t pos = 8 + sizeof(element);
    uint64_t length;
    memcpy(&length, serialized + pos, sizeof(length));
    assert(length == 4);
    pos += sizeof(length) + 1;
    assert(memcmp(serialized + pos, "left", 4) == 0);
    pos += 4;
    memcpy(&length, serialized + pos, sizeof(length));
    assert(length == 5);
    pos += sizeof(length) + 1;
    assert(memcmp(serialized + pos, "right", 5) == 0);
    assert(n00bb_comptime_emit_reloc_value(fds[1], element,
        sizeof(element), offsets, 2, mask) == 0);
    uint8_t serialized_value[sizeof(serialized)];
    assert(read(fds[0], serialized_value, sizeof(serialized_value))
        == sizeof(serialized_value));
    assert(memcmp(serialized_value, serialized,
                  sizeof(serialized)) == 0);
    struct {
        n00b_abi_array_descriptor_header_v1 header;
        uint64_t extent;
    } shapes[] = {
        {{.total_count = 2, .rank = 1, .flat_rank = 1}, 2},
        {{.total_count = 1, .rank = 1, .flat_rank = 1}, 1},
        {{.total_count = 2, .rank = 1, .flat_rank = 1}, 2},
    };
    uint64_t first[] = {11, UINT64_MAX, 12, UINT64_MAX};
    uint64_t second[] = {13, UINT64_MAX};
    uint8_t inner_mask[16] = {1, 1, 1, 1, 1, 1, 1, 1};
    n00b_abi_array_value_v1 children[] = {
        {first, &shapes[0].header},
        {second, &shapes[1].header},
    };
    assert(n00bb_comptime_emit_nested_array(fds[1], children,
        &shapes[2].header, sizeof(inner_mask), inner_mask) == 0);
    uint64_t expected_nested[] = {2, 2, 11, 0, 12, 0, 1, 13, 0};
    uint64_t actual_nested[sizeof(expected_nested) / sizeof(uint64_t)];
    assert(read(fds[0], actual_nested, sizeof(actual_nested))
        == sizeof(actual_nested));
    assert(memcmp(actual_nested, expected_nested,
                  sizeof(expected_nested)) == 0);
    close(fds[0]);
    close(fds[1]);
#endif
    return 0;
}
