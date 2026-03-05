/**
 * @file vfs.h
 * @brief VFS core: mount table, file handles, path resolution, operations.
 *
 * The VFS layer provides POSIX-ish file semantics over any storage
 * backend.  It manages:
 * - A mount table mapping VFS path prefixes to backends
 * - A file handle table with open/close state and seek offsets
 * - Hook/filter chains per mount
 *
 * Path resolution uses longest-prefix matching against the mount table.
 */
#pragma once

#include "vfs/types.h"
#include "vfs/backend.h"
#include "vfs/hooks.h"
#include "core/data_lock.h"

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct n00b_vfs       n00b_vfs_t;
typedef struct n00b_vfs_mount n00b_vfs_mount_t;

// ============================================================================
// Mount point
// ============================================================================

/** @brief Mount flags. */
typedef enum {
    N00B_VFS_MOUNT_READONLY = (1 << 0),
} n00b_vfs_mount_flags_t;

/**
 * @brief A mount point binding a VFS path prefix to a backend.
 */
struct n00b_vfs_mount {
    n00b_string_t      *mount_path;  /**< VFS path prefix (e.g. "/data"). */
    n00b_vfs_backend_t *backend;
    n00b_vfs_hook_t   **hooks;       /**< Hook array (sorted by priority). */
    uint32_t            nhooks;
    uint32_t            hooks_cap;
    n00b_rwlock_t      *lock;        /**< Protects hooks and metadata. */
    uint32_t            flags;
    bool                active;
};

// ============================================================================
// Handle state
// ============================================================================

typedef enum {
    N00B_VFS_HANDLE_OPEN,
    N00B_VFS_HANDLE_CLOSING,
    N00B_VFS_HANDLE_CLOSED,
} n00b_vfs_handle_state_t;

/**
 * @brief An open file handle in the VFS.
 */
typedef struct n00b_vfs_handle {
    n00b_vfs_fh_t            fh;
    n00b_string_t           *path;         /**< Full VFS path. */
    n00b_string_t           *backend_path; /**< Path relative to mount root. */
    uint32_t                 flags;
    _Atomic(uint64_t)        offset;
    n00b_vfs_mount_t        *mount;
    n00b_vfs_handle_state_t  state;
    n00b_buffer_t           *write_buf;    /**< Accumulated writes (for backends without random write). */
} n00b_vfs_handle_t;

// ============================================================================
// VFS instance
// ============================================================================

/**
 * @brief The VFS instance — owns mount table and handle table.
 */
struct n00b_vfs {
    n00b_vfs_mount_t  **mounts;       /**< Sorted by path length desc. */
    uint32_t            nmounts;
    uint32_t            mounts_cap;
    n00b_vfs_handle_t **handles;      /**< Indexed by (fh - 1). */
    uint32_t            nhandles;
    uint32_t            handles_cap;
    _Atomic(uint64_t)   next_fh;
    n00b_rwlock_t      *mount_lock;
    n00b_rwlock_t      *handle_lock;
};

// ============================================================================
// Lifecycle
// ============================================================================

extern n00b_result_t(n00b_vfs_t *) n00b_vfs_new(void);
extern void n00b_vfs_destroy(n00b_vfs_t *vfs);

// ============================================================================
// Mount management
// ============================================================================

/**
 * @brief Mount a backend at a VFS path.
 * @param vfs      VFS instance.
 * @param path     Mount path prefix (e.g. "/data").
 * @param backend  Initialized backend.
 * @param flags    Mount flags (0 for defaults).
 */
extern n00b_result_t(n00b_vfs_mount_t *)
n00b_vfs_mount(n00b_vfs_t *vfs, n00b_string_t *path,
               n00b_vfs_backend_t *backend, uint32_t flags);

/**
 * @brief Unmount a backend at the given path.
 *
 * Fails if there are open handles on this mount.
 */
extern n00b_result_t(bool)
n00b_vfs_unmount(n00b_vfs_t *vfs, n00b_string_t *path);

// ============================================================================
// Hook registration
// ============================================================================

/**
 * @brief Register a hook on a mount.
 * @param mount     Target mount.
 * @param point     Hook interception point.
 * @param fn        Callback function.
 * @param cookie    Opaque user data.
 * @param priority  Lower runs first (default 0).
 */
extern n00b_result_t(bool)
n00b_vfs_hook_add(n00b_vfs_mount_t *mount, n00b_vfs_hook_point_t point,
                  n00b_vfs_hook_fn fn, void *cookie, int32_t priority);

// ============================================================================
// File operations
// ============================================================================

extern n00b_result_t(n00b_vfs_fh_t)
n00b_vfs_open(n00b_vfs_t *vfs, n00b_string_t *path, uint32_t flags);

extern n00b_result_t(n00b_buffer_t *)
n00b_vfs_read(n00b_vfs_t *vfs, n00b_vfs_fh_t fh, uint64_t length);

extern n00b_result_t(uint64_t)
n00b_vfs_write(n00b_vfs_t *vfs, n00b_vfs_fh_t fh, n00b_buffer_t *data);

extern n00b_result_t(bool)
n00b_vfs_close(n00b_vfs_t *vfs, n00b_vfs_fh_t fh);

extern n00b_result_t(uint64_t)
n00b_vfs_seek(n00b_vfs_t *vfs, n00b_vfs_fh_t fh, int64_t offset, int whence);

/**
 * @brief Truncate a file to a given size.
 *
 * If the file is shorter, it is extended with zero bytes.
 * If longer, it is shortened.
 */
extern n00b_result_t(bool)
n00b_vfs_truncate(n00b_vfs_t *vfs, n00b_string_t *path, uint64_t size);

/**
 * @brief Flush a file handle's write buffer to the backend without closing.
 */
extern n00b_result_t(bool)
n00b_vfs_flush(n00b_vfs_t *vfs, n00b_vfs_fh_t fh);

// ============================================================================
// Metadata operations
// ============================================================================

extern n00b_result_t(n00b_vfs_obj_stat_t)
n00b_vfs_stat(n00b_vfs_t *vfs, n00b_string_t *path);

extern n00b_result_t(n00b_vfs_list_result_t *)
n00b_vfs_readdir(n00b_vfs_t *vfs, n00b_string_t *path, uint32_t max_entries);

extern n00b_result_t(bool)
n00b_vfs_mkdir(n00b_vfs_t *vfs, n00b_string_t *path);

extern n00b_result_t(bool)
n00b_vfs_delete(n00b_vfs_t *vfs, n00b_string_t *path);

extern n00b_result_t(bool)
n00b_vfs_rename(n00b_vfs_t *vfs, n00b_string_t *old_path,
                n00b_string_t *new_path);
