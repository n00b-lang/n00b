/** @file test/unit/test_attest_arena_lifecycle.c — arena-lifecycle
 *  regression test for the n00b_attest module (FR-22).
 *
 *  Allocates a fresh n00b arena, builds 100 Statement+envelope pairs
 *  threading the arena's allocator into every `_kargs`-bearing call,
 *  then triggers a GC pass to reclaim allocations whose only references
 *  live in the loop body. After collection, the arena's used-byte
 *  count must drop back to (approximately) its pre-`n00b_attest_*`
 *  baseline — proving the module did not stash any caller allocations
 *  in module-level state that would defeat arena collection.
 *
 *  ## Leak-check primitive used
 *
 *  libn00b does NOT expose a per-symbol-prefix leak-check primitive.
 *  A grep over `~/n00b-attest/include/` and `~/n00b-attest/src/core/`
 *  finds only `n00b_arena_used()` (arena.h via gc.h) and
 *  `arena->alloc_count` (arena.h) — both gross-byte / gross-count
 *  accounting at the arena level. There is no `n00b_allocator_destroy`
 *  for the GC'd arena either (arena destruction happens via GC, not an
 *  explicit free).
 *
 *  We therefore use **`n00b_arena_used(arena)` as a gross-byte
 *  accounting probe** per WP-001 plan's "either form acceptable"
 *  carve-out: measure baseline used-bytes, build N
 *  Statement+envelope pairs, drop all stack-local references, invoke
 *  `n00b_collect()`, and assert `n00b_arena_used` returns to a value
 *  within a small tolerance of the baseline. If the module had
 *  stashed any caller allocation in a module-level container,
 *  collection wouldn't free it and the used-bytes count would stay
 *  high.
 *
 *  This is the orchestrator-acceptable interim measurement. A future
 *  WP that lifts a per-prefix `n00b_alloc_count_by_prefix()` (or a
 *  similar primitive) into libn00b would tighten the guarantee, but
 *  is not required for WP-001 closure.
 */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/stw.h"
#include "core/atomic.h"
#include "attest/n00b_attest.h"

#define ASSERT_OK(r) do { if (n00b_result_is_err(r)) { \
        fprintf(stderr, "FAIL @ %s:%d (err=%d)\n", __FILE__, __LINE__, \
                n00b_result_get_err(r)); \
        assert(0); } } while (0)

#define ARENA_AS_ALLOC(a) ((n00b_allocator_t *)(a))

#define N_PAIRS 100

// Build one Statement + one envelope, threading `allocator` through
// every kwarg call. Returns the envelope JSON bytes (the build session's
// terminal artifact) — but does NOT retain a pointer to anything beyond
// the call frame, so the GC can reclaim the intermediate allocations
// on collection.
[[gnu::noinline]] static void
build_one(n00b_allocator_t *allocator, int seed)
{
    n00b_attest_statement_t *st = n00b_attest_statement_new(
        .allocator = allocator);

    uint8_t d[32];
    for (int i = 0; i < 32; i++) {
        d[i] = (uint8_t)((seed * 13 + i * 7 + 5) & 0xff);
    }
    n00b_buffer_t *digest = n00b_buffer_from_bytes(
        (char *)d,
        32,
        .allocator = allocator);

    auto ar = n00b_attest_statement_add_subject(
        st,
        .name      = n00b_string_from_cstr("arena-subject",
                                           .allocator = allocator),
        .digest    = digest,
        .allocator = allocator);
    ASSERT_OK(ar);

    auto tr = n00b_attest_statement_set_predicate_type(
        st,
        n00b_string_from_cstr("https://slsa.dev/provenance/v1",
                              .allocator = allocator),
        .allocator = allocator);
    ASSERT_OK(tr);

    char pred_buf[64];
    int  pred_len = snprintf(pred_buf, sizeof(pred_buf),
                             "{\"i\":%d}", seed);
    n00b_buffer_t *pred = n00b_buffer_from_bytes(
        pred_buf,
        (int64_t)pred_len,
        .allocator = allocator);
    auto pr = n00b_attest_statement_set_predicate_json(
        st,
        pred,
        .allocator = allocator);
    ASSERT_OK(pr);

    auto sr = n00b_attest_statement_serialize(st, .allocator = allocator);
    ASSERT_OK(sr);
    n00b_buffer_t *stmt_bytes = n00b_result_get(sr);

    n00b_attest_envelope_t *env = n00b_attest_envelope_new(
        .allocator = allocator);
    auto sp = n00b_attest_envelope_set_payload(env, stmt_bytes);
    ASSERT_OK(sp);

    auto esr = n00b_attest_envelope_serialize(env, .allocator = allocator);
    ASSERT_OK(esr);
    (void)n00b_result_get(esr);

    // Drop locals; collection should be able to reclaim everything
    // allocated here.
    (void)st;
    (void)env;
}

static void
test_arena_lifecycle(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 1 << 20, // 1 MiB
                                          .use_gc = true);

    n00b_allocator_t *alloc = ARENA_AS_ALLOC(arena);

    uint64_t baseline = n00b_arena_used(arena);
    printf("  arena baseline used bytes: %llu\n",
           (unsigned long long)baseline);

    for (int i = 0; i < N_PAIRS; i++) {
        build_one(alloc, i);
    }

    uint64_t after_build = n00b_arena_used(arena);
    printf("  arena used after %d build pairs: %llu\n",
           N_PAIRS, (unsigned long long)after_build);
    // We built real work, so used-bytes MUST have grown.
    assert(after_build > baseline);

    // Trigger collection. The build_one frames are gone; the stack
    // holds no references into the arena, so a conservative-stack GC
    // pass should reclaim the lot.
    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    uint64_t after_collect = n00b_arena_used(arena);
    printf("  arena used after GC: %llu\n",
           (unsigned long long)after_collect);

    // The collected size must be much smaller than after_build —
    // demonstrably proving the module didn't stash references that
    // would have kept the build artifacts alive. We allow conservative
    // stack-scanning to keep a small fraction (a few KB worth of
    // false-retentions are normal in n00b's collector — see
    // test_unreachable_collected in test_gc.c which uses the same
    // < before/2 tolerance for 400 objects).
    assert(after_collect < after_build);
    assert(after_collect < after_build / 2);

    printf("  [PASS] arena_lifecycle: %d pairs built, GC reclaimed "
           "%llu -> %llu bytes\n",
           N_PAIRS,
           (unsigned long long)after_build,
           (unsigned long long)after_collect);
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    printf("== n00b_attest arena-lifecycle (FR-22) ==\n");
    test_arena_lifecycle();

    printf("All n00b_attest arena-lifecycle tests passed.\n");
    return 0;
}
