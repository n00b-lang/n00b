#include "core.h"
#include "lib/cache.h"
#include "harness.h"

static int subject_a;
static int subject_b;
static int result_x;

static void
test_a_miss_on_an_empty_cache(void)
{
    core_cache_t c;

    core_cache_init(&c);

    void *out = &result_x;

    check("an empty cache misses",
          !core_cache_get(&c, &subject_a, 1, 0, &out));
    check("a miss leaves the out parameter alone", out == &result_x);

    core_cache_free(&c);
}

static void
test_a_stored_result_comes_back(void)
{
    core_cache_t c;

    core_cache_init(&c);
    core_cache_put(&c, &subject_a, 1, 0, &result_x);

    void *out = nullptr;

    check("a stored result hits", core_cache_get(&c, &subject_a, 1, 0, &out));
    check("and is the value that went in", out == &result_x);

    core_cache_free(&c);
}

static void
test_the_three_key_fields_all_discriminate(void)
{
    core_cache_t c;

    core_cache_init(&c);
    core_cache_put(&c, &subject_a, 1, 7, &result_x);

    void *out = nullptr;

    check("a different subject misses",
          !core_cache_get(&c, &subject_b, 1, 7, &out));
    check("a different query misses",
          !core_cache_get(&c, &subject_a, 2, 7, &out));
    check("a different param misses",
          !core_cache_get(&c, &subject_a, 1, 8, &out));
    check("the exact key hits", core_cache_get(&c, &subject_a, 1, 7, &out));

    core_cache_free(&c);
}

static void
test_a_null_result_is_cached_not_re_derived(void)
{
    core_cache_t c;

    core_cache_init(&c);
    core_cache_put(&c, &subject_a, 1, 0, nullptr);

    void *out = &result_x;

    check("a cached null is a hit, not a miss",
          core_cache_get(&c, &subject_a, 1, 0, &out));
    check("and yields null", out == nullptr);

    core_cache_free(&c);
}

static void
test_a_put_over_a_live_key_replaces_it(void)
{
    core_cache_t c;

    core_cache_init(&c);
    core_cache_put(&c, &subject_a, 1, 0, &result_x);
    core_cache_put(&c, &subject_a, 1, 0, &subject_b);

    void *out = nullptr;

    core_cache_get(&c, &subject_a, 1, 0, &out);

    check("the second put wins", out == &subject_b);

    core_cache_free(&c);
}

static void
test_invalidation_retires_every_entry_at_once(void)
{
    core_cache_t c;

    core_cache_init(&c);
    core_cache_put(&c, &subject_a, 1, 0, &result_x);
    core_cache_put(&c, &subject_b, 1, 0, &result_x);

    core_cache_invalidate(&c);

    void *out = nullptr;

    check("an entry from the old generation misses",
          !core_cache_get(&c, &subject_a, 1, 0, &out));
    check("as does every other one",
          !core_cache_get(&c, &subject_b, 1, 0, &out));

    core_cache_put(&c, &subject_a, 1, 0, &result_x);

    check("a re-put after invalidation hits again",
          core_cache_get(&c, &subject_a, 1, 0, &out));

    core_cache_free(&c);
}

static void
test_clear_releases_the_entries(void)
{
    core_cache_t c;

    core_cache_init(&c);
    core_cache_put(&c, &subject_a, 1, 0, &result_x);
    core_cache_put(&c, &subject_b, 1, 0, &result_x);
    core_cache_clear(&c);


    void *out = nullptr;

    check("and nothing survives it",
          !core_cache_get(&c, &subject_a, 1, 0, &out));

    core_cache_put(&c, &subject_a, 1, 0, &result_x);

    check("a cleared cache still works",
          core_cache_get(&c, &subject_a, 1, 0, &out));

    core_cache_free(&c);
}

static void
test_a_disabled_cache_answers_nothing(void)
{
    core_cache_t c;

    core_cache_init(&c);

    core_cache_set_enabled(&c, false);

    core_cache_put(&c, &subject_a, 1, 0, &result_x);

    void *out = nullptr;

    check("and always misses", !core_cache_get(&c, &subject_a, 1, 0, &out));

    core_cache_set_enabled(&c, true);
    core_cache_put(&c, &subject_a, 1, 0, &result_x);

    check("turning it back on restores it",
          core_cache_get(&c, &subject_a, 1, 0, &out));

    core_cache_free(&c);
}

static void
test_a_null_cache_is_inert(void)
{
    void *out = nullptr;

    check("get on a null cache misses",
          !core_cache_get(nullptr, &subject_a, 1, 0, &out));

    core_cache_put(nullptr, &subject_a, 1, 0, &result_x);
    core_cache_invalidate(nullptr);
    core_cache_clear(nullptr);
    core_cache_free(nullptr);
}

static void
test_many_keys_survive_a_resize(void)
{
    core_cache_t c;
    static int   subjects[512];

    core_cache_init(&c);

    for (int i = 0; i < 512; i++) {
        core_cache_put(&c, &subjects[i], 1, (uint64_t)i, &subjects[i]);
    }


    bool all = true;

    for (int i = 0; i < 512; i++) {
        void *out = nullptr;

        if (!core_cache_get(&c, &subjects[i], 1, (uint64_t)i, &out)
            || out != &subjects[i]) {
            all = false;
        }
    }

    check("and every one reads back after the resizes", all);

    core_cache_free(&c);
}

int main(void)
{
    test_a_miss_on_an_empty_cache();
    test_a_stored_result_comes_back();
    test_the_three_key_fields_all_discriminate();
    test_a_null_result_is_cached_not_re_derived();
    test_a_put_over_a_live_key_replaces_it();
    test_invalidation_retires_every_entry_at_once();
    test_clear_releases_the_entries();
    test_a_disabled_cache_answers_nothing();
    test_a_null_cache_is_inert();
    test_many_keys_survive_a_resize();

    return report("result cache");
}
