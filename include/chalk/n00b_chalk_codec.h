#pragma once

/**
 * @file n00b_chalk_codec.h
 * @brief Codec identifiers, dispatcher entry points, and the
 *        operation-result wrapper types shared across codecs.
 */

#include <n00b.h>
#include "parsers/json.h"
#include "adt/dict.h"
#include "adt/result.h"

/** Identifies one of the supported codec implementations. */
typedef enum {
    N00B_CHALK_CODEC_NONE = 0,
    N00B_CHALK_CODEC_ELF,
    N00B_CHALK_CODEC_ELF_FALLBACK,
    N00B_CHALK_CODEC_MACHO,
    N00B_CHALK_CODEC_MACOS_WRAP,
    N00B_CHALK_CODEC_GGUF,
    N00B_CHALK_CODEC_SAFETENSORS,
    N00B_CHALK_CODEC_ZIP,
    N00B_CHALK_CODEC_PYC,
    N00B_CHALK_CODEC_SOURCE,
    N00B_CHALK_CODEC_SIDECAR_MODEL,
    N00B_CHALK_CODEC_SIDECAR_CERT,
} n00b_chalk_codec_id_t;

/** Discriminates artifact-bytes output (in-band) from sidecar-file
 *  output (e.g. model_sidecar, cert_sidecar). */
typedef enum {
    N00B_CHALK_OUT_IN_BAND,
    N00B_CHALK_OUT_SIDECAR,
} n00b_chalk_out_kind_t;

/** Result of an insert/delete operation. For in-band codecs the
 *  `bytes` field carries the rewritten artifact; for sidecar codecs
 *  it carries the sidecar JSON and the artifact bytes are unchanged. */
typedef struct {
    n00b_chalk_out_kind_t kind;
    n00b_buffer_t        *bytes;
    n00b_string_t        *sidecar_suffix;
} n00b_chalk_io_result_t;

/** Mark plus the source it was found in. */
typedef struct {
    n00b_chalk_codec_id_t codec;
    n00b_chalk_out_kind_t source;
    n00b_dict_t(n00b_string_t *, n00b_json_node_t *) * mark;
} n00b_chalk_extract_result_t;

/** Forward-declare the mark handle so codec headers can mention it
 *  without pulling in n00b_chalk_mark.h. */
typedef struct n00b_chalk_mark n00b_chalk_mark_t;

/** Detect the codec that owns the given bytes / path. `hint_path` may
 *  be NULL when no path information is available. */
n00b_chalk_codec_id_t n00b_chalk_detect_buffer(n00b_buffer_t *bytes,
                                               n00b_string_t *hint_path);
n00b_chalk_codec_id_t n00b_chalk_detect_file(n00b_string_t *path);

/* Dispatcher entry points. Each routes by detected codec and forwards
 * to the corresponding per-codec entry point. */
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_delete_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_extract_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_hash_buffer(n00b_buffer_t *bytes);

n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_extract_sidecar_buffer(n00b_buffer_t *sidecar_bytes);

n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_insert_file(n00b_string_t *path, n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_delete_file(n00b_string_t *path);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_extract_file(n00b_string_t *path);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_hash_file(n00b_string_t *path);
