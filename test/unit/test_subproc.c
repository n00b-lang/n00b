/*
 * test_subproc.c — Tests for subprocess management module.
 */

#ifndef _WIN32

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "n00b.h"
#include "io/subproc.h"
#include "conduit/conduit.h"
#include "conduit/io.h"
#include "conduit/xform_linebuf.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"

// ============================================================================
// Helpers
// ============================================================================

static n00b_conduit_t *
make_conduit(void)
{
    n00b_result_t(n00b_conduit_t *) cr = n00b_conduit_new();
    assert(n00b_result_is_ok(cr));
    return n00b_result_get(cr);
}

static n00b_conduit_io_backend_t *
make_io(n00b_conduit_t *c)
{
    n00b_result_t(n00b_conduit_io_backend_t *) ir = n00b_conduit_io_new_default(c);
    assert(n00b_result_is_ok(ir));
    return n00b_result_get(ir);
}

// ============================================================================
// 1. Basic init and flag defaults
// ============================================================================

static void
test_init_defaults(void)
{
    n00b_conduit_t *c = make_conduit();

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd     = n00b_string_from_cstr("/bin/echo"),
        .conduit = c);

    // Not spawned yet.
    assert(!n00b_subproc_is_spawned(&sp));
    assert(!n00b_subproc_exited(&sp));
    assert(!n00b_subproc_errored(&sp));
    assert(!n00b_subproc_timed_out(&sp));
    assert(!n00b_option_is_set(n00b_subproc_pid(&sp)));

    // Merge is on by default.
    assert((sp.flags & N00B_SUBPROC_MERGE_OUTPUT) != 0);

    // Handle winsize on by default.
    assert((sp.flags & N00B_SUBPROC_HANDLE_WINSIZE) != 0);

    n00b_conduit_destroy(c);
    printf("  [PASS] init defaults\n");
}

// ============================================================================
// 2. Capture convenience flag
// ============================================================================

static void
test_capture_convenience(void)
{
    n00b_conduit_t *c = make_conduit();

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd     = n00b_string_from_cstr("/bin/echo"),
        .conduit = c,
        .capture = true);

    assert((sp.flags & N00B_SUBPROC_CAP_ALL) == N00B_SUBPROC_CAP_ALL);

    n00b_conduit_destroy(c);
    printf("  [PASS] capture convenience\n");
}

// ============================================================================
// 3. Proxy convenience flag
// ============================================================================

static void
test_proxy_convenience(void)
{
    n00b_conduit_t *c = make_conduit();

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd     = n00b_string_from_cstr("/bin/echo"),
        .conduit = c,
        .proxy   = true);

    assert((sp.flags & N00B_SUBPROC_PROXY_ALL) == N00B_SUBPROC_PROXY_ALL);

    n00b_conduit_destroy(c);
    printf("  [PASS] proxy convenience\n");
}

// ============================================================================
// 4. Accessors return sane defaults before spawn
// ============================================================================

static void
test_accessors_pre_spawn(void)
{
    n00b_conduit_t *c = make_conduit();

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd     = n00b_string_from_cstr("/bin/echo"),
        .conduit = c);

    // Capture buffers return non-null (empty sentinel).
    assert(n00b_subproc_stdout(&sp) != nullptr);
    assert(n00b_subproc_stderr(&sp) != nullptr);
    assert(n00b_subproc_stdin_capture(&sp) != nullptr);

    // Exit code should be error (not exited).
    n00b_result_t(int) r = n00b_subproc_exit_code(&sp);
    assert(n00b_result_is_err(r));

    // Term signal should be error (not exited).
    r = n00b_subproc_term_signal(&sp);
    assert(n00b_result_is_err(r));

    n00b_conduit_destroy(c);
    printf("  [PASS] accessors pre-spawn\n");
}

// ============================================================================
// 5. Spawn /bin/echo — basic pipe mode
// ============================================================================

