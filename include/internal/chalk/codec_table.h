#pragma once

/** @file codec_table.h — Per-codec dispatch table.
 *
 *  Each entry binds a codec identifier to its four operation
 *  callbacks. Functions are looked up by magic-byte detection in
 *  `src/chalk/codec_table.c`. */

#include <n00b.h>
#include "adt/result.h"
#include <chalk/n00b_chalk_codec.h>
#include <chalk/n00b_chalk_mark.h>

typedef struct {
    n00b_chalk_codec_id_t codec;
    n00b_result_t(n00b_chalk_io_result_t *) (*insert_buffer)(n00b_buffer_t *,
                                                             n00b_chalk_mark_t *);
    n00b_result_t(n00b_chalk_io_result_t *) (*delete_buffer)(n00b_buffer_t *);
    n00b_result_t(n00b_chalk_extract_result_t *) (*extract_buffer)(n00b_buffer_t *);
    n00b_result_t(n00b_buffer_t *) (*hash_buffer)(n00b_buffer_t *);
} n00b_chalk_codec_entry_t;

extern const n00b_chalk_codec_entry_t n00b_chalk_codec_table[];
extern const size_t                   n00b_chalk_codec_table_len;

/** Resolve a codec from raw bytes (and an optional path hint, used by
 *  codecs that need extension to disambiguate). */
n00b_chalk_codec_id_t
    n00b_chalk_codec_detect(n00b_buffer_t *bytes, n00b_string_t *hint_path);

/** Look up the dispatch entry for a codec. Returns NULL if `codec` is
 *  NONE or unrecognized. */
const n00b_chalk_codec_entry_t *
    n00b_chalk_codec_entry(n00b_chalk_codec_id_t codec);
