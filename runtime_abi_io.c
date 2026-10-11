#include "n00b-abi-v1.h"

#include <limits.h>
#include <stdint.h>

#include "abi_string.h"

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

int32_t n00bb_bootstrap_fd_write(int32_t fd, uint64_t low, uint64_t high)
{
    n00b_abi_string_v1 text = {.words = {low, high}};
    return n00b_abi_v1_raw_fd_write(fd, text);
}

int32_t n00bb_bootstrap_string_equal(uint64_t left_low, uint64_t left_high,
                                     uint64_t right_low, uint64_t right_high)
{
    n00b_abi_string_v1 left = {.words = {left_low, left_high}};
    n00b_abi_string_v1 right = {.words = {right_low, right_high}};
    n00bb_abi_string_header_t left_header, right_header;
    if (!n00bb_abi_string_decode((const uint8_t *)&left, sizeof(left),
                                 &left_header)
        || !n00bb_abi_string_decode((const uint8_t *)&right, sizeof(right),
                                    &right_header)
        || left_header.tag == N00BB_ABI_STRING_RICH
        || right_header.tag == N00BB_ABI_STRING_RICH) __builtin_trap();
    if (left_header.length != right_header.length) return 0;
    const uint8_t *left_bytes = left_header.tag == N00BB_ABI_STRING_SHORT
        ? left.short_value.bytes : left.long_value.data;
    const uint8_t *right_bytes = right_header.tag == N00BB_ABI_STRING_SHORT
        ? right.short_value.bytes : right.long_value.data;
    for (uint64_t i = 0; i < left_header.length; i++)
        if (left_bytes[i] != right_bytes[i]) return 0;
    return 1;
}

