#pragma once

#include "core.h"
#include "lib/string.h"
#include "lib/buffer.h"

extern uint64_t      core_xxh3_64bits_raw(const void *data, size_t len);
extern core_uint128_t core_xxh3_128bits_raw(const void *data, size_t len);

extern core_hash_value_t core_hash_raw(const void *data, size_t len);
extern core_hash_value_t core_hash_word(void *value);
extern core_hash_value_t core_string_hash(core_string_t s);
extern core_hash_value_t core_buffer_hash(core_buffer_t *b);
extern core_hash_value_t core_hash(void *obj, core_hash_fn fn);
