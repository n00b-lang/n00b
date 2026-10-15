#include <stdint.h>

int32_t n00b_ffi_dispatch(uint64_t handle, const void *arguments, void *result);

int
fire(uint64_t handle, int value)
{
    int result = -100;
    if (n00b_ffi_dispatch(handle, &value, &result) != 0) {
        return -200;
    }
    return result;
}

int
read_int(uintptr_t address)
{
    return *(const int *)address;
}

void
write_int(uintptr_t address, int value)
{
    *(int *)address = value;
}
