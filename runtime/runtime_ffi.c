#include <stddef.h>
#include <stdint.h>

#include "n00b-abi-v1.h"
#include "abi_string.h"

enum { N00B_FFI_STRING = 2 };

static void *
string_data(void *object, size_t *length)
{
    n00b_abi_string_v1 *text = object;
    n00bb_abi_string_header_t header;
    if (!n00bb_abi_string_decode((const uint8_t *)text, sizeof *text,
                                 &header)
        || header.tag == N00BB_ABI_STRING_RICH) __builtin_trap();
    if (length) *length = (size_t)header.length;
    return header.tag == N00BB_ABI_STRING_SHORT ? text->short_value.bytes
                                                : text->long_value.data;
}

void *
n00b_ffi_borrow(void *object, size_t *length, uint32_t access)
{
    if (length) *length = 0;
    if (!object) return NULL;
    if (access & N00B_FFI_STRING) return string_data(object, length);
    const n00b_abi_array_value_v1 *array = object;
    const n00b_abi_array_descriptor_header_v1 *descriptor = array->descriptor;
    if (length && descriptor) *length = (size_t)descriptor->total_count;
    return array->data;
}

void
n00b_ffi_release(void *object)
{
    (void)object;
}
