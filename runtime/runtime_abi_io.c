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
    bool indirect = (type_field & UINT32_C(0x80000000))
        || type->size_bits > 64;
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
    if (type->kind == N00B_ABI_TYPE_STRING)
        return indirect && payload && dispatch
            ? dispatch(array_extent, byte_stride, type_field, payload, fd)
            : -1;
    if (type->kind == N00B_ABI_TYPE_ARRAY)
        return indirect ? repr_array(snapshot, fd, type, payload, dispatch)
                        : -1;
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
