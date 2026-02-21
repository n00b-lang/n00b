/*
 * test_dual_backend.c — Tests for multi-backend support in the conduit system.
 *
 * Exercises backend registration, lookup, unregistration, and the
 * default-backend accessor against a live conduit instance.
 */

#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "core/alloc.h"
#include "core/list.h"
#include "core/runtime.h"

// ============================================================================
// 1. Register a single backend
// ============================================================================

static void
test_backend_register(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);
    assert(c != nullptr);

    n00b_result_t(n00b_conduit_io_backend_t *) ir =
        n00b_conduit_io_new(c, n00b_result_get(n00b_conduit_io_default_ops()));
    assert(n00b_result_is_ok(ir));
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);
    assert(io != nullptr);

    // num_backends should now be 1.
    assert(n00b_list_len(c->io_backends) == 1);

    // default_backend should return Some.
    n00b_option_t(n00b_conduit_io_backend_t *) def =
        n00b_conduit_default_backend(c);
    assert(n00b_option_is_set(def));
    assert(n00b_option_get(def) == io);

    n00b_conduit_io_destroy(io);
    assert(n00b_list_len(c->io_backends) == 0);

    n00b_conduit_destroy(c);

    printf("  [PASS] backend register\n");
}

// ============================================================================
// 2. Register two backends
// ============================================================================

static void
test_two_backends(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);
    assert(c != nullptr);

    n00b_result_t(n00b_conduit_io_backend_t *) ir1 =
        n00b_conduit_io_new(c, n00b_result_get(n00b_conduit_io_default_ops()));
    assert(n00b_result_is_ok(ir1));
    n00b_conduit_io_backend_t *io1 = n00b_result_get(ir1);
    assert(io1 != nullptr);

    n00b_result_t(n00b_conduit_io_backend_t *) ir2 =
        n00b_conduit_io_new(c, n00b_result_get(n00b_conduit_io_default_ops()));
    assert(n00b_result_is_ok(ir2));
    n00b_conduit_io_backend_t *io2 = n00b_result_get(ir2);
    assert(io2 != nullptr);

    // num_backends should now be 2.
    assert(n00b_list_len(c->io_backends) == 2);

    // default_backend should return Some pointing to the first registered backend.
    n00b_option_t(n00b_conduit_io_backend_t *) def =
        n00b_conduit_default_backend(c);
    assert(n00b_option_is_set(def));
    assert(n00b_option_get(def) == io1);

    n00b_conduit_io_destroy(io2);
    n00b_conduit_io_destroy(io1);

    n00b_conduit_destroy(c);

    printf("  [PASS] two backends\n");
}

// ============================================================================
// 3. Unregister the first backend; second becomes default
// ============================================================================

static void
test_backend_unregister(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);
    assert(c != nullptr);

    n00b_result_t(n00b_conduit_io_backend_t *) ir1 =
        n00b_conduit_io_new(c, n00b_result_get(n00b_conduit_io_default_ops()));
    assert(n00b_result_is_ok(ir1));
    n00b_conduit_io_backend_t *io1 = n00b_result_get(ir1);
    assert(io1 != nullptr);

    n00b_result_t(n00b_conduit_io_backend_t *) ir2 =
        n00b_conduit_io_new(c, n00b_result_get(n00b_conduit_io_default_ops()));
    assert(n00b_result_is_ok(ir2));
    n00b_conduit_io_backend_t *io2 = n00b_result_get(ir2);
    assert(io2 != nullptr);

    assert(n00b_list_len(c->io_backends) == 2);

    // Destroy the first backend; this calls unregister internally.
    // The second backend shifts into slot 0 and becomes the new default.
    n00b_conduit_io_destroy(io1);

    assert(n00b_list_len(c->io_backends) == 1);

    n00b_option_t(n00b_conduit_io_backend_t *) def =
        n00b_conduit_default_backend(c);
    assert(n00b_option_is_set(def));
    assert(n00b_option_get(def) == io2);

    n00b_conduit_io_destroy(io2);

    n00b_conduit_destroy(c);

    printf("  [PASS] backend unregister\n");
}

