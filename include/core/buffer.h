/**
 * @file buffer.h
 * @brief Mutable growable byte buffer with thread-safe access.
 *
 * `n00b_buffer_t` is a length-prefixed mutable buffer for building data.
 * Supports concatenation, slicing, searching, hex encoding/decoding,
 * and conversion to `n00b_string_t`.
 *
 * Thread safety is provided by a per-buffer spinlock.  All public
 * mutating operations acquire the lock; read-only operations also
 * lock to ensure a consistent snapshot.
 *
 * ### Usage
 *
 * ```c
 * n00b_buffer_t *b = n00b_buffer_empty();
 * n00b_buffer_resize(b, 1024);
 * n00b_buffer_set_index(b, 0, 0x42);
 * n00b_buffer_t *hex = n00b_buffer_from_bytes("\xde\xad", 2);
 * n00b_string_t *s   = n00b_buffer_to_hex_str(hex);
 * n00b_buffer_free(b);
 * n00b_buffer_free(hex);
 * ```
 *
 * ### Related modules
 *
 * - `core/alloc.h` -- allocation API used for all buffer memory
 * - `core/string.h` -- string type returned by conversion functions
 * - `core/option.h` / `core/result.h` -- used for fallible returns
 */
#pragma once

#include "n00b.h"
#include "core/alloc.h"
#include "core/atomic.h"
#include "core/align.h"
#include "core/option.h"
#include "core/result.h"
#include "core/string.h"
#include "util/utf8.h"

// ============================================================================
// Constants
// ============================================================================

/** @brief Minimum allocation size for empty buffers. */
#define N00B_EMPTY_BUFFER_ALLOC 16

// ============================================================================
// Error codes
// ============================================================================

/** @brief Buffer index out of bounds. */
#define N00B_ERR_BUFFER_INDEX_OOB  (-1)

/** @brief Buffer slice out of bounds. */
#define N00B_ERR_BUFFER_SLICE_OOB  (-2)

/** @brief Invalid buffer initializer arguments. */
#define N00B_ERR_BUFFER_BAD_INIT   (-3)

// ============================================================================
// Type declarations for option/result returns
// ============================================================================

n00b_option_decl(int64_t);
n00b_option_decl(size_t);
n00b_result_decl(uint8_t);
n00b_result_decl(bool);

// ============================================================================
// Struct definition
// ============================================================================

/**
 * @brief Mutable growable byte buffer.
 *
 * @c data points to the backing store (allocated separately).
 * @c byte_len is the number of valid bytes; @c alloc_len is the
 * allocated capacity (always a power of 2).
 *
 * All access is protected by a spinlock.
 */
struct n00b_buffer_t {
    char             *data;
    size_t            byte_len;
    size_t            alloc_len;
    n00b_spin_lock_t  lock;
    n00b_allocator_t *allocator;
    int32_t           flags;
};

// ============================================================================
// Lock macros (internal)
// ============================================================================

/** @internal Acquire spinlock (busy-wait). */
#define _n00b_buffer_lock(b)                                                                   \
    while (n00b_atomic_or(&(b)->lock, 1) != 0)                                                 \
        ;

/** @internal Release spinlock. */
#define _n00b_buffer_unlock(b) n00b_atomic_store(&(b)->lock, 0)

/**
 * @internal Acquire write lock with automatic release via defer.
 * @pre `n00b_enable_defer()` must be in scope.
 */
#define n00b_buffer_acquire_w(b)                                                               \
    {                                                                                          \
        _n00b_buffer_lock(b);                                                                  \
        n00b_defer(_n00b_buffer_unlock(b));                                                    \
    }

/**
 * @internal Acquire read lock with automatic release via defer.
 * @pre `n00b_enable_defer()` must be in scope.
 */
