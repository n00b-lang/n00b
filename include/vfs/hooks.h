/**
 * @file hooks.h
 * @brief VFS hook/filter types and registration API.
 *
 * Hooks are synchronous callbacks invoked at interception points
 * in the VFS operation pipeline.  They run inline with the request
 * and can inspect, modify, or deny operations.
 *
 * Hooks are registered per-mount and executed in priority order
 * (lower priority value runs first).
 */
#pragma once

#include "vfs/types.h"

// ============================================================================
// Hook points
// ============================================================================

typedef enum {
    N00B_VFS_HOOK_PRE_OPEN,
    N00B_VFS_HOOK_POST_OPEN,
    N00B_VFS_HOOK_PRE_READ,
    N00B_VFS_HOOK_POST_READ,
    N00B_VFS_HOOK_PRE_WRITE,
    N00B_VFS_HOOK_POST_WRITE,
    N00B_VFS_HOOK_PRE_CLOSE,
    N00B_VFS_HOOK_POST_CLOSE,
    N00B_VFS_HOOK_PRE_DELETE,
    N00B_VFS_HOOK_PRE_RENAME,
    N00B_VFS_HOOK_PRE_MKDIR,
    N00B_VFS_HOOK_PRE_STAT,
    N00B_VFS_HOOK_ACCESS_CHECK,
    N00B_VFS_HOOK_COUNT_,     /**< Sentinel — not a valid hook point. */
} n00b_vfs_hook_point_t;

// ============================================================================
// Hook context — passed to every hook callback
// ============================================================================

/**
 * @brief Context passed to hook callbacks.
 *
 * Pre-hooks can set @c denied to true to abort the operation.
 * Post-read hooks can replace @c data to transform the result.
 */
typedef struct n00b_vfs_hook_ctx {
    n00b_vfs_hook_point_t point;
    n00b_string_t        *path;
    n00b_buffer_t        *data;       /**< Read/write data (nullable). */
    uint64_t              offset;
    uint64_t              length;
    uint32_t              flags;      /**< Open flags for open hooks. */
    n00b_vfs_fh_t         fh;         /**< File handle (0 for pre-open, delete, etc). */
    n00b_string_t        *rename_dst; /**< Destination path for rename hooks. */
    void                 *mount;      /**< Owning mount (n00b_vfs_mount_t *). */
    bool                  denied;     /**< Set to true to deny the operation. */
    n00b_err_t            deny_err;   /**< Error code if denied (default: HOOK_DENIED). */
} n00b_vfs_hook_ctx_t;

// ============================================================================
// Hook callback and registration
// ============================================================================

/**
 * @brief Hook callback signature.
 * @param ctx     Mutable context for this hook invocation.
 * @param cookie  Opaque user data from registration.
 */
typedef void (*n00b_vfs_hook_fn)(n00b_vfs_hook_ctx_t *ctx, void *cookie);

/**
 * @brief A registered hook.
 */
typedef struct n00b_vfs_hook {
    n00b_vfs_hook_point_t point;
    n00b_vfs_hook_fn      fn;
    void                 *cookie;
    int32_t               priority;  /**< Lower runs first. */
} n00b_vfs_hook_t;

// ============================================================================
// Hook execution (internal — called by VFS core)
// ============================================================================

/**
 * @brief Run all hooks for a given point on a hook list.
 *
 * Hooks execute in priority order.  If any hook sets @c ctx->denied,
 * remaining hooks are skipped and the function returns the deny error.
 *
 * @param hooks   Array of hook pointers (from mount).
 * @param nhooks  Number of hooks in the array.
 * @param ctx     Hook context (modified in place).
 * @return        N00B_VFS_ERR_NONE if all hooks pass, or the deny error code.
 */
extern n00b_err_t
_n00b_vfs_hooks_run(n00b_vfs_hook_t **hooks, uint32_t nhooks,
                    n00b_vfs_hook_ctx_t *ctx);
