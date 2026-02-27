#pragma once
/**
 * @file hash.h
 * @brief Hashing functions for string keys (standalone extraction).
 *
 * Uses FNV-1a to produce 128-bit hash values. The full n00b runtime
 * uses SipHash; FNV-1a is sufficient for the parser's dictionary needs.
 */

#include "n00b.h"
#include "core/string.h"
#include "core/buffer.h"

/**
 * @brief FNV-1a hash over a raw byte buffer, producing a 128-bit value.
 *
 * We compute a 64-bit FNV-1a hash and then replicate it into
 * a 128-bit value with a second round using a different offset basis,
 * so that the full n00b_hash_value_t (unsigned __int128) space is used.
 */
static inline n00b_hash_value_t
n00b_hash_raw(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    // First 64-bit FNV-1a pass.
    uint64_t h1 = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h1 ^= p[i];
        h1 *= 0x100000001b3ULL;
    }

    // Second 64-bit FNV-1a pass with a different offset basis.
    uint64_t h2 = 0x6c62272e07bb0142ULL;
    for (size_t i = 0; i < len; i++) {
        h2 ^= p[i];
        h2 *= 0x100000001b3ULL;
    }

    return ((n00b_hash_value_t)h2 << 64) | (n00b_hash_value_t)h1;
}

/**
 * @brief Hash a NUL-terminated C string.
 * @param value Pointer to the C string (cast from void* for API compat).
 * @return 128-bit hash value.
 */
static inline n00b_hash_value_t
n00b_hash_cstring(void *value)
{
    const char *s   = (const char *)value;
    size_t      len = s ? strlen(s) : 0;
    return n00b_hash_raw(s, len);
}

/**
 * @brief Hash a single machine word (pointer-sized value).
 * @param value Pointer to the word to hash.
 * @return 128-bit hash value.
 */
static inline n00b_hash_value_t
n00b_hash_word(void *value)
{
    uintptr_t w = (uintptr_t)value;
    return n00b_hash_raw(&w, sizeof(w));
}

/**
 * @brief Hash an n00b_string_t by its UTF-8 content.
 * @param s The string to hash.
 * @return 128-bit hash value.
 */
static inline n00b_hash_value_t
n00b_string_hash(n00b_string_t s)
{
    return n00b_hash_raw(s.data, s.u8_bytes);
}

/**
 * @brief Hash an n00b_buffer_t by its raw byte content.
 * @param b Pointer to the buffer.
 * @return 128-bit hash value.
 */
static inline n00b_hash_value_t
n00b_buffer_hash(n00b_buffer_t *b)
{
    if (!b || !b->data) {
        return 0;
    }
    return n00b_hash_raw(b->data, b->byte_len);
}

/**
 * @brief Hash an object using a caller-supplied hash function.
 * @param obj Object to hash.
 * @param fn  Hash function to invoke on @p obj.
 * @return 128-bit hash value.
 */
static inline n00b_hash_value_t
n00b_hash(void *obj, n00b_hash_fn fn)
{
    if (fn) {
        return fn(obj);
    }
    return n00b_hash_word(obj);
}
