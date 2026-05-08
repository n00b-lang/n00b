/**
 * @file subproc.h
 * @brief High-level subprocess management built on conduit primitives.
 *
 * Provides configurable spawning, monitoring, and communication with
 * child processes.  Supports pipe mode on all platforms, PTY mode on POSIX
 * and Windows, I/O capture and
 * proxying via conduit pub/sub, transform pipeline injection, and
 * smart completion detection through shared done flags.
 *
 * @details
 * This is a **user-facing I/O abstraction**, not a conduit module.
 * It builds entirely on top of conduit primitives:
 *
 * | Concept              | Conduit building block                     |
 * |----------------------|--------------------------------------------|
 * | FD management        | `n00b_conduit_fd_owner_t`                  |
 * | Subscriptions        | Conduit topics + subscriptions + inboxes   |
 * | Capture buffers      | Inbox subscription + drain-on-read         |
 * | Transforms           | `n00b_conduit_xform_t` pipeline            |
 * | Exit detection       | `n00b_conduit_proc_topic()`                |
 * | Signal handling      | `n00b_conduit_signal_topic()`              |
 * | Completion           | Shared done flags plus platform events     |
 * | Event loop           | `n00b_conduit_service_t`                   |
 *
 * ## Usage modes
 *
 * - **Synchronous:** `n00b_subproc_run(sp)` = spawn + block + close.
 * - **Asynchronous:** `n00b_subproc_spawn(sp)` returns immediately;
 *   caller polls, subscribes, or calls `n00b_subproc_wait(sp)`.
 *
 * ## Data type rules
 *
 * The public API uses n00b types exclusively:
 * - `n00b_string_t` for text (command, arguments, environment)
 * - `n00b_buffer_t *` for byte data (I/O payloads, captures)
 * - `n00b_array_t(n00b_string_t *)` for argument/environment lists
 * - No `char *` crosses the public API boundary
 *
 */
#pragma once

#include "n00b.h"
#include "adt/result.h"
#include "adt/option.h"
#include "adt/array.h"
#include "core/buffer.h"
#include "core/string.h"
#include "conduit/conduit.h"
#include "conduit/fd_managed.h"
#include "conduit/fd_writer.h"
#include "conduit/io.h"
#include "conduit/xform_types.h"
#include "conduit/proc_lifecycle.h"
#include "conduit/signal.h"
#include "text/strings/string_ops.h"

#include <sys/types.h>
#include <signal.h>
#ifdef _WIN32
#include "core/platform.h"
struct termios {
    int _n00b_unused;
};
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};
#else
#include <termios.h>
#include <sys/ioctl.h>
#endif

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct n00b_subproc n00b_subproc_t;
#ifdef _WIN32
typedef struct n00b_subproc_win_state n00b_subproc_win_state_t;
#endif

// ============================================================================
// Flags
// ============================================================================

/**
 * @brief Subprocess configuration flags.
 *
 * Bitfield controlling capture, proxy, PTY, and merging behavior.
 * Set via convenience `_kargs` in the constructor or manually.
 */
typedef enum {
    N00B_SUBPROC_CAP_STDIN      = 1 << 0,  /**< Capture data written to stdin */
    N00B_SUBPROC_CAP_STDOUT     = 1 << 1,  /**< Capture child stdout */
    N00B_SUBPROC_CAP_STDERR     = 1 << 2,  /**< Capture child stderr */
    N00B_SUBPROC_CAP_ALL        = (1 << 0) | (1 << 1) | (1 << 2),

    N00B_SUBPROC_PROXY_STDIN    = 1 << 3,  /**< Relay parent stdin to child */
    N00B_SUBPROC_PROXY_STDOUT   = 1 << 4,  /**< Relay child stdout to parent */
    N00B_SUBPROC_PROXY_STDERR   = 1 << 5,  /**< Relay child stderr to parent */
    N00B_SUBPROC_PROXY_ALL      = (1 << 3) | (1 << 4) | (1 << 5),

    N00B_SUBPROC_CLOSE_STDIN    = 1 << 6,  /**< Close stdin after injection */
    N00B_SUBPROC_USE_PTY        = 1 << 7,  /**< Use PTY instead of pipes */
    N00B_SUBPROC_RAW_ARGV       = 1 << 8,  /**< Don't prepend cmd to argv */
    N00B_SUBPROC_MERGE_OUTPUT   = 1 << 9,  /**< Merge stderr into stdout */
    N00B_SUBPROC_PTY_STDERR     = 1 << 10, /**< Separate PTY for stderr */
    N00B_SUBPROC_HANDLE_WINSIZE = 1 << 11, /**< Proxy SIGWINCH to child PTY */
} n00b_subproc_flags_t;

