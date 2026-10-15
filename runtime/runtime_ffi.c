#include <stddef.h>
#include <stdint.h>

#include "n00b-abi-v1.h"
#include "abi_string.h"

extern void n00b_gc_root_push(void **slot);
extern void n00b_gc_root_pop(uint64_t depth);

enum { MAX_BORROWS = 256 };
enum { N00B_FFI_STRING = 2 };

static _Thread_local void *borrowed[MAX_BORROWS];
static _Thread_local uint32_t borrow_count;

static void *
string_data(void *object, size_t *length, void **managed)
{
    n00b_abi_string_v1 *text = object;
    n00bb_abi_string_header_t header;
    if (!n00bb_abi_string_decode((const uint8_t *)text, sizeof *text,
                                 &header)
        || header.tag == N00BB_ABI_STRING_RICH) __builtin_trap();
    if (length) *length = (size_t)header.length;
    if (header.tag == N00BB_ABI_STRING_SHORT) {
        *managed = NULL;
        return text->short_value.bytes;
    }
    *managed = text->long_value.data;
    return text->long_value.data;
}

void *
n00b_ffi_borrow(void *object, size_t *length, uint32_t access)
{
    if (length) *length = 0;
    if (!object) return NULL;
    if (borrow_count >= MAX_BORROWS) __builtin_trap();
    void *data = NULL;
    if (access & N00B_FFI_STRING) {
        data = string_data(object, length, &borrowed[borrow_count]);
    } else {
        const n00b_abi_array_value_v1 *array = object;
        const n00b_abi_array_descriptor_header_v1 *descriptor =
            array->descriptor;
        if (length && descriptor) *length = (size_t)descriptor->total_count;
        data = array->data;
        borrowed[borrow_count] = data;
    }
    n00b_gc_root_push(&borrowed[borrow_count]);
    borrow_count++;
    return data;
}

void
n00b_ffi_release(void *object)
{
    if (!object || !borrow_count) return;
    borrow_count--;
    borrowed[borrow_count] = NULL;
    n00b_gc_root_pop(1);
}
