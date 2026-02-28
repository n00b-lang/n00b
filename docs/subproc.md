# Subprocess Management (`subproc`)

## Overview

The `subproc` module provides a high-level, configurable interface for
spawning, monitoring, and communicating with child processes. It is the
n00b equivalent of Python's `subprocess` module, but with first-class
PTY support, I/O capture and proxying via conduit pub/sub, transform
pipeline injection points, and smart completion detection.

**This is a user-facing I/O abstraction, not a conduit module.** It
lives under `include/conduit/subproc.h` / `src/conduit/subproc.c`, but builds
entirely on top of conduit primitives:

| Old n00b concept     | Conduit building block                         |
|----------------------|------------------------------------------------|
| `n00b_stream_t` FDs  | `n00b_conduit_fd_owner_t` (managed FD)         |
| Stream subscriptions | Conduit topics + subscriptions + inboxes       |
| Capture buffers      | `n00b_conduit_stream_reader_t` → `n00b_buffer_t *` |
| Observable pub/sub   | Conduit publisher + typed topic                |
| Filters/transforms   | `n00b_conduit_xform_t` pipeline                |
| Exit stream          | `n00b_conduit_proc_topic()` (proc_lifecycle)   |
| Signal handling      | `n00b_conduit_signal_topic()` (signal module)  |
| Condition variable   | Unified done-inbox (see R3)                    |
| Event loop           | `n00b_conduit_service_t` (service thread pool) |

**Data type rules.** The public API uses n00b types exclusively:
- `n00b_string_t` for text (command, arguments, environment entries)
- `n00b_buffer_t *` for byte data (I/O payloads, captures)
- `n00b_array_t(n00b_string_t)` for argument and environment lists
- No `char *` / `(char *, size_t)` pairs cross the public API boundary
- Conversion to `char **` (for `execve()`) happens at the last moment,
  in internal code only

**Object system.** `n00b_subproc_t` is a vtable-registered type.
Users create instances via `n00b_new()` or stack/static-allocate and
call `n00b_subproc_init()`. The init function is the `_kargs`-bearing
constructor; `n00b_new()` dispatches to it via the vtable's
`N00B_BI_CONSTRUCTOR` slot.

**Thread model.** Subproc does not create its own I/O threads. It
registers managed FDs with the conduit's service thread pool (or a
caller-provided IO backend). The system IO threads automatically adopt
the FDs.

---

## Requirements

### R1. Process Spawning

**R1.1 — Pipe-based spawning (no TTY).**
Create child process connected via anonymous pipes for stdin, stdout,
and stderr. Each pipe endpoint becomes a managed FD in the conduit
system. Only create pipes for streams that are captured or proxied;
leave others inherited from the parent.

**R1.2 — PTY-based spawning.**
Create child process connected via a pseudo-terminal. The PTY master
FD serves as both stdin and stdout (and optionally stderr). Required
for interactive programs (shells, REPLs, curses apps). Must support:
- `openpty()` for PTY allocation
- `setsid()` + `TIOCSCTTY` for session leader / controlling terminal
- Optional separate stderr PTY (`err_pty` flag) for programs that
  write to stderr independently

**R1.3 — Pre-exec hook and common pre-exec conveniences.**
User-supplied callback `void (*hook)(void *param)` invoked in the
child process between `fork()` and `exec()`. Enables custom setup
(e.g., `chroot`, `setuid`, `rlimit`, closing extra FDs, setting up
namespaces).

In addition, the most common pre-exec operations should be expressible
as keyword arguments without requiring a hook:
- **`stdin_inject`** — `n00b_buffer_t *` written to the child's stdin
  immediately after spawn. Common for feeding passwords, config data,
  or scripted input.
- **`close_stdin`** — Close the child's stdin after injection (or
  immediately if no injection). Many programs (e.g., compilers, text
  processors) wait for stdin EOF before producing output.
- **`cwd`** — `n00b_string_t` working directory for the child. Calls
  `chdir()` in the child before exec. (Low priority but ergonomic.)

**R1.4 — Environment control.**
Optionally supply a complete environment as
`n00b_array_t(n00b_string_t)`. Each entry is a `"KEY=VALUE"` string.
If not provided, inherit the parent's environment. Conversion to
`char **envp` happens internally at the `execve()` call site.

**R1.5 — Argv construction.**
Two modes:
- **Normal mode:** The module prepends the command name to the
  user-supplied argv array (like `execvp` convention).
- **Raw mode** (`raw_argv` flag): The argv array is used as-is,
  giving full control to the caller.

The user-facing type is `n00b_array_t(n00b_string_t)`. Conversion to
`char **argv` happens internally at the `execve()` call site.

**R1.6 — Gate synchronization.**
Parent and child synchronize via a gate pipe so that the parent can
finish setting up subscriptions and I/O routing before the child calls
`execve()`. The child blocks on a read from the gate; the parent
writes after setup is complete.