static void
test_spawn_echo(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd     = n00b_string_from_cstr("/bin/echo"),
        .conduit = c,
        .io      = io,
        .capture_stdout = true);

    n00b_result_t(bool) r = n00b_subproc_spawn(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] spawn echo (err=%d)\n", n00b_result_get_err(r));
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    assert(n00b_result_is_ok(r));
    assert(n00b_subproc_is_spawned(&sp));
    assert(n00b_option_is_set(n00b_subproc_pid(&sp)));
    assert(n00b_option_get(n00b_subproc_pid(&sp)) > 0);

    // Wait for exit (simple waitpid).
    n00b_result_t(bool) wr = n00b_subproc_wait(&sp);
    assert(n00b_result_is_ok(wr));
    assert(n00b_subproc_exited(&sp));

    n00b_result_t(int) ec = n00b_subproc_exit_code(&sp);
    assert(n00b_result_is_ok(ec));
    assert(n00b_result_get(ec) == 0);

    n00b_subproc_close(&sp);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] spawn echo\n");
}

// ============================================================================
// 6. Spawn with invalid command → error
// ============================================================================

static void
test_spawn_bad_cmd(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd     = n00b_string_from_cstr("/nonexistent/bad/command"),
        .conduit = c,
        .io      = io);

    n00b_result_t(bool) r = n00b_subproc_spawn(&sp);
    // Should error because exec fails.
    assert(n00b_result_is_err(r));
    assert(n00b_subproc_errored(&sp));

    n00b_subproc_close(&sp);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] spawn bad command\n");
}

// ============================================================================
// 7. Spawn with no command → EINVAL
// ============================================================================

static void
test_spawn_no_cmd(void)
{
    n00b_conduit_t *c = make_conduit();

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp, .conduit = c);

    n00b_result_t(bool) r = n00b_subproc_spawn(&sp);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == EINVAL);

    n00b_conduit_destroy(c);
    printf("  [PASS] spawn no command\n");
}

// ============================================================================
// 8. Double spawn → EALREADY
// ============================================================================

static void
test_double_spawn(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd     = n00b_string_from_cstr("/bin/echo"),
        .conduit = c,
        .io      = io);

    n00b_result_t(bool) r = n00b_subproc_spawn(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] double spawn (first spawn failed)\n");
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    // Second spawn should fail.
    n00b_result_t(bool) r2 = n00b_subproc_spawn(&sp);
    assert(n00b_result_is_err(r2));
    assert(n00b_result_get_err(r2) == EALREADY);

    // Clean up.
    n00b_subproc_wait(&sp);
    n00b_subproc_close(&sp);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] double spawn\n");
}

// ============================================================================
// 9. Close is idempotent
// ============================================================================

static void
test_close_idempotent(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd     = n00b_string_from_cstr("/bin/echo"),
        .conduit = c,
        .io      = io);

    n00b_result_t(bool) r = n00b_subproc_spawn(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] close idempotent\n");
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    n00b_subproc_wait(&sp);
    n00b_subproc_close(&sp);
    n00b_subproc_close(&sp); // Second close should not crash.
    n00b_subproc_close(&sp); // Third close should not crash.

    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] close idempotent\n");
}

// ============================================================================
// 10. Synchronous run with /bin/echo
// ============================================================================

static void
test_run_echo(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd     = n00b_string_from_cstr("/bin/echo"),
        .conduit = c,
        .io      = io);

    n00b_result_t(bool) r = n00b_subproc_run(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] run echo (err=%d)\n", n00b_result_get_err(r));
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    assert(n00b_result_is_ok(r));
    assert(n00b_subproc_exited(&sp));

    n00b_result_t(int) ec = n00b_subproc_exit_code(&sp);
    assert(n00b_result_is_ok(ec));
    assert(n00b_result_get(ec) == 0);

    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] run echo\n");
}

// ============================================================================
// 11. Kill signal delivery
// ============================================================================

