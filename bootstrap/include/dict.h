/**
 * @file dict.h
 * @brief Minimal thread-safe dictionary with module struct API.
 *
 * Extracted from n00b. Uses per-bucket spin locks and linear probing
 * for thread-safe concurrent access.
 *
 */

#pragma once

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

/** @brief Minimum dictionary size (number of buckets) */
#define NCC_DICT_MIN_SIZE 16

/** @brief Hash value type (128-bit for collision resistance) */
typedef __int128_t ncc_hash_t;

/** @brief Hash function type */
typedef ncc_hash_t (*ncc_hash_fn)(void *);

/**
 * @brief Dictionary bucket storing a single key-value pair.
 */
typedef struct {
    ncc_hash_t  hv;           /**< Cached hash value */
    void            *key;          /**< Key pointer */
    void            *value;        /**< Value pointer */
    uint32_t         insert_order; /**< Insertion sequence number */
    _Atomic uint32_t flags;        /**< Atomic flags for locking */
} ncc_dict_bucket_t;

/**
 * @brief Dictionary storage (array of buckets).
 */
typedef struct {
    uint32_t               last_slot;   /**< Last valid slot index */
    uint32_t               threshold;   /**< Resize threshold */
    _Atomic uint64_t       used_count;  /**< Number of used buckets */
    ncc_dict_bucket_t buckets[];   /**< Bucket array */
} ncc_dict_store_t;

/**
 * @brief Thread-safe dictionary.
 */
typedef struct {
    ncc_hash_fn                 hash_fn;         /**< Hash function (nullptr = pointer identity) */
    _Atomic(ncc_dict_store_t *) store;           /**< Current storage */
    _Atomic uint32_t                 insertion_epoch; /**< Insertion counter */
    _Atomic int64_t                  wait_ct;         /**< Waiting thread count */
    _Atomic int64_t                  length;          /**< Number of entries */
    _Atomic uint32_t                 futex;           /**< Futex for synchronization */
} ncc_dict_t;

/*
 * Direct function declarations (for internal use or when avoiding indirection)
 */
extern void ncc_dict_init(ncc_dict_t *d, uint32_t start_capacity, ncc_hash_fn hash_fn);
extern void ncc_dict_free(ncc_dict_t *d);
extern void *ncc_dict_put(ncc_dict_t *d, void *key, void *value);
[[nodiscard]] extern void *ncc_dict_get(ncc_dict_t *d, void *key, bool *found);
extern bool ncc_dict_replace(ncc_dict_t *d, void *key, void *value);
extern bool ncc_dict_add(ncc_dict_t *d, void *key, void *value);
extern bool ncc_dict_remove(ncc_dict_t *d, void *key);
[[nodiscard]] extern bool ncc_dict_contains(ncc_dict_t *d, void *key);
extern int64_t ncc_dict_len(ncc_dict_t *d);

/** Hash function for C strings */
extern ncc_hash_t ncc_hash_cstring(void *s);

/** Hash function for pointers (identity hash) */
extern ncc_hash_t ncc_hash_ptr(void *p);
