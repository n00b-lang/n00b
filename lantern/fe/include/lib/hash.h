#pragma once

#include "ncc.h"
#include "lib/string.h"
#include "lib/buffer.h"

extern uint64_t      ncc_xxh3_64bits_raw(const void *data, size_t len);
extern ncc_uint128_t ncc_xxh3_128bits_raw(const void *data, size_t len);

extern ncc_hash_value_t ncc_hash_raw(const void *data, size_t len);
extern ncc_hash_value_t ncc_hash_cstring(void *value);
extern ncc_hash_value_t ncc_hash_word(void *value);
extern ncc_hash_value_t ncc_string_hash(ncc_string_t s);
extern ncc_hash_value_t ncc_buffer_hash(ncc_buffer_t *b);
extern ncc_hash_value_t ncc_hash(void *obj, ncc_hash_fn fn);
