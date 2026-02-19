// Test: constexpr_eval, constexpr_max, constexpr_min.
#include <stdio.h>

int
main(void)
{
    int a = constexpr_eval(2 + 3);
    int b = constexpr_max(10, 20);
    int c = constexpr_min(10, 20);
    printf("constexpr ok: %d %d %d\n", a, b, c);
    return (a == 5 && b == 20 && c == 10) ? 0 : 1;
}