// ============================================================================
// Completion condition enum
// ============================================================================

/**
 * @brief Completion condition for the subprocess wait loop.
 */
typedef enum {
    /** Wait for process exit AND all I/O fully flushed (default). */
    N00B_SUBPROC_DONE_IO_DRAINED,
    /** Wait only for process exit (fast, may miss trailing I/O). */
    N00B_SUBPROC_DONE_PROC_EXIT,
    /** Wait for stdout EOF only. */
    N00B_SUBPROC_DONE_STDOUT_EOF,
    /** User-supplied predicate. */
    N00B_SUBPROC_DONE_CUSTOM,
} n00b_subproc_done_t;

// ============================================================================
// Timeout policy enum
// ============================================================================

/**
 * @brief Action to take when timeout expires.
 */
typedef enum {
    N00B_SUBPROC_TIMEOUT_SIGTERM, /**< Send SIGTERM (default) */
    N00B_SUBPROC_TIMEOUT_SIGKILL, /**< Send SIGKILL */
    N00B_SUBPROC_TIMEOUT_DETACH,  /**< Return to caller, process keeps running */
} n00b_subproc_timeout_t;

// ============================================================================
// Callback types
// ============================================================================

/**
 * @brief Hook invoked immediately before the child process starts.
 *
 * On POSIX this runs in the child between fork() and exec(). On Windows
 * it runs in the parent immediately before CreateProcessA(), because
 * Windows has no fork/exec child phase.
 */
typedef void (*n00b_pre_exec_hook_t)(void *param);

/**
 * @brief Custom completion predicate for N00B_SUBPROC_DONE_CUSTOM.
 * @return true when the caller's completion condition is satisfied.
 */
typedef bool (*n00b_subproc_done_fn_t)(n00b_subproc_t *sp, void *ctx);

// ============================================================================
// Done-inbox completion flags
// ============================================================================

/** Process exited (SIGCHLD / waitpid). */
#define N00B_SUBPROC_DONE_F_PROC_EXIT    (1u << 0)
/** Stdout read side reached EOF. */
#define N00B_SUBPROC_DONE_F_STDOUT_EOF   (1u << 1)
/** Stderr read side reached EOF. */
#define N00B_SUBPROC_DONE_F_STDERR_EOF   (1u << 2)
/** All stdin writes completed. */
#define N00B_SUBPROC_DONE_F_STDIN_DONE   (1u << 3)
/** Stdout capture buffer fully drained. */
#define N00B_SUBPROC_DONE_F_STDOUT_DRAIN (1u << 4)
/** Stderr capture buffer fully drained. */
#define N00B_SUBPROC_DONE_F_STDERR_DRAIN (1u << 5)
/** SIGPIPE received. */
#define N00B_SUBPROC_DONE_F_SIGPIPE      (1u << 6)

// ============================================================================
// Type declarations needed by the struct
// ============================================================================


/**
 * @brief Buffer inbox type — used for user-supplied I/O subscriptions.
 *
 * Users create these inboxes (initialized against the same conduit),
 * pass them as `stdout_subs` / `stderr_subs` / `stdin_subs` at init
 * time, and they get subscribed to the appropriate I/O topic before
 * the gate opens.
 */