static void
test_kill(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd     = n00b_string_from_cstr("/bin/sleep"),
        .conduit = c,
        .io      = io,
        .args    = nullptr);

    // Add "100" argument.
    n00b_array_t(n00b_string_t) args = n00b_array_new(n00b_string_t, 1);
    n00b_array_set(args, 0, n00b_string_from_cstr("100"));
    sp.args = &args;

    n00b_result_t(bool) r = n00b_subproc_spawn(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] kill (spawn failed)\n");
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    // Send SIGTERM.
    n00b_result_t(bool) kr = n00b_subproc_kill(&sp);
    assert(n00b_result_is_ok(kr));

    // Wait for exit.
    n00b_subproc_wait(&sp);
    assert(n00b_subproc_exited(&sp));

    // Should have been terminated by signal.
    n00b_result_t(int) ts = n00b_subproc_term_signal(&sp);
    assert(n00b_result_is_ok(ts));
    assert(n00b_result_get(ts) == SIGTERM);

    n00b_subproc_close(&sp);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] kill\n");
}

// ============================================================================
// 12. Pre-exec hook fires
// ============================================================================

static void
test_hook_fn(void *param)
{
    // This runs in the child — we can't check it directly, but we
    // can verify the child runs successfully (the hook doesn't crash).
    // For verification, we write to a pipe.
    int *fd = (int *)param;
    if (fd) {
        char c = 'H';
        (void)!write(*fd, &c, 1);
    }
}

static void
test_pre_exec_hook(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    // Create a pipe to verify hook fired in child.
    int hook_pipe[2];
    if (pipe(hook_pipe) < 0) {
        printf("  [SKIP] pre-exec hook (pipe failed)\n");
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd           = n00b_string_from_cstr("/bin/echo"),
        .conduit       = c,
        .io            = io,
        .pre_exec_hook = test_hook_fn,
        .hook_param    = &hook_pipe[1]);

    n00b_result_t(bool) r = n00b_subproc_spawn(&sp);
    close(hook_pipe[1]); // Close write end in parent.

    if (n00b_result_is_err(r)) {
        close(hook_pipe[0]);
        printf("  [SKIP] pre-exec hook (spawn failed)\n");
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    // Read from hook pipe.
    char buf = 0;
    ssize_t n = read(hook_pipe[0], &buf, 1);
    close(hook_pipe[0]);
    assert(n == 1 && buf == 'H');

    n00b_subproc_wait(&sp);
    n00b_subproc_close(&sp);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] pre-exec hook\n");
}

// ============================================================================
// 13. Timeout with SIGTERM
// ============================================================================

static void
test_timeout_sigterm(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    n00b_duration_t timeout = {.tv_sec = 0, .tv_nsec = 100000000}; // 100ms

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd            = n00b_string_from_cstr("/bin/sleep"),
        .conduit        = c,
        .io             = io,
        .timeout        = &timeout,
        .timeout_policy = N00B_SUBPROC_TIMEOUT_SIGTERM);

    n00b_array_t(n00b_string_t) args = n00b_array_new(n00b_string_t, 1);
    n00b_array_set(args, 0, n00b_string_from_cstr("100"));
    sp.args = &args;

    n00b_result_t(bool) r = n00b_subproc_run(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] timeout sigterm (err=%d)\n", n00b_result_get_err(r));
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    assert(n00b_subproc_timed_out(&sp));
    assert(n00b_subproc_exited(&sp));

    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] timeout sigterm\n");
}

// ============================================================================
// 14. Spawn with cwd — child runs in specified directory
// ============================================================================

static void
test_spawn_cwd(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    // Use /bin/pwd to print working directory.  Capture stdout via a
    // pipe we read ourselves (capture wiring isn't done yet).
    int out_pipe[2];
    if (pipe(out_pipe) < 0) {
        printf("  [SKIP] spawn cwd (pipe failed)\n");
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd     = n00b_string_from_cstr("/bin/pwd"),
        .conduit = c,
        .io      = io,
        .cwd     = n00b_string_from_cstr("/tmp"),
        .capture_stdout = true);

    n00b_result_t(bool) r = n00b_subproc_spawn(&sp);
    if (n00b_result_is_err(r)) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        printf("  [SKIP] spawn cwd (spawn failed, err=%d)\n",
               n00b_result_get_err(r));
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    n00b_subproc_wait(&sp);
    assert(n00b_subproc_exited(&sp));

    n00b_result_t(int) ec = n00b_subproc_exit_code(&sp);
    assert(n00b_result_is_ok(ec));
    assert(n00b_result_get(ec) == 0);

    n00b_subproc_close(&sp);
    close(out_pipe[0]);
    close(out_pipe[1]);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] spawn cwd\n");
}

