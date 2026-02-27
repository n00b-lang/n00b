#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/dict.h"
#include "core/runtime.h"

// Public typed-dict declarations under test.
n00b_dict_decl(uint64_t, uint64_t);
n00b_dict_decl(char *, uint64_t);

typedef n00b_dict_t(uint64_t, uint64_t) u64_dict_t;
typedef n00b_dict_t(char *, uint64_t)   str_dict_t;

static void
test_typed_wrapper_put_get_remove(void)
{
    u64_dict_t dict;
    n00b_dict_init(&dict, .skip_obj_hash = true);

    uint64_t key = 42;
    uint64_t val = 100;

    assert(n00b_dict_put(&dict, key, val) == nullptr);

    bool  found = false;
    void *ptr   = n00b_dict_get(&dict, key, &found);
    assert(found);
    assert(*(uint64_t *)ptr == 100);
    assert(n00b_dict_contains(&dict, key));
    assert(n00b_dict_internal_len((_n00b_dict_internal_t *)&dict) == 1);

    assert(n00b_dict_remove(&dict, key));
    (void)n00b_dict_get(&dict, key, &found);
    assert(!found);
    assert(n00b_dict_internal_len((_n00b_dict_internal_t *)&dict) == 0);

    printf("  [PASS] typed_wrapper_put_get_remove\n");
}

static void
test_typed_wrapper_string_keys(void)
{
    str_dict_t dict;
    n00b_dict_init(&dict, .hash = n00b_hash_cstring, .skip_obj_hash = true);

    char    *k1 = "alpha";
    uint64_t v1 = 1;

    n00b_dict_put(&dict, k1, v1);

    bool  found = false;
    void *ptr   = n00b_dict_get(&dict, k1, &found);
    assert(found);
    assert(*(uint64_t *)ptr == 1);

    char *missing = "missing";
    (void)n00b_dict_get(&dict, missing, &found);
    assert(!found);

    printf("  [PASS] typed_wrapper_string_keys\n");
}

static void
test_typed_wrapper_cas(void)
{
    u64_dict_t dict;
    n00b_dict_init(&dict, .skip_obj_hash = true);

    uint64_t key      = 7;
    uint64_t inserted = 100;
    bool     ok       = n00b_dict_cas(&dict,
                                      key,
                                      nullptr,
                                      &inserted,
                                      .null_old_means_absence = true);
    assert(ok);

    bool  found = false;
    void *ptr   = n00b_dict_get(&dict, key, &found);
    assert(found);
    assert(*(uint64_t *)ptr == inserted);

    uint64_t expected = 100;
    void    *old_ptr  = &expected;
    uint64_t updated  = 200;

    ok = n00b_dict_cas(&dict, key, &old_ptr, &updated);
    assert(ok);

    ptr = n00b_dict_get(&dict, key, &found);
    assert(found);
    assert(*(uint64_t *)ptr == updated);

    printf("  [PASS] typed_wrapper_cas\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running typed dict wrapper tests...\n");

    test_typed_wrapper_put_get_remove();
    test_typed_wrapper_string_keys();
    test_typed_wrapper_cas();

    printf("All typed dict wrapper tests passed.\n");
    return 0;
}