**R1.7 — FD safety.**
Ensure pipe/PTY FDs are >= 3 before forking (dup up from 0-2 range)
to avoid clobbering stdin/stdout/stderr. Clear `FD_CLOEXEC` on FDs
that the child needs to inherit.

### R2. I/O Routing

**R2.1 — Capture.**
Accumulate all data from a stream (stdin, stdout, stderr) into a
`n00b_buffer_t *`. Retrievable after process completion (see R3 for
when "complete" means) or during execution via snapshot. Three
independent capture flags:
- `capture_stdin` — capture data written to the child's stdin
- `capture_stdout` — capture child's stdout output
- `capture_stderr` — capture child's stderr output
- `capture` convenience — enables all three

**R2.2 — Proxy to parent.**
Relay child's output to the parent process's terminal. Three
independent proxy flags:
- `proxy_stdin` — relay parent's stdin to the child
- `proxy_stdout` — relay child's stdout to parent's stdout
- `proxy_stderr` — relay child's stderr to parent's stderr
- `proxy` convenience — enables all three

**R2.3 — Output merging.**
When `merge` is true (the default), stderr output is merged into
stdout's capture buffer and proxy stream, mimicking `2>&1`. Separate
stderr capture only occurs when `merge` is false.

**R2.4 — Custom output topics.**
Users can supply conduit topics that accept `n00b_buffer_t *` payloads
as additional output sinks for stdout and/or stderr. These receive all
data events alongside built-in capture and proxy subscribers. The
topics are wired as downstream subscribers of the subproc's internal
read topics (or the output of the user's transform chain, if one is
installed — see R2.5).

```c
// Example: tee stdout to a logging topic
n00b_conduit_topic_base_t *log_topic = ...;
auto sp = n00b_subproc_new(cmd, conduit,
    .capture = true,
    .stdout_topics = my_topic_array);  // n00b_array_t of topic ptrs
```

**R2.5 — Transform pipeline injection.**
Subproc exposes four injection points where the user can insert
`n00b_conduit_xform_t(n00b_buffer_t *, n00b_buffer_t *)` transform
chains. All transforms in this system are buffer-to-buffer — they
receive `n00b_buffer_t *` and produce `n00b_buffer_t *`.

| Injection point   | Where it sits in the pipeline               |
|-------------------|----------------------------------------------|
| `stdout_xforms`   | Between stdout FD read topic and subscribers |
| `stderr_xforms`   | Between stderr FD read topic and subscribers |
| `proxy_xforms`    | Between capture/merge and the proxy write    |
| `stdin_xforms`    | Between parent stdin and child stdin write   |

The user passes an array of transform specs (or pre-built xform
objects). Subproc wires them as a chain: FD read topic → xform[0] →
xform[1] → ... → xform[N] → {capture, proxy, custom topics}.

Built-in transforms usable here:
- `xform_linebuf` — line-buffer byte streams (emit complete lines)
- `xform_ansi_strip` — strip ANSI escape sequences
- `xform_render` — render n00b styled strings to ANSI bytes
- `xform_json` — parse JSON from byte stream
- `xform_hexdump` — hex dump byte stream

Example: strip ANSI from stdout before capture, but proxy raw:
```c
// stdout_xforms strips ANSI; proxy_xforms is empty (raw passthrough)
auto sp = n00b_subproc_new(cmd, conduit,
    .capture = true,
    .proxy   = true,
    .stdout_xforms = ansi_strip_chain,
    .proxy_xforms  = nullptr);
```

### R3. Process Lifecycle and Completion

**The core problem:** Process exit (SIGCHLD / waitpid) does not mean
I/O is complete. The child may have written data to pipes that hasn't
been read yet, or the read data may still be flowing through transform
chains, or capture buffers may not have received all bytes. Using
capture buffers before all data is flushed through causes truncation.

**R3.1 — Completion model: unified done-inbox.**
Instead of a raw condition variable, subproc uses a single conduit
inbox that waits for **done signals** from all relevant pipeline
stages. The done-inbox collects `done_topic` messages from:

1. **FD EOF events** — stdout/stderr read topics emit
   `N00B_CONDUIT_FD_ST_READ_EOF` when the pipe/PTY read side hits EOF.
2. **Transform flush** — each xform in the chain receives a `flush()`
   call when its upstream closes. After flushing, it closes its output
   topic, which propagates downstream.
3. **Capture buffer drain** — the stream reader accumulates all
   remaining data and signals completion.
4. **Process exit** — the `proc_lifecycle` topic fires the exit event
   (SIGCHLD, waitpid status).
5. **Signal events** — SIGPIPE delivery indicates broken pipe.

The done-inbox waits until a configurable **completion condition** is
met. The condition is a `_kargs` enum:

```c
typedef enum {
    N00B_SUBPROC_DONE_IO_DRAINED,  // Default: wait for process exit
                                    // AND all I/O fully flushed through
                                    // capture/proxy pipelines.
    N00B_SUBPROC_DONE_PROC_EXIT,   // Wait only for process exit (may
                                    // miss trailing I/O — fast path).
    N00B_SUBPROC_DONE_STDOUT_EOF,  // Wait for stdout EOF only (useful
                                    // for piped-output programs).
    N00B_SUBPROC_DONE_CUSTOM,      // User provides their own done
                                    // predicate function.
} n00b_subproc_done_t;
```

