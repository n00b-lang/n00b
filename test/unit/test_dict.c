#include <stdio.h>
#include <assert.h>
#include <string.h>

#define N00B_USE_INTERNAL_API
#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/dict.h"

typedef struct {
    uint64_t a;
    uint64_t b;
    uint64_t c;
    uint64_t d;
} big_value_t;

typedef _n00b_dict_internal_t u64_dict_t;
typedef _n00b_dict_internal_t big_dict_t;
typedef _n00b_dict_internal_t str_dict_t;

static void
u64_dict_init(u64_dict_t *dict)
{
    _n00b_dict_internal_init(dict, sizeof(uint64_t), sizeof(uint64_t));
    dict->skip_obj_hash = true;
}

static void
big_dict_init(big_dict_t *dict)
{
    _n00b_dict_internal_init(dict, sizeof(uint64_t), sizeof(big_value_t));
    dict->skip_obj_hash = true;
}

static void
str_dict_init(str_dict_t *dict)
{
    _n00b_dict_internal_init(dict, sizeof(char *), sizeof(uint64_t));
    dict->skip_obj_hash = true;
    dict->fn            = n00b_hash_cstring;
}

static void *
u64_dict_put(u64_dict_t *dict, uint64_t key, uint64_t value)
{
    return _n00b_dict_internal_put(dict, sizeof(key), sizeof(value), &key, &value);
}

static void *
u64_dict_get(u64_dict_t *dict, uint64_t key, bool *found)
{
    return _n00b_dict_internal_get(dict, sizeof(key), sizeof(uint64_t), &key, found);
}

static bool
u64_dict_add(u64_dict_t *dict, uint64_t key, uint64_t value)
{
    return _n00b_dict_internal_add(dict, sizeof(key), sizeof(value), &key, &value);
}

static bool
u64_dict_remove(u64_dict_t *dict, uint64_t key)
{
    return _n00b_dict_internal_remove(dict, sizeof(key), sizeof(uint64_t), &key);
}

static bool
u64_dict_contains(u64_dict_t *dict, uint64_t key)
{
    bool found = false;
    (void)u64_dict_get(dict, key, &found);
    return found;
}

static bool
u64_dict_cas(u64_dict_t *dict, uint64_t key, void **old_item_ptr, uint64_t *new_item)
{
    return _n00b_dict_internal_cas(dict,
                                   sizeof(key),
                                   sizeof(uint64_t),
                                   &key,
                                   old_item_ptr,
                                   new_item);
}

static bool
u64_dict_cas_insert(u64_dict_t *dict, uint64_t key, uint64_t *new_item)
{
    return _n00b_dict_internal_cas(dict,
                                   sizeof(key),
                                   sizeof(uint64_t),
                                   &key,
                                   nullptr,
                                   new_item,
                                   .null_old_means_absence = true);
}

static void *
big_dict_put(big_dict_t *dict, uint64_t key, big_value_t value)
{
    return _n00b_dict_internal_put(dict, sizeof(key), sizeof(value), &key, &value);
}

static void *
big_dict_get(big_dict_t *dict, uint64_t key, bool *found)
{
    return _n00b_dict_internal_get(dict, sizeof(key), sizeof(big_value_t), &key, found);
}

static void *
str_dict_put(str_dict_t *dict, char *key, uint64_t value)
{
    return _n00b_dict_internal_put(dict, sizeof(key), sizeof(value), &key, &value);
}

static void *
str_dict_get(str_dict_t *dict, char *key, bool *found)
{
    return _n00b_dict_internal_get(dict, sizeof(key), sizeof(uint64_t), &key, found);
}

// ============================================================================
// 1. Init empty — length 0
// ============================================================================

static void
test_init_empty(void)
{
    u64_dict_t dict;
    u64_dict_init(&dict);

    assert(n00b_dict_internal_len(&dict) == 0);

    printf("  [PASS] init_empty\n");
}

