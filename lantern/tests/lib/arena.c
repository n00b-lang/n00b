#include "core.h"
#include "lib/arena.h"
#include "harness.h"

static void
test_extend_grows_the_last_allocation_in_place(void)
{
    core_arena_t a;

    core_arena_init(&a, 0);

    char  *p    = core_arena_alloc(&a, 64);
    size_t used = a.current->used;

    memset(p, 'x', 64);

    check("the last allocation extends in place",
          core_arena_extend(&a, p, 64, 128));
    check("and the block accounts for exactly the new size",
          a.current->used == used + 64);
    check("the next allocation starts after the extension",
          (char *)core_arena_alloc(&a, 8) == p + 128);

    bool kept = true;

    for (int i = 0; i < 64; i++) {
        if (p[i] != 'x') {
            kept = false;
        }
    }

    check("the existing bytes survive", kept);

    core_arena_free(&a);
}

static void
test_the_extension_is_zeroed(void)
{
    core_arena_t a;

    core_arena_init(&a, 0);

    char *p = core_arena_alloc(&a, 32);

    memset(p, 'z', 32);
    core_arena_extend(&a, p, 32, 96);

    bool zero = true;

    for (int i = 32; i < 96; i++) {
        if (p[i] != 0) {
            zero = false;
        }
    }

    check("bytes past the old size are zero", zero);

    core_arena_free(&a);
}

static void
test_extend_refuses_when_not_the_last_allocation(void)
{
    core_arena_t a;

    core_arena_init(&a, 0);

    char *p = core_arena_alloc(&a, 64);
    char *q = core_arena_alloc(&a, 64);

    memset(q, 'q', 64);

    check("an allocation with something after it does not extend",
          !core_arena_extend(&a, p, 64, 128));

    bool intact = true;

    for (int i = 0; i < 64; i++) {
        if (q[i] != 'q') {
            intact = false;
        }
    }

    check("so the allocation after it is untouched", intact);

    core_arena_free(&a);
}

static void
test_extend_refuses_when_the_block_is_too_small(void)
{
    core_arena_t a;

    core_arena_init(&a, 1024);

    char *p = core_arena_alloc(&a, 512);

    check("an extension past the block capacity is refused",
          !core_arena_extend(&a, p, 512, 4096));

    core_arena_free(&a);
}

static void
test_extend_is_inert_on_junk(void)
{
    core_arena_t a;
    char         off_arena[16];

    core_arena_init(&a, 0);

    check("a null pointer does not extend",
          !core_arena_extend(&a, nullptr, 0, 16));
    check("a pointer the arena never handed out does not extend",
          !core_arena_extend(&a, off_arena, 16, 32));

    char *p = core_arena_alloc(&a, 32);

    check("an old size larger than the block is refused",
          !core_arena_extend(&a, p, 1u << 30, 1u << 31));

    core_arena_free(&a);
}

static void
test_repeated_extension_does_not_consume_extra_space(void)
{
    core_arena_t a;

    core_arena_init(&a, 0);

    char  *p    = core_arena_alloc(&a, 16);
    size_t used = a.current->used;

    for (size_t n = 16; n < 4096; n *= 2) {
        check("each doubling extends in place",
              core_arena_extend(&a, p, n, n * 2));
    }

    check("the arena holds exactly the final size, not the sum",
          a.current->used == used - 16 + 4096);
    check("and stayed in one block", a.first == a.current);

    core_arena_free(&a);
}

int main(void)
{
    test_extend_grows_the_last_allocation_in_place();
    test_the_extension_is_zeroed();
    test_extend_refuses_when_not_the_last_allocation();
    test_extend_refuses_when_the_block_is_too_small();
    test_extend_is_inert_on_junk();
    test_repeated_extension_does_not_consume_extra_space();

    return report("arena");
}
