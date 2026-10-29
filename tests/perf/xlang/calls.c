#include <stdint.h>

static int64_t
fib(int64_t n)
{
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int
main(int argc, char **argv)
{
    (void)argv;
    return fib(34 + argc) == 9227465 ? 0 : 1;
}