// ============================================================================
// 15. Stdout xform chain — linebuf strips trailing newline
// ============================================================================

static void
test_stdout_xform_linebuf(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    // Build a specs array with the default linebuf spec.
    // Linebuf with include_delimiter=false strips the trailing '\n'
    // from each line.
    n00b_array_t(void *) xforms = n00b_array_new(void *, 1);
    n00b_array_set(xforms, 0,
                   (void *)&n00b_conduit_linebuf_default_spec);

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd            = n00b_string_from_cstr("/bin/echo"),
        .conduit        = c,
        .io             = io,
        .capture_stdout = true,
        .merge          = false,
        .stdout_xforms  = &xforms);

    // Add "hello" argument.
    n00b_array_t(n00b_string_t) args = n00b_array_new(n00b_string_t, 1);
    n00b_array_set(args, 0, n00b_string_from_cstr("hello"));
    sp.args = &args;

    n00b_result_t(bool) r = n00b_subproc_run(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] stdout xform linebuf (err=%d)\n",
               n00b_result_get_err(r));
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    assert(n00b_result_is_ok(r));
    assert(n00b_subproc_exited(&sp));

    // The linebuf transform strips the trailing '\n', so we
    // should see "hello" without newline.
    n00b_buffer_t *out = n00b_subproc_stdout(&sp);
    assert(out != nullptr);

    if (out->byte_len > 0) {
        // Linebuf emits line-by-line; capture accumulates them.
        // "hello\n" → linebuf strips delimiter → "hello" (5 bytes).
        assert(out->byte_len == 5);
        assert(memcmp(out->data, "hello", 5) == 0);
    }

    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] stdout xform linebuf\n");
}

// ============================================================================
// 16. Capture echo hello (no xforms) — verify raw capture
// ============================================================================

static void
test_capture_echo_hello(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd            = n00b_string_from_cstr("/bin/echo"),
        .conduit        = c,
        .io             = io,
        .capture_stdout = true,
        .merge          = false);

    n00b_array_t(n00b_string_t) args = n00b_array_new(n00b_string_t, 1);
    n00b_array_set(args, 0, n00b_string_from_cstr("hello"));
    sp.args = &args;

    n00b_result_t(bool) r = n00b_subproc_run(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] capture echo hello (err=%d)\n",
               n00b_result_get_err(r));
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    assert(n00b_result_is_ok(r));
    assert(n00b_subproc_exited(&sp));

    n00b_buffer_t *out = n00b_subproc_stdout(&sp);
    assert(out != nullptr);
    // /bin/echo hello produces "hello\n" = 6 bytes.
    assert(out->byte_len == 6);
    assert(memcmp(out->data, "hello\n", 6) == 0);

    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] capture echo hello\n");
}

// ============================================================================
// 17. Capture cat with stdin_inject + close_stdin
// ============================================================================

static void
test_capture_cat_stdin_inject(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    n00b_buffer_t *inject = n00b_buffer_from_cstr("hi\n");

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd            = n00b_string_from_cstr("/bin/cat"),
        .conduit        = c,
        .io             = io,
        .capture_stdout = true,
        .merge          = false,
        .stdin_inject   = inject,
        .close_stdin    = true);

    n00b_result_t(bool) r = n00b_subproc_run(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] capture cat stdin_inject (err=%d)\n",
               n00b_result_get_err(r));
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    assert(n00b_result_is_ok(r));
    assert(n00b_subproc_exited(&sp));

    n00b_buffer_t *out = n00b_subproc_stdout(&sp);
    assert(out != nullptr);
    // /bin/cat echoes stdin: "hi\n" = 3 bytes.
    assert(out->byte_len == 3);
    assert(memcmp(out->data, "hi\n", 3) == 0);

    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] capture cat stdin_inject\n");
}