**`N00B_SUBPROC_DONE_IO_DRAINED` (default):** The smart completion
mode. Subproc tracks a set of "pending" topics and ticks them off as
done signals arrive. The set includes:
- proc_lifecycle exit event
- stdout EOF (or all stdout xforms flushed + capture drained)
- stderr EOF (or all stderr xforms flushed + capture drained)
- All pending writes to stdin completed

Only when all tracked items are done does the wait return. This
guarantees capture buffers are complete.

**`N00B_SUBPROC_DONE_PROC_EXIT`:** Lightweight mode. Returns as soon
as the process exits. Caller is responsible for draining I/O if
needed. Good for fire-and-forget or when the caller only cares about
the exit code.

**`N00B_SUBPROC_DONE_STDOUT_EOF`:** Returns when stdout closes. Useful
for programs where stdout closing is the natural end signal (e.g.,
`cat`, `grep`).

**`N00B_SUBPROC_DONE_CUSTOM`:** User supplies a
`bool (*done_fn)(n00b_subproc_t *sp, void *ctx)` predicate. Called
each time a done signal arrives. Returns true when the user's
condition is satisfied.

**R3.2 — Synchronous run.**
`n00b_subproc_run()` spawns the child and blocks on the done-inbox
until the completion condition is met (or timeout expires). Then calls
`close()` and returns.

**R3.3 — Asynchronous spawn.**
`n00b_subproc_spawn()` spawns the child and returns immediately. The
caller can:
- Poll the done-inbox manually
- Subscribe to the exit topic for notification
- Call `n00b_subproc_wait()` later to block

**R3.4 — Timeout and timeout policy.**
Optional timeout for synchronous runs. Uses `n00b_duration_t`. When
the timeout expires, the done-inbox wait returns and the **timeout
policy** is applied:

```c
typedef enum {
    N00B_SUBPROC_TIMEOUT_SIGTERM,  // Default: send SIGTERM
    N00B_SUBPROC_TIMEOUT_SIGKILL,  // Send SIGKILL (hard kill)
    N00B_SUBPROC_TIMEOUT_DETACH,   // Switch to async: return to caller,
                                    // process keeps running. Caller can
                                    // later call kill/wait/close.
} n00b_subproc_timeout_t;
```

The `N00B_SUBPROC_TIMEOUT_DETACH` option is critical for workflows
where a timeout means "check on it and decide" rather than "kill it".

**R3.5 — Signal delivery.**
Send signals to the child process cleanly:
```c
n00b_subproc_kill(sp, .signal = SIGTERM);
n00b_subproc_kill(sp, .signal = SIGKILL);
n00b_subproc_kill(sp);  // default: SIGTERM
```

**R3.6 — Signal monitoring.**
Subscribe to SIGCHLD and SIGPIPE via the conduit signal module to
detect child death and broken pipes. In PTY mode, also subscribe to
SIGWINCH to proxy terminal size changes. All signal handling goes
through conduit topics — no direct `signal()` / `sigaction()` calls
from subproc code.

**R3.7 — Process close.**
Orderly teardown: close all FD owners, unsubscribe all subscribers,
unregister signal subscriptions, and unwatch the process from
proc_lifecycle. Must be idempotent (safe to call multiple times).

### R4. Terminal Management (PTY mode)

**R4.1 — Termcap propagation.**
If the caller supplies `termcap` settings (`struct termios`), apply
them to the child's PTY. Otherwise, inherit the parent's terminal
settings (captured via `tcgetattr` at init time).

**R4.2 — Window size proxying.**
When `handle_win_size` is true (default in PTY mode), subscribe to
SIGWINCH via the conduit signal module and forward the parent's
current `winsize` to the child's PTY via `TIOCSWINSZ`.

**R4.3 — Signal reset in child.**
After fork, the child must reset all signal dispositions to `SIG_DFL`
to avoid inheriting the parent's handlers (which reference parent
address space).

**R4.4 — Unbuffered I/O.**
In PTY mode, unbuffer stdin/stdout/stderr on both parent and child
sides for responsive interactive I/O.

### R5. Error Handling

**R5.1 — Result types everywhere.**
All public API functions that can fail return `n00b_result_t`. No
bare-pointer-as-error returns. Key error conditions:
- No command set
- Fork failure
- PTY allocation failure
- Pipe creation failure
- Exec failure (child reports back via error pipe)

**R5.2 — Exec failure reporting.**
In PTY mode, the child uses an additional pipe to report exec
failures back to the parent *before* the parent finishes setup. The
parent reads the errno, reaps the failed child, and returns an error
result.

**R5.3 — State machine enforcement.**
Post-spawn mutations are forbidden. Attempting to set command, args,
or env after `spawn()` is a programming error (assert/abort, not
result error).

### R6. Accessor API

