#include <stdint.h>

#include "lib/string.h"

typedef struct {
    uint8_t bytes[16];
    uint64_t length;
} short_t;

volatile short_t sink;

static uint64_t
run(int64_t rounds, core_string_t tail)
{
    uint64_t total = 0;
    for (int64_t round = 0; round < rounds; round++) {
        short_t joined = {.length = 1 + tail.u8_bytes};
        joined.bytes[0] = 'x';
        for (size_t i = 0; i < tail.u8_bytes; i++)
            joined.bytes[1 + i] = (uint8_t)tail.data[i];
        sink = joined;
        total += joined.length;
    }
    return total;
}

int
main(int argc, char **argv)
{
    (void)argv;
    return run(12000000 * argc, CORE_STRING_STATIC("short tail"))
        == 132000000 ? 0 : 1;
}
