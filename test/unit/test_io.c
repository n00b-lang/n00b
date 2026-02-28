/*
 * test_io.c — Tests for conduit IO backend lifecycle and event delivery.
 *
 * Tests use a pipe pair to verify FD readability detection through the
 * platform-default IO backend.
 */

#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include "n00b.h"
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "core/alloc.h"
#include "core/runtime.h"

// ============================================================================
// 1. IO backend create / destroy
// ============================================================================

static void
test_io_create_destroy(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_io_backend_t *) ir = n00b_conduit_io_new_default(c);
    assert(n00b_result_is_ok(ir));
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);
    assert(io != nullptr);

    n00b_string_t *name = n00b_conduit_io_name(io);
    assert(name->data != nullptr);
    printf("    (backend: %.*s)\n", (int)name->u8_bytes, name->data);

    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] IO backend create/destroy\n");
}

// ============================================================================
// 2. Watch a pipe FD for readability
// ============================================================================

static void
test_io_watch_pipe(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_io_backend_t *) ir = n00b_conduit_io_new_default(c);
    assert(n00b_result_is_ok(ir));
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    int fds[2];
    int rc = pipe(fds);
    assert(rc == 0);

    // Watch read end for readability.
    n00b_result_t(n00b_conduit_topic_base_t *) tr =
        n00b_conduit_io_watch(io, fds[0], N00B_CONDUIT_IO_READ, nullptr);
    assert(n00b_result_is_ok(tr));

    // Write some data so the read end becomes readable.
    const char *msg = "hello";
    (void)write(fds[1], msg, strlen(msg));

    // Poll — should get at least 1 event.
    auto poll_r = n00b_conduit_io_poll(io, 100);
    assert(n00b_result_is_ok(poll_r));
    int n = n00b_result_get(poll_r);
    assert(n >= 1);

    // Clean up.
    n00b_conduit_io_unwatch(io, fds[0]);
    close(fds[0]);
    close(fds[1]);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] watch pipe FD\n");
}

// ============================================================================
// 3. Unwatch removes FD
// ============================================================================

static void
test_io_unwatch(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_io_backend_t *) ir = n00b_conduit_io_new_default(c);
    assert(n00b_result_is_ok(ir));
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    int fds[2];
    int rc = pipe(fds);
    assert(rc == 0);

    n00b_conduit_io_watch(io, fds[0], N00B_CONDUIT_IO_READ, nullptr);
    bool ok = n00b_conduit_io_unwatch(io, fds[0]);
    assert(ok);

    // Write data. Poll should return 0 since we unwatched.
    (void)write(fds[1], "x", 1);
    auto poll_r2 = n00b_conduit_io_poll(io, 50);
    assert(n00b_result_is_ok(poll_r2));
    int n = n00b_result_get(poll_r2);
    assert(n == 0);

    close(fds[0]);
    close(fds[1]);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] unwatch removes FD\n");
}

// ============================================================================
// 4. Shutdown prevents new watches
// ============================================================================

static void
test_io_shutdown(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_io_backend_t *) ir = n00b_conduit_io_new_default(c);
    assert(n00b_result_is_ok(ir));
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    n00b_conduit_io_shutdown(io);

    int fds[2];
    int rc = pipe(fds);
    assert(rc == 0);

    n00b_result_t(n00b_conduit_topic_base_t *) tr =
        n00b_conduit_io_watch(io, fds[0], N00B_CONDUIT_IO_READ, nullptr);
    assert(n00b_result_is_err(tr));
    assert(n00b_result_get_err(tr) == N00B_CONDUIT_ERR_SHUTDOWN);

    close(fds[0]);
    close(fds[1]);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] shutdown prevents watches\n");
}

// ============================================================================
// 5. Null arg handling
// ============================================================================

static void
test_io_null_args(void)
{
    // Null backend.
    assert(n00b_result_is_err(n00b_conduit_io_poll(nullptr, 0)));
    n00b_conduit_io_run(nullptr);   // Should not crash.
    n00b_conduit_io_shutdown(nullptr); // Should not crash.
    n00b_conduit_io_destroy(nullptr);  // Should not crash.

    printf("  [PASS] null arg handling\n");
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_io:\n");
    fflush(stdout);

    test_io_create_destroy();
    fflush(stdout);
    test_io_watch_pipe();
    fflush(stdout);
    test_io_unwatch();
    fflush(stdout);
    test_io_shutdown();
    fflush(stdout);
    test_io_null_args();
    fflush(stdout);

    printf("All IO tests passed.\n");
    n00b_shutdown();
    return 0;
}
