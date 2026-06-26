#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "vks/vks.h"

// ============================================================================
// Helpers
// ============================================================================

typedef n00b_vks_store_t(n00b_string_t *, n00b_string_t *) str_store_t;

static str_store_t *
setup(void)
{
    str_store_t *s = n00b_vks_store_new(n00b_string_t *, n00b_string_t *);
    assert(s != nullptr);
    assert(s->mem != nullptr);
    return s;
}

static bool
str_eq(n00b_string_t *a, const char *b)
{
    if (a == nullptr) {
        return false;
    }
    size_t bl = strlen(b);
    return a->u8_bytes == bl && memcmp(a->data, b, bl) == 0;
}

// ============================================================================
// VKS-01: put / get / missing
// ============================================================================

static void
test_put_get(void)
{
    str_store_t   *s = setup();
    n00b_string_t *k = n00b_string_from_cstr("a");
    n00b_string_t *v = n00b_string_from_cstr("1");

    n00b_vks_put(s, k, v);

    bool           found = false;
    n00b_string_t *got   = n00b_vks_get(s, k, &found);
    assert(found);
    assert(str_eq(got, "1"));

    n00b_string_t *missing = n00b_string_from_cstr("missing");
    found                  = true;
    got                    = n00b_vks_get(s, missing, &found);
    assert(!found);

    printf("  [PASS] VKS-01 put_get\n");
}

// ============================================================================
// VKS-02: put_if_absent
// ============================================================================

static void
test_put_if_absent(void)
{
    str_store_t   *s  = setup();
    n00b_string_t *ka = n00b_string_from_cstr("a");
    n00b_string_t *v1 = n00b_string_from_cstr("1");
    n00b_string_t *v2 = n00b_string_from_cstr("2");

    n00b_vks_put(s, ka, v1);

    // "a" already present -> no overwrite, returns false.
    bool stored = n00b_vks_put_if_absent(s, ka, v2);
    assert(!stored);

    bool           found = false;
    n00b_string_t *got   = n00b_vks_get(s, ka, &found);
    assert(found);
    assert(str_eq(got, "1")); // unchanged

    // "b" absent -> stored, returns true.
    n00b_string_t *kb = n00b_string_from_cstr("b");
    stored            = n00b_vks_put_if_absent(s, kb, v2);
    assert(stored);

    found = false;
    got   = n00b_vks_get(s, kb, &found);
    assert(found);
    assert(str_eq(got, "2"));

    printf("  [PASS] VKS-02 put_if_absent\n");
}

// ============================================================================
// VKS-03: keys
// ============================================================================

static void
test_keys(void)
{
    str_store_t   *s = setup();
    n00b_string_t *ka = n00b_string_from_cstr("a");
    n00b_string_t *kb = n00b_string_from_cstr("b");
    n00b_string_t *kc = n00b_string_from_cstr("c");
    n00b_string_t *v  = n00b_string_from_cstr("x");

    n00b_vks_put(s, ka, v);
    n00b_vks_put(s, kb, v);
    n00b_vks_put(s, kc, v);

    n00b_result_t(n00b_list_t *) r = n00b_vks_keys(s);
    assert(n00b_result_is_ok(r));

    n00b_list_t(n00b_string_t *) *keys =
        (n00b_list_t(n00b_string_t *) *)n00b_result_get(r);
    assert(keys != nullptr);
    assert(n00b_list_len(*keys) == 3);

    // Every returned key must be one of the three we inserted (non-vacuous:
    // confirm presence of each).
    bool saw_a = false, saw_b = false, saw_c = false;
    n00b_list_foreach(*keys, kp) {
        n00b_string_t *k = *kp;
        if (str_eq(k, "a")) saw_a = true;
        if (str_eq(k, "b")) saw_b = true;
        if (str_eq(k, "c")) saw_c = true;
    }
    assert(saw_a && saw_b && saw_c);

    printf("  [PASS] VKS-03 keys\n");
}

// ============================================================================
// VKS-OVERWRITE
// ============================================================================

static void
test_overwrite(void)
{
    str_store_t   *s  = setup();
    n00b_string_t *k  = n00b_string_from_cstr("a");
    n00b_string_t *v1 = n00b_string_from_cstr("1");
    n00b_string_t *v9 = n00b_string_from_cstr("9");

    n00b_vks_put(s, k, v1);
    n00b_vks_put(s, k, v9);

    bool           found = false;
    n00b_string_t *got   = n00b_vks_get(s, k, &found);
    assert(found);
    assert(str_eq(got, "9"));

    printf("  [PASS] VKS-OVERWRITE\n");
}

// ============================================================================
// VKS-DIRTY: mutation hook + flush reset
// ============================================================================

static void
test_dirty(void)
{
    str_store_t *s = setup();

    _n00b_vks_store_internal_t *si = (_n00b_vks_store_internal_t *)s;
    assert(si->dirty == 0);

    n00b_string_t *v = n00b_string_from_cstr("v");

    const uint32_t N = 5;
    for (uint32_t i = 0; i < N; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "k%u", i);
        n00b_string_t *k = n00b_string_from_cstr(buf);
        n00b_vks_put(s, k, v);
    }

    // Each put fires the mutation hook exactly once.
    assert(si->dirty == N);

    // A delete also fires the hook.
    char buf0[8];
    snprintf(buf0, sizeof(buf0), "k0");
    // Re-create same content key; pointer identity differs, so the dict may not
    // find it — but the hook still fires on the attempt. We instead delete a
    // key we hold a pointer to, to keep the assertion meaningful.
    n00b_string_t *kd = n00b_string_from_cstr("del");
    n00b_vks_put(s, kd, v);
    assert(si->dirty == N + 1);
    bool removed = n00b_vks_del(s, kd);
    assert(removed);
    assert(si->dirty == N + 2);

    // flush resets dirty to 0 (no backend -> no-op flush).
    n00b_result_t(bool) fr = n00b_vks_flush(s);
    assert(n00b_result_is_ok(fr));
    assert(n00b_result_get(fr) == true);
    assert(si->dirty == 0);

    printf("  [PASS] VKS-DIRTY\n");
}

// ============================================================================
// err string helper
// ============================================================================

static void
test_err_names(void)
{
    assert(str_eq(n00b_vks_err_str(N00B_VKS_ERR_NOT_FOUND), "NOT_FOUND"));
    assert(str_eq(n00b_vks_err_str(N00B_VKS_ERR_EXISTS), "EXISTS"));
    assert(str_eq(n00b_vks_err_str(99999), "UNKNOWN"));

    printf("  [PASS] err_names\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running VKS core tests...\n");

    test_put_get();
    test_put_if_absent();
    test_keys();
    test_overwrite();
    test_dirty();
    test_err_names();

    printf("All VKS core tests passed.\n");
    n00b_shutdown();
    return 0;
}
