/*
 * test_dict_literal_real_headers.c — WP-011 Phase 5a regression
 *
 * Locks in the typeid-alignment fix (Phase 5a finding 1): the
 * canonical `n00b_dict_t(K, V)` macro in `include/adt/dict.h` now
 * uses `typeid("n00b_dict", k, v)` (was `typeid("dict", k, v)`),
 * matching the WP-010 list precedent (`n00b_list_t(T)` →
 * `typeid("n00b_list", T)`) and what ncc's `tag_is_dict_type` looks
 * for.
 *
 * This fixture uses the REAL `n00b_dict_t(K, V)` from
 * `include/adt/dict.h` — no private re-declaration — together with
 * the REAL n00b meson r-string config (the test executable is built
 * with `exe_c_args + n00b_static_init_helper_flags`, so dict literal
 * lowering goes through ncc's full xform pipeline and the
 * static-init helper).
 *
 * It also exercises Phase 5a finding 2 — `is_rstr_element_type`
 * widening to accept the `<rstr_string_type> + "*"` form (the
 * key type extracted from a `typeid("n00b_dict", n00b_string_t *, int)`
 * is `n00b_string_t *`, which the matcher previously rejected since
 * the configured `rstr_string_type` is `n00b_string_t` (no `*`)).
 *
 * If either gap reopens, this fixture fails at the ncc xform stage
 * (no canonical-tag match → dict literal lowering doesn't run) or
 * at the helper stage (r-string keys aren't recognized as static
 * pointer-key elements → bad image emission).
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

#include "n00b.h"
#include "adt/dict.h"
#include "core/runtime.h"

// Use the REAL n00b_dict_t from include/adt/dict.h. No private
// re-declaration: the whole point of this regression test is that
// the canonical macro's typeid matches what ncc looks for.
static n00b_dict_t(n00b_string_t *, int) test_dict = d{
    r"foo": 1,
    r"bar": 2,
    r"baz": 3,
};

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    // Layout invariants — the static dict image must report 3 live
    // entries at the floor capacity (N00B_DICT_MIN_SIZE == 16).
    assert(atomic_load_explicit(&test_dict.length,
                                memory_order_relaxed)
           == 3);
    assert(test_dict.skip_obj_hash == 0);
    assert(test_dict.lock == nullptr);

    auto store = test_dict.store;
    assert(store != nullptr);
    assert(store->last_slot == 15u);
    assert(atomic_load_explicit(&store->used_count,
                                memory_order_relaxed)
           == 3u);

    // Lookup configuration. The dict literal helper emits the dict
    // with `.fn=nullptr, .skip_obj_hash=0`, which routes lookups
    // through `n00b_hash(*key, nullptr)`. That path consults the
    // static-range descriptor's `cached_hash`, which differs across
    // r-string occurrences (the dict-key descriptors carry the
    // content-XXH3 that ncc precomputed; standalone `r"foo"` lvalues
    // carry `cached_hash=0` per Phase 3c.ii.b). Configure the dict
    // to hash query keys directly via `n00b_string_hash`, mirroring
    // exactly what the helper used to populate `bucket.hv`. This is
    // safe to mutate at runtime: the dict object lives in writable
    // .data (helper emits a designated initializer for the value
    // target), and `.skip_obj_hash + .fn` are stable for the
    // lifetime of the dict.
    test_dict.skip_obj_hash = 1;
    test_dict.fn            = (n00b_hash_fn)n00b_string_hash;

    // Content-based lookup via the canonical n00b_dict_get macro.
    // The macro takes `&(key)`, so the query key must be an lvalue.
    // Bind each r-string literal to a local pointer first.
    bool           found;
    n00b_string_t *q_foo  = r"foo";
    n00b_string_t *q_bar  = r"bar";
    n00b_string_t *q_baz  = r"baz";
    n00b_string_t *q_miss = r"quux";

    int v_foo = n00b_dict_get(&test_dict, q_foo, &found);
    assert(found);
    assert(v_foo == 1);

    int v_bar = n00b_dict_get(&test_dict, q_bar, &found);
    assert(found);
    assert(v_bar == 2);

    int v_baz = n00b_dict_get(&test_dict, q_baz, &found);
    assert(found);
    assert(v_baz == 3);

    // Missing key — must report not found.
    found = true;
    (void)n00b_dict_get(&test_dict, q_miss, &found);
    assert(!found);

    printf("test_dict_literal_real_headers: PASS\n");

    n00b_shutdown();
    return 0;
}
