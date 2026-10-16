#include <stdint.h>

int32_t n00b_callback_table_dispatch_doubler(uint64_t handle, uint64_t arguments, uint64_t result);

int
fire(uint64_t handle, int value)
{
    int result = -100;
    if (n00b_callback_table_dispatch_doubler(handle, (uint64_t)(uintptr_t)&value, (uint64_t)(uintptr_t)&result)
        != 0) {
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
