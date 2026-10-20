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
core_xxh3_64bits_raw(const void *data, size_t len)
{
    return XXH3_64bits(data, len);
}

core_uint128_t
core_xxh3_128bits_raw(const void *data, size_t len)
{
    XXH128_hash_t hv = XXH3_128bits(data, len);

    return ((core_uint128_t)hv.high64 << 64) | (core_uint128_t)hv.low64;
}

core_hash_value_t
core_hash_raw(const void *data, size_t len)
{
    return core_xxh3_64bits_raw(data, len);
}

core_hash_value_t
core_hash_word(void *value)
{
    uintptr_t w = (uintptr_t)value;

    return core_hash_raw(&w, sizeof(w));
}

core_hash_value_t
core_string_hash(core_string_t s)
{
    return core_hash_raw(s.data, s.u8_bytes);
}

core_hash_value_t
core_buffer_hash(core_buffer_t *b)
{
    if (!b || !b->data) {
        return 0;
    }

    return core_hash_raw(b->data, b->byte_len);
}

core_hash_value_t
core_hash(void *obj, core_hash_fn fn)
{
    if (fn) {
        return fn(obj);
    }

    return core_hash_word(obj);
}
