#include <stdbool.h>
#include <stdint.h>

void
n00b_comptime_freeze(uint64_t module)
{
    (void)module;
    __builtin_trap();
}

bool
n00b_comptime_frozen(uint64_t module)
{
    (void)module;
    return false;
}
