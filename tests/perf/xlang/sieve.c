#include <stdint.h>

#include "lib/alloc.h"

static int64_t
count_primes(int64_t limit)
{
    bool *composite = core_alloc_array(bool, limit + 1);
    int64_t count = 0;
    for (int64_t n = 2; n <= limit; n++) {
        if (composite[n]) continue;
        count++;
        for (int64_t multiple = n * n; multiple <= limit; multiple += n)
            composite[multiple] = true;
    }
    core_free(composite);
    return count;
}

int
main(int argc, char **argv)
{
    (void)argv;
    int64_t total = 0;
    for (int64_t round = 0; round < 6 * argc; round++)
        total += count_primes(2000000);
    return total == 893598 ? 0 : 1;
}