int32_t n00bb_bootstrap_string_compare(uint64_t left_low, uint64_t left_high,
                                       uint64_t right_low, uint64_t right_high)
{
    n00b_abi_string_v1 left = {.words = {left_low, left_high}};
    n00b_abi_string_v1 right = {.words = {right_low, right_high}};
    n00bb_abi_string_header_t left_header, right_header;
    if (!n00bb_abi_string_decode((const uint8_t *)&left, sizeof left,
                                 &left_header)
        || !n00bb_abi_string_decode((const uint8_t *)&right, sizeof right,
                                    &right_header)
        || left_header.tag == N00BB_ABI_STRING_RICH
        || right_header.tag == N00BB_ABI_STRING_RICH) __builtin_trap();
    const uint8_t *left_bytes = left_header.tag == N00BB_ABI_STRING_SHORT
        ? left.short_value.bytes : left.long_value.data;
    const uint8_t *right_bytes = right_header.tag == N00BB_ABI_STRING_SHORT
        ? right.short_value.bytes : right.long_value.data;
    uint64_t common = left_header.length < right_header.length
        ? left_header.length : right_header.length;
    for (uint64_t i = 0; i < common; i++) {
        if (left_bytes[i] < right_bytes[i]) return -1;
        if (left_bytes[i] > right_bytes[i]) return 1;
    }
    if (left_header.length < right_header.length) return -1;
    if (left_header.length > right_header.length) return 1;
    return 0;
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

static int32_t repr_bytes(int32_t fd, const char *text, uint32_t count)
{
    return write_all(fd, (const uint8_t *)text, count) ? (int32_t)count : -1;
}

static int32_t repr_integer(int32_t fd, unsigned __int128 bits,
                            uint32_t width, bool is_signed)
{
    if (!width || width > 128) return -1;
    if (width < 128) bits &= ((unsigned __int128)1 << width) - 1;
    bool negative = is_signed && (bits >> (width - 1)) & 1;
    if (negative) {
        if (width < 128) bits |= ~(((unsigned __int128)1 << width) - 1);
        bits = 0 - bits;
    }
    char digits[40];
    char *start = digits + sizeof digits;
    do {
        *--start = (char)('0' + bits % 10);
        bits /= 10;
    } while (bits);
    if (negative) *--start = '-';
    return repr_bytes(fd, start, (uint32_t)(digits + sizeof digits - start));
}

static int32_t repr_pointer(int32_t fd, uintptr_t pointer)
{
    static const char hex[] = "0123456789abcdef";
    char digits[sizeof(uintptr_t) * 2];
    for (size_t i = sizeof digits; i > 0; i--) {
        digits[i - 1] = hex[pointer & 15];
        pointer >>= 4;
    }
    return repr_bytes(fd, digits, sizeof digits);
}

static char *repr_append_unsigned(char *end, uint64_t value)
{
    char digits[24];
    char *start = digits + sizeof digits;
    do {
        *--start = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    size_t count = (size_t)(digits + sizeof digits - start);
    __builtin_memcpy(end, start, count);
    return end + count;
}

static char *repr_fraction(char *end, double fraction)
{
    char *dot = end;
    *end++ = '.';
    for (unsigned i = 0; i < 6; i++) {
        fraction *= 10.0;
        int digit = (int)fraction;
        if (digit < 0) digit = 0;
        if (digit > 9) digit = 9;
        *end++ = (char)('0' + digit);
        fraction -= (double)digit;
    }
    while (end > dot + 2 && end[-1] == '0') end--;
    return end;
}

static int32_t repr_decimal_exponent(double *value)
{
    int32_t exponent = 0;
    while (*value >= 10.0) {
        *value /= 10.0;
        exponent++;
    }
    while (*value < 1.0) {
        *value *= 10.0;
        exponent--;
    }
    return exponent;
}

static int32_t repr_float(int32_t fd, uint64_t payload, uint32_t width)
{
    double value;
    uint64_t mantissa;
    bool special;
    bool negative;
    if (width == 32) {
        uint32_t bits = (uint32_t)payload;
        float single;
        __builtin_memcpy(&single, &bits, sizeof single);
        value = single;
        negative = (bits >> 31) != 0;
        special = ((bits >> 23) & 0xff) == 0xff;
        mantissa = bits & UINT32_C(0x7fffff);
    } else if (width == 64) {
        __builtin_memcpy(&value, &payload, sizeof value);
        negative = (payload >> 63) != 0;
        special = ((payload >> 52) & 0x7ff) == 0x7ff;
        mantissa = payload & UINT64_C(0x000fffffffffffff);
    } else return -1;

    char buffer[64];
    char *end = buffer;
    if (negative) {
        *end++ = '-';
        value = -value;
    }
    if (special) {
        const char *word = mantissa ? "nan" : "infinity";
        size_t count = mantissa ? 3 : 8;
        __builtin_memcpy(end, word, count);
        end += count;
    } else if (value == 0.0) {
        __builtin_memcpy(end, "0.0", 3);
        end += 3;
    } else {
        double scaled = value;
        int32_t exponent = repr_decimal_exponent(&scaled);
        if (exponent < -4 || exponent >= 6) {
            int first = (int)scaled;
            *end++ = (char)('0' + first);
            end = repr_fraction(end, scaled - (double)first);
            *end++ = 'e';
            if (exponent < 0) {
                *end++ = '-';
                exponent = -exponent;
            }
            end = repr_append_unsigned(end, (uint32_t)exponent);
        } else {
            uint64_t whole = (uint64_t)value;
            end = repr_append_unsigned(end, whole);
            end = repr_fraction(end, value - (double)whole);
        }
    }
    return repr_bytes(fd, buffer, (uint32_t)(end - buffer));
}

static int32_t repr_string(int32_t fd, const n00b_abi_string_v1 *text)
{
    n00bb_abi_string_header_t header;
    if (!n00bb_abi_string_decode((const uint8_t *)text, sizeof(*text),
                                 &header)
        || header.tag == N00BB_ABI_STRING_RICH
        || header.length > INT32_MAX) return -1;
    const uint8_t *bytes = header.tag == N00BB_ABI_STRING_SHORT
        ? text->short_value.bytes : text->long_value.data;
    return bytes && write_all(fd, bytes, header.length)
        ? (int32_t)header.length : header.length == 0 ? 0 : -1;
}

typedef int32_t (*repr_dispatch_t)(uint16_t array_extent,
                                   uint16_t byte_stride, uint32_t type_field,
                                   uint64_t payload, int32_t fd);

int32_t n00bb_bootstrap_repr_default(int32_t fd, uint16_t array_extent,
                                     uint16_t byte_stride, uint32_t type_field,
                                     uint64_t payload, repr_dispatch_t dispatch);

int32_t n00bb_bootstrap_type_is_array(uint32_t type_field)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_type_v1 *type = snapshot
        ? n00b_abi_v1_type(snapshot, type_field & UINT32_C(0x7fffffff))
        : NULL;
    return type && type->kind == N00B_ABI_TYPE_ARRAY;
}

int32_t n00bb_bootstrap_type_specializes(uint32_t type_field,
                                         uint32_t general_type_index)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    if (!snapshot || !general_type_index) __builtin_trap();
    int32_t match = n00b_abi_v1_type_specializes(snapshot,
        type_field & UINT32_C(0x7fffffff), general_type_index);
    if (match < 0) __builtin_trap();
    return match;
}

