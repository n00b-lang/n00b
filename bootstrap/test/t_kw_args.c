// Test: keyword arguments via _kargs.
#include <stdio.h>

static int
add(int x, int y) _kargs
{
    int bias = 0;
}
{
    return x + y + bias;
}

int
main(void)
{
    int r1 = add(1, 2);
    int r2 = add(1, 2, .bias = 10);
    printf("kw_args ok: %d %d\n", r1, r2);
    return (r1 == 3 && r2 == 13) ? 0 : 1;
}
