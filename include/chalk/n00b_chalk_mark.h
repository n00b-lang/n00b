#pragma once

/**
 * @file n00b_chalk_mark.h
 * @brief Construction and serialization of chalk marks.
 *
 * libchalk emits exactly six keys: MAGIC, CHALK_ID, METADATA_ID,
 * HASH, CHALK_VERSION, TIMESTAMP_WHEN_CHALKED, and the optional
 * ATTESTATION pass-through. CHALK_ID, METADATA_ID, HASH,
 * CHALK_VERSION, and TIMESTAMP_WHEN_CHALKED are computed by
 * n00b_chalk_mark_finalize(); MAGIC is always present at index 0;
 * ATTESTATION is set by the caller and stored verbatim.
 */

#include <n00b.h>
#include "parsers/json.h"
#include "adt/dict.h"
#include "adt/result.h"
#include <chalk/n00b_chalk_codec.h>

#define N00B_CHALK_VERSION_STRING "2.0.0"
#define N00B_CHALK_MAGIC_STRING   "dadfedabbadabbed"

/** Allocate a fresh mark. Returns NULL on OOM (very unlikely with the
 *  n00b allocator). */
n00b_chalk_mark_t *n00b_chalk_mark_new(void);

/** Attach an ATTESTATION value. `value` is a JSON tree owned by the
 *  caller; libchalk takes ownership. Result payload is unused
 *  (`bool` per n00b convention for "result with no value"). */
n00b_result_t(bool) n00b_chalk_mark_set_attestation(n00b_chalk_mark_t *mark,
                                                    n00b_json_node_t *value);

/** Finalize the mark and return its canonical JSON bytes ready for
 *  embedding. `unchalked_sha256_32` must be a 32-byte buffer holding
 *  the codec-computed SHA-256 of the unchalked artifact. */
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_mark_finalize(n00b_chalk_mark_t *mark,
                             n00b_buffer_t     *unchalked_sha256_32);

/** Introspection: borrow the mark's underlying ordered dict. */
n00b_dict_t(n00b_string_t *, n00b_json_node_t *) *
    n00b_chalk_mark_as_dict(n00b_chalk_mark_t *mark);
