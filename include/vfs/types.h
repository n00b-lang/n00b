/**
 * @file types.h
 * @brief Shared types, enums, and error codes for the VFS subsystem.
 *
 * This header is included by all other VFS headers.  It provides:
 * - VFS-specific error codes (offset 2000+ to avoid collision with
 *   conduit error codes at 1000+)
 * - Object metadata types shared between backends, VFS core, and cache
 * - List result types for directory enumeration
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "core/buffer.h"
#include "adt/result.h"
#include "adt/option.h"

// ============================================================================
// Error codes
//
// VFS operations return n00b_result_t with these domain-specific error
// codes (offset 2000 to avoid collision with errno and conduit errors).
// ============================================================================

enum {
    N00B_VFS_ERR_NONE          = 0,
    N00B_VFS_ERR_NULL_ARG      = 2000,
    N00B_VFS_ERR_ALLOC,
    N00B_VFS_ERR_NOT_FOUND,
    N00B_VFS_ERR_EXISTS,
    N00B_VFS_ERR_IS_DIR,
    N00B_VFS_ERR_NOT_DIR,
    N00B_VFS_ERR_NOT_EMPTY,
    N00B_VFS_ERR_PERMISSION,
    N00B_VFS_ERR_IO,
    N00B_VFS_ERR_NO_SPACE,
    N00B_VFS_ERR_INVALID_PATH,
    N00B_VFS_ERR_INVALID_HANDLE,
    N00B_VFS_ERR_CLOSED,
    N00B_VFS_ERR_BACKEND,
    N00B_VFS_ERR_CACHE,
    N00B_VFS_ERR_MOUNT,
    N00B_VFS_ERR_HOOK_DENIED,
    N00B_VFS_ERR_STALE,
    N00B_VFS_ERR_CROSS_DEVICE,
    N00B_VFS_ERR_NOT_SUPPORTED,
    N00B_VFS_ERR_READ_ONLY,
};

// ============================================================================
// Object kinds
// ============================================================================

typedef enum {
    N00B_VFS_OBJ_FILE,
    N00B_VFS_OBJ_DIR,
    N00B_VFS_OBJ_SYMLINK,
} n00b_vfs_obj_kind_t;

// ============================================================================
// Object metadata
// ============================================================================

/**
 * @brief Metadata for a single object in a backend or VFS.
 *
 * Returned by stat operations. Fields that are not meaningful for a
 * given backend are zero-initialized.
 */
typedef struct n00b_vfs_obj_stat {
    n00b_vfs_obj_kind_t kind;
    uint64_t            size;
    uint64_t            atime_ns;     /**< Access time — nanoseconds since epoch. */
    uint64_t            mtime_ns;     /**< Modify time — nanoseconds since epoch. */
    uint64_t            ctime_ns;     /**< Change time — nanoseconds since epoch. */
    n00b_string_t      *etag;         /**< Backend-specific version tag (nullable). */
    n00b_string_t      *content_type; /**< MIME type (nullable). */
    uint32_t            mode;         /**< POSIX permission bits (0 if unavailable). */
} n00b_vfs_obj_stat_t;

// ============================================================================
// Directory listing types
// ============================================================================

/**
 * @brief Single entry in a directory listing.
 */
typedef struct n00b_vfs_list_entry {
    n00b_string_t      *name;
    n00b_vfs_obj_kind_t kind;
    uint64_t            size;
    uint64_t            mtime_ns;
} n00b_vfs_list_entry_t;

/**
 * @brief Result of a directory/prefix listing with continuation support.
 *
 * @c continuation is nullptr when the listing is complete.
 * Backends that support pagination populate it with an opaque
 * token for the next call.
 */
typedef struct n00b_vfs_list_result {
    n00b_vfs_list_entry_t *entries;
    uint32_t               count;
    n00b_string_t         *continuation;
    bool                   truncated;
} n00b_vfs_list_result_t;

// ============================================================================
// File handle type
// ============================================================================

typedef uint64_t n00b_vfs_fh_t;
#define N00B_VFS_FH_INVALID 0

// ============================================================================
// Open flags
// ============================================================================

typedef enum {
    N00B_VFS_OPEN_READ   = (1 << 0),
    N00B_VFS_OPEN_WRITE  = (1 << 1),
    N00B_VFS_OPEN_CREATE = (1 << 2),
    N00B_VFS_OPEN_TRUNC  = (1 << 3),
    N00B_VFS_OPEN_APPEND = (1 << 4),
    N00B_VFS_OPEN_EXCL   = (1 << 5),
} n00b_vfs_open_flags_t;

#define N00B_VFS_O_R  (N00B_VFS_OPEN_READ)
#define N00B_VFS_O_W  (N00B_VFS_OPEN_WRITE | N00B_VFS_OPEN_CREATE | N00B_VFS_OPEN_TRUNC)
#define N00B_VFS_O_RW (N00B_VFS_OPEN_READ | N00B_VFS_OPEN_WRITE | N00B_VFS_OPEN_CREATE)
#define N00B_VFS_O_A  (N00B_VFS_OPEN_WRITE | N00B_VFS_OPEN_CREATE | N00B_VFS_OPEN_APPEND)

// ============================================================================
// Utility: error code to string
// ============================================================================

/**
 * @brief Return a human-readable name for a VFS error code.
 * @param err  Error code from the N00B_VFS_ERR_* enum.
 * @return     Static string, e.g. "NOT_FOUND".  "UNKNOWN" for unrecognized codes.
 */
extern const char *n00b_vfs_err_name(n00b_err_t err);
