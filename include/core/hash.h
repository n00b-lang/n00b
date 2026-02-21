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
 * @brief Hash a raw byte buffer of known length.
 * @param data Pointer to the bytes.
 * @param len  Number of bytes to hash.
 * @return     128-bit hash value.
 */
extern n00b_uint128_t n00b_hash_raw(const void *data, size_t len);

/**
 * @brief Hash an `n00b_string_t` (by its UTF-8 content).
 * @param s  The string to hash.
 * @return   128-bit hash value.
 */
extern n00b_uint128_t n00b_string_hash(n00b_string_t s);

/**
 * @brief Hash an `n00b_buffer_t` (by its raw byte content).
 * @param b  Pointer to the buffer to hash.
 * @return   128-bit hash value.
 */
extern n00b_uint128_t n00b_buffer_hash(n00b_buffer_t *b);