Read-only accessors for post-spawn state:

| Accessor                        | Returns                           |
|---------------------------------|-----------------------------------|
| `n00b_subproc_stdout()`         | `n00b_buffer_t *` (captured data) |
| `n00b_subproc_stderr()`         | `n00b_buffer_t *` (captured data) |
| `n00b_subproc_stdin_capture()`  | `n00b_buffer_t *` (captured data) |
| `n00b_subproc_exit_code()`      | `n00b_result_t(int)` (exit status)|
| `n00b_subproc_term_signal()`    | `n00b_result_t(int)` (signal)     |
| `n00b_subproc_errored()`        | `bool`                            |
| `n00b_subproc_exited()`         | `bool`                            |
| `n00b_subproc_timed_out()`      | `bool`                            |
| `n00b_subproc_using_pty()`      | `bool`                            |
| `n00b_subproc_pid()`            | `pid_t`                           |

All capture accessors return `n00b_buffer_t *` (never bare
`char *` / `size_t` pairs).

### R7. Non-Requirements (explicit exclusions)

- **Windows:** Not supported initially (`#ifndef _WIN32` guard).
- **Session management:** The old n00b `session_t` (state machines,
  triggers, capture logs, replay) is a separate higher-level module.
  Subproc focuses on process I/O, not session orchestration.

---

## API Design

### Data Types

```c
// ============================================================================
// Flags
// ============================================================================

typedef enum {
    N00B_SUBPROC_CAP_STDIN       = 1 << 0,
    N00B_SUBPROC_CAP_STDOUT      = 1 << 1,
    N00B_SUBPROC_CAP_STDERR      = 1 << 2,
    N00B_SUBPROC_CAP_ALL         = (1 << 0) | (1 << 1) | (1 << 2),

    N00B_SUBPROC_PROXY_STDIN     = 1 << 3,
    N00B_SUBPROC_PROXY_STDOUT    = 1 << 4,
    N00B_SUBPROC_PROXY_STDERR    = 1 << 5,
    N00B_SUBPROC_PROXY_ALL       = (1 << 3) | (1 << 4) | (1 << 5),

    N00B_SUBPROC_CLOSE_STDIN     = 1 << 6,
    N00B_SUBPROC_USE_PTY         = 1 << 7,
    N00B_SUBPROC_RAW_ARGV        = 1 << 8,
    N00B_SUBPROC_MERGE_OUTPUT    = 1 << 9,
    N00B_SUBPROC_PTY_STDERR      = 1 << 10,
    N00B_SUBPROC_HANDLE_WINSIZE  = 1 << 11,
} n00b_subproc_flags_t;

// ============================================================================
// Completion + timeout policy enums
// ============================================================================

typedef enum {
    N00B_SUBPROC_DONE_IO_DRAINED,  // Default: exit + all I/O flushed
    N00B_SUBPROC_DONE_PROC_EXIT,   // Exit only (fast, may truncate)
    N00B_SUBPROC_DONE_STDOUT_EOF,  // Stdout EOF
    N00B_SUBPROC_DONE_CUSTOM,      // User predicate
} n00b_subproc_done_t;

typedef enum {
    N00B_SUBPROC_TIMEOUT_SIGTERM,  // Default: SIGTERM on timeout
    N00B_SUBPROC_TIMEOUT_SIGKILL,  // SIGKILL on timeout
    N00B_SUBPROC_TIMEOUT_DETACH,   // Switch to async, return to caller
} n00b_subproc_timeout_t;

// ============================================================================
// Callback types
// ============================================================================

typedef void (*n00b_pre_exec_hook_t)(void *param);
typedef bool (*n00b_subproc_done_fn_t)(n00b_subproc_t *sp, void *ctx);

// ============================================================================
// Subprocess struct
// ============================================================================

typedef struct n00b_subproc {
    // -- Configuration (set before spawn, immutable after) --

    n00b_string_t                  cmd;
    n00b_array_t(n00b_string_t)   *args;            // null = no args
    n00b_array_t(n00b_string_t)   *env;             // null = inherit
    n00b_buffer_t                 *stdin_inject;     // null = no injection
    n00b_string_t                  cwd;              // null = inherit
    struct termios                *termcap;          // null = inherit parent
    n00b_pre_exec_hook_t           pre_exec_hook;
    void                          *hook_param;
    n00b_duration_t               *timeout;          // null = no limit
    n00b_subproc_done_t            done_condition;
    n00b_subproc_timeout_t         timeout_policy;
    n00b_subproc_done_fn_t         done_fn;          // for DONE_CUSTOM
    void                          *done_fn_ctx;
    uint32_t                       flags;

    // -- Transform chains (buffer→buffer xforms at each injection point) --

    // User-provided arrays of xform specs.  Each entry is a
    // n00b_conduit_xform_t(n00b_buffer_t *, n00b_buffer_t *) *.
    // null = no transforms (passthrough).
    n00b_array_t(void *)          *stdout_xforms;
    n00b_array_t(void *)          *stderr_xforms;
    n00b_array_t(void *)          *proxy_xforms;
    n00b_array_t(void *)          *stdin_xforms;

    // -- Custom output topics (buffer topics wired as extra sinks) --

    n00b_array_t(n00b_conduit_topic_base_t *) *stdout_topics;
    n00b_array_t(n00b_conduit_topic_base_t *) *stderr_topics;

    // -- Runtime (conduit wiring) --

    n00b_conduit_t                *conduit;
    n00b_conduit_io_backend_t     *io;

    n00b_conduit_fd_owner_t       *stdin_owner;
    n00b_conduit_fd_owner_t       *stdout_owner;
    n00b_conduit_fd_owner_t       *stderr_owner;    // may == stdout_owner

    // Capture stream readers (Layer 2)
    n00b_conduit_stream_reader_t  *cap_stdin;
    n00b_conduit_stream_reader_t  *cap_stdout;
    n00b_conduit_stream_reader_t  *cap_stderr;

    // Capture buffers (accumulated output)
    n00b_buffer_t                 *buf_stdin;
    n00b_buffer_t                 *buf_stdout;
    n00b_buffer_t                 *buf_stderr;

    // -- Done-inbox (unified completion) --

    // Single inbox that collects done signals from all pipeline stages.
    // The wait loop checks done_condition against accumulated state.
    void                          *done_inbox;       // typed inbox TBD
    uint32_t                       done_flags;       // bitmask of completed stages

    // -- Process lifecycle --

    n00b_conduit_topic_base_t     *exit_topic;
    pid_t                          pid;
    int                            exit_status;
    int                            term_signal;
    int                            saved_errno;

    // -- Gate pipe --

    int                            gate;

    // -- Terminal state (PTY mode) --

    struct winsize                 dimensions;
    struct termios                 initial_termcap;
    struct termios                 child_termcap;

    // -- Signal subscriptions --

    n00b_conduit_sub_handle_t      sigchld_sub;
    n00b_conduit_sub_handle_t      sigpipe_sub;
    n00b_conduit_sub_handle_t      sigwinch_sub;

    // -- Status --

    _Atomic(bool)                  spawned;
    _Atomic(bool)                  exited;
    bool                           errored;
    bool                           timed_out;
} n00b_subproc_t;
```