// ============================================================================
// 2. Put / get — put key/value, get returns correct value
// ============================================================================

static void
test_put_get(void)
{
    u64_dict_t dict;
    u64_dict_init(&dict);

    uint64_t k = 42, v = 100;
    u64_dict_put(&dict, k, v);

    bool  found;
    void *ptr = u64_dict_get(&dict, k, &found);
    assert(found);
    assert(*(uint64_t *)ptr == 100);
    assert(n00b_dict_internal_len(&dict) == 1);

    printf("  [PASS] put_get\n");
}

// ============================================================================
// 3. Overwrite — put same key twice; old value returned via pointer
// ============================================================================

static void
test_overwrite(void)
{
    u64_dict_t dict;
    u64_dict_init(&dict);

    uint64_t k = 1, v1 = 10, v2 = 20;

    void *prev = u64_dict_put(&dict, k, v1);
    assert(prev == nullptr);

    prev = u64_dict_put(&dict, k, v2);
    assert(prev != nullptr);
    assert(*(uint64_t *)prev == 20);

    bool  found;
    void *ptr = u64_dict_get(&dict, k, &found);
    assert(found);
    assert(*(uint64_t *)ptr == 20);

    printf("  [PASS] overwrite\n");
}

// ============================================================================
// 4. Not found — get missing key returns found=false
// ============================================================================

static void
test_not_found(void)
{
    u64_dict_t dict;
    u64_dict_init(&dict);

    uint64_t k1 = 1, v1 = 10, k999 = 999;
    u64_dict_put(&dict, k1, v1);

    bool  found;
    void *ptr = u64_dict_get(&dict, k999, &found);
    assert(!found);
    (void)ptr;

    printf("  [PASS] not_found\n");
}

// ============================================================================
// 5. Remove — put then remove; get returns found=false after removal
// ============================================================================

static void
test_remove(void)
{
    u64_dict_t dict;
    u64_dict_init(&dict);

    uint64_t k = 1, v = 10;
    u64_dict_put(&dict, k, v);

    bool found;
    (void)u64_dict_get(&dict, k, &found);
    assert(found);

    bool removed = u64_dict_remove(&dict, k);
    assert(removed);
    (void)u64_dict_get(&dict, k, &found);
    assert(!found);
    assert(n00b_dict_internal_len(&dict) == 0);

    printf("  [PASS] remove\n");
}

// ============================================================================
// 6. Add existing — returns false when key exists
// ============================================================================

static void
test_add_existing(void)
{
    u64_dict_t dict;
    u64_dict_init(&dict);

    uint64_t k = 1, v1 = 10, v2 = 20;

    bool added = u64_dict_add(&dict, k, v1);
    assert(added);

    added = u64_dict_add(&dict, k, v2);
    assert(!added);

    // Value should still be the original.
    bool  found;
    void *ptr = u64_dict_get(&dict, k, &found);
    assert(found);
    assert(*(uint64_t *)ptr == 10);

    printf("  [PASS] add_existing\n");
}

// ============================================================================
// 7. Contains — true/false for present/absent keys
// ============================================================================

static void
test_contains(void)
{
    u64_dict_t dict;
    u64_dict_init(&dict);

    uint64_t k42 = 42, k99 = 99, v = 100;

    assert(!u64_dict_contains(&dict, k42));

    u64_dict_put(&dict, k42, v);

    assert(u64_dict_contains(&dict, k42));
    assert(!u64_dict_contains(&dict, k99));

    printf("  [PASS] contains\n");
}

// ============================================================================
// 8. Growth — insert 200+ entries forcing resize; verify all
// ============================================================================

static void
test_growth(void)
{
    u64_dict_t dict;
    u64_dict_init(&dict);

    for (uint64_t i = 1; i <= 200; i++) {
        uint64_t v = i * 10;
        u64_dict_put(&dict, i, v);
    }

    assert(n00b_dict_internal_len(&dict) == 200);

    for (uint64_t i = 1; i <= 200; i++) {
        bool  found;
        void *ptr = u64_dict_get(&dict, i, &found);
        assert(found);
        assert(*(uint64_t *)ptr == i * 10);
    }

    printf("  [PASS] growth\n");
}

