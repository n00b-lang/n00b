/**
 * @file frontend_fuse.h
 * @brief FUSE frontend for macOS (macFUSE) and other FUSE-capable systems.
 *
 * Maps FUSE high-level callbacks to VFS operations.  Each FUSE op is
 * a thin wrapper around the corresponding @c n00b_vfs_* function.
 *
 * Only compiled when macFUSE (or libfuse) is available.
 */
#pragma once

#include "vfs/frontend.h"

/**
 * @brief Create a FUSE frontend.
 * @param vfs          VFS instance to expose.
 * @param mount_point  Directory to mount on (must exist).
 */
extern n00b_result_t(n00b_vfs_frontend_t *)
n00b_vfs_frontend_fuse_new(n00b_vfs_t *vfs, n00b_string_t *mount_point);

/**
 * @brief The vtable for the FUSE frontend.
 */
extern const n00b_vfs_frontend_ops_t n00b_vfs_frontend_fuse_ops;