typedef n00b_conduit_inbox_t(n00b_buffer_t *) n00b_subproc_buf_inbox_t;


// ============================================================================
// Subprocess struct
// ============================================================================

/**
 * @brief Subprocess instance.
 *
 * Manages the full lifecycle of a child process: configuration,
 * spawning, I/O routing, completion detection, and teardown.
 *
 * Fields above the "Runtime" separator are set during construction
 * and are immutable after `n00b_subproc_spawn()`.
 */
struct n00b_subproc {
    // -- Configuration (set before spawn, immutable after) --

    n00b_string_t                 *cmd;           /**< Command to execute */
    n00b_array_t(n00b_string_t *)  *args;          /**< Argument list (null = none) */
    n00b_array_t(n00b_string_t *)  *env;           /**< Environment (null = inherit) */
    n00b_buffer_t                 *stdin_inject;   /**< Initial stdin data (null = none) */
    n00b_string_t                 *cwd;           /**< Working directory (null = inherit) */
    struct termios                *termcap;        /**< PTY terminal settings (null = inherit) */
    n00b_pre_exec_hook_t           pre_exec_hook; /**< Pre-start callback */
    void                          *hook_param;    /**< Hook user data */
    n00b_duration_t               *timeout;       /**< Timeout (null = no limit) */
    n00b_subproc_done_t            done_condition; /**< Completion condition */
    n00b_subproc_timeout_t         timeout_policy; /**< Timeout action */
    n00b_subproc_done_fn_t         done_fn;       /**< Custom done predicate */
    void                          *done_fn_ctx;   /**< Context for done_fn */
    uint32_t                       flags;         /**< n00b_subproc_flags_t bitmask */

    // -- Transform chains (buffer->buffer at each injection point) --

    n00b_array_t(void *)          *stdout_xforms; /**< Stdout transform specs */
    n00b_array_t(void *)          *stderr_xforms; /**< Stderr transform specs */
    n00b_array_t(void *)          *proxy_xforms;  /**< Proxy transform specs */
    n00b_array_t(void *)          *stdin_xforms;  /**< Stdin transform specs */

    // -- Custom subscriptions (user inboxes wired before gate opens) --

    n00b_array_t(n00b_subproc_buf_inbox_t *) *stdout_subs;  /**< Extra stdout inboxes */
    n00b_array_t(n00b_subproc_buf_inbox_t *) *stderr_subs;  /**< Extra stderr inboxes */
    n00b_array_t(n00b_subproc_buf_inbox_t *) *stdin_subs;   /**< Extra stdin inboxes */

    // -- Runtime (conduit wiring) --

    n00b_conduit_t                *conduit;
    n00b_conduit_io_backend_t     *io;

    n00b_option_t(n00b_conduit_fd_owner_t *) stdin_owner;
    n00b_option_t(n00b_conduit_fd_owner_t *) stdout_owner;
    n00b_option_t(n00b_conduit_fd_owner_t *) stderr_owner; /**< May == stdout_owner in PTY */

    n00b_conduit_inbox_t(n00b_buffer_t *)  *cap_stdout;    /**< Stdout capture inbox */
    n00b_conduit_inbox_t(n00b_buffer_t *)  *cap_stderr;   /**< Stderr capture inbox */
    n00b_conduit_inbox_t(n00b_buffer_t *)  *cap_stdin;    /**< Stdin capture inbox (via obs topic) */
    n00b_conduit_sub_handle_t               cap_stdout_sub; /**< Stdout capture sub handle */
    n00b_conduit_sub_handle_t               cap_stderr_sub; /**< Stderr capture sub handle */
    n00b_conduit_sub_handle_t               cap_stdin_sub;  /**< Stdin capture sub handle */

