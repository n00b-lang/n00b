/*
 * naudit/preprocess.c — C preprocessor pre-pass.
 *
 * Spawns `ncc -E -x c <cpp_args...> <file_path>` and captures
 * stdout. Used by the engine to expand `n00b_alloc(T)` /
 * `typehash(T)` / `n00b_alloc_array_with_opts(T, n, opts)` and
 * similar type-as-macro-argument constructs that aren't valid raw
 * C — the grammar can't parse them without preprocessor expansion.
 *
 * We use ncc itself as the preprocessor because it natively
 * understands the n00b/ncc C dialect; ncc handles the version /
 * flag dance internally.
 *
 * We pass the file path directly rather than piping stdin: a
 * stdin/stdout pipe pair deadlocks (child fills stdout buffer
 * before consuming all stdin). And we redirect child stdin to
 * /dev/null because ncc otherwise inherits an interactive TTY and
 * spins at 100% CPU probing it.
 *
 * All allocation goes through n00b_alloc / n00b_buffer_t — no
 * malloc family per § 2.3.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/string.h"
#include "conduit/print.h"
#include "text/strings/string_ops.h"
#include "text/strings/format.h"

#include "naudit/preprocess.h"
#include "naudit/errors.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* Split @p cpp_args on whitespace into a fresh argv-style array.
 * Returns the count (without the trailing nullptr). The returned
 * arrays are GC-managed via n00b_alloc_array, no manual free.
 * Token bytes are copied out of @p cpp_args into a fresh owning
 * buffer so the in-place NUL-termination doesn't mutate the
 * caller's string. */
static int
split_cpp_args(n00b_string_t *cpp_args, char ***out_argv)
{
    *out_argv = nullptr;
    if (!cpp_args || cpp_args->u8_bytes == 0) {
        return 0;
    }

    int64_t n   = cpp_args->u8_bytes;
    char   *buf = (char *)n00b_alloc_array(uint8_t, n + 1);
    memcpy(buf, cpp_args->data, (size_t)n);
    buf[n] = 0;

    /* Count tokens. */
    int  count = 0;
    bool in    = false;
    for (int64_t i = 0; i <= n; i++) {
        char c   = buf[i];
        bool sep = (c == ' ' || c == '\t' || c == '\n' || c == 0);
        if (!sep && !in) {
            in = true;
            count++;
        }
        else if (sep && in) {
            in = false;
        }
    }
    if (count == 0) {
        return 0;
    }

    char **argv = (char **)n00b_alloc_array(uintptr_t,
                                            (int64_t)(count + 1));
    int  idx = 0;
    in       = false;
    for (int64_t i = 0; i <= n; i++) {
        char c   = buf[i];
        bool sep = (c == ' ' || c == '\t' || c == '\n' || c == 0);
        if (!sep && !in) {
            in          = true;
            argv[idx++] = buf + i;
        }
        else if (sep && in) {
            in     = false;
            buf[i] = 0;
        }
    }
    argv[idx] = nullptr;

    *out_argv = argv;
    return count;
}

/* Drain @p fd into a growable n00b_buffer_t until EOF. Returns the
 * resulting buffer or nullptr on read error. Uses fixed-size
 * stack chunks + n00b_buffer_concat to grow; no malloc family. */
static n00b_buffer_t *
drain_fd(int fd)
{
    n00b_buffer_t *acc = n00b_buffer_empty();
    uint8_t        chunk[8192];

    for (;;) {
        ssize_t r = read(fd, chunk, sizeof(chunk));
        if (r < 0) {
            if (errno == EINTR) continue;
            return nullptr;
        }
        if (r == 0) break;
        n00b_buffer_t *piece = n00b_buffer_from_bytes((char *)chunk,
                                                     (int64_t)r);
        n00b_buffer_concat(acc, piece);
    }

    return acc;
}