// ============================================================================
// 4. default_backend on a conduit with no backends returns None
// ============================================================================

static void
test_default_backend_empty(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);
    assert(c != nullptr);

    // No backends registered yet.
    assert(n00b_list_len(c->io_backends) == 0);

    n00b_option_t(n00b_conduit_io_backend_t *) def =
        n00b_conduit_default_backend(c);
    assert(!n00b_option_is_set(def));

    n00b_conduit_destroy(c);

    printf("  [PASS] default backend empty\n");
}

// ============================================================================
// 5. Many backends — list grows dynamically
// ============================================================================

#define TEST_MANY_BACKENDS 16

static void
test_many_backends(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);
    assert(c != nullptr);

    n00b_conduit_io_backend_t *backends[TEST_MANY_BACKENDS];

    for (int i = 0; i < TEST_MANY_BACKENDS; i++) {
        n00b_result_t(n00b_conduit_io_backend_t *) ir =
            n00b_conduit_io_new(c, n00b_result_get(n00b_conduit_io_default_ops()));
        assert(n00b_result_is_ok(ir));
        backends[i] = n00b_result_get(ir);
    }

    assert(n00b_list_len(c->io_backends) == TEST_MANY_BACKENDS);

    // Default is still the first registered.
    n00b_option_t(n00b_conduit_io_backend_t *) def =
        n00b_conduit_default_backend(c);
    assert(n00b_option_is_set(def));
    assert(n00b_option_get(def) == backends[0]);

    // Clean up.
    for (int i = TEST_MANY_BACKENDS - 1; i >= 0; i--) {
        n00b_conduit_io_destroy(backends[i]);
    }

    n00b_conduit_destroy(c);
    printf("  [PASS] many backends\n");
}

// ============================================================================
// 6. Backend by name
// ============================================================================

static void
test_backend_by_name(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);
    assert(c != nullptr);

    n00b_result_t(n00b_conduit_io_backend_t *) ir =
        n00b_conduit_io_new(c, n00b_result_get(n00b_conduit_io_default_ops()));
    assert(n00b_result_is_ok(ir));
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    // Look up by the backend's ops name.
    if (io->ops->name) {
        n00b_string_t name = io->ops->name();
        n00b_option_t(n00b_conduit_io_backend_t *) found =
            n00b_conduit_backend_by_name(c, name);
        assert(n00b_option_is_set(found));
        assert(n00b_option_get(found) == io);
    }

    // Non-existent name must return None.
    n00b_option_t(n00b_conduit_io_backend_t *) none =
        n00b_conduit_backend_by_name(c, *r"no_such_backend_xyz");
    assert(!n00b_option_is_set(none));

    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] backend by name\n");
}

// ============================================================================
// 7. Null args to register/unregister
// ============================================================================

static void
test_null_args(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    // register_backend(null, io) — need a backend to test with.
    n00b_result_t(n00b_conduit_io_backend_t *) ir =
        n00b_conduit_io_new(c, n00b_result_get(n00b_conduit_io_default_ops()));
    assert(n00b_result_is_ok(ir));
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    // register_backend(null, io) must fail.
    n00b_result_t(bool) r1 = n00b_conduit_register_backend(nullptr, io);
    assert(n00b_result_is_err(r1));

    // register_backend(c, null) must fail.
    n00b_result_t(bool) r2 = n00b_conduit_register_backend(c, nullptr);
    assert(n00b_result_is_err(r2));

    // unregister_backend(null, io) must not crash.
    n00b_conduit_unregister_backend(nullptr, io);

    // unregister_backend(c, null) must not crash.
    n00b_conduit_unregister_backend(c, nullptr);

    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] null args\n");
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_dual_backend:\n");
    fflush(stdout);

    test_backend_register();
    fflush(stdout);
    test_two_backends();
    fflush(stdout);
    test_backend_unregister();
    fflush(stdout);
    test_default_backend_empty();
    fflush(stdout);
    test_many_backends();
    fflush(stdout);
    test_backend_by_name();
    fflush(stdout);
    test_null_args();
    fflush(stdout);

    printf("All dual backend tests passed.\n");
    n00b_shutdown();
    return 0;
}