    n00b_conduit_filter_t(n00b_buffer_t *) *proxy_stdout; /**< Stdout → parent fd_writer */
    n00b_conduit_filter_t(n00b_buffer_t *) *proxy_stderr; /**< Stderr → parent fd_writer */

    n00b_option_t(n00b_conduit_fd_owner_t *) parent_stdin_owner; /**< Managed parent STDIN (proxy) */
    n00b_conduit_filter_t(n00b_buffer_t *) *proxy_stdin; /**< Parent stdin → child fd_writer */

    // Internal stdin observation topic — captures data flowing into
    // the child's stdin.  cap_stdin and stdin_subs subscribe to this.
    n00b_conduit_topic_t(n00b_buffer_t *) *stdin_obs_topic;

    n00b_conduit_topic_base_t    *eff_stdout_topic;  /**< Post-xform stdout topic */
    n00b_conduit_topic_base_t    *eff_stderr_topic;  /**< Post-xform stderr topic */

    n00b_buffer_t                 *buf_stdin;     /**< Accumulated stdin capture */
    n00b_buffer_t                 *buf_stdout;    /**< Accumulated stdout capture */
    n00b_buffer_t                 *buf_stderr;    /**< Accumulated stderr capture */

    // -- Completion state --
    //
    // POSIX uses one inbox subscribed to the done_topics of each watched
    // resource (stdout read topic, stderr read topic, proc lifecycle
    // topic). Each done message carries the originating topic pointer as
    // payload. Windows updates the same done_flags and required_mask from
    // Win32 process waits and pipe EOF events.
    //
    // The proc_inbox is a separate typed inbox that receives the actual
    // proc exit event on POSIX (carrying exit_status).  When POSIX sees the
    // exit event, it extracts exit_status, then closes the proc topic, which
    // fires the proc done_topic and notifies the done_inbox.

    n00b_conduit_inbox_t(n00b_conduit_topic_base_t *) *done_inbox;
    n00b_conduit_sub_handle_t      done_stdout_sub;  /**< Sub to stdout read done_topic */
    n00b_conduit_sub_handle_t      done_stderr_sub;  /**< Sub to stderr read done_topic */
    n00b_conduit_sub_handle_t      done_proc_sub;    /**< Sub to proc done_topic */
    uint32_t                       done_flags;    /**< Bitmask of completed stages */
    uint32_t                       required_mask; /**< Mask for IO_DRAINED mode */

    // -- Process lifecycle --

    n00b_conduit_topic_base_t     *proc_topic;    /**< Proc lifecycle topic */
    n00b_conduit_proc_inbox_t     *proc_inbox;    /**< Inbox for proc exit events */
    n00b_conduit_sub_handle_t      proc_inbox_sub; /**< Sub to proc topic */
    n00b_option_t(pid_t)           pid;           /**< Child PID (none = not spawned) */
    n00b_option_t(int)             exit_status;   /**< Exit code (none = not exited) */
    n00b_option_t(int)             term_signal;   /**< Kill signal (none = not signaled) */
    int                            saved_errno;   /**< errno from exec failure */

    // -- Gate pipe --

    n00b_option_t(int)             gate;          /**< Parent side of gate pipe */

    // -- Terminal state (PTY mode) --

    struct winsize                 dimensions;
    struct termios                 initial_termcap;
    struct termios                 child_termcap;

    // -- Signal subscriptions --

    n00b_conduit_signal_inbox_t   *signal_inbox;   /**< Shared inbox for all signals */
    n00b_conduit_sub_handle_t      sigpipe_sub;
    n00b_conduit_sub_handle_t      sigwinch_sub;

    // -- Status --

    _Atomic(bool)                  spawned;
    _Atomic(bool)                  exited;
    bool                           closed;
    bool                           errored;
    bool                           timed_out;
    bool                           termcap_saved; /**< initial_termcap is valid */

#ifdef _WIN32
    n00b_subproc_win_state_t      *win; /**< Private Windows backend state */
#endif
};

// ============================================================================
// Constructor
// ============================================================================

