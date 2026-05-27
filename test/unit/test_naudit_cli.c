/*
 * WP-001 Phase 4 regression test — CLI driver (terminal output).
 *
 * Exercises the library-shaped entry `n00b_audit_run_cli(argc, argv)`
 * directly (no subproc) against three cases:
 *
 *   1. fixture_null.c + guidance_phase4.bnf → ok-int 1, captured
 *      stdout contains the rule id `n00b.s2_1.null`, at least one
 *      captured line begins with `  | ` (the good-example prefix).
 *   2. fixture_nullptr.c + guidance_phase4.bnf → ok-int 0,
 *      captured stdout is empty.
 *   3. <fixture_dir>/does_not_exist.c → ok-int 2, captured stderr
 *      is non-empty.
 *
 * Capture mechanism: POSIX `pipe()` + `dup2()` around fd 1 (stdout)
 * and fd 2 (stderr). libn00b's fd_writer holds the integer fd
 * number and writes via the POSIX `write()` syscall, so post-dup2
 * writes land on our pipe read-end. The relaxed test-file
 * convention (NCC.md "NO LIBC ALLOWED" exemption for test sources)
 * makes libc `<unistd.h>` / `<assert.h>` / `<stdio.h>` /
 * `<string.h>` fair game for harness scaffolding.
 *
 * Bootstrap shape mirrors test/unit/test_audit_engine.c per the
 * relaxed test convention: main(argc, argv) + n00b_init_simple
 * first thing, fixture-path discovery via the
 * N00B_AUDIT_TEST_FIXTURE_DIR macro (no hardcoded paths in C
 * source — set by test/meson.build).
 *
 * Per project DECISIONS.md D-005, there is no `severity` field
 * anywhere in this test — no assertion mentions severity, no
 * field is checked on n00b_audit_violation_t for severity.
 *
 * The fixture .c files contain `NULL` and `nullptr` as PARSE-TARGET
 * input; the n00b-api-guidelines § 2.1 NULL→nullptr rule does NOT
 * apply to them.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"

#include "naudit/naudit.h"
#include "naudit/errors.h"

#ifndef N00B_AUDIT_TEST_FIXTURE_DIR
#error "N00B_AUDIT_TEST_FIXTURE_DIR must be set by the build (see test/meson.build)"
#endif

#define CAPTURE_BUF_SIZE (64 * 1024)

/* ------------------------------------------------------------ */
/* Fixture-path + argv builders.                                */
/* ------------------------------------------------------------ */

static n00b_string_t *
fixture_path(const char *fname)
{
    char buf[1024];
    int  n = snprintf(buf, sizeof(buf), "%s/%s",
                      N00B_AUDIT_TEST_FIXTURE_DIR, fname);
    assert(n > 0 && (size_t)n < sizeof(buf));
    return n00b_string_from_cstr(buf);
}

/* ------------------------------------------------------------ */
/* Stdout/stderr capture via POSIX pipe + dup2.                 */
/* ------------------------------------------------------------ */

typedef struct {
    int  saved_stdout;
    int  saved_stderr;
    int  stdout_pipe[2]; /* [0]=read, [1]=write */
    int  stderr_pipe[2];
} capture_t;

/*
 * Install pipe-backed redirects for fd 1 and fd 2. Each pipe's
 * write end is dup2'd onto the corresponding stdio fd, then closed
 * (the dup2 already gave it a new fd). The read end stays open in
 * the test process so we can read back the captured bytes after
 * the call.
 *
 * The original fd 1 / fd 2 are dup'd into `saved_*` so the test
 * can restore them after each case (otherwise the next case's
 * setup would clobber).
 */