static int32_t repr_array(const n00b_abi_snapshot_v1 *snapshot, int32_t fd,
                          const n00b_abi_type_v1 *type, uint64_t payload,
                          repr_dispatch_t dispatch)
{
    if (!payload || !dispatch || type->components.count != 1) return -1;
    uint32_t element_index;
    if (n00b_abi_v1_index_at(snapshot, type->components, 0,
                             &element_index) != 1) return -1;
    const n00b_abi_type_v1 *element = n00b_abi_v1_type(snapshot,
                                                        element_index);
    if (!element || !element->alignment_bytes || !element->size_bits
        || element->size_bits > UINT64_MAX - 7) return -1;
    uint64_t size = (element->size_bits + 7) / 8;
    uint64_t alignment = element->alignment_bytes;
    if (size > UINT64_MAX - alignment + 1) return -1;
    uint64_t stride = ((size + alignment - 1) / alignment) * alignment;
    n00b_abi_array_value_v1 array;
    __builtin_memcpy(&array, (const void *)(uintptr_t)payload, sizeof array);
    const n00b_abi_array_descriptor_header_v1 *descriptor = array.descriptor;
    if (!array.data || !descriptor || descriptor->rank != 1
        || descriptor->flat_rank != 1 || descriptor->flags
        || descriptor->reserved_zero
        || descriptor->total_count != ((const uint64_t *)(descriptor + 1))[0]
        || descriptor->total_count > UINT64_MAX / stride) return -1;

    int64_t written = repr_bytes(fd, "[", 1);
    if (written < 0) return -1;
    const uint8_t *item = array.data;
    for (uint64_t i = 0; i < descriptor->total_count; i++, item += stride) {
        if (i) {
            int32_t separator = repr_bytes(fd, ", ", 2);
            if (separator < 0) return -1;
            written += separator;
        }
        uint64_t item_payload = 0;
        if (size <= sizeof item_payload)
            __builtin_memcpy(&item_payload, item, size);
        else item_payload = (uint64_t)(uintptr_t)item;
        uint32_t item_field = element_index
            | (size > sizeof item_payload ? UINT32_C(0x80000000) : 0);
        int32_t part = dispatch(0, 0, item_field, item_payload, fd);
        if (part < 0) return -1;
        written += part;
        if (written > INT32_MAX) return -1;
    }
    if (repr_bytes(fd, "]", 1) < 0 || written == INT32_MAX) return -1;
    return (int32_t)written + 1;
}

