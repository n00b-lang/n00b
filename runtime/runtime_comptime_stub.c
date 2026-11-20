#include <stdbool.h>
#include <stdint.h>

#include "n00b-abi-v1.h"

// A program reports a literal handler's failure as an uncaught failure, so
// recording and checking literal failures (for a comptime run's
// diagnostics) does nothing here.
void
n00b_comptime_literal_failed(uint64_t module, uint64_t site,
                             n00b_abi_string_v1 message)
{
    (void)module;
    (void)site;
    (void)message;
}

void
n00b_comptime_literals_checked(uint64_t module)
{
    (void)module;
}

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
