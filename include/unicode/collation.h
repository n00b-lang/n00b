#pragma once

/** @file collation.h
 *  @brief Unicode collation (sort keys and locale-independent comparison).
 *
 *  Generates binary sort keys for efficient repeated comparisons and
 *  provides a direct collation comparison function.
 */

#include "unicode/types_ext.h"

/** @brief A binary sort key for efficient repeated comparisons. */
typedef struct {
    uint8_t *data; /**< Raw sort key bytes (binary-comparable) */
    uint32_t len;  /**< Length of the sort key in bytes */
} n00b_unicode_sort_key_t;

/** @brief Generate a binary sort key for a string.
 *  @param s  The string to generate a key for.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A sort key; free with n00b_unicode_sort_key_free().
 */
n00b_unicode_sort_key_t
n00b_unicode_sort_key(n00b_string_t s) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/** @brief Compare two strings using Unicode collation order.
 *  @param a  First string.
 *  @param b  Second string.
 *  @return Negative, zero, or positive (like strcmp).
 */
int n00b_unicode_collate(n00b_string_t a, n00b_string_t b);

/** @brief Free a sort key's allocated data.
 *  @param key  The sort key to free.
 */
void n00b_unicode_sort_key_free(n00b_unicode_sort_key_t *key);
