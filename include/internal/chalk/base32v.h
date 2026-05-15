#pragma once

/** @file base32v.h — nimutils' "v" base32 alphabet.
 *
 *  Omits I, L, O, U. Standard 5-bit-per-char algorithm, big-endian bit
 *  ordering, no padding. */

#include <n00b.h>

/** Encode `data` of length `len` into the v-alphabet. Returns a fresh
 *  ASCII string. */
n00b_string_t *n00b_chalk_base32v_encode(const uint8_t *data, size_t len);

/** Format the first 20 chars of a base32v of `sha256` (32 raw bytes)
 *  as `XXXXXX-XXXX-XXXX-XXXXXX`, the chalk CHALK_ID / METADATA_ID
 *  layout. */
n00b_string_t *n00b_chalk_id_format_sha256(const uint8_t sha256[32]);
