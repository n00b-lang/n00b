/**
 * @file file_map.h
 * @brief Memory-mapped file access via `n00b_buffer_t`.
 *
 * Wraps a file mapping in a regular `n00b_buffer_t *` so it composes
 * with every byte-oriented API in n00b (`n00b_bstream_new`,
 * `n00b_sha256_hash`, slicing, etc.) without copies. The mapping is
 * registered with the buffer GC via the `N00B_BUF_F_MMAP` flag —
 * collection runs `munmap(2)` instead of `n00b_free`. Buffers obtained
 * via `n00b_file_mmap` are read-only by default; mutation (resize,
 * concat, etc.) is not supported and asserts.
 *
 * Usage:
 * @code
 *     auto r = n00b_file_mmap(path);
 *     if (n00b_result_is_err(r)) return ...;
 *     n00b_buffer_t  *buf = n00b_result_get(r);
 *     n00b_bstream_t *bs  = n00b_bstream_new(buf);
 *     // ... parse without ever resident-loading the file ...
 * @endcode
 */
#pragma once

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/result.h"

/**
 * @brief madvise(2) access-pattern hints for an mmap'd buffer.
 */
typedef enum {
    N00B_MMAP_ADVICE_NORMAL,
    N00B_MMAP_ADVICE_SEQUENTIAL,  /**< Read once, forward (hashing, scanning). */
    N00B_MMAP_ADVICE_RANDOM,      /**< Random access (parsers). */
    N00B_MMAP_ADVICE_WILLNEED,    /**< Pre-fault the region. */
    N00B_MMAP_ADVICE_DONTNEED,    /**< Release pages back to the kernel. */
} n00b_file_mmap_advice_t;

/**
 * @brief Memory-map a file into an `n00b_buffer_t`.
 *
 * The returned buffer aliases the mapping — there is no copy. The GC
 * runs `munmap(2)` when the buffer becomes unreachable. Zero-byte
 * files succeed and yield an empty (non-mapped) buffer.
 *
 * On Windows the call currently fails with `ENOSYS`; mmap support
 * there will land with the WSAPoll backend.
 *
 * @param path  File path to map (NUL-terminated in @p path->data).
 *
 * @kw writable  Map as `PROT_READ|PROT_WRITE` + `MAP_SHARED` (writes
 *               flush to disk). Default: false (read-only,
 *               `MAP_PRIVATE`).
 * @kw populate  Linux: pass `MAP_POPULATE` so pages are pre-faulted.
 *               Use for sequential-hash workloads where you'll touch
 *               every page anyway. Default: false. Ignored elsewhere.
 *
 * @return Ok(buf) on success, Err(errno) on `open`/`fstat`/`mmap` failure.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_file_mmap(n00b_string_t *path) _kargs
{
    bool writable = false;
    bool populate = false;
};

/**
 * @brief Apply an `madvise(2)` hint to an mmap-backed buffer.
 *
 * No-op if @p buf is null, has no data, or is not mmap-backed. The
 * underlying `madvise` call is best-effort — failures are silently
 * ignored, matching POSIX semantics.
 */
extern void
n00b_file_mmap_advise(n00b_buffer_t *buf, n00b_file_mmap_advice_t advice);