#define n00b_buffer_acquire_r(b)                                                               \
    {                                                                                          \
        _n00b_buffer_lock(b);                                                                  \
        n00b_defer(_n00b_buffer_unlock(b));                                                    \
    }

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Initialize a buffer from various sources.
 *
 * Exactly one of `raw`, `hex`, or `ptr` may be given (or none, for a
 * sized empty buffer).  `length` is required unless `hex` is given.
 *
 * @param buf  Buffer to initialize (typically freshly allocated).
 *
 * @kw length    Byte capacity (-1 = infer from hex).
 * @kw raw       Raw bytes to copy into the buffer.
 * @kw hex       Hex-encoded string to decode.
 * @kw ptr       Adopt an existing data pointer (no copy).
 * @kw allocator Allocator to use for internal allocations.
 *
 * @pre @p buf is non-nullptr and uninitialized.
 * @post @p buf is ready for use; lock is initialized.
 */
extern void
n00b_buffer_init(n00b_buffer_t *buf) _kargs
{
    int64_t           length    = -1;
    char             *raw       = nullptr;
    n00b_string_t    *hex       = nullptr;
    char             *ptr       = nullptr;
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Concatenate two buffers, returning a new buffer.
 *
 * If either argument is nullptr, returns the other (or nullptr if both are nullptr).
 *
 * @param b1 First buffer.
 * @param b2 Second buffer.
 * @return   New buffer containing `b1 || b2`.
 *
 * @post Returned buffer is independently allocated.
 */
extern n00b_buffer_t *
n00b_buffer_add(n00b_buffer_t *b1, n00b_buffer_t *b2) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Return the number of valid bytes.
 * @param buf Buffer to query.
 */
extern n00b_size_t n00b_buffer_len(n00b_buffer_t *buf);

/**
 * @brief Resize the buffer to @p new_sz bytes.
 *
 * If growing, existing data is preserved and new bytes are zero-filled.
 * If shrinking, the allocation may stay the same size but @c byte_len
 * is reduced.
 *
 * @param buf    Buffer to resize.
 * @param new_sz New byte length.
 *
 * @pre @p buf is non-nullptr.
 * @post `n00b_buffer_len(buf) == new_sz`.
 */
extern void n00b_buffer_resize(n00b_buffer_t *buf, uint64_t new_sz);

/**
 * @brief Search for a sub-buffer within a buffer.
 *
 * Returns the byte offset of the first occurrence, or none if not found.
 *
 * @param buf Buffer to search in.
 * @param sub Sub-buffer to search for.
 *
 * @kw start Starting byte offset (default 0).
 * @kw end   Ending byte offset (default none, meaning end of buffer).
 *
 * @return `n00b_option_t(int64_t)` -- set to offset if found, none otherwise.
 */
extern n00b_option_t(int64_t)
n00b_buffer_find(n00b_buffer_t *buf, n00b_buffer_t *sub) _kargs
{
    n00b_option_t(size_t) start = n00b_option_set(size_t, 0);
    n00b_option_t(size_t) end   = n00b_option_none(size_t);
};

/**
 * @brief Get a single byte by index (supports negative indexing).
 *
 * @param buf Buffer to index into.
 * @param n   Byte index (negative counts from end).
 * @return    `n00b_result_t(uint8_t)` -- ok with byte, or err on OOB.
 */
extern n00b_result_t(uint8_t) n00b_buffer_get_index(n00b_buffer_t *buf, int64_t n);

/**
 * @brief Set a single byte by index (supports negative indexing).
 *
 * @param buf Buffer to modify.
 * @param n   Byte index (negative counts from end).
 * @param c   Byte value to write.
 * @return    `n00b_result_t(bool)` -- ok(true) on success, or err on OOB.
 */
extern n00b_result_t(bool) n00b_buffer_set_index(n00b_buffer_t *buf, int64_t n, uint8_t c);

/**
 * @brief Extract a sub-buffer (supports negative indexing).
 *
 * @param buf   Source buffer.
 * @param start Start byte index.
 * @param end   End byte index (exclusive; negative counts from end).
 * @return      New buffer with the slice data.
 *
 * @kw allocator Allocator for the new buffer (nullptr = runtime default).
 */
extern n00b_buffer_t *
n00b_buffer_get_slice(n00b_buffer_t *buf, int64_t start, int64_t end) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Replace a slice of a buffer with another buffer's contents.
 *
 * @param buf   Buffer to modify.
 * @param start Start byte index of the region to replace.
 * @param end   End byte index (exclusive) of the region to replace.
 * @param val   Replacement buffer (may be nullptr to just delete).
 * @return      `n00b_result_t(bool)` -- ok(true) on success, or err on OOB.
 */
extern n00b_result_t(bool)
n00b_buffer_set_slice(n00b_buffer_t *buf, int64_t start, int64_t end, n00b_buffer_t *val);

/**
 * @brief Deep-copy a buffer.
 *
 * @param buf Buffer to copy.
 * @return    New independently-allocated buffer with identical contents.
 *
 * @kw allocator Allocator for the new buffer (nullptr = runtime default).
 */
extern n00b_buffer_t *
n00b_buffer_copy(n00b_buffer_t *buf) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Get a raw C pointer to the buffer data.
 *
 * @param buf     Buffer to access.
 * @param len_ptr If non-nullptr, receives the byte length.
 * @return        Pointer to the internal data (not a copy).
 *
 * @pre The caller ensures no concurrent mutation.
 */
extern char *n00b_buffer_to_c(n00b_buffer_t *buf, int64_t *len_ptr);

/**
 * @brief Convert buffer contents to a string.
 *
 * Copies the bytes into a new `n00b_string_t`.  No UTF-8 validation
 * is performed; the byte count is used for both `u8_bytes` and
 * `codepoints` (to be refined when the string subsystem is ported).
 *
 * @param buf Buffer to convert.
 * @return    New string with the buffer data.
 */
extern n00b_string_t n00b_buffer_to_string(n00b_buffer_t *buf);

/**
 * @brief Hex-encode the buffer contents into a string.
 *
 * @param buf Buffer to encode.
 * @return    New string with lowercase hex digits ("deadbeef").
 */
extern n00b_string_t n00b_buffer_to_hex_str(n00b_buffer_t *buf);

/**
 * @brief Join an array of buffers with a separator.
 *
 * @param items  Array of buffer pointers.
 * @param count  Number of elements in @p items.
 * @param joiner Separator buffer (may be nullptr for no separator).
 * @return       New buffer with all items joined.
 *
 * @pre @p count > 0 and @p items[0..count-1] are valid.
 */
extern n00b_buffer_t *
n00b_buffer_join(n00b_buffer_t **items, size_t count,
                 n00b_buffer_t *joiner) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Create a buffer containing the UTF-8 encoding of a codepoint.
 *
 * @param cp Unicode codepoint.
 * @return   New buffer with 1--4 bytes of UTF-8.
 */
extern n00b_buffer_t *
n00b_buffer_from_codepoint(n00b_codepoint_t cp) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Free a buffer's data and zero the struct.
 *
 * @param buf Buffer to free.
 * @post @p buf->data is nullptr, @p buf->byte_len is 0.
 */
extern void n00b_buffer_free(n00b_buffer_t *buf);

// ============================================================================
// Inline convenience constructors
// ============================================================================

/**
 * @brief Allocate an empty buffer with minimal capacity.
 *
 * @kw allocator Allocator for the buffer (nullptr = runtime default).
 *
 * @return New empty buffer.
 */
static inline n00b_buffer_t *
n00b_buffer_empty() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_buffer_t *buf = n00b_alloc(n00b_buffer_t, .allocator = allocator);

    n00b_buffer_init(buf, .length = 0, .allocator = allocator);
    return buf;
}

/**
 * @brief Create a buffer from a raw byte array.
 *
 * @param bytes Pointer to source bytes (copied).
 * @param len   Number of bytes.
 * @return      New buffer with the data.
 *
 * @kw allocator Allocator for the buffer (nullptr = runtime default).
 *
 * @pre @p bytes is non-nullptr when @p len > 0.
 */
static inline n00b_buffer_t *
n00b_buffer_from_bytes(char *bytes, int64_t len) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_buffer_t *buf = n00b_alloc(n00b_buffer_t, .allocator = allocator);

    n00b_buffer_init(buf, .raw = bytes, .length = len, .allocator = allocator);
    return buf;
}