static void
capture_begin(capture_t *cap)
{
    cap->saved_stdout = dup(1);
    cap->saved_stderr = dup(2);
    assert(cap->saved_stdout >= 0);
    assert(cap->saved_stderr >= 0);

    int rc = pipe(cap->stdout_pipe);
    assert(rc == 0);
    rc = pipe(cap->stderr_pipe);
    assert(rc == 0);

    /* Make the read ends non-blocking so capture_end's drain can
     * terminate cleanly even if there's no data. */
    int flags = fcntl(cap->stdout_pipe[0], F_GETFL, 0);
    assert(flags >= 0);
    fcntl(cap->stdout_pipe[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(cap->stderr_pipe[0], F_GETFL, 0);
    assert(flags >= 0);
    fcntl(cap->stderr_pipe[0], F_SETFL, flags | O_NONBLOCK);

    rc = dup2(cap->stdout_pipe[1], 1);
    assert(rc >= 0);
    rc = dup2(cap->stderr_pipe[1], 2);
    assert(rc >= 0);

    close(cap->stdout_pipe[1]);
    close(cap->stderr_pipe[1]);
}

/*
 * Read all available bytes from the capture pipes into the supplied
 * buffers (which must each be at least CAPTURE_BUF_SIZE bytes), then
 * restore the saved fd 1 / fd 2. The output buffers are
 * NUL-terminated.
 */
static void
capture_end(capture_t *cap, char *stdout_out, size_t stdout_cap,
            char *stderr_out, size_t stderr_cap)
{
    /* Restore original fds first so any subsequent test output is
     * visible to the parent meson runner. */
    dup2(cap->saved_stdout, 1);
    dup2(cap->saved_stderr, 2);
    close(cap->saved_stdout);
    close(cap->saved_stderr);

    /* Drain. */
    size_t off = 0;
    for (;;) {
        if (off + 1 >= stdout_cap) break;
        ssize_t n = read(cap->stdout_pipe[0], stdout_out + off,
                          stdout_cap - off - 1);
        if (n <= 0) break;
        off += (size_t)n;
    }
    stdout_out[off] = '\0';

    off = 0;
    for (;;) {
        if (off + 1 >= stderr_cap) break;
        ssize_t n = read(cap->stderr_pipe[0], stderr_out + off,
                          stderr_cap - off - 1);
        if (n <= 0) break;
        off += (size_t)n;
    }
    stderr_out[off] = '\0';

    close(cap->stdout_pipe[0]);
    close(cap->stderr_pipe[0]);
}

/* ------------------------------------------------------------ */
/* Test cases.                                                  */
/* ------------------------------------------------------------ */

static n00b_string_t **
build_argv4(n00b_string_t *prog, n00b_string_t *flag, n00b_string_t *val,
            n00b_string_t *positional, int *argc_out)
{
    n00b_string_t **argv = (n00b_string_t **)n00b_alloc_array(
        n00b_string_t *, 5);
    argv[0] = prog;
    argv[1] = flag;
    argv[2] = val;
    argv[3] = positional;
    argv[4] = nullptr;
    *argc_out = 4;
    return argv;
}

static void
test_fixture_null(void)
{
    int argc = 0;
    n00b_string_t **argv = build_argv4(
        n00b_string_from_cstr("n00b-audit"),
        n00b_string_from_cstr("--guidance"),
        fixture_path("guidance_phase4.bnf"),
        fixture_path("fixture_null.c"),
        &argc);

    capture_t cap;
    capture_begin(&cap);
    n00b_result_t(int) r = n00b_audit_run_cli(argc, argv);
    static char out[CAPTURE_BUF_SIZE];
    static char err[CAPTURE_BUF_SIZE];
    capture_end(&cap, out, sizeof(out), err, sizeof(err));

    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  fixture_null run_cli returned err code=%d\n",
                n00b_result_get_err(r));
        fprintf(stderr, "  captured stderr: %s\n", err);
    }
    assert(n00b_result_is_ok(r));
    int code = n00b_result_get(r);
    if (code != 1) {
        fprintf(stderr, "  expected exit-int 1 (violations), got %d\n", code);
        fprintf(stderr, "  captured stdout: %s\n", out);
        fprintf(stderr, "  captured stderr: %s\n", err);
    }
    assert(code == 1);

    /* Header line must carry the rule id. */
    assert(strstr(out, "n00b.s2_1.null") != nullptr);

    /* At least one captured line begins with `  | ` (the
     * good-example prefix). Search line-by-line — `strstr` for
     * `\n  | ` catches non-leading lines; check the leading line
     * separately for the case where the block prefix opens the
     * whole capture (it won't for terminal output, but be
     * defensive). */
    bool found_prefix = (strstr(out, "\n  | ") != nullptr)
                        || (strncmp(out, "  | ", 4) == 0);
    if (!found_prefix) {
        fprintf(stderr, "  expected `  | ` prefixed line in stdout; got:\n%s\n",
                out);
    }
    assert(found_prefix);

    printf("  [PASS] fixture_null (exit=1, stdout=%zu bytes)\n",
           strlen(out));
}

