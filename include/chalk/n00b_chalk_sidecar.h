#pragma once

/** @file n00b_chalk_sidecar.h — Sidecar codec for artifact kinds that
 *  cannot be marked in-band (model weights such as .onnx/.bin, etc.).
 *
 *  Insert writes a `<path>.chalk` sibling file; extract reads it back.
 *  All operations return a result struct with `kind ==
 *  N00B_CHALK_OUT_SIDECAR` to make the asymmetric I/O explicit. */

#include <n00b.h>
#include <chalk/n00b_chalk_codec.h>

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_sidecar_insert_buffer(n00b_buffer_t *bytes,
                                     n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_sidecar_delete_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_sidecar_extract_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_sidecar_hash_buffer(n00b_buffer_t *bytes);

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_sidecar_insert_file(n00b_string_t *path,
                                   n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_sidecar_delete_file(n00b_string_t *path);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_sidecar_extract_file(n00b_string_t *path);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_sidecar_hash_file(n00b_string_t *path);

/** Parse a sidecar file's bytes directly (no artifact involved). */
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_sidecar_extract_sidecar_buffer(n00b_buffer_t *sidecar_bytes);
