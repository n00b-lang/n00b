#include "n00b-abi-v1.h"
#include "abi_string.h"

#include <stdint.h>

typedef void *(*n00bb_string_allocator_t)(uint32_t, uint32_t, uint64_t,
                                          uint32_t, void *);

static uint8_t *
string_storage(n00b_abi_string_v1 *result, uint64_t length,
               n00bb_abi_string_encoding_t encoding, uint32_t byte_type,
               n00bb_string_allocator_t allocate)
{
    *result = (n00b_abi_string_v1){0};
    if (length <= N00BB_ABI_STRING_SHORT_CAPACITY) {
        result->short_value.selector = (uint8_t)(length
            | ((uint64_t)encoding << 4)
            | ((uint64_t)N00BB_ABI_STRING_SHORT << 6));
        return result->short_value.bytes;
    }
    if (length > N00B_ABI_STRING_LONG_LENGTH_MASK || !allocate)
        __builtin_trap();
    uint8_t *data = allocate(N00B_ABI_ALLOCATOR_GC, byte_type, length, 1,
                             NULL);
    if (!data) __builtin_trap();
    result->long_value.data = data;
    result->long_value.selector = length
        | ((uint64_t)encoding << N00B_ABI_STRING_ENCODING_SHIFT)
        | ((uint64_t)N00BB_ABI_STRING_LONG << N00B_ABI_STRING_TAG_SHIFT);
    return data;
}


static uint64_t
utf8_sequence(const uint8_t *bytes, uint64_t remaining)
{
    uint8_t lead = bytes[0];
    if (lead < 0x80) return 1;
    uint64_t length = lead >= 0xc2 && lead <= 0xdf ? 2
                    : lead >= 0xe0 && lead <= 0xef ? 3
                    : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
    if (!length || length > remaining) return 0;
    for (uint64_t i = 1; i < length; i++)
        if ((bytes[i] & 0xc0) != 0x80) return 0;
    if (lead == 0xe0 && bytes[1] < 0xa0) return 0;
    if (lead == 0xed && bytes[1] > 0x9f) return 0;
    if (lead == 0xf0 && bytes[1] < 0x90) return 0;
    if (lead == 0xf4 && bytes[1] > 0x8f) return 0;
    return length;
}

int32_t n00bb_bootstrap_string_from_utf8(const n00b_abi_array_value_v1 *array,
                                         uint32_t byte_type,
                                         n00bb_string_allocator_t allocate,
                                         n00b_abi_string_v1 *result)
{
    if (!result) __builtin_trap();
    const uint8_t *bytes = array ? array->data : NULL;
    const n00b_abi_array_descriptor_header_v1 *descriptor = array
        ? array->descriptor : NULL;
    uint64_t length = descriptor ? descriptor->total_count : 0;
    bool ascii = true;
    for (uint64_t at = 0; at < length;) {
        uint64_t step = utf8_sequence(bytes + at, length - at);
        if (!step) return 0;
        ascii = ascii && step == 1;
        at += step;
    }
    n00b_abi_string_v1 text;
    uint8_t *data = string_storage(&text, length,
        ascii ? N00BB_ABI_STRING_ASCII : N00BB_ABI_STRING_UTF8, byte_type,
        allocate);
    for (uint64_t i = 0; i < length; i++) data[i] = bytes[i];
    *result = text;
    return 1;
}
