#pragma once

/** @file sidecar_internal.h — shared sidecar primitives.
 *
 *  Both the model-sidecar codec (sidecar.c) and the cert codec
 *  (certs.c) share this implementation. The only meaningful
 *  difference between them is the codec id recorded in
 *  extract_result.codec; the same helpers handle both. */

#include "n00b.h"
#include "core/buffer.h"
#include "parsers/json.h"
#include "adt/dict.h"
#include "chalk/n00b_chalk.h"

/** SHA-256 a buffer into 32 raw bytes. */
n00b_buffer_t *n00b_chalk_sha256_buffer(n00b_buffer_t *in);

/** Convert a parsed JSON OBJECT into the typed mark dict. */
n00b_dict_t(n00b_string_t *, n00b_json_node_t *) *
n00b_chalk_json_object_to_dict(n00b_json_node_t *root);

/** Parse `sidecar_bytes` (UTF-8 JSON) into an extract_result with
 *  source = SIDECAR and codec = `codec`. */
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_sidecar_parse_bytes(n00b_buffer_t        *sidecar_bytes,
                                   n00b_chalk_codec_id_t codec);

/** Insert: ignore input, finalize mark, return sidecar bytes. */
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_sidecar_insert_impl(n00b_buffer_t *bytes,
                                   n00b_chalk_mark_t *mark);

/** Delete: return empty bytes with sidecar discriminator (signals
 *  the caller to remove the sidecar file). */
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_sidecar_delete_impl(n00b_buffer_t *bytes);

/** Plain sha256 of the artifact bytes. */
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_sidecar_hash_impl(n00b_buffer_t *bytes);