/**
 * @brief Initialize a subprocess.
 *
 * Registered as `N00B_BI_CONSTRUCTOR` for `n00b_subproc_t`.
 * Users call `n00b_new(n00b_type_subproc(), ...)` or allocate and
 * call this directly.
 *
 * @param sp  Allocated (possibly zeroed) subproc struct.
 *
 * @kw cmd              Command to execute (`n00b_string_t`).
 * @kw conduit          Conduit instance.
 * @kw io               IO backend (null = conduit's default).
 * @kw args             Argument list (`n00b_array_t(n00b_string_t *) *`).
 * @kw env              Environment (`n00b_array_t(n00b_string_t *) *`).
 * @kw capture          Enable all capture flags.
 * @kw proxy            Enable all proxy flags.
 * @kw merge            Merge stderr into stdout (default: true).
 * @kw pty              Use PTY mode.
 * @kw raw_argv         Don't prepend cmd to argv.
 * @kw err_pty          Separate PTY for stderr.
 * @kw handle_win_size  Proxy SIGWINCH (default: true when pty).
 * @kw stdin_inject     Initial stdin data (`n00b_buffer_t *`).
 * @kw close_stdin      Close stdin after injection.
 * @kw cwd              Working directory (`n00b_string_t`).
 * @kw termcap          Terminal settings for PTY (`struct termios *`).
 * @kw timeout          Timeout duration (`n00b_duration_t *`).
 * @kw timeout_policy   Timeout action (default: SIGTERM).
 * @kw done_condition   Completion condition (default: IO_DRAINED).
 * @kw done_fn          Custom completion predicate.
 * @kw done_fn_ctx      Context for done_fn.
 * @kw pre_exec_hook    Pre-start callback.
 * @kw hook_param       Hook user data.
 * @kw stdout_xforms    Transform chain for stdout.
 * @kw stderr_xforms    Transform chain for stderr.
 * @kw proxy_xforms     Transform chain for proxy output.
 * @kw stdin_xforms     Transform chain for stdin.
 * @kw stdout_subs      Extra inboxes subscribed to post-xform stdout.
 * @kw stderr_subs      Extra inboxes subscribed to post-xform stderr.
 * @kw stdin_subs       Extra inboxes subscribed to child stdin writes.
 */
extern void n00b_subproc_init(n00b_subproc_t *sp)
    _kargs {
        n00b_string_t                             *cmd;
        n00b_conduit_t                            *conduit         = nullptr;
        n00b_conduit_io_backend_t                 *io              = nullptr;
        n00b_array_t(n00b_string_t *)              *args            = nullptr;
        n00b_array_t(n00b_string_t *)              *env             = nullptr;
        bool                                       capture         = false;
        bool                                       capture_stdin   = false;
        bool                                       capture_stdout  = false;
        bool                                       capture_stderr  = false;
        bool                                       proxy           = false;
        bool                                       proxy_stdin     = false;
        bool                                       proxy_stdout    = false;
        bool                                       proxy_stderr    = false;
        bool                                       merge           = true;
        bool                                       pty             = false;
        bool                                       raw_argv        = false;
        bool                                       err_pty         = false;
        bool                                       handle_win_size = true;
        n00b_buffer_t                             *stdin_inject    = nullptr;
        bool                                       close_stdin     = false;
        n00b_string_t                             *cwd;
        struct termios                            *termcap         = nullptr;
        n00b_duration_t                           *timeout         = nullptr;
        n00b_subproc_timeout_t                     timeout_policy  = N00B_SUBPROC_TIMEOUT_SIGTERM;
        n00b_subproc_done_t                        done_condition  = N00B_SUBPROC_DONE_IO_DRAINED;
        n00b_subproc_done_fn_t                     done_fn         = nullptr;
        void                                      *done_fn_ctx    = nullptr;
        n00b_pre_exec_hook_t                       pre_exec_hook   = nullptr;
        void                                      *hook_param     = nullptr;
        n00b_array_t(void *)                      *stdout_xforms  = nullptr;
        n00b_array_t(void *)                      *stderr_xforms  = nullptr;
        n00b_array_t(void *)                      *proxy_xforms   = nullptr;
        n00b_array_t(void *)                      *stdin_xforms   = nullptr;
        n00b_array_t(n00b_subproc_buf_inbox_t *)   *stdout_subs    = nullptr;
        n00b_array_t(n00b_subproc_buf_inbox_t *)   *stderr_subs    = nullptr;
        n00b_array_t(n00b_subproc_buf_inbox_t *)   *stdin_subs     = nullptr;
    };

