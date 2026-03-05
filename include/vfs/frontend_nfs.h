/**
 * @file frontend_nfs.h
 * @brief Userspace NFSv3 server frontend for Linux.
 *
 * Implements a minimal NFSv3 subset over TCP, using the kernel's
 * built-in NFS client for mounting.  No external dependencies — XDR
 * and ONC RPC are implemented from scratch.
 *
 * Only compiled on Linux.
 */
#pragma once

#include "vfs/frontend.h"

/**
 * @brief Create an NFSv3 frontend.
 * @param vfs          VFS instance to expose.
 * @param mount_point  Directory to mount on (must exist).
 * @param port         TCP port for the NFS server (0 = ephemeral).
 */
extern n00b_result_t(n00b_vfs_frontend_t *)
n00b_vfs_frontend_nfs_new(n00b_vfs_t *vfs, n00b_string_t *mount_point,
                          uint16_t port);

/**
 * @brief The vtable for the NFS frontend.
 */
extern const n00b_vfs_frontend_ops_t n00b_vfs_frontend_nfs_ops;
