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
// TODO: replace
// extern n00b_uint128_t n00b_string_hash(n00b_string_t *s);
// extern n00b_uint128_t n00b_buffer_hash(n00b_buf_t *b);
