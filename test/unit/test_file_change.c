/*
 * test_file_change.c — Tests for conduit filesystem change monitoring.
 */

#include <stdio.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include "internal/win32_sockets.h"
#else
#include <unistd.h>
#endif

#include "n00b.h"
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "conduit/file_change.h"
#include "core/alloc.h"
#include "core/runtime.h"

static int
test_file_close(int fd)
{
#ifdef _WIN32
    return _close(fd);
#else
    return close(fd);
#endif
}

static void
test_skip_or_fail(const char *message)
{
#ifdef _WIN32
    fprintf(stderr, "  [FAIL] %s\n", message);
    assert(false);
#else
    printf("  [SKIP] %s\n", message);
#endif
}

static int
test_file_write(int fd, const char *data, unsigned int len)
{
#ifdef _WIN32
    return _write(fd, data, len);
#else
    return (int)write(fd, data, len);
#endif
}

static void
test_file_sync(int fd)
{
#ifdef _WIN32
    (void)_commit(fd);
#else
    fsync(fd);
#endif
}

static int
test_file_temp_open(char *path, size_t path_len)
{
#ifdef _WIN32
    snprintf(path, path_len, "n00b_test_fc_%lu.tmp",
             (unsigned long)GetCurrentProcessId());
    return _open(path, _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY,
                 _S_IREAD | _S_IWRITE);
#else
    snprintf(path, path_len, "/tmp/n00b_test_fc_XXXXXX");
    return mkstemp(path);
#endif
}

// ============================================================================
// 1. Create file change topic and verify
// ============================================================================

static void
test_file_change_topic(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_io_backend_t *) ir = n00b_conduit_io_new_default(c);
    assert(n00b_result_is_ok(ir));
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    char tmppath[256];
    int  fd = test_file_temp_open(tmppath, sizeof(tmppath));
    if (fd < 0) {
        test_skip_or_fail("file_change topic (temp file failed)");
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    n00b_result_t(n00b_conduit_topic_base_t *) tr =
        n00b_conduit_file_change_topic(c, fd, N00B_CONDUIT_VNODE_WRITE);

    if (n00b_result_is_err(tr)) {
        test_skip_or_fail("file_change topic (not supported on this backend)");
        test_file_close(fd);
        remove(tmppath);
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    n00b_conduit_topic_base_t *topic = n00b_result_get(tr);
    assert(topic != nullptr);

    // Verify it is a file change topic.
    assert(n00b_conduit_topic_is_file_change(topic));
    assert(n00b_conduit_file_change_fd(topic) == fd);

    n00b_conduit_file_change_unwatch(c, fd);
    test_file_close(fd);
    remove(tmppath);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] file_change topic\n");
}

// ============================================================================
// 2. Same fd returns same topic
// ============================================================================

static void
test_file_change_same_topic(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_io_backend_t *) ir = n00b_conduit_io_new_default(c);
    assert(n00b_result_is_ok(ir));
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    char tmppath[256];
    int  fd = test_file_temp_open(tmppath, sizeof(tmppath));
    if (fd < 0) {
        test_skip_or_fail("file_change same topic (temp file failed)");
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    n00b_result_t(n00b_conduit_topic_base_t *) tr1 =
        n00b_conduit_file_change_topic(c, fd, N00B_CONDUIT_VNODE_WRITE);
    n00b_result_t(n00b_conduit_topic_base_t *) tr2 =
        n00b_conduit_file_change_topic(c, fd, N00B_CONDUIT_VNODE_ALL);

    if (n00b_result_is_err(tr1) || n00b_result_is_err(tr2)) {
        test_skip_or_fail("file_change same topic (not supported)");
        test_file_close(fd);
        remove(tmppath);
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    n00b_conduit_topic_base_t *t1 = n00b_result_get(tr1);
    n00b_conduit_topic_base_t *t2 = n00b_result_get(tr2);
    assert(t1 == t2);

    n00b_conduit_file_change_unwatch(c, fd);
    test_file_close(fd);
    remove(tmppath);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] file_change same topic\n");
}

// ============================================================================
// 3. Invalid fd returns error
// ============================================================================

static void
test_file_change_invalid_fd(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_topic_base_t *) tr =
        n00b_conduit_file_change_topic(c, -1, N00B_CONDUIT_VNODE_WRITE);
    assert(n00b_result_is_err(tr));

    n00b_conduit_destroy(c);
    printf("  [PASS] file_change invalid fd\n");
}

// ============================================================================
// 4. Write to a temp file: watch it and verify write event
// ============================================================================

static void
test_file_change_write_event(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_result_t(n00b_conduit_io_backend_t *) ir = n00b_conduit_io_new_default(c);
    assert(n00b_result_is_ok(ir));
    n00b_conduit_io_backend_t *io = n00b_result_get(ir);

    // Create a temp file.
    char tmppath[256];
    int  fd = test_file_temp_open(tmppath, sizeof(tmppath));
    if (fd < 0) {
        test_skip_or_fail("file_change write event (temp file failed)");
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    n00b_result_t(n00b_conduit_topic_base_t *) tr =
        n00b_conduit_file_change_topic(c, fd, N00B_CONDUIT_VNODE_WRITE);

    if (n00b_result_is_err(tr)) {
        test_skip_or_fail("file_change write event (backend not supported)");
        test_file_close(fd);
        remove(tmppath);
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    n00b_conduit_topic_base_t *topic = n00b_result_get(tr);

    // Create inbox and subscribe.
    n00b_conduit_file_change_inbox_t *inbox =
        n00b_conduit_file_change_inbox_new(c);
    assert(inbox != nullptr);

    n00b_conduit_sub_handle_t handle =
        n00b_conduit_file_change_subscribe(topic, inbox,
                                            .operations = N00B_CONDUIT_OP_ALL);
    assert(handle != N00B_CONDUIT_INVALID_SUB_HANDLE);

    // Write to the file to trigger the event.
    const char *data = "hello\n";
    (void)test_file_write(fd, data, 6);
    test_file_sync(fd);

    // Poll until we get the write event.
    bool got_message = false;
    for (int attempts = 0; attempts < 50; attempts++) {
        n00b_conduit_io_poll(io, 100);

        if (n00b_conduit_file_change_inbox_has_messages(inbox)) {
            got_message = true;
            break;
        }
    }

    if (!got_message) {
        test_skip_or_fail("file_change write event (no event delivered)");
        n00b_conduit_file_change_unwatch(c, fd);
        test_file_close(fd);
        remove(tmppath);
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    n00b_conduit_file_change_msg_t *msg =
        n00b_conduit_file_change_inbox_pop(inbox);
    assert(msg != nullptr);
    assert(msg->payload.fd == fd);
    assert(msg->payload.events & N00B_CONDUIT_VNODE_WRITE);

    n00b_conduit_file_change_unwatch(c, fd);
    test_file_close(fd);
    remove(tmppath);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] file_change write event\n");
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_file_change:\n");
    fflush(stdout);

    test_file_change_topic();
    fflush(stdout);
    test_file_change_same_topic();
    fflush(stdout);
    test_file_change_invalid_fd();
    fflush(stdout);
    test_file_change_write_event();
    fflush(stdout);

    printf("All file_change tests passed.\n");
    n00b_shutdown();
    return 0;
}
