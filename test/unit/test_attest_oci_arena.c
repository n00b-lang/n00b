/** @file test/unit/test_attest_oci_arena.c — FR-22 arena-
 *  friendliness regression for the OCI client surface (WP-004
 *  Phase 4).
 *
 *  Constructs N=50 OCI client handles through a single n00b arena,
 *  exercises a manifest-build + digest-of-buffer pair against each
 *  handle (NO network — pure in-memory operations so the test is
 *  not Docker-gated), releases each handle, then triggers a GC
 *  pass and asserts the arena's used-bytes count drops back close
 *  to the pre-burst baseline.
 *
 *  This is the WP-004 sibling of WP-002 Phase 4's
 *  `test_attest_signer_arena.c` and WP-003 Phase 4's
 *  `test_attest_verifier_arena.c`: same `n00b_arena_used()` /
 *  `n00b_collect()` accounting, same "drop all stack locals + run
 *  collection" pattern, same `< before / 2` tolerance. The precedent's
 *  diagnostic narrative on the choice of leak-check primitive applies
 *  verbatim — libn00b does not (yet) expose a per-symbol-prefix leak
 *  primitive, so we use arena-level gross-byte accounting.
 *
 *  Per WP-004 Phase 1's design, the OCI client's `_client_new` stores
 *  the allocator on the handle so subsequent allocations during
 *  request dispatch + manifest-build inherit the arena (FR-21 / FR-22).
 *  This test passes `.allocator = arena` only at `_client_new` time
 *  and verifies the in-memory operations land in the same arena via
 *  the gross arena-used delta. No network call is made — the test
 *  exercises `_manifest_build` + `_digest_of_buffer` (Phase 2's pure
 *  in-memory primitives) which are sufficient to demonstrate per-arena
 *  allocator inheritance through the OCI client substrate without
 *  depending on a registry.
 *
 *  Test-file carve-out (D-030) applies — libc I/O for stdout logging
 *  is acceptable per the established test-file precedent.
 */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
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
#include "internal/attest/oci/registry.h"

#define ASSERT_OK(r) do { if (n00b_result_is_err(r)) { \
        fprintf(stderr, "FAIL @ %s:%d (err=%d)\n", __FILE__, __LINE__, \
                (int)n00b_result_get_err(r)); \
        assert(0); } } while (0)

#define ARENA_AS_ALLOC(a) ((n00b_allocator_t *)(a))

/** Outer fan-out per the Phase 4 spec: N=50 resolve / op / release
 *  through one arena. Matches the signer-/verifier-arena tests'
 *  fan-out constant verbatim so the three tests are visually paired. */
#define N00B_ATTEST_TEST_ARENA_FANOUT 50

// ---------------------------------------------------------------------------
// Per-pass: build a client, exercise an in-memory manifest-build +
// digest, release the client.
//
// Marked noinline so the call frame and its locals are distinct
// stack-scan units — the test's correctness depends on the
// conservative scanner being able to drop these locals after the
// frame returns. Same precaution `test_attest_signer_arena` /
// `test_attest_verifier_arena` take on their per-pass helpers.
// ---------------------------------------------------------------------------

[[gnu::noinline]] static void
client_op_release_once(n00b_allocator_t *allocator,
                       n00b_string_t    *registry_url,
                       n00b_string_t    *image_digest,
                       n00b_string_t    *envelope_digest,
                       n00b_string_t    *predicate_type,
                       n00b_string_t    *signer_keyid,
                       n00b_buffer_t    *envelope_bytes)
{
    // Construct: per Phase 1 the client stores `allocator` on its
    // handle so the subsequent in-memory operations (in this test,
    // the manifest-build + digest-of-buffer Phase-2 primitives)
    // inherit it without explicit re-threading.
    auto cr = n00b_attest_oci_client_new(registry_url,
                                          .allocator = allocator);
    ASSERT_OK(cr);
    n00b_attest_oci_client_t *client = n00b_result_get(cr);

    // Exercise an in-memory manifest-build + digest-of-buffer pair.
    // The two primitives between them touch every Phase-2 allocation
    // site that does NOT cross the network: serializer (manifest
    // body) + SHA-256 hex formatter (digest). No HTTP round-trip;
    // the test is not Docker-gated.
    auto mb_r = n00b_attest_oci_manifest_build(
        image_digest,
        4096,  // synthetic image_manifest_size — any value works
        envelope_digest,
        (uint64_t)envelope_bytes->byte_len,
        predicate_type,
        signer_keyid,
        .allocator = allocator);
    ASSERT_OK(mb_r);
    n00b_buffer_t *manifest_bytes = n00b_result_get(mb_r);

    auto dig_r = n00b_attest_oci_digest_of_buffer(manifest_bytes,
                                                   .allocator = allocator);
    ASSERT_OK(dig_r);

    // Release the client — the handle's cached state returns to
    // the arena via the arena's own reclamation cycle.
    n00b_attest_oci_client_release(client);

    // Drop locals so the conservative scanner can reclaim the
    // burst on the next GC pass.
    (void)client;
    (void)manifest_bytes;
}

