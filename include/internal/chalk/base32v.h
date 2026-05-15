#pragma once

/** @file base32v.h — nimutils' "v" base32 alphabet.
 *
 *  Omits I, L, O, U. Standard 5-bit-per-char algorithm, big-endian bit
 *  ordering, no padding. */

#include <n00b.h>

/** Encode `data` of length `len` into the v-alphabet. Returns a fresh
 *  ASCII string. */
n00b_string_t *n00b_chalk_base32v_encode(const uint8_t *data, size_t len);

/** Format the first 20 chars of a base32v as
 *  `XXXXXX-XXXX-XXXX-XXXXXX`. The two callers — CHALK_ID and
 *  METADATA_ID — encode different inputs:
 *
 *    CHALK_ID:    base32v of the 64-char lowercase hex string of
 *                 the unchalked artifact's SHA-256 (chalk codecs'
 *                 getUnchalkedHash returns a hex string, and
 *                 defaultChalkId calls idFormat on it directly).
 *    METADATA_ID: base32v of the 32 raw SHA-256 bytes of the
 *                 normalized mark (chalkjson.nim:520 computes
 *                 the raw digest and passes it to idFormat).
 *
 *  Both helpers below produce the canonical 23-char dashed form. */
n00b_string_t *n00b_chalk_id_format_sha256_bytes(const uint8_t sha256[32]);
n00b_string_t *n00b_chalk_id_format_sha256_hex  (const uint8_t sha256[32]);
