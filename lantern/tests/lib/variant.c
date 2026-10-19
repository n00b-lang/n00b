#include <string.h>

#include "lib/variant.h"
#include "harness.h"

typedef struct {
    int left;
    int right;
} pair_t;

core_variant_decl(test_variant_t, int, core_string_t, pair_t, long);

int
main(void)
{
    test_variant_t empty = core_variant_empty(test_variant_t);
    test_variant_t number = core_variant_make_1(test_variant_t, 42);
    test_variant_t text = core_variant_make_2(test_variant_t,
                                              CORE_STRING_STATIC("lantern"));
    test_variant_t pair = core_variant_make_3(test_variant_t,
                                              ((pair_t){.left = 3,
                                                        .right = 5}));
    test_variant_t wide = core_variant_make_4(test_variant_t, 1L << 40);

    check("empty variant has no active alternative", empty.tag == 0);
    check("first alternative tag and extraction",
          core_variant_is(number, 1) && core_variant_get_1(number) == 42);
    check("second alternative tag and extraction",
          core_variant_is(text, 2)
          && core_string_eq(core_variant_get_2(text),
                            CORE_STRING_STATIC("lantern")));
    pair_t got = core_variant_get_3(pair);
    check("aggregate alternative extraction",
          core_variant_is(pair, 3) && got.left == 3 && got.right == 5);
    check("fourth alternative tag and extraction",
          core_variant_is(wide, 4) && core_variant_get_4(wide) == (1L << 40));
    return 0;
}