### Done-inbox completion flags

```c
// Bits in done_flags, checked against the done_condition.
#define N00B_SUBPROC_DONE_F_PROC_EXIT    (1 << 0)
#define N00B_SUBPROC_DONE_F_STDOUT_EOF   (1 << 1)
#define N00B_SUBPROC_DONE_F_STDERR_EOF   (1 << 2)
#define N00B_SUBPROC_DONE_F_STDIN_DONE   (1 << 3)
#define N00B_SUBPROC_DONE_F_STDOUT_DRAIN (1 << 4)  // capture buffer drained
#define N00B_SUBPROC_DONE_F_STDERR_DRAIN (1 << 5)  // capture buffer drained
#define N00B_SUBPROC_DONE_F_SIGPIPE      (1 << 6)

// The mask for DONE_IO_DRAINED: process exited AND all active I/O drained.
// Computed at spawn time based on which streams are active.
// e.g., if only stdout is captured:
//   required_mask = PROC_EXIT | STDOUT_EOF | STDOUT_DRAIN
```

### Constructor (vtable init)

```c
/**
 * @brief Initialize a subprocess.
 *
 * Registered as N00B_BI_CONSTRUCTOR for n00b_subproc_t.
 * Users call n00b_new(n00b_type_subproc(), ...) or allocate +
 * call this directly.
 *
 * @param sp  Allocated (possibly zeroed) subproc struct.
 *
 * @kw cmd              Command to execute.
 * @kw conduit          Conduit instance.
 * @kw io               IO backend (null = conduit's default).
 * @kw args             n00b_array_t(n00b_string_t) argument list.
 * @kw env              n00b_array_t(n00b_string_t) environment.
 * @kw capture          Enable all capture flags.
 * @kw proxy            Enable all proxy flags.
 * @kw merge            Merge stderr → stdout (default: true).
 * @kw pty              PTY mode.
 * @kw raw_argv         Don't prepend cmd to argv.
 * @kw err_pty          Separate PTY for stderr.
 * @kw handle_win_size  Proxy SIGWINCH (default: true when pty).
 * @kw stdin_inject     n00b_buffer_t * initial stdin data.
 * @kw close_stdin      Close stdin after injection.
 * @kw cwd              n00b_string_t working directory.
 * @kw termcap          Terminal settings for PTY.
 * @kw timeout          Timeout duration.
 * @kw timeout_policy   What to do on timeout (default: SIGTERM).
 * @kw done_condition   Completion condition (default: IO_DRAINED).
 * @kw done_fn          Custom completion predicate.
 * @kw done_fn_ctx      Context for done_fn.
 * @kw pre_exec_hook    Child pre-exec callback.
 * @kw hook_param       Hook user data.
 * @kw stdout_xforms    Transform chain for stdout.
 * @kw stderr_xforms    Transform chain for stderr.
 * @kw proxy_xforms     Transform chain for proxy output.
 * @kw stdin_xforms     Transform chain for stdin.
 * @kw stdout_topics    Extra buffer topics for stdout.
 * @kw stderr_topics    Extra buffer topics for stderr.
 * @kw run              Spawn + wait (default: true).
 * @kw spawn            Spawn only (mutually exclusive with run).
 */
void n00b_subproc_init(n00b_subproc_t *sp) _kargs { ... };
```