static void
test_fixture_nullptr(void)
{
    int argc = 0;
    n00b_string_t **argv = build_argv4(
        n00b_string_from_cstr("n00b-audit"),
        n00b_string_from_cstr("--guidance"),
        fixture_path("guidance_phase4.bnf"),
        fixture_path("fixture_nullptr.c"),
        &argc);

    capture_t cap;
    capture_begin(&cap);
    n00b_result_t(int) r = n00b_audit_run_cli(argc, argv);
    static char out[CAPTURE_BUF_SIZE];
    static char err[CAPTURE_BUF_SIZE];
    capture_end(&cap, out, sizeof(out), err, sizeof(err));

    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  fixture_nullptr run_cli returned err code=%d\n",
                n00b_result_get_err(r));
        fprintf(stderr, "  captured stderr: %s\n", err);
    }
    assert(n00b_result_is_ok(r));
    int code = n00b_result_get(r);
    if (code != 0) {
        fprintf(stderr, "  expected exit-int 0 (no violations), got %d\n",
                code);
        fprintf(stderr, "  captured stdout: %s\n", out);
        fprintf(stderr, "  captured stderr: %s\n", err);
    }
    assert(code == 0);
    assert(strlen(out) == 0);

    printf("  [PASS] fixture_nullptr (exit=0, stdout empty)\n");
}

static void
test_missing_target(void)
{
    int argc = 0;
    n00b_string_t **argv = build_argv4(
        n00b_string_from_cstr("n00b-audit"),
        n00b_string_from_cstr("--guidance"),
        fixture_path("guidance_phase4.bnf"),
        fixture_path("does_not_exist.c"),
        &argc);

    capture_t cap;
    capture_begin(&cap);
    n00b_result_t(int) r = n00b_audit_run_cli(argc, argv);
    static char out[CAPTURE_BUF_SIZE];
    static char err[CAPTURE_BUF_SIZE];
    capture_end(&cap, out, sizeof(out), err, sizeof(err));

    assert(n00b_result_is_ok(r));
    int code = n00b_result_get(r);
    if (code != 2) {
        fprintf(stderr,
                "  expected exit-int 2 (internal err), got %d\n", code);
        fprintf(stderr, "  captured stdout: %s\n", out);
        fprintf(stderr, "  captured stderr: %s\n", err);
    }
    assert(code == 2);
    assert(strlen(err) > 0);

    printf("  [PASS] missing target (exit=2, stderr present)\n");
}

/*
 * Regression: a guidance file whose `@violation_nt` references a
 * non-existent NT used to send the parser into a pathological
 * loop / SIGSEGV. The engine now validates each rule's
 * violation_nt resolves to an NT in the merged grammar and fails
 * fast with GUIDANCE_SCHEMA. The CLI surfaces this as exit-int 2.
 */
static void
test_dangling_violation_nt(void)
{
    int argc = 0;
    n00b_string_t **argv = build_argv4(
        n00b_string_from_cstr("n00b-audit"),
        n00b_string_from_cstr("--guidance"),
        fixture_path("guidance_ok.bnf"),
        fixture_path("fixture_null.c"),
        &argc);

    capture_t cap;
    capture_begin(&cap);
    n00b_result_t(int) r = n00b_audit_run_cli(argc, argv);
    static char out[CAPTURE_BUF_SIZE];
    static char err[CAPTURE_BUF_SIZE];
    capture_end(&cap, out, sizeof(out), err, sizeof(err));

    assert(n00b_result_is_ok(r));
    int code = n00b_result_get(r);
    if (code != 2) {
        fprintf(stderr,
                "  expected exit-int 2 (schema), got %d\n", code);
        fprintf(stderr, "  captured stdout: %s\n", out);
        fprintf(stderr, "  captured stderr: %s\n", err);
    }
    assert(code == 2);
    assert(strlen(err) > 0);

    printf("  [PASS] dangling violation_nt (exit=2, stderr present)\n");
}

static void
test_err_str_cli_codes(void)
{
    /* Round-trip every CLI_* code through err_str. */
    assert(n00b_audit_err_str(N00B_AUDIT_ERR_CLI_ARGS)     != nullptr);
    assert(n00b_audit_err_str(N00B_AUDIT_ERR_CLI_BAD_ARGS) != nullptr);
    assert(n00b_audit_err_str(N00B_AUDIT_ERR_CLI_RENDER)   != nullptr);
    printf("  [PASS] err_str CLI codes\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    test_err_str_cli_codes();
    test_fixture_null();
    test_fixture_nullptr();
    test_missing_target();
    test_dangling_violation_nt();

    printf("All n00b-audit Phase 4 CLI regression checks passed.\n");
    return 0;
}
