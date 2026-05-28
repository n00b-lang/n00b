/**
 * @file hash.h
 * @brief Hashing functions for the n00b runtime.
 *
 * Provides word-level hashing, C-string hashing, and a generic hash
 * dispatcher that invokes a caller-supplied hash function.
 */
#pragma once

#include "n00b.h"

/**
 * @brief Hash a single machine word (pointer-sized value).
 * @param value Pointer to the word to hash.
 * @return      128-bit hash value.
 */
extern n00b_uint128_t n00b_hash_word(void *value);

/**
 * @brief Hash a NUL-terminated C string.
 * @param value Pointer to the C string.
 * @return      128-bit hash value.
 */
extern n00b_uint128_t n00b_hash_cstring(void *value);

/**
 * @brief Hash an object using a caller-supplied hash function.
 * @param obj Object to hash.
 * @param fn  Hash function to invoke on @p obj.
 * @return    128-bit hash value.
 */
extern n00b_uint128_t n00b_hash(void *obj, n00b_hash_fn fn);

/**
 * @brief Low-level XXH3 64-bit helper used by parser/string caches.
 * @param data Pointer to input bytes.
 * @param len  Number of input bytes.
 * @return     64-bit XXH3 hash.
 */
extern uint64_t n00b_xxh3_64bits_raw(const void *data, size_t len);

/**
 * @brief Low-level XXH3 128-bit helper used by runtime hash functions.
 * @param data Pointer to input bytes.
 * @param len  Number of input bytes.
 * @return     128-bit XXH3 hash.
 */
extern n00b_uint128_t n00b_xxh3_128bits_raw(const void *data, size_t len);

/**
 * @brief Hash a raw byte buffer of known length.
 * @param data Pointer to the bytes.
 * @param len  Number of bytes to hash.
 * @return     128-bit hash value.
 */
extern n00b_uint128_t n00b_hash_raw(const void *data, size_t len);

/**
 * @brief Hash an `n00b_string_t` (by its UTF-8 content).
 *
 * Signature matches `n00b_hash_fn` (`n00b_uint128_t (*)(void *)`).
 * Expects a pointer to `n00b_string_t`, which it casts internally.
 *
 * @param key  Pointer to the `n00b_string_t` to hash (cast to `void *`).
 * @return     128-bit hash value.
 */
extern n00b_uint128_t n00b_string_hash(void *key);

/**
 * @brief Hash an `n00b_buffer_t` (by its raw byte content).
 * @param b  Pointer to the buffer to hash.
 * @return   128-bit hash value.
 */
extern n00b_uint128_t n00b_buffer_hash(n00b_buffer_t *b);