// ============================================================================
// 9. String pointer keys — using n00b_hash_cstring
// ============================================================================

static void
test_string_keys(void)
{
    str_dict_t dict;
    str_dict_init(&dict);

    char    *key1 = "hello", *key2 = "world", *key3 = "test";
    uint64_t v1 = 1, v2 = 2, v3 = 3;

    str_dict_put(&dict, key1, v1);
    str_dict_put(&dict, key2, v2);
    str_dict_put(&dict, key3, v3);

    bool  found;
    void *ptr;

    ptr = str_dict_get(&dict, key1, &found);
    assert(found);
    assert(*(uint64_t *)ptr == 1);

    ptr = str_dict_get(&dict, key2, &found);
    assert(found);
    assert(*(uint64_t *)ptr == 2);

    ptr = str_dict_get(&dict, key3, &found);
    assert(found);
    assert(*(uint64_t *)ptr == 3);

    char *missing = "missing";
    ptr           = str_dict_get(&dict, missing, &found);
    assert(!found);

    printf("  [PASS] string_keys\n");
}

// ============================================================================
// 10. Large value types — struct values larger than 8 bytes
// ============================================================================

static void
test_large_values(void)
{
    big_dict_t dict;
    big_dict_init(&dict);

    for (uint64_t i = 1; i <= 50; i++) {
        big_value_t bv = {.a = i, .b = i * 2, .c = i * 3, .d = i * 4};
        big_dict_put(&dict, i, bv);
    }

    assert(n00b_dict_internal_len(&dict) == 50);

    for (uint64_t i = 1; i <= 50; i++) {
        bool  found;
        void *ptr = big_dict_get(&dict, i, &found);
        assert(found);
        big_value_t *bv = (big_value_t *)ptr;
        assert(bv->a == i);
        assert(bv->b == i * 2);
        assert(bv->c == i * 3);
        assert(bv->d == i * 4);
    }

    printf("  [PASS] large_values\n");
}

// ============================================================================
// 11. Mixed operations — interleave put/get/remove/add
// ============================================================================

static void
test_mixed_operations(void)
{
    u64_dict_t dict;
    u64_dict_init(&dict);

    for (uint64_t i = 1; i <= 10; i++) {
        uint64_t v = i * 100;
        u64_dict_put(&dict, i, v);
    }
    assert(n00b_dict_internal_len(&dict) == 10);

    for (uint64_t i = 2; i <= 10; i += 2) {
        assert(u64_dict_remove(&dict, i));
    }
    assert(n00b_dict_internal_len(&dict) == 5);

    for (uint64_t i = 1; i <= 10; i++) {
        bool  found;
        void *ptr = u64_dict_get(&dict, i, &found);
        if (i % 2 == 1) {
            assert(found);
            assert(*(uint64_t *)ptr == i * 100);
        }
        else {
            assert(!found);
        }
    }

    for (uint64_t i = 2; i <= 10; i += 2) {
        uint64_t v = i * 200;
        assert(u64_dict_add(&dict, i, v));
    }
    assert(n00b_dict_internal_len(&dict) == 10);

    for (uint64_t i = 1; i <= 10; i++) {
        bool  found;
        void *ptr = u64_dict_get(&dict, i, &found);
        assert(found);
        if (i % 2 == 1) {
            assert(*(uint64_t *)ptr == i * 100);
        }
        else {
            assert(*(uint64_t *)ptr == i * 200);
        }
    }

    printf("  [PASS] mixed_operations\n");
}

// ============================================================================
// 12. Remove then reinsert — deleted slots are reusable
// ============================================================================

