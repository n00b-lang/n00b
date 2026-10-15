#include <stddef.h>
#include <stdint.h>

#include "n00b-abi-v1.h"

extern void n00b_gc_root_push(void **slot);
extern void n00b_gc_root_pop(uint64_t depth);

enum { MAX_BORROWS = 256 };

static _Thread_local void *borrowed[MAX_BORROWS];
static _Thread_local uint32_t borrow_count;

void *
n00b_ffi_borrow(void *object, size_t *length, uint32_t access)
{
    (void)access;
    const n00b_abi_array_value_v1 *array = object;
    if (length) *length = 0;
    if (!array) return NULL;
    if (borrow_count >= MAX_BORROWS) __builtin_trap();
    borrowed[borrow_count] = array->data;
    n00b_gc_root_push(&borrowed[borrow_count]);
    borrow_count++;
    const n00b_abi_array_descriptor_header_v1 *descriptor = array->descriptor;
    if (length && descriptor) *length = (size_t)descriptor->total_count;
    return array->data;
}

void
n00b_ffi_release(void *object)
{
    if (!object || !borrow_count) return;
    borrow_count--;
    borrowed[borrow_count] = NULL;
    n00b_gc_root_pop(1);
}
