// Test: __builtin_offsetof/offsetof parse correctly in static_assert and
// statement expressions.
#include <stddef.h>

struct sample_t {
    int    a;
    double b;
};

int
main(void)
{
    static_assert(offsetof(struct sample_t, a) == 0);
    static_assert(offsetof(struct sample_t, b) > 0);

    int ok = ({
        static_assert(offsetof(struct sample_t, a) == 0);
        offsetof(struct sample_t, b) > 0 ? 1 : 0;
    });

    return ok ? 0 : 1;
}
