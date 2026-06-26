/**
 * @file backend_snapshot.h
 * @brief Snapshot-to-VFS durability backend for VKS (D-012: snapshot-only).
 *
 * This backend persists a VKS store as a single marshaled object graph stored
 * at one path inside an @c n00b_vfs_t.  It implements the snapshot-only
 * contract from @ref backend.h:
 *
 * - @c load   reads the bytes at @p path (if present), unmarshals them into a
 *             dict, and installs that dict as the store's in-memory index.  A
 *             missing or corrupt image leaves the store's empty dict intact.
 * - @c snapshot marshals the store's embedded dict to a buffer and writes it
 *             atomically (write to a temp path, then rename over @p path).
 *
 * There is no per-key write-through: durability is whole-store snapshots only.
 */
#pragma once

#include "vks/backend.h"
#include "vfs/vfs.h"

/**
 * @brief Create a snapshot-to-VFS durability backend.
 *
 * @param vfs   The VFS the snapshot lives in (must outlive the backend).
 * @param path  Absolute VFS path of the snapshot object.
 * @kw allocator Allocator for the backend instance and its context.
 *
 * @return Ok(backend) with @c ctx already initialized, or an error result.
 *
 * @pre  @p vfs and @p path are non-null.
 * @post The returned backend's @c ops is @ref n00b_vks_backend_snapshot_ops.
 *
 * @note Trust boundary: only vks itself writes these snapshots. On @c load the
 *       backend still verifies the unmarshaled object's runtime type identity
 *       (the typehash recorded in its GC/allocation header) against that of the
 *       store's freshly built empty dict (created for this store's exact K/V).
 *       The comparison reads only the allocation header, never dereferencing the
 *       unmarshaled object as a dict, and the fully parameterized
 *       @c n00b_dict(K, V) typehash subsumes the per-element key/value type
 *       check. Any mismatch or corruption falls back to the empty dict, so a
 *       stale or foreign image is never installed.
 */
extern n00b_result_t(n00b_vks_backend_t *)
n00b_vks_backend_snapshot_new(n00b_vfs_t *vfs, n00b_string_t *path) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief The vtable for the snapshot-to-VFS backend.
 */
extern const n00b_vks_backend_ops_t n00b_vks_backend_snapshot_ops;
