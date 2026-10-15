#include "n00b-abi-v1.h"
#include "abi_string.h"

#include <stdint.h>

extern void n00b_gc_root_push(void **slot);
extern void n00b_gc_root_pop(uint64_t depth);

typedef void *(*n00bb_string_allocator_t)(uint32_t, uint32_t, uint64_t,
                                          uint32_t, void *);

static const uint8_t *
string_bytes(const n00b_abi_string_v1 *text, uint64_t *length,
             n00bb_abi_string_encoding_t *encoding)
{
    n00bb_abi_string_header_t header;
    if (!n00bb_abi_string_decode((const uint8_t *)text, sizeof *text,
                                 &header)
        || header.tag == N00BB_ABI_STRING_RICH) __builtin_trap();
    *length = header.length;
    *encoding = header.encoding;
    return header.tag == N00BB_ABI_STRING_SHORT ? text->short_value.bytes
                                                : text->long_value.data;
}

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

static n00bb_abi_string_encoding_t
joined_encoding(n00bb_abi_string_encoding_t left,
                n00bb_abi_string_encoding_t right)
{
    if (left == N00BB_ABI_STRING_BINARY || right == N00BB_ABI_STRING_BINARY)
        return N00BB_ABI_STRING_BINARY;
    if (left == N00BB_ABI_STRING_UTF8 || right == N00BB_ABI_STRING_UTF8)
        return N00BB_ABI_STRING_UTF8;
    return N00BB_ABI_STRING_ASCII;
}

void n00bb_bootstrap_string_add(uint64_t left_low, uint64_t left_high,
                                uint64_t right_low, uint64_t right_high,
                                uint32_t byte_type,
                                n00bb_string_allocator_t allocate,
                                n00b_abi_string_v1 *result)
{
    n00b_abi_string_v1 left = {.words = {left_low, left_high}};
    n00b_abi_string_v1 right = {.words = {right_low, right_high}};
    uint64_t left_length = 0, right_length = 0, length = 0;
    n00bb_abi_string_encoding_t left_encoding, right_encoding;
    const uint8_t *left_bytes = string_bytes(&left, &left_length,
                                             &left_encoding);
    const uint8_t *right_bytes = string_bytes(&right, &right_length,
                                              &right_encoding);
    if (!result || __builtin_add_overflow(left_length, right_length, &length))
        __builtin_trap();
    void *roots[2] = {left.words[1] >> N00B_ABI_STRING_TAG_SHIFT
                          ? left.long_value.data : NULL,
                      right.words[1] >> N00B_ABI_STRING_TAG_SHIFT
                          ? right.long_value.data : NULL};
    n00b_gc_root_push(&roots[0]);
    n00b_gc_root_push(&roots[1]);
    n00b_abi_string_v1 joined;
    uint8_t *data = string_storage(&joined, length,
        joined_encoding(left_encoding, right_encoding), byte_type, allocate);
    n00b_gc_root_pop(2);
    for (uint64_t i = 0; i < left_length; i++) data[i] = left_bytes[i];
    for (uint64_t i = 0; i < right_length; i++)
        data[left_length + i] = right_bytes[i];
    *result = joined;
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
    void *root = array ? array->data : NULL;
    n00b_gc_root_push(&root);
    n00b_abi_string_v1 text;
    uint8_t *data = string_storage(&text, length,
        ascii ? N00BB_ABI_STRING_ASCII : N00BB_ABI_STRING_UTF8, byte_type,
        allocate);
    n00b_gc_root_pop(1);
    for (uint64_t i = 0; i < length; i++) data[i] = bytes[i];
    *result = text;
    return 1;
}
