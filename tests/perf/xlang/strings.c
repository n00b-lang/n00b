#include <stdint.h>

#include "lib/buffer.h"

core_buffer_t sink;

static uint64_t
run(int64_t rounds, core_string_t tail)
{
    uint64_t total = 0;
    for (int64_t round = 0; round < rounds; round++) {
        core_buffer_t joined = {0};
        core_buffer_puts(&joined, "x");
        core_buffer_append_string(&joined, tail);
        total += joined.byte_len;
        core_buffer_release(&sink);
        sink = joined;
    }
    core_buffer_release(&sink);
    return total;
}

int
main(int argc, char **argv)
{
    (void)argv;
    core_string_t tail =
        CORE_STRING_STATIC("a piece of text long enough to need the heap");
    return run(12000000 * argc, tail) == 540000000 ? 0 : 1;
}
