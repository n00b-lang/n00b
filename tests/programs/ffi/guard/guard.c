#include <stdint.h>

typedef int (*compute_fn)(int value);

uint64_t n00b_ffi_enter(uint64_t buffer);
void n00b_ffi_leave(uint64_t previous);
int32_t n00b_ffi_unwind_save(uint64_t *buffer) __attribute__((returns_twice));

int
guarded_call(compute_fn compute, int value)
{
    uint64_t buffer[24];
    uint64_t previous = n00b_ffi_enter((uint64_t)(uintptr_t)buffer);
    if (n00b_ffi_unwind_save(buffer)) {
        n00b_ffi_leave(previous);
        return -1000;
    }
    int result = compute(value);
    n00b_ffi_leave(previous);
    return result;
}