// ============================================================================
// Lifecycle API
// ============================================================================

/**
 * @brief Spawn the subprocess (non-blocking).
 *
 * Sets up pipes/PTY, forks, configures child, registers managed FDs,
 * wires transforms, subscriptions, and completion state. Returns after the
 * child is ready to exec.
 *
 * @details
 * **PATH search behavior:** When `env` is nullptr (inherit parent
 * environment), the child uses `execvp()` which searches `PATH` for
 * the command.  When `env` is provided, the child uses `execve()`
 * which requires an absolute or relative path — no `PATH` search.
 * To use a custom environment with `PATH` search, set `env` and
 * include the desired `PATH` entry, then pass an absolute path as `cmd`.
 *
 * @pre  cmd is set, not yet spawned.
 * @post pid > 0, FD owners registered, gate released.
 *
 * @return Ok(true) on success, Err(errno) on failure.
 */
extern n00b_result_t(bool) n00b_subproc_spawn(n00b_subproc_t *sp);

/**
 * @brief Spawn and wait for completion (blocking).
 *
 * Calls `spawn()`, blocks until the completion
 * condition is met or timeout expires, applies timeout_policy if
 * needed, then calls `close()` (unless detached).
 *
 * @return Ok(true) if completed normally, Ok(false) on timeout+detach,
 *         Err(errno) on spawn failure.
 */
extern n00b_result_t(bool) n00b_subproc_run(n00b_subproc_t *sp);

/**
 * @brief Block until the completion condition is met.
 *
 * For use after `n00b_subproc_spawn()` in async workflows.
 *
 * @kw timeout  Override the init-time timeout (null = use init value).
 *
 * @return Ok(true) on completion, Ok(false) on timeout+detach,
 *         Err(errno) on error.
 */
extern n00b_result_t(bool) n00b_subproc_wait(n00b_subproc_t *sp)
    _kargs {
        n00b_duration_t *timeout = nullptr;
    };

/**
 * @brief Orderly shutdown.  Idempotent.
 *
 * Closes all FD owners, unsubscribes from all topics, unregisters
 * signal subscriptions, and unwatches pid from proc_lifecycle.
 */
extern void n00b_subproc_close(n00b_subproc_t *sp);

/**
 * @brief Write data to child's stdin.
 *
 * @param sp    Subprocess.
 * @param data  Buffer to write (contents are copied internally).
 * @return Ok(request_id) on success, Err(errno) on failure.
 */
extern n00b_result_t(uint64_t)
n00b_subproc_write_stdin(n00b_subproc_t *sp, n00b_buffer_t *data);

/**
 * @brief Send a signal to the child process.
 *
 * @kw signal  Signal number (default: SIGTERM).
 *
 * @return Ok(true) on success, Err(errno) on failure.
 */
extern n00b_result_t(bool) n00b_subproc_kill(n00b_subproc_t *sp)
    _kargs {
        int signal = SIGTERM;
    };

/**
 * @brief Proxy current terminal window size to child (PTY mode only).
 */
extern void n00b_subproc_proxy_winsize(n00b_subproc_t *sp);