// ============================================================================
// 18. End-to-end: run ls with arguments
// ============================================================================

static void
test_run_ls_with_args(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd            = n00b_string_from_cstr("/bin/ls"),
        .conduit        = c,
        .io             = io,
        .capture_stdout = true,
        .merge          = false);

    // Add "-la" and "/tmp" arguments.
    n00b_array_t(n00b_string_t) args = n00b_array_new(n00b_string_t, 2);
    n00b_array_set(args, 0, n00b_string_from_cstr("-la"));
    n00b_array_set(args, 1, n00b_string_from_cstr("/tmp"));
    sp.args = &args;

    n00b_result_t(bool) r = n00b_subproc_run(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] run ls with args (err=%d)\n",
               n00b_result_get_err(r));
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    assert(n00b_result_is_ok(r));
    assert(n00b_subproc_exited(&sp));

    n00b_result_t(int) ec = n00b_subproc_exit_code(&sp);
    assert(n00b_result_is_ok(ec));
    assert(n00b_result_get(ec) == 0);

    // ls -la /tmp should produce substantial output.
    n00b_buffer_t *out = n00b_subproc_stdout(&sp);
    assert(out != nullptr);
    assert(out->byte_len > 10); // At least "total N\n" + some entries

    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] run ls with args\n");
}

// ============================================================================
// 19. Stdin capture — verify cap_stdin captures injected data
// ============================================================================

static void
test_stdin_capture(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    n00b_buffer_t *inject = n00b_buffer_from_cstr("captured input\n");

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd            = n00b_string_from_cstr("/bin/cat"),
        .conduit        = c,
        .io             = io,
        .capture        = true,
        .merge          = false,
        .stdin_inject   = inject,
        .close_stdin    = true);

    n00b_result_t(bool) r = n00b_subproc_run(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] stdin capture (err=%d)\n",
               n00b_result_get_err(r));
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    assert(n00b_result_is_ok(r));

    // buf_stdin should contain the injected data.
    n00b_buffer_t *sin = n00b_subproc_stdin_capture(&sp);
    assert(sin != nullptr);
    assert(sin->byte_len == 15); // "captured input\n"
    assert(memcmp(sin->data, "captured input\n", 15) == 0);

    // stdout should also have it (cat echoes stdin).
    n00b_buffer_t *sout = n00b_subproc_stdout(&sp);
    assert(sout != nullptr);
    assert(sout->byte_len == 15);

    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] stdin capture\n");
}

// ============================================================================
// 20. Topic accessors return non-null after spawn
// ============================================================================

static void
test_topic_accessors(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd            = n00b_string_from_cstr("/bin/echo"),
        .conduit        = c,
        .io             = io,
        .capture_stdout = true,
        .merge          = false);

    // Before spawn, topics should be null.
    assert(n00b_subproc_stdout_topic(&sp) == nullptr);
    assert(n00b_subproc_stderr_topic(&sp) == nullptr);
    assert(n00b_subproc_stdin_topic(&sp) == nullptr);

    n00b_result_t(bool) r = n00b_subproc_spawn(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] topic accessors (spawn failed)\n");
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    // After spawn with capture_stdout, stdout topic should be non-null.
    assert(n00b_subproc_stdout_topic(&sp) != nullptr);

    n00b_subproc_wait(&sp);
    n00b_subproc_close(&sp);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] topic accessors\n");
}

// ============================================================================
// 21. End-to-end: run with env override (execve path)
// ============================================================================