static void
test_remove_reinsert(void)
{
    u64_dict_t dict;
    u64_dict_init(&dict);

    uint64_t k = 42, v1 = 100, v2 = 200;
    u64_dict_put(&dict, k, v1);
    assert(u64_dict_remove(&dict, k));
    assert(n00b_dict_internal_len(&dict) == 0);

    u64_dict_put(&dict, k, v2);
    assert(n00b_dict_internal_len(&dict) == 1);

    bool  found;
    void *ptr = u64_dict_get(&dict, k, &found);
    assert(found);
    assert(*(uint64_t *)ptr == 200);

    printf("  [PASS] remove_reinsert\n");
}

// ============================================================================
// 13. CAS — basic success/failure paths
// ============================================================================

static void
test_cas_insert(void)
{
    u64_dict_t dict;
    u64_dict_init(&dict);

    uint64_t key     = 42;
    uint64_t new_val = 100;
    bool     ok      = u64_dict_cas_insert(&dict, key, &new_val);
    assert(ok);

    bool  found;
    void *ptr = u64_dict_get(&dict, key, &found);
    assert(found);
    assert(*(uint64_t *)ptr == 100);

    uint64_t new_val2 = 200;
    ok                = u64_dict_cas_insert(&dict, key, &new_val2);
    assert(!ok);

    ptr = u64_dict_get(&dict, key, &found);
    assert(found);
    assert(*(uint64_t *)ptr == 100);

    printf("  [PASS] cas_insert\n");
}

static void
test_cas_update(void)
{
    u64_dict_t dict;
    u64_dict_init(&dict);

    uint64_t k = 42, v = 100;
    u64_dict_put(&dict, k, v);

    uint64_t old_val = 100;
    uint64_t new_val = 200;
    void    *old_ptr = &old_val;
    bool     ok      = u64_dict_cas(&dict, k, &old_ptr, &new_val);
    assert(ok);

    bool  found;
    void *ptr = u64_dict_get(&dict, k, &found);
    assert(found);
    assert(*(uint64_t *)ptr == 200);

    uint64_t wrong_old = 100;
    old_ptr            = &wrong_old;
    uint64_t new_val2  = 300;
    ok                 = u64_dict_cas(&dict, k, &old_ptr, &new_val2);
    assert(!ok);

    ptr = u64_dict_get(&dict, k, &found);
    assert(found);
    assert(*(uint64_t *)ptr == 200);

    printf("  [PASS] cas_update\n");
}

// ============================================================================
// 14. Remove missing — removing a non-existent key returns false
// ============================================================================

static void
test_remove_missing(void)
{
    u64_dict_t dict;
    u64_dict_init(&dict);

    uint64_t k       = 999;
    bool     removed = u64_dict_remove(&dict, k);
    assert(!removed);

    printf("  [PASS] remove_missing\n");
}

// ============================================================================
// 15. Multiple removes and length tracking
// ============================================================================

static void
test_length_tracking(void)
{
    u64_dict_t dict;
    u64_dict_init(&dict);

    for (uint64_t i = 1; i <= 5; i++) {
        u64_dict_put(&dict, i, i);
    }
    assert(n00b_dict_internal_len(&dict) == 5);

    uint64_t k3 = 3, k1 = 1;
    u64_dict_remove(&dict, k3);
    assert(n00b_dict_internal_len(&dict) == 4);

    u64_dict_remove(&dict, k1);
    assert(n00b_dict_internal_len(&dict) == 3);

    uint64_t v33 = 33;
    u64_dict_put(&dict, k3, v33);
    assert(n00b_dict_internal_len(&dict) == 4);

    printf("  [PASS] length_tracking\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running dict internal tests...\n");

    test_init_empty();
    test_put_get();
    test_overwrite();
    test_not_found();
    test_remove();
    test_add_existing();
    test_contains();
    test_growth();
    test_string_keys();
    test_large_values();
    test_mixed_operations();
    test_remove_reinsert();
    test_cas_insert();
    test_cas_update();
    test_remove_missing();
    test_length_tracking();

    printf("All dict internal tests passed.\n");
    return 0;
}