// ---------------------------------------------------------------------------
// The arena-friendliness test.
// ---------------------------------------------------------------------------

static void
test_oci_arena_fr22(void)
{
    // One-time fixture setup. The fixture inputs live in the runtime
    // allocator, NOT the arena under test — the arena should be loaded
    // only by the per-pass client + manifest operations.
    n00b_string_t *registry_url    = n00b_string_from_cstr(
        "https://localhost:5000");
    n00b_string_t *image_digest    = n00b_string_from_cstr(
        "sha256:abc123abc123abc123abc123abc123abc123abc123abc123abc123abc123ab");
    n00b_string_t *envelope_digest = n00b_string_from_cstr(
        "sha256:def456def456def456def456def456def456def456def456def456def456de");
    n00b_string_t *predicate_type  = n00b_string_from_cstr(
        "https://slsa.dev/provenance/v1");
    n00b_string_t *signer_keyid    = n00b_string_from_cstr(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

    // Synthetic envelope bytes — any non-empty buffer works for the
    // manifest-build's `envelope_size` slot.
    static const char k_env[] = "{\"payload\":\"...\"}";
    n00b_buffer_t    *envelope_bytes = n00b_buffer_from_bytes(
        (char *)k_env,
        (int64_t)(sizeof(k_env) - 1));

    // Allocate the arena under test.
    n00b_arena_t     *arena = n00b_new_arena(.size   = 1 << 20, // 1 MiB
                                              .use_gc = true);
    n00b_allocator_t *alloc = ARENA_AS_ALLOC(arena);

    uint64_t baseline = n00b_arena_used(arena);
    printf("  arena baseline used bytes: %llu\n",
           (unsigned long long)baseline);

    for (int i = 0; i < N00B_ATTEST_TEST_ARENA_FANOUT; i++) {
        client_op_release_once(alloc,
                                registry_url,
                                image_digest,
                                envelope_digest,
                                predicate_type,
                                signer_keyid,
                                envelope_bytes);
    }

    uint64_t after_burst = n00b_arena_used(arena);
    printf("  arena used after %d client_new/manifest_build/release passes: %llu\n",
           N00B_ATTEST_TEST_ARENA_FANOUT,
           (unsigned long long)after_burst);

    // Real work happened — used-bytes MUST have grown.
    assert(after_burst > baseline);

    // Conservative-stack GC pass. The client_op_release_once frames
    // are gone; the stack holds no references into the arena, so
    // collection should reclaim the burst.
    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    uint64_t after_collect = n00b_arena_used(arena);
    printf("  arena used after GC: %llu\n",
           (unsigned long long)after_collect);

    // Same tolerance the WP-002 / WP-003 sibling tests use
    // (< before / 2).
    assert(after_collect < after_burst);
    assert(after_collect < after_burst / 2);

    printf("  [PASS] oci_arena_fr22: %d clients built, "
           "GC reclaimed %llu -> %llu bytes\n",
           N00B_ATTEST_TEST_ARENA_FANOUT,
           (unsigned long long)after_burst,
           (unsigned long long)after_collect);

    (void)envelope_bytes;
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);
    n00b_attest_module_init();

    printf("== n00b_attest OCI arena (FR-22) ==\n");
    test_oci_arena_fr22();

    printf("All n00b_attest OCI arena tests passed.\n");
    return 0;
}