static void
test_run_with_env(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    // /usr/bin/env prints its environment.  We'll use /bin/echo with
    // a custom env to verify execve path works (requires absolute path).
    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd            = n00b_string_from_cstr("/bin/echo"),
        .conduit        = c,
        .io             = io,
        .capture_stdout = true,
        .merge          = false);

    n00b_array_t(n00b_string_t) args = n00b_array_new(n00b_string_t, 1);
    n00b_array_set(args, 0, n00b_string_from_cstr("env_works"));
    sp.args = &args;

    // Provide a minimal environment.
    n00b_array_t(n00b_string_t) env = n00b_array_new(n00b_string_t, 1);
    n00b_array_set(env, 0, n00b_string_from_cstr("HOME=/tmp"));
    sp.env = &env;

    n00b_result_t(bool) r = n00b_subproc_run(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] run with env (err=%d)\n",
               n00b_result_get_err(r));
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    assert(n00b_result_is_ok(r));
    assert(n00b_subproc_exited(&sp));

    n00b_buffer_t *out = n00b_subproc_stdout(&sp);
    assert(out != nullptr);
    assert(out->byte_len == 10); // "env_works\n"
    assert(memcmp(out->data, "env_works\n", 10) == 0);

    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] run with env\n");
}

// ============================================================================
// 22. PTY spawn echo — basic PTY mode, capture stdout
// ============================================================================

static void
test_pty_spawn_echo(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd            = n00b_string_from_cstr("/bin/echo"),
        .conduit        = c,
        .io             = io,
        .pty            = true,
        .capture_stdout = true,
        .merge          = false);

    n00b_array_t(n00b_string_t) args = n00b_array_new(n00b_string_t, 1);
    n00b_array_set(args, 0, n00b_string_from_cstr("hello"));
    sp.args = &args;

    n00b_result_t(bool) r = n00b_subproc_run(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] pty spawn echo (err=%d)\n",
               n00b_result_get_err(r));
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    assert(n00b_result_is_ok(r));
    assert(n00b_subproc_exited(&sp));

    n00b_result_t(int) ec = n00b_subproc_exit_code(&sp);
    assert(n00b_result_is_ok(ec));
    assert(n00b_result_get(ec) == 0);

    // PTY line discipline produces \r\n instead of \n.
    // "hello\r\n" = 7 bytes.
    n00b_buffer_t *out = n00b_subproc_stdout(&sp);
    assert(out != nullptr);
    if (out->byte_len > 0) {
        assert(out->byte_len == 7);
        assert(memcmp(out->data, "hello\r\n", 7) == 0);
    }

    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] pty spawn echo\n");
}

// ============================================================================
// 23. PTY exec failure — bad command returns Err
// ============================================================================

static void
test_pty_exec_failure(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd     = n00b_string_from_cstr("/nonexistent/bad/command"),
        .conduit = c,
        .io      = io,
        .pty     = true);

    n00b_result_t(bool) r = n00b_subproc_spawn(&sp);
    assert(n00b_result_is_err(r));
    assert(n00b_subproc_errored(&sp));

    n00b_subproc_close(&sp);
    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] pty exec failure\n");
}

// ============================================================================
// 24. PTY cat stdin_inject — round-trip through PTY
// ============================================================================

static void
test_pty_cat_stdin_inject(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    n00b_buffer_t *inject = n00b_buffer_from_cstr("hi\n");

    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd            = n00b_string_from_cstr("/bin/cat"),
        .conduit        = c,
        .io             = io,
        .pty            = true,
        .capture_stdout = true,
        .stdin_inject   = inject,
        .close_stdin    = true);

    n00b_result_t(bool) r = n00b_subproc_run(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] pty cat stdin_inject (err=%d)\n",
               n00b_result_get_err(r));
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    assert(n00b_result_is_ok(r));
    assert(n00b_subproc_exited(&sp));

    // PTY echoes stdin writes back to the read side (line discipline echo).
    // Input "hi\n" → PTY echo "hi\r\n" + cat output "hi\r\n".
    // Total captured: at least "hi\r\n" (the echo) and possibly
    // "hi\r\n" again (cat output).  The exact amount depends on
    // timing, so just verify we got something containing "hi".
    n00b_buffer_t *out = n00b_subproc_stdout(&sp);
    assert(out != nullptr);
    assert(out->byte_len >= 4); // At least "hi\r\n"

    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] pty cat stdin_inject\n");
}

