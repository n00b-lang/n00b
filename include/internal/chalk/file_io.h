#pragma once

/** @file file_io.h — shared file-mode helper for chalk codecs.
 *
 *  All file-mode codec entry points share the same shape:
 *   - Read the artifact bytes at `path` into a buffer.
 *   - Run the corresponding buffer-mode op.
 *   - For IN_BAND results, write the bytes back to `path`. For
 *     SIDECAR results, write to `path` + suffix (typically
 *     ".chalk"). For HASH/EXTRACT, no write.
 *
 *  This header exposes the read/write helpers; per-codec _file
 *  entry points are written as one-line forwarders. */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "chalk/n00b_chalk.h"

/** Read every byte at `path` into a fresh buffer. */
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_read_file(n00b_string_t *path);

/** Write `bytes` to `path`, truncating any prior contents. */
n00b_result_t(bool)
    n00b_chalk_write_file(n00b_string_t *path, n00b_buffer_t *bytes);

/** Read the sidecar file at `path` + sidecar suffix (".chalk"). */
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_read_sidecar(n00b_string_t *path);

/** Write `bytes` as the sidecar for `path`. */
n00b_result_t(bool)
    n00b_chalk_write_sidecar(n00b_string_t *path, n00b_buffer_t *bytes);

/** Delete the sidecar for `path`. */
n00b_result_t(bool)
    n00b_chalk_delete_sidecar(n00b_string_t *path);

/* Composers used by codec wrappers. The codec passes its own buffer-
 * mode entry points; the helpers handle the I/O glue. */

typedef n00b_result_t(n00b_chalk_io_result_t *)
    (*n00b_chalk_buf_io_fn_t)(n00b_buffer_t *, n00b_chalk_mark_t *);
typedef n00b_result_t(n00b_chalk_io_result_t *)
    (*n00b_chalk_buf_io_unary_t)(n00b_buffer_t *);
typedef n00b_result_t(n00b_chalk_extract_result_t *)
    (*n00b_chalk_buf_extract_fn_t)(n00b_buffer_t *);
typedef n00b_result_t(n00b_buffer_t *)
    (*n00b_chalk_buf_hash_fn_t)(n00b_buffer_t *);

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_file_insert_via(n00b_string_t        *path,
                               n00b_chalk_mark_t    *mark,
                               n00b_chalk_buf_io_fn_t buf_insert);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_file_delete_via(n00b_string_t            *path,
                               n00b_chalk_buf_io_unary_t buf_delete);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_file_extract_via(n00b_string_t              *path,
                                n00b_chalk_buf_extract_fn_t buf_extract);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_file_hash_via(n00b_string_t            *path,
                             n00b_chalk_buf_hash_fn_t  buf_hash);

/** Streaming SHA-256 of a file. Reads the file in fixed-size chunks
 *  (no full-file buffer) and returns the 32 raw digest bytes.
 *
 *  Use this from a codec's `_hash_file` entry point whenever the
 *  codec's unchalked hash is "sha256 over the file contents" — i.e.
 *  the codec doesn't need to parse / transform / reshape the bytes
 *  to compute the canonical-unchalked form. Applies to certs,
 *  sidecar, and elf_fallback today. */
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_hash_file_stream(n00b_string_t *path);