### Lifecycle API

```c
/**
 * Spawn the subprocess (non-blocking).
 *
 * Sets up pipes/PTY, forks, configures child, registers managed FDs,
 * wires transforms, subscriptions, and done-inbox. Returns after the
 * child is ready to exec.
 *
 * @pre  cmd is set, not yet spawned.
 * @post pid > 0, FD owners registered, gate released.
 */
n00b_result_t(bool) n00b_subproc_spawn(n00b_subproc_t *sp);

/**
 * Spawn and wait for completion (blocking).
 *
 * Calls spawn(), then blocks on the done-inbox until the completion
 * condition is met or timeout expires. Applies timeout_policy if
 * timeout fires. Calls close() before returning (unless detached).
 *
 * @return Ok(true) if completed normally, Ok(false) on timeout+detach,
 *         Err(errno) on spawn failure.
 */
n00b_result_t(bool) n00b_subproc_run(n00b_subproc_t *sp);

/**
 * Block on the done-inbox until completion condition is met.
 *
 * For use after n00b_subproc_spawn() in async workflows. Optionally
 * pass a timeout that overrides the one from init.
 */
n00b_result_t(bool) n00b_subproc_wait(n00b_subproc_t *sp)
    _kargs { n00b_duration_t *timeout = nullptr; };

/**
 * Orderly shutdown.  Idempotent.
 *
 * Closes all FD owners, unsubscribes from all topics, unregisters
 * signal subscriptions, unwatches pid from proc_lifecycle.
 */
void n00b_subproc_close(n00b_subproc_t *sp);

/**
 * Write data to child's stdin.
 *
 * @param data  Buffer to write (contents are copied internally).
 * @return Ok(request_id) on success, Err(errno) on failure.
 */
n00b_result_t(uint64_t)
n00b_subproc_write_stdin(n00b_subproc_t *sp, n00b_buffer_t *data);

/**
 * Send a signal to the child process.
 *
 * @kw signal  Signal number (default: SIGTERM).
 */
n00b_result_t(bool) n00b_subproc_kill(n00b_subproc_t *sp)
    _kargs { int signal = SIGTERM; };

/**
 * Proxy current terminal window size to child (PTY mode only).
 */
void n00b_subproc_proxy_winsize(n00b_subproc_t *sp);
```

### Accessors

```c
// Capture retrieval (returns empty buffer if not capturing).
// All return n00b_buffer_t *, never bare char * or (ptr, len).
n00b_buffer_t *n00b_subproc_stdout(n00b_subproc_t *sp);
n00b_buffer_t *n00b_subproc_stderr(n00b_subproc_t *sp);
n00b_buffer_t *n00b_subproc_stdin_capture(n00b_subproc_t *sp);

// Status
static inline bool  n00b_subproc_exited(n00b_subproc_t *sp);
static inline bool  n00b_subproc_errored(n00b_subproc_t *sp);
static inline bool  n00b_subproc_timed_out(n00b_subproc_t *sp);
static inline bool  n00b_subproc_using_pty(n00b_subproc_t *sp);
static inline pid_t n00b_subproc_pid(n00b_subproc_t *sp);

// Post-exit (result error if not applicable)
n00b_result_t(int) n00b_subproc_exit_code(n00b_subproc_t *sp);
n00b_result_t(int) n00b_subproc_term_signal(n00b_subproc_t *sp);
```

---

## Internal Architecture

### Transform pipeline wiring

When the user provides transform chains, subproc wires them inline
between the raw FD read topic and the downstream subscribers:

```
                    ┌─────────────────────────────────────────────┐
                    │            stdout pipeline                  │
                    │                                             │
 [stdout_owner]     │  xform[0]     xform[1]     xform[N]        │
  read_topic ──────►│ buf→buf ────► buf→buf ────► buf→buf ──────►├──► capture
                    │                                             │──► proxy_xforms → proxy write
                    │                                             │──► custom topics
                    └─────────────────────────────────────────────┘
```

All transforms are `n00b_conduit_xform_t(n00b_buffer_t *, n00b_buffer_t *)`.
Each xform subscribes to the upstream topic and publishes to its own
output topic. The final output topic fans out to capture, proxy, and
custom subscribers.

The proxy pipeline has its own separate injection point:
```
                     proxy_xforms (optional)
 capture output ──► [buf→buf] ──► [buf→buf] ──► parent stdout fd_owner write
```

This means you can strip ANSI for capture but leave it for the
terminal, or add formatting for the terminal but not for capture.

