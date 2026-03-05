/**
 * @file frontend.h
 * @brief VFS mount frontend vtable and dispatcher.
 *
 * A frontend exposes a VFS instance to the host OS as a mounted
 * filesystem.  Platform-specific frontends (NFS on Linux, FUSE on
 * macOS, WinFSP on Windows) implement the @c n00b_vfs_frontend_ops_t
 * vtable.
 *
 * The VFS can also be used directly via the library API without any
 * frontend (library mode).
 */
#pragma once

#include "vfs/vfs.h"

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct n00b_vfs_frontend     n00b_vfs_frontend_t;
typedef struct n00b_vfs_frontend_ops n00b_vfs_frontend_ops_t;

// ============================================================================
// Frontend vtable
// ============================================================================

struct n00b_vfs_frontend_ops {
    /** @brief Human-readable frontend name (e.g. "nfs", "fuse", "winfsp"). */
    n00b_string_t *(*name)(void);

    /**
     * @brief Start serving the mount.
     *
     * This may spawn a background thread or run in the caller's thread.
     * Returns when the mount is ready for I/O.
     */
    n00b_result_t(bool) (*start)(n00b_vfs_frontend_t *fe);

    /** @brief Stop serving and unmount. */
    void (*stop)(n00b_vfs_frontend_t *fe);

    /** @brief Check if the frontend is running. */
    bool (*is_running)(n00b_vfs_frontend_t *fe);
};

// ============================================================================
// Frontend instance
// ============================================================================

struct n00b_vfs_frontend {
    const n00b_vfs_frontend_ops_t *ops;
    n00b_vfs_t                    *vfs;
    n00b_string_t                 *mount_point;  /**< OS-level mount path. */
    void                          *ctx;          /**< Frontend-specific state. */
    _Atomic(bool)                  running;
};

// ============================================================================
// Dispatcher: create platform-appropriate frontend
// ============================================================================

/**
 * @brief Detect the platform and create an appropriate frontend.
 *
 * On Linux, creates an NFSv3 frontend.  On macOS with macFUSE, creates
 * a FUSE frontend.  Returns an error on unsupported platforms or if
 * the required frontend is not compiled in.
 *
 * @param vfs          VFS instance to expose.
 * @param mount_point  OS path where the filesystem will be mounted.
 */
extern n00b_result_t(n00b_vfs_frontend_t *)
n00b_vfs_frontend_auto(n00b_vfs_t *vfs, n00b_string_t *mount_point);

/**
 * @brief Start a frontend (delegates to ops->start).
 */
extern n00b_result_t(bool)
n00b_vfs_frontend_start(n00b_vfs_frontend_t *fe);

/**
 * @brief Stop a frontend and unmount (delegates to ops->stop).
 */
extern void
n00b_vfs_frontend_stop(n00b_vfs_frontend_t *fe);

/**
 * @brief Check if a frontend is currently running.
 */
extern bool
n00b_vfs_frontend_is_running(n00b_vfs_frontend_t *fe);
