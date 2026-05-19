/**
 * @file std_streams.h
 * @brief Public accessors for the runtime's managed stdin / stdout / stderr.
 *
 * Returns the conduit `n00b_conduit_fd_owner_t *` that the runtime
 * manages for process file descriptors 0, 1, and 2.  The owners are
 * created eagerly during `n00b_init()` (see `src/core/init.c`); these
 * accessors are one-line wrappers around the runtime fields, exposed
 * here so callers don't reach through `n00b_get_runtime()` directly.
 *
 * The returned owner exposes the standard Layer 1 / Layer 2 conduit
 * surface for the FD:
 *
 *  - **Layer 1 (raw).** Subscribe to `owner->read_topic` for as-done
 *    `n00b_buffer_t *` chunks.  Use `n00b_conduit_fd_write_submit`
 *    (non-blocking) or `n00b_fd_owner_write` (blocking) for writes.
 *  - **Layer 2 (stream).** Wrap the owner with
 *    `n00b_conduit_stream_reader_new(c, owner)` for accumulator-style
 *    "read N bytes" / "read until delimiter" semantics.
 *  - **Status.** Subscribe to `owner->status_topic` for EOF / error
 *    notifications.
 *
 * For the common bulk-I/O case (consume all of stdin into an
 * `n00b_buffer_t`; write an `n00b_buffer_t` to stdout) the
 * higher-level `core/file.h` API is usually a better fit — open a
 * `/dev/stdin` / `/dev/stdout` path through `n00b_file_open()`.  The
 * `n00b_stdin()` / `n00b_stdout()` / `n00b_stderr()` accessors here
 * are the canonical entry point when the caller needs the FD owner
 * itself (for subscription, transform wiring, or direct write submit).
 *
 * Returns `nullptr` only if `n00b_init()` failed to manage the FD
 * during runtime startup — in practice this happens only when the
 * process inherited a fully-closed standard stream and the kernel
 * refused `fd_manage`'s `dup`/`fcntl` setup.  Callers may treat a
 * `nullptr` return as "this stream is unavailable on this process."
 */
#pragma once

#include "n00b.h"
#include "core/runtime.h"
#include "conduit/fd_managed.h"

/**
 * @brief Get the runtime's managed-FD owner for process stdin (fd 0).
 *
 * @return  The runtime's `stdin_owner`, or `nullptr` if the runtime
 *          failed to manage fd 0 during initialization.
 *
 * @pre  `n00b_init()` has returned successfully.
 *
 * @details
 * Reads stay quiescent until the first subscriber attaches to
 * `owner->read_topic`; the conduit IO thread does not consume bytes
 * from fd 0 before then.  See `n00b_conduit_fd_manage` for the
 * Layer 1 / Layer 2 lifecycle contract.
 */
extern n00b_conduit_fd_owner_t *n00b_stdin(void);

/**
 * @brief Get the runtime's managed-FD owner for process stdout (fd 1).
 *
 * @return  The runtime's `stdout_owner`, or `nullptr` if the runtime
 *          failed to manage fd 1 during initialization.
 *
 * @pre  `n00b_init()` has returned successfully.
 *
 * @details
 * This is the same owner that `n00b_print()` and `n00b_printf()`
 * reach through internally.  Callers needing direct byte-level
 * `write()` semantics — without the print API's vtable-driven
 * `to_string` conversion — go through this owner.
 */
extern n00b_conduit_fd_owner_t *n00b_stdout(void);

/**
 * @brief Get the runtime's managed-FD owner for process stderr (fd 2).
 *
 * @return  The runtime's `stderr_owner`, or `nullptr` if the runtime
 *          failed to manage fd 2 during initialization.
 *
 * @pre  `n00b_init()` has returned successfully.
 */
extern n00b_conduit_fd_owner_t *n00b_stderr(void);
