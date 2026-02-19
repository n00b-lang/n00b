/**
 * @file pipe_io.h
 * @brief Non-blocking pipe I/O helpers for subprocess communication.
 *
 * Provides `ncc_pipe_io()` — a poll()-based loop that writes data to a
 * child process's stdin and optionally captures its stdout, avoiding
 * deadlocks when both input and output exceed the kernel pipe buffer.
 */
#pragma once

#include <stddef.h>
#include "buf.h"

/**
 * @brief Write data to a child's stdin and optionally read its stdout.
 *
 * Uses non-blocking I/O with `poll()` to concurrently write to `write_fd`
 * and read from `read_fd`, avoiding deadlocks when data exceeds the pipe
 * buffer size.
 *
 * After completion, both file descriptors are closed.
 *
 * @param write_fd  File descriptor to write to (child's stdin pipe).
 *                  Closed after all data is written or on error.
 * @param read_fd   File descriptor to read from (child's stdout pipe),
 *                  or -1 for write-only mode.
 *                  Closed after EOF or on error.
 * @param data      Data to write to the child process.
 * @param len       Length of data in bytes.
 * @param prog_name Program name for error messages (may be nullptr).
 * @return Captured output as `ncc_buf_t *` (caller owns), or nullptr
 *         in write-only mode (read_fd < 0).
 *
 * @pre `write_fd` is a valid, open file descriptor.
 * @pre `read_fd` is a valid, open file descriptor or -1.
 * @post Both `write_fd` and `read_fd` (if >= 0) are closed.
 */
extern ncc_buf_t *ncc_pipe_io(int write_fd, int read_fd,
                              const char *data, size_t len,
                              const char *prog_name);