### Spawn flow (pipe mode)

```
n00b_subproc_spawn(sp)
├── validate configuration
├── create gate pipe
├── create stdin/stdout/stderr pipes (only for captured/proxied streams)
├── ensure FDs >= 3 (dup up from 0-2 range)
├── fork()
│
├── PARENT:
│   ├── close child-side pipe ends
│   ├── n00b_conduit_fd_manage() for each parent-side pipe end
│   ├── wire stdout_xforms chain (if any)
│   ├── wire stderr_xforms chain (if any)
│   ├── wire stdin_xforms chain (if any)
│   ├── set up capture stream readers → buf_stdout, buf_stderr, buf_stdin
│   ├── wire proxy subscriptions through proxy_xforms (if any)
│   ├── wire custom output topics
│   ├── compute done_flags required_mask from active streams
│   ├── subscribe done-inbox to all relevant done_topics
│   ├── n00b_conduit_proc_topic(conduit, child_pid) → subscribe to done-inbox
│   ├── subscribe to SIGCHLD, SIGPIPE via conduit signal module
│   ├── inject stdin data via n00b_subproc_write_stdin (if configured)
│   ├── close stdin owner (if close_stdin set)
│   ├── write gate → child unblocks and calls exec
│   └── return Ok(true)
│
└── CHILD:
    ├── close parent-side pipe ends
    ├── dup2 child-side pipes → 0, 1, 2
    ├── chdir(cwd) if set
    ├── run pre_exec_hook (if set)
    ├── wait on gate read (blocks until parent is ready)
    ├── convert args to char ** (internal)
    ├── convert env to char ** (internal, or use n00b_raw_envp())
    └── execve(cmd_cstr, argv_cstr, envp_cstr)
```

### Spawn flow (PTY mode)

```
n00b_subproc_spawn(sp)  [PTY]
├── validate configuration
├── subscribe SIGCHLD, SIGPIPE via conduit signal module
├── get parent terminal size
├── openpty() → (master_fd, replica_fd)
├── optional: openpty() for stderr aux PTY
├── create gate pipe
├── create error-report pipe (for exec failure)
├── fork()
│
├── PARENT:
│   ├── close replica_fd, error-pipe write end
│   ├── read error-pipe → if data, child exec failed → reap, return Err
│   ├── n00b_conduit_fd_manage() for master_fd (stdin+stdout)
│   ├── optional: n00b_conduit_fd_manage() for aux PTY master (stderr)
│   ├── wire transform chains (same as pipe mode)
│   ├── set up capture, proxy, custom subscribers
│   ├── wire done-inbox
│   ├── n00b_conduit_proc_topic(conduit, child_pid)
│   ├── proxy initial SIGWINCH → TIOCSWINSZ on master_fd
│   ├── subscribe SIGWINCH → proxy_winsize callback
│   ├── apply termcap (if provided)
│   ├── set master_fd non-blocking
│   ├── unbuffer parent stdin/stdout/stderr
│   ├── write gate → child calls exec
│   └── return Ok(true)
│
└── CHILD:
    ├── close master_fd, aux master, error-pipe read end
    ├── setsid() — become session leader
    ├── ioctl(replica_fd, TIOCSCTTY) — claim controlling terminal
    │   └── on failure: write errno to error-pipe, _exit(127)
    ├── dup2 replica_fd → 0, 1, 2 (or split with aux for stderr)
    ├── close replica_fd if > 2
    ├── unbuffer stdin/stdout/stderr
    ├── reset all signal dispositions to SIG_DFL
    ├── chdir(cwd) if set
    ├── run pre_exec_hook (if set)
    ├── wait on gate read
    ├── convert args/env to char ** (internal)
    └── execve(cmd_cstr, argv_cstr, envp_cstr)
```

### Conduit wiring diagram (full pipeline)

```
                 ┌──── stdin_inject (n00b_buffer_t *) ──┐
                 │                                       ▼
parent stdin ──► [stdin_xforms?] ──────────────────► [stdin_owner] ──► child stdin
                                                         │
                                                    (if capturing)
                                                         │
                                                   [stream_reader]
                                                         │
                                                    [buf_stdin]

child stdout ──► [stdout_owner] ──► [stdout_xforms?] ──┬──► [stream_reader → buf_stdout]
                  read_topic                            ├──► [proxy_xforms? → parent stdout write]
                                                        └──► [custom stdout_topics]

child stderr ──► [stderr_owner] ──► [stderr_xforms?] ──┬──► [stream_reader → buf_stderr]
                (or merged into      (if not merged)    ├──► [proxy_xforms? → parent stderr write]
                 stdout pipeline)                       └──► [custom stderr_topics]

child pid ──► [proc_lifecycle topic] ──┐
SIGCHLD ──► [signal topic] ──────────┤
SIGPIPE ──► [signal topic] ──────────┤
stdout EOF ──────────────────────────┤
stderr EOF ──────────────────────────┤  ┌─────────┐
buf_stdout drained ──────────────────┼─►│done-    │──► check done_condition
buf_stderr drained ──────────────────┤  │inbox    │    → if met: wake waiter
stdin writes complete ───────────────┘  └─────────┘
```