n00b_result_t(n00b_buffer_t *)
n00b_audit_preprocess_c(n00b_string_t *file_path, n00b_string_t *cpp_args)
{
    if (!file_path || file_path->u8_bytes == 0) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_AUDIT_ERR_ENGINE_TARGET_NOT_FOUND);
    }

    int out_pipe[2] = {-1, -1};
    if (pipe(out_pipe) < 0) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_AUDIT_ERR_SIGN_SUBPROCESS);
    }

    char **extra_argv = nullptr;
    int    n_extra    = split_cpp_args(cpp_args, &extra_argv);

    /* Fixed prefix: ncc -E -P -x c (5 entries), then extra args,
     * then the file path, then nullptr terminator. */
    int    fixed = 5;
    int    argc  = fixed + n_extra + 1;
    char **argv  = (char **)n00b_alloc_array(uintptr_t,
                                             (int64_t)(argc + 1));
    argv[0] = (char *)"ncc";
    argv[1] = (char *)"-E";
    argv[2] = (char *)"-P";   /* suppress line markers (`# N "file"`) —
                                 n00b's c_tokenizer doesn't have ncc's
                                 handle_line_marker yet, so the
                                 markers cause parse failures. */
    argv[3] = (char *)"-x";
    argv[4] = (char *)"c";
    for (int i = 0; i < n_extra; i++) {
        argv[fixed + i] = extra_argv[i];
    }
    argv[fixed + n_extra]     = (char *)file_path->data;
    argv[fixed + n_extra + 1] = nullptr;

    /* /dev/null for child stdin — ncc otherwise inherits an
     * interactive TTY and spins at 100% CPU. */
    int dev_null_fd = open("/dev/null", O_RDONLY);
    if (dev_null_fd < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        return n00b_result_err(n00b_buffer_t *,
                               N00B_AUDIT_ERR_SIGN_SUBPROCESS);
    }

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(dev_null_fd);
        close(out_pipe[0]); close(out_pipe[1]);
        return n00b_result_err(n00b_buffer_t *,
                               N00B_AUDIT_ERR_SIGN_SUBPROCESS);
    }
    posix_spawn_file_actions_adddup2(&actions, dev_null_fd, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);

    /* libn00b raises RLIMIT_NOFILE to ~very high (sample showed
     * ncc spinning in its "close unintended child fds" loop —
     * 2575 of 2578 samples in close(2) — when invoked from naudit,
     * vs 2.7s standalone). POSIX_SPAWN_CLOEXEC_DEFAULT (macOS
     * extension) tells the kernel to close every fd not explicitly
     * dup2'd above, sidestepping ncc's userland loop entirely. */
    posix_spawnattr_t spawnattr;
    bool              attr_ok = (posix_spawnattr_init(&spawnattr) == 0);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    if (attr_ok) {
        posix_spawnattr_setflags(&spawnattr, POSIX_SPAWN_CLOEXEC_DEFAULT);
    }
#endif

    /* ncc's close_unintended_child_fds iterates the inherited
     * RLIMIT_NOFILE in userspace, calling close() on every fd
     * number 3..nofile. naudit (via libn00b runtime init) bumps
     * RLIMIT_NOFILE very high, so the loop dominates wall-clock
     * (sample showed 99% of CPU in close()). Drop the soft limit
     * to a reasonable value just for the child via setrlimit on
     * the parent before spawn — the child inherits, and we
     * restore the parent afterward. POSIX_SPAWN_CLOEXEC_DEFAULT
     * doesn't help: it closes the fds in the kernel, but ncc
     * still iterates the limit in userland. */
    struct rlimit prev_nofile = {0, 0};
    bool          rlimit_lowered = false;
    if (getrlimit(RLIMIT_NOFILE, &prev_nofile) == 0) {
        struct rlimit child_nofile = prev_nofile;
        if (child_nofile.rlim_cur > 1024) {
            child_nofile.rlim_cur = 1024;
            if (setrlimit(RLIMIT_NOFILE, &child_nofile) == 0) {
                rlimit_lowered = true;
            }
        }
    }

    pid_t pid      = 0;
    int   spawn_rc = posix_spawnp(&pid, "ncc", &actions,
                                  attr_ok ? &spawnattr : nullptr,
                                  argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (attr_ok) posix_spawnattr_destroy(&spawnattr);

    if (rlimit_lowered) {
        setrlimit(RLIMIT_NOFILE, &prev_nofile);
    }
    close(dev_null_fd);
    close(out_pipe[1]);

    if (spawn_rc != 0) {
        close(out_pipe[0]);
        return n00b_result_err(n00b_buffer_t *,
                               N00B_AUDIT_ERR_SIGN_SUBPROCESS);
    }

    n00b_buffer_t *out = drain_fd(out_pipe[0]);
    close(out_pipe[0]);

    int wstatus = 0;
    for (;;) {
        pid_t w = waitpid(pid, &wstatus, 0);
        if (w == pid) break;
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) {
            return n00b_result_err(n00b_buffer_t *,
                                   N00B_AUDIT_ERR_SIGN_SUBPROCESS);
        }
    }

    if (!out
        || !WIFEXITED(wstatus)
        || WEXITSTATUS(wstatus) != 0) {
        n00b_eprintf("n00b-audit: ncc -E failed (exit «#»); "
                     "preprocessor diagnostics above may explain the "
                     "issue. Pass project include paths via "
                     "--cpp-args (e.g. -I /path).",
                     (int64_t)(WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1));
        return n00b_result_err(n00b_buffer_t *,
                               N00B_AUDIT_ERR_ENGINE_PARSE);
    }

    return n00b_result_ok(n00b_buffer_t *, out);
}
