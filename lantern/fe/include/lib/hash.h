#pragma once

#include "ncc.h"
#include "lib/string.h"
#include "lib/buffer.h"

static inline ncc_hash_value_t
ncc_hash_raw(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    uint64_t h1 = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h1 ^= p[i];
        h1 *= 0x100000001b3ULL;
    }

    uint64_t h2 = 0x6c62272e07bb0142ULL;
    for (size_t i = 0; i < len; i++) {
        h2 ^= p[i];
        h2 *= 0x100000001b3ULL;
    }

    return ((ncc_hash_value_t)h2 << 64) | (ncc_hash_value_t)h1;
}

static inline ncc_hash_value_t
ncc_hash_cstring(void *value)
{
    const char *s   = (const char *)value;
    size_t      len = s ? strlen(s) : 0;
    return ncc_hash_raw(s, len);
}

static inline ncc_hash_value_t
ncc_hash_word(void *value)
{
    uintptr_t w = (uintptr_t)value;
    return ncc_hash_raw(&w, sizeof(w));
}

static inline ncc_hash_value_t
ncc_string_hash(ncc_string_t s)
{
    return ncc_hash_raw(s.data, s.u8_bytes);
}

static inline ncc_hash_value_t
ncc_buffer_hash(ncc_buffer_t *b)
{
    if (!b || !b->data) {
        return 0;
    }
    return ncc_hash_raw(b->data, b->byte_len);
}

static inline ncc_hash_value_t
ncc_hash(void *obj, ncc_hash_fn fn)
{
    if (fn) {
        return fn(obj);
    }
    return ncc_hash_word(obj);
}
