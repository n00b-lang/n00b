#pragma once

#include "n00b.h"

extern n00b_uint128_t n00b_hash_word(void *);
extern n00b_uint128_t n00b_hash_cstring(void *);
extern n00b_uint128_t n00b_hash(void *, n00b_hash_fn);
// TODO: replace
// extern n00b_uint128_t n00b_string_hash(n00b_string_t *s);
// extern n00b_uint128_t n00b_buffer_hash(n00b_buf_t *b);
