/**
 * @file cache.h
 * @brief VFS caching subsystem.
 *
 * Provides a local file cache between the VFS layer and storage backends.
 * For local directory backends, uses hard links for zero-copy reads and
 * copy-on-write for writes.  For remote/object backends, caches full
 * objects locally with ETag-based revalidation.
 */
#pragma once

#include "vfs/types.h"
#include "vfs/backend.h"
#include "core/data_lock.h"

// ============================================================================
// Cache entry states
// ============================================================================

typedef enum {
    N00B_VFS_CACHE_CLEAN,    /**< Matches backend, safe to serve. */
    N00B_VFS_CACHE_DIRTY,    /**< Local writes not yet flushed. */
    N00B_VFS_CACHE_STALE,    /**< Suspected stale, needs revalidation. */
    N00B_VFS_CACHE_INVALID,  /**< Must fetch from backend. */
    N00B_VFS_CACHE_LINKED,   /**< Hard-linked to source (local only). */
} n00b_vfs_cache_state_t;

// ============================================================================
// Cache policy
// ============================================================================

typedef struct n00b_vfs_cache_policy {
    uint64_t max_size_bytes;    /**< Max total cache size (0 = unlimited). */
    uint64_t max_entry_age_ns;  /**< Max age before revalidation. */
    uint32_t max_entries;       /**< Max number of cached objects. */
    bool     write_through;     /**< Flush writes immediately. */
    bool     use_hard_links;    /**< Use hard links for local backends. */
} n00b_vfs_cache_policy_t;

// ============================================================================
// Cache entry
// ============================================================================

typedef struct n00b_vfs_cache_entry {
    n00b_string_t              *backend_path;
    n00b_string_t              *cache_path;
    n00b_string_t              *etag;
    _Atomic(n00b_vfs_cache_state_t) state;
    _Atomic(int32_t)            open_count;
    _Atomic(int32_t)            write_count;
    _Atomic(uint64_t)           size;
    uint64_t                    mtime_ns;
    _Atomic(uint64_t)           last_validated_ns;
    _Atomic(uint64_t)           last_access_ns;   /**< For LRU eviction. */
} n00b_vfs_cache_entry_t;

// ============================================================================
// Cache instance
// ============================================================================

typedef struct n00b_vfs_cache {
    n00b_string_t           *cache_dir;
    n00b_vfs_cache_policy_t  policy;
    n00b_vfs_cache_entry_t **entries;
    uint32_t                 nentries;
    uint32_t                 entries_cap;
    _Atomic(uint64_t)        total_size;
    n00b_vfs_backend_t      *backend;
    n00b_rwlock_t           *lock;
} n00b_vfs_cache_t;

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Create a new cache instance.
 * @param cache_dir  Directory for cached files (created if needed).
 * @param backend    The backend this cache fronts.
 * @param policy     Caching policy.
 */
extern n00b_result_t(n00b_vfs_cache_t *)
n00b_vfs_cache_new(n00b_string_t *cache_dir, n00b_vfs_backend_t *backend,
                   n00b_vfs_cache_policy_t policy);

extern void n00b_vfs_cache_destroy(n00b_vfs_cache_t *cache);

// ── Cache operations (called by VFS layer) ─────────────────────────

/**
 * @brief Get data through the cache.
 *
 * Returns cached data if fresh, otherwise fetches from backend
 * and populates cache.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_vfs_cache_get(n00b_vfs_cache_t *cache, n00b_string_t *path,
                   uint64_t offset, uint64_t length);

/**
 * @brief Write data through the cache.
 *
 * Writes to the cache file.  If write-through, also flushes to backend.
 */
extern n00b_result_t(bool)
n00b_vfs_cache_put(n00b_vfs_cache_t *cache, n00b_string_t *path,
                   n00b_buffer_t *data);

/**
 * @brief Invalidate a cache entry.
 */
extern n00b_result_t(bool)
n00b_vfs_cache_invalidate(n00b_vfs_cache_t *cache, n00b_string_t *path);

/**
 * @brief Flush a dirty cache entry to the backend.
 */
extern n00b_result_t(bool)
n00b_vfs_cache_flush(n00b_vfs_cache_t *cache, n00b_string_t *path);

/**
 * @brief Evict the least-recently-used cache entry.
 *
 * Dirty entries are flushed before eviction.
 */
extern void n00b_vfs_cache_evict_lru(n00b_vfs_cache_t *cache);

/**
 * @brief Notify the cache that a file is being opened for read/write.
 *
 * For hard-linked entries opened for write, this breaks the link (COW).
 */
extern n00b_result_t(n00b_vfs_cache_entry_t *)
n00b_vfs_cache_open(n00b_vfs_cache_t *cache, n00b_string_t *path,
                    bool for_write);

/**
 * @brief Notify the cache that a file handle has been closed.
 */
extern void
n00b_vfs_cache_close(n00b_vfs_cache_t *cache, n00b_string_t *path,
                     bool was_write);
