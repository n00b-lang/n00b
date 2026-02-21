/**
 * @file file.h
 * @brief File-to-FD binding with managed I/O for the conduit system.
 *
 * Opens a file by name, wraps the FD with n00b_conduit_fd_manage() for
 * Layer 1/2 I/O, and creates a string-URI topic `"file:<mode>:<path>"`.
 *
 * Usage:
 * @code
 *     n00b_conduit_file_t *f =
 *         n00b_conduit_file_open(c, io, "/tmp/log", N00B_CONDUIT_FILE_R);
 *     n00b_conduit_fd_owner_t *owner = n00b_conduit_file_fd_owner(f);
 *     // subscribe to owner->read_topic for data ...
 *     n00b_conduit_file_close(f);
 * @endcode
 */
#pragma once

#include "conduit/fd_managed.h"
#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct n00b_conduit_file n00b_conduit_file_t;

n00b_result_decl(n00b_conduit_file_t *);

// ============================================================================
// File Open Mode Flags
// ============================================================================

/**
 * @brief File open mode flags.
 */
typedef enum {
    N00B_CONDUIT_FILE_READ     = (1 << 0), /**< Read mode */
    N00B_CONDUIT_FILE_WRITE    = (1 << 1), /**< Write mode */
    N00B_CONDUIT_FILE_CREATE   = (1 << 2), /**< Create if it doesn't exist */
    N00B_CONDUIT_FILE_TRUNCATE = (1 << 3), /**< Truncate on open */
    N00B_CONDUIT_FILE_APPEND   = (1 << 4), /**< Append mode (writes at end) */
} n00b_conduit_file_mode_t;

/** @brief Read-only mode. */
#define N00B_CONDUIT_FILE_R   (N00B_CONDUIT_FILE_READ)
/** @brief Write mode with create and truncate. */
#define N00B_CONDUIT_FILE_W   (N00B_CONDUIT_FILE_WRITE | N00B_CONDUIT_FILE_CREATE | N00B_CONDUIT_FILE_TRUNCATE)
/** @brief Append mode with create. */
#define N00B_CONDUIT_FILE_A   (N00B_CONDUIT_FILE_WRITE | N00B_CONDUIT_FILE_CREATE | N00B_CONDUIT_FILE_APPEND)
/** @brief Read-write mode with create. */
#define N00B_CONDUIT_FILE_RW  (N00B_CONDUIT_FILE_READ | N00B_CONDUIT_FILE_WRITE | N00B_CONDUIT_FILE_CREATE)
/** @brief Read-append mode with create. */
#define N00B_CONDUIT_FILE_RA  (N00B_CONDUIT_FILE_READ | N00B_CONDUIT_FILE_WRITE | N00B_CONDUIT_FILE_CREATE | N00B_CONDUIT_FILE_APPEND)

// ============================================================================
// File Structure
// ============================================================================

/**
 * @brief File handle -- wraps a managed FD with string-URI topic.
 */
struct n00b_conduit_file {
    n00b_conduit_t              *conduit;
    n00b_conduit_io_backend_t   *io;
    n00b_conduit_fd_owner_t     *owner;
    n00b_conduit_topic_base_t   *file_topic;
    n00b_string_t                path;       /**< File path */
    int                          fd;
    uint32_t                     mode;       /**< n00b_conduit_file_mode_t bits */
};

// ============================================================================
// File API
// ============================================================================

/**
 * @brief Open a file and manage its FD.
 *
 * Opens path with the given mode flags, creates a managed FD owner,
 * and registers a string-URI topic `"file:<mode>:<path>"`.
 *
 * @param c    Conduit instance.
 * @param io   I/O backend.
 * @param path File path to open.
 * @param mode n00b_conduit_file_mode_t bitmask.
 * @return Ok(file) on success, or Err(errno) on failure.
 */
extern n00b_result_t(n00b_conduit_file_t *)
n00b_conduit_file_open(n00b_conduit_t *c, n00b_conduit_io_backend_t *io,
                       const char *path, uint32_t mode);

/**
 * @brief Close file -- closes fd, topics.
 */
extern void
n00b_conduit_file_close(n00b_conduit_file_t *f);

/**
 * @brief Get the underlying fd_owner for Layer 1/2 I/O.
 */
extern n00b_option_t(n00b_conduit_fd_owner_t *)
n00b_conduit_file_fd_owner(n00b_conduit_file_t *f);

/**
 * @brief Get the file's string-URI topic.
 */
extern n00b_option_t(n00b_conduit_topic_base_t *)
n00b_conduit_file_topic(n00b_conduit_file_t *f);