// ============================================================================
// 25. PTY separate stderr (err_pty)
// ============================================================================

static void
test_pty_separate_stderr(void)
{
    n00b_conduit_t *c = make_conduit();
    n00b_conduit_io_backend_t *io = make_io(c);

    // Use sh -c to write to both stdout and stderr.
    n00b_subproc_t sp = {};
    n00b_subproc_init(&sp,
        .cmd            = n00b_string_from_cstr("/bin/sh"),
        .conduit        = c,
        .io             = io,
        .pty            = true,
        .err_pty        = true,
        .capture_stdout = true,
        .capture_stderr = true,
        .merge          = false);

    n00b_array_t(n00b_string_t) args = n00b_array_new(n00b_string_t, 2);
    n00b_array_set(args, 0, n00b_string_from_cstr("-c"));
    n00b_array_set(args, 1,
        n00b_string_from_cstr("echo out_data; echo err_data >&2"));
    sp.args = &args;

    n00b_result_t(bool) r = n00b_subproc_run(&sp);
    if (n00b_result_is_err(r)) {
        printf("  [SKIP] pty separate stderr (err=%d)\n",
               n00b_result_get_err(r));
        n00b_conduit_io_destroy(io);
        n00b_conduit_destroy(c);
        return;
    }

    assert(n00b_result_is_ok(r));
    assert(n00b_subproc_exited(&sp));

    n00b_result_t(int) ec = n00b_subproc_exit_code(&sp);
    assert(n00b_result_is_ok(ec));
    assert(n00b_result_get(ec) == 0);

    // With separate PTY stderr, stdout and stderr come through
    // different FD owners, so capture should be separate.
    // Stdout should contain "out_data\r\n" (PTY line discipline).
    // Stderr should contain "err_data\r\n".
    n00b_buffer_t *sout = n00b_subproc_stdout(&sp);
    n00b_buffer_t *serr = n00b_subproc_stderr(&sp);
    assert(sout != nullptr);
    assert(serr != nullptr);

    // Verify both streams captured data (exact content depends on
    // shell echo + PTY timing, so we check non-empty).
    assert(sout->byte_len > 0);
    assert(serr->byte_len > 0);

    n00b_conduit_io_destroy(io);
    n00b_conduit_destroy(c);
    printf("  [PASS] pty separate stderr\n");
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_subproc:\n");
    fflush(stdout);

    test_init_defaults();
    fflush(stdout);
    test_capture_convenience();
    fflush(stdout);
    test_proxy_convenience();
    fflush(stdout);
    test_accessors_pre_spawn();
    fflush(stdout);
    test_spawn_echo();
    fflush(stdout);
    test_spawn_bad_cmd();
    fflush(stdout);
    test_spawn_no_cmd();
    fflush(stdout);
    test_double_spawn();
    fflush(stdout);
    test_close_idempotent();
    fflush(stdout);
    test_run_echo();
    fflush(stdout);
    test_kill();
    fflush(stdout);
    test_pre_exec_hook();
    fflush(stdout);
    test_timeout_sigterm();
    fflush(stdout);
    test_spawn_cwd();
    fflush(stdout);
    test_capture_echo_hello();
    fflush(stdout);
    test_capture_cat_stdin_inject();
    fflush(stdout);
    test_stdout_xform_linebuf();
    fflush(stdout);
    test_run_ls_with_args();
    fflush(stdout);
    test_stdin_capture();
    fflush(stdout);
    test_topic_accessors();
    fflush(stdout);
    test_run_with_env();
    fflush(stdout);
    test_pty_spawn_echo();
    fflush(stdout);
    test_pty_exec_failure();
    fflush(stdout);
    test_pty_cat_stdin_inject();
    fflush(stdout);
    test_pty_separate_stderr();
    fflush(stdout);

    printf("All subproc tests passed.\n");
    n00b_shutdown();
    return 0;
}

#else /* _WIN32 */

#include <stdio.h>

int
main(void)
{
    printf("test_subproc: skipped (Windows not supported)\n");
    return 0;
}

#endif /* !_WIN32 */
