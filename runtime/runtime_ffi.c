#include <stddef.h>
#include <stdint.h>

#include "n00b-abi-v1.h"

void *
n00b_ffi_borrow(void *object, size_t *length, uint32_t access)
{
    (void)access;
    if (length) *length = 0;
    if (!object) return NULL;
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
