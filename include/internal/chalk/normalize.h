#pragma once

/** @file normalize.h — Binary canonical normalization for METADATA_HASH.
 *
 *  Byte format matches chalk/src/normalize.nim exactly: type-tag +
 *  little-endian uint32/uint64 + UTF-8 payload. Outer wrapping is a
 *  table (\x05) excluding keys named MAGIC, SIGNATURE, SIGN_PARAMS. */

#include <n00b.h>

n00b_buffer_t *
    n00b_chalk_normalize(n00b_dict_t(n00b_string_t *, n00b_json_t *) *dict);