int32_t n00bb_bootstrap_repr_default(int32_t fd, uint16_t array_extent,
                                     uint16_t byte_stride, uint32_t type_field,
                                     uint64_t payload, repr_dispatch_t dispatch)
{
    (void)array_extent;
    (void)byte_stride;
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    uint32_t type_index = type_field & UINT32_C(0x7fffffff);
    const n00b_abi_type_v1 *type = snapshot
        ? n00b_abi_v1_type(snapshot, type_index) : NULL;
    if (!type || !type_index || fd < 0) return -1;
    const uint8_t *name = NULL;
    uint64_t name_length = 0;
    if (n00b_abi_v1_string_bytes(snapshot, &type->canonical_name,
                                &name, &name_length) != 1) return -1;
    bool boolean = name_length == 4 && !__builtin_memcmp(name, "bool", 4);
    bool floating = (name_length == 3
        && (!__builtin_memcmp(name, "f64", 3)
            || !__builtin_memcmp(name, "f32", 3)))
        || (name_length == 5 && !__builtin_memcmp(name, "float", 5));
    if (boolean) return repr_bytes(fd, payload ? "True" : "False",
                                   payload ? 4 : 5);
    if (type->kind == N00B_ABI_TYPE_REFERENCE)
        return repr_pointer(fd, (uintptr_t)payload);
    if (type->kind == N00B_ABI_TYPE_PRIMITIVE && floating)
        return repr_float(fd, payload, type->semantic_width_bits);
    if (type->kind == N00B_ABI_TYPE_STRING) {
        if (!(type_field & UINT32_C(0x80000000)) || !payload) return -1;
        return repr_string(fd, (const n00b_abi_string_v1 *)(uintptr_t)payload);
    }
    if (type->kind == N00B_ABI_TYPE_ARRAY)
        return type_field & UINT32_C(0x80000000)
            ? repr_array(snapshot, fd, type, payload, dispatch) : -1;
    if (type->kind == N00B_ABI_TYPE_PRIMITIVE && !floating
        && type->semantic_width_bits && type->semantic_width_bits <= 128) {
        unsigned __int128 bits = payload;
        if (type->unpacked_storage_bits > 64) {
            if (!payload) return -1;
            __builtin_memcpy(&bits, (const void *)(uintptr_t)payload,
                             sizeof bits);
        }
        return repr_integer(fd, bits, type->semantic_width_bits,
            (type->flags & N00B_ABI_TYPE_FLAG_SIGNED) != 0);
    }
    if (repr_bytes(fd, "<", 1) < 0 || name_length > INT32_MAX
        || !write_all(fd, name, name_length)
        || repr_bytes(fd, ">", 1) < 0) return -1;
    return (int32_t)name_length + 2;
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

static bool write_masked_segment(int32_t fd, const uint8_t *element,
                                 const uint8_t *mask, uint32_t start,
                                 uint32_t end);
static bool reloc_offsets_valid(uint32_t stride,
                                const uint32_t *string_offsets,
                                uint32_t offset_count,
                                const uint8_t *byte_mask);
static bool write_reloc_element(int32_t fd, const uint8_t *element,
                                uint32_t stride,
                                const uint32_t *string_offsets,
                                uint32_t offset_count,
                                const uint8_t *byte_mask);

static int32_t write_nested_array(
    int32_t fd, const n00b_abi_array_value_v1 *data,
    const n00b_abi_array_descriptor_header_v1 *descriptor,
    uint32_t inner_stride, const uint32_t *string_offsets,
    uint32_t offset_count, const uint8_t *byte_mask)
{
    if (fd < 0 || !data || !descriptor || !inner_stride || !byte_mask
        || (string_offsets && !reloc_offsets_valid(inner_stride,
            string_offsets, offset_count, byte_mask))
        || descriptor->rank != 1 || descriptor->flat_rank != 1
        || descriptor->flags || descriptor->reserved_zero
        || descriptor->total_count != ((const uint64_t *)(descriptor + 1))[0]
        || descriptor->total_count > SIZE_MAX / sizeof(*data)) return -1;
    uint64_t count = descriptor->total_count;
    if (!write_all(fd, (const uint8_t *)&count, sizeof(count))) return -1;
    for (uint64_t i = 0; i < count; i++) {
        const n00b_abi_array_descriptor_header_v1 *child =
            data[i].descriptor;
        if (!data[i].data || !child || child->rank != 1
            || child->flat_rank != 1 || child->flags || child->reserved_zero
            || child->total_count != ((const uint64_t *)(child + 1))[0]
            || child->total_count > SIZE_MAX / inner_stride) return -1;
        uint64_t child_count = child->total_count;
        if (!write_all(fd, (const uint8_t *)&child_count,
                       sizeof(child_count))) return -1;
        for (uint64_t j = 0; j < child_count; j++) {
            const uint8_t *element = (const uint8_t *)data[i].data
                + j * inner_stride;
            if (string_offsets
                ? !write_reloc_element(fd, element, inner_stride,
                    string_offsets, offset_count, byte_mask)
                : !write_masked_segment(fd, element, byte_mask, 0,
                                        inner_stride)) return -1;
        }
    }
    return 0;
}

int32_t n00bb_comptime_emit_nested_array(
    int32_t fd, const n00b_abi_array_value_v1 *data,
    const n00b_abi_array_descriptor_header_v1 *descriptor,
    uint32_t inner_stride, const uint8_t *byte_mask)
{
    return write_nested_array(fd, data, descriptor, inner_stride,
                              nullptr, 0, byte_mask);
}

int32_t n00bb_comptime_emit_nested_reloc_array(
    int32_t fd, const n00b_abi_array_value_v1 *data,
    const n00b_abi_array_descriptor_header_v1 *descriptor,
    uint32_t inner_stride, const uint32_t *string_offsets,
    uint32_t offset_count, const uint8_t *byte_mask)
{
    return write_nested_array(fd, data, descriptor, inner_stride,
                              string_offsets, offset_count, byte_mask);
}

int32_t n00bb_comptime_emit_value(int32_t fd, const void *data,
                                   uint32_t size)
{
    const uint64_t count = 1;
    if (fd < 0 || !data || !size
        || !write_all(fd, (const uint8_t *)&count, sizeof(count))
        || !write_all(fd, data, size)) return -1;
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

static bool write_masked_segment(int32_t fd, const uint8_t *element,
                                 const uint8_t *mask, uint32_t start,
                                 uint32_t end)
{
    const uint8_t zero[sizeof(n00b_abi_string_v1)] = {0};
    for (uint32_t cursor = start; cursor < end;) {
        uint32_t next = cursor + 1;
        while (next < end && mask[next] == mask[cursor]) next++;
        if (mask[cursor] == UINT8_MAX) {
            if (!write_all(fd, element + cursor, next - cursor)) return false;
        } else if (!mask[cursor]) {
            for (uint32_t left = next - cursor; left;) {
                uint32_t chunk = left < sizeof(zero) ? left : sizeof(zero);
                if (!write_all(fd, zero, chunk)) return false;
                left -= chunk;
            }
        } else {
            for (uint32_t at = cursor; at < next; at++) {
                uint8_t masked = element[at] & mask[at];
                if (!write_all(fd, &masked, sizeof(masked))) return false;
            }
        }
        cursor = next;
    }
    return true;
}

static bool reloc_offsets_valid(uint32_t stride,
                                const uint32_t *string_offsets,
                                uint32_t offset_count,
                                const uint8_t *byte_mask)
{
    if (!stride || !string_offsets || !offset_count || !byte_mask)
        return false;
    uint32_t end = 0;
    for (uint32_t i = 0; i < offset_count; i++) {
        uint32_t offset = string_offsets[i];
        if (offset < end || offset > stride
            || sizeof(n00b_abi_string_v1) > stride - offset) return false;
        end = offset + sizeof(n00b_abi_string_v1);
    }
    return true;
}

static bool write_reloc_element(int32_t fd, const uint8_t *element,
                                uint32_t stride,
                                const uint32_t *string_offsets,
                                uint32_t offset_count,
                                const uint8_t *byte_mask)
{
    const uint8_t zero[sizeof(n00b_abi_string_v1)] = {0};
    uint32_t cursor = 0;
    for (uint32_t j = 0; j < offset_count; j++) {
        uint32_t offset = string_offsets[j];
        if (!write_masked_segment(fd, element, byte_mask, cursor, offset)
            || !write_all(fd, zero, sizeof(zero))) return false;
        cursor = offset + sizeof(zero);
    }
    if (!write_masked_segment(fd, element, byte_mask, cursor, stride))
        return false;
    for (uint32_t j = 0; j < offset_count; j++) {
        n00b_abi_string_v1 value;
        const uint8_t *source = element + string_offsets[j];
        uint8_t *destination = (uint8_t *)&value;
        for (size_t b = 0; b < sizeof(value); b++)
            destination[b] = source[b];
        if (n00bb_comptime_emit_string(fd, value.words[0],
                                       value.words[1])) return false;
    }
    return true;
}

int32_t n00bb_comptime_emit_reloc_value(
    int32_t fd, const void *data, uint32_t stride,
    const uint32_t *string_offsets, uint32_t offset_count,
    const uint8_t *byte_mask)
{
    const uint64_t count = 1;
    if (fd < 0 || !data || !reloc_offsets_valid(stride, string_offsets,
                                                offset_count, byte_mask)
        || !write_all(fd, (const uint8_t *)&count, sizeof(count))
        || !write_reloc_element(fd, data, stride, string_offsets,
                                offset_count, byte_mask)) return -1;
    return 0;
}

int32_t n00bb_comptime_emit_reloc_array(
    int32_t fd, const void *data,
    const n00b_abi_array_descriptor_header_v1 *descriptor,
    uint32_t stride, const uint32_t *string_offsets, uint32_t offset_count,
    const uint8_t *byte_mask)
{
    if (fd < 0 || !data || !descriptor
        || !reloc_offsets_valid(stride, string_offsets, offset_count,
                                byte_mask) || descriptor->rank != 1
        || descriptor->flat_rank != 1 || descriptor->flags
        || descriptor->reserved_zero
        || descriptor->total_count != ((const uint64_t *)(descriptor + 1))[0]
        || descriptor->total_count > SIZE_MAX / stride)
        return -1;
    uint64_t count = descriptor->total_count;
    if (!write_all(fd, (const uint8_t *)&count, sizeof(count))) return -1;
    for (uint64_t i = 0; i < count; i++)
        if (!write_reloc_element(fd, (const uint8_t *)data + i * stride,
                stride, string_offsets, offset_count, byte_mask)) return -1;
    return 0;
}

int32_t n00bb_comptime_emit_record_array(
    int32_t fd, const void *record, uint32_t record_size,
    uint32_t array_offset, const void *data,
    const n00b_abi_array_descriptor_header_v1 *descriptor,
    uint32_t stride, const uint32_t *string_offsets, uint32_t offset_count,
    const uint8_t *byte_mask)
{
    const uint64_t one = 1;
    const uint8_t zero_array[sizeof(n00b_abi_array_value_v1)] = {0};
    if (fd < 0 || !record || !data || !descriptor || !stride
        || array_offset > record_size
        || record_size - array_offset < sizeof(zero_array)
        || !write_all(fd, (const uint8_t *)&one, sizeof(one))
        || !write_all(fd, record, array_offset)
        || !write_all(fd, zero_array, sizeof(zero_array))
        || !write_all(fd, (const uint8_t *)record + array_offset
            + sizeof(zero_array),
            record_size - array_offset - sizeof(zero_array)))
        return -1;
    return offset_count
        ? n00bb_comptime_emit_reloc_array(fd, data, descriptor, stride,
            string_offsets, offset_count, byte_mask)
        : n00bb_comptime_emit_array(fd, data, descriptor, stride);
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