### Done-inbox wait loop (pseudocode)

```c
while (!done_condition_met(sp)) {
    auto msg = inbox_pop_timed(sp->done_inbox, remaining_timeout);
    if (!msg) {
        // Timeout expired
        apply_timeout_policy(sp);
        return;
    }
    update_done_flags(sp, msg);
}
// All done — capture buffers are complete, safe to read.
```

Where `done_condition_met` checks:
- `IO_DRAINED`: `(done_flags & required_mask) == required_mask`
- `PROC_EXIT`:  `done_flags & DONE_F_PROC_EXIT`
- `STDOUT_EOF`: `done_flags & DONE_F_STDOUT_EOF`
- `CUSTOM`:     `sp->done_fn(sp, sp->done_fn_ctx)`

---

## Differences from Old n00b

| Area                | Old n00b                          | New (subproc)                       |
|---------------------|-----------------------------------|-------------------------------------|
| I/O multiplexing    | Custom stream + observable        | Conduit topics + managed FDs        |
| Capture             | Stream → buffer cookie            | Stream reader → `n00b_buffer_t *`   |
| Exit detection      | `n00b_new_exit_stream(pid)`       | `n00b_conduit_proc_topic(c, pid)`   |
| Signal handling     | `n00b_signal_register()` (direct) | Conduit signal topics               |
| Event loop          | Implicit (stream thread pool)     | Explicit `n00b_conduit_service_t`   |
| Error handling      | `N00B_CRAISE()` (exceptions)      | `n00b_result_t` (no exceptions)     |
| Object system       | vtable + `n00b_new()`             | vtable + `n00b_new()` (same)        |
| Allocator           | Typed GC arena                    | `n00b_alloc()` with system pool     |
| Transform pipeline  | Filter chain on stream            | `n00b_conduit_xform_t` pipeline     |
| String type         | `n00b_string_t *` (heap pointer)  | `n00b_string_t` (by value, 40 bytes)|
| Completion detect   | Condition variable on exit        | Done-inbox (exit + I/O drain)       |
| Timeout handling    | Kill only                         | SIGTERM / SIGKILL / detach          |
| Data interchange    | `char *`, `n00b_buf_t *` mixed    | `n00b_buffer_t *` everywhere        |
| Argv/env types      | `n00b_list_t *` of strings        | `n00b_array_t(n00b_string_t)`       |

---

## Implementation Order

1. **Type registration + struct** — Register `n00b_subproc_t` with
   vtable. Define flags, enums, `n00b_pre_exec_hook_t`.

2. **Init function** — `n00b_subproc_init()` with full `_kargs`.
   Validate configuration, compute flag bitmask, store all config.

3. **Pipe-mode spawn** — Gate pipe, FD management, basic
   parent/child fork/exec setup. `char **` conversion at execve only.

4. **Transform chain wiring** — Wire `stdout_xforms`, `stderr_xforms`,
   `proxy_xforms`, `stdin_xforms` as
   `xform_t(n00b_buffer_t *, n00b_buffer_t *)` chains between topics.

5. **I/O routing** — Capture (stream readers → buffers), proxy
   (subscribe chain output → parent fd_owner write through
   proxy_xforms), custom topic wiring, stdin injection and close.

6. **Done-inbox + completion model** — Wire done-inbox to all
   relevant done_topics. Compute `required_mask`. Implement wait loop
   with `done_condition` checking.

7. **Synchronous run + timeout** — `n00b_subproc_run()` = spawn +
   done-inbox wait + timeout policy application + close.

8. **PTY-mode spawn** — `openpty`, `setsid`, `TIOCSCTTY`,
   error-report pipe, aux PTY for stderr.

9. **Terminal management** — Termcap propagation, SIGWINCH proxy,
   unbuffered I/O.

10. **Kill + accessors** — `n00b_subproc_kill(.signal=SIGTERM)`,
    capture retrieval, exit code, status queries.

11. **Tests** — Unit tests for: pipe mode basic, PTY mode, capture,
    proxy, timeout + each policy, inject + close_stdin, transforms,
    custom topics, done conditions, pre-exec hook, cwd.

---

## Open Questions

1. **Conduit ownership:** Should `n00b_subproc_init()` accept a
   conduit, or create its own? Leaning toward accepting one (the
   caller likely has a conduit already), with a convenience that
   creates a throwaway conduit for simple use cases.

2. **Capture accumulation limits:** Stream readers accumulate
   unboundedly. Should we support a configurable max capture size
   (backpressure or truncation)?

3. **Process group management:** Should subproc support `setpgid()`
   for job control? Useful for shells and pipelines. Deferring for now
   — can be done via `pre_exec_hook`.

4. **Transform spec type:** The `stdout_xforms` array currently holds
   `void *` (opaque xform pointers). Should we define a typed spec
   struct that can be declaratively constructed?