/**
 * @brief Restore parent terminal to its pre-spawn state (PTY mode only).
 *
 * Called automatically by `n00b_subproc_close()`.  Can also be called
 * manually if the caller needs to restore terminal state earlier (e.g.,
 * before printing an error message after a child crash).  Idempotent.
 */
extern void n00b_subproc_restore_terminal(n00b_subproc_t *sp);

// ============================================================================
// Accessors
// ============================================================================

/**
 * @brief Get captured stdout data.
 * @return Buffer with captured data, or empty buffer if not capturing.
 */
extern n00b_buffer_t *n00b_subproc_stdout(n00b_subproc_t *sp);

/**
 * @brief Get captured stderr data.
 * @return Buffer with captured data, or empty buffer if not capturing.
 */
extern n00b_buffer_t *n00b_subproc_stderr(n00b_subproc_t *sp);

/**
 * @brief Get captured stdin data.
 * @return Buffer with captured data, or empty buffer if not capturing.
 */
extern n00b_buffer_t *n00b_subproc_stdin_capture(n00b_subproc_t *sp);

/**
 * @brief Get exit code.
 * @return Ok(exit_code) if the process exited normally,
 *         Err(ECHILD) if not exited or was signaled.
 */
extern n00b_result_t(int) n00b_subproc_exit_code(n00b_subproc_t *sp);

/**
 * @brief Get termination signal.
 * @return Ok(signal) if the process was killed by a signal,
 *         Err(ECHILD) if not signaled.
 */
extern n00b_result_t(int) n00b_subproc_term_signal(n00b_subproc_t *sp);

/** @brief True if the process has exited. */
static inline bool
n00b_subproc_exited(n00b_subproc_t *sp)
{
    return atomic_load(&sp->exited);
}

/** @brief True if a spawn or exec error occurred. */
static inline bool
n00b_subproc_errored(n00b_subproc_t *sp)
{
    return sp->errored;
}

/** @brief True if the process was killed due to timeout. */
static inline bool
n00b_subproc_timed_out(n00b_subproc_t *sp)
{
    return sp->timed_out;
}

/** @brief True if using PTY mode. */
static inline bool
n00b_subproc_using_pty(n00b_subproc_t *sp)
{
    return (sp->flags & N00B_SUBPROC_USE_PTY) != 0;
}

/** @brief Get the child process PID (none if not spawned). */
static inline n00b_option_t(pid_t)
n00b_subproc_pid(n00b_subproc_t *sp)
{
    return sp->pid;
}

/** @brief True if the process has been spawned. */
static inline bool
n00b_subproc_is_spawned(n00b_subproc_t *sp)
{
    return atomic_load(&sp->spawned);
}

// ============================================================================
// Topic accessors
// ============================================================================

/**
 * @brief Get the effective stdout output topic (post-xforms).
 *
 * Returns the topic that capture, proxy, and user subscriptions see.
 * Valid after spawn; nullptr before spawn or if stdout is not captured/proxied.
 *
 * @note Adding subscriptions after spawn may miss early data — prefer
 *       passing inboxes via `stdout_subs` at init time for race-free wiring.
 */
extern n00b_conduit_topic_t(n00b_buffer_t *) *
n00b_subproc_stdout_topic(n00b_subproc_t *sp);

/**
 * @brief Get the effective stderr output topic (post-xforms).
 *
 * Same semantics as `n00b_subproc_stdout_topic` but for stderr.
 * Returns nullptr when merge is active (stderr flows through stdout topic).
 */
extern n00b_conduit_topic_t(n00b_buffer_t *) *
n00b_subproc_stderr_topic(n00b_subproc_t *sp);

/**
 * @brief Get the child stdin write topic (raw FD layer).
 *
 * Returns the stdin fd_owner's read topic if stdin is managed, nullptr
 * otherwise.  For observing data written to child stdin, prefer using
 * `stdin_subs` at init time.
 */
extern n00b_conduit_topic_t(n00b_buffer_t *) *
n00b_subproc_stdin_topic(n00b_subproc_t *sp);
