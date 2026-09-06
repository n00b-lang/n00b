#include "lib/hash.h"
#include "lib/buffer.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#define XXH_INLINE_ALL
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

static_assert(XXH_VERSION_NUMBER >= 800,
              "XXH3 is only value-stable from xxHash 0.8.0 onward");

uint64_t
ncc_xxh3_64bits_raw(const void *data, size_t len)
{
    return XXH3_64bits(data, len);
}

ncc_uint128_t
ncc_xxh3_128bits_raw(const void *data, size_t len)
{
    XXH128_hash_t hv = XXH3_128bits(data, len);

    return ((ncc_uint128_t)hv.high64 << 64) | (ncc_uint128_t)hv.low64;
}

ncc_hash_value_t
ncc_hash_raw(const void *data, size_t len)
{
    return ncc_xxh3_64bits_raw(data, len);
}

ncc_hash_value_t
ncc_hash_cstring(void *value)
{
    const char *s = (const char *)value;

    return ncc_hash_raw(s, s ? strlen(s) : 0);
}

ncc_hash_value_t
ncc_hash_word(void *value)
{
    uintptr_t w = (uintptr_t)value;

    return ncc_hash_raw(&w, sizeof(w));
}

ncc_hash_value_t
ncc_string_hash(ncc_string_t s)
{
    return ncc_hash_raw(s.data, s.u8_bytes);
}

ncc_hash_value_t
ncc_buffer_hash(ncc_buffer_t *b)
{
    if (!b || !b->data) {
        return 0;
    }

    return ncc_hash_raw(b->data, b->byte_len);
}

ncc_hash_value_t
ncc_hash(void *obj, ncc_hash_fn fn)
{
    if (fn) {
        return fn(obj);
    }

    return ncc_hash_word(obj);
}
