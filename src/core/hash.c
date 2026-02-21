// IWYU pragma: no_include <sys/_endian.h>

#define XXH_INLINE_ALL
#define XXH_NO_XXH3
#define N00B_HASH_INTERNAL

#include "n00b.h"

#if defined(__has_builtin)
#if __has_builtin(__builtin_bswap64)
#define n00b_bswap64(x) __builtin_bswap64((uint64_t)(x))
#endif
#endif

#if !defined(n00b_bswap64)
static inline uint64_t
n00b_bswap64(uint64_t x)
{
    return ((x & 0x00000000000000FFULL) << 56) | ((x & 0x000000000000FF00ULL) << 40)
         | ((x & 0x0000000000FF0000ULL) << 24) | ((x & 0x00000000FF000000ULL) << 8)
         | ((x & 0x000000FF00000000ULL) >> 8) | ((x & 0x0000FF0000000000ULL) >> 24)
         | ((x & 0x00FF000000000000ULL) >> 40) | ((x & 0xFF00000000000000ULL) >> 56);
}
#endif

#include <string.h> // IWYU pragma: keep ; for strlen
#include "core/alloc.h"
#include "core/alloc_mdata.h"
#include "core/hash.h"
#include "core/type_info.h"
#include "core/string.h"
#include "core/buffer.h"
#include "vendor/xxhash.h"

static inline n00b_uint128_t
n00b_xxh64_pair(const void *data, size_t len)
{
    uint64_t low  = XXH64(data, len, 0);
    uint64_t high = XXH64(data, len, 0x9E3779B185EBCA87ULL);

    return ((n00b_uint128_t)high << 64) | (n00b_uint128_t)low;
}

n00b_uint128_t
n00b_hash(void *obj, n00b_hash_fn fn)
{
    n00b_alloc_info_t ainfo = n00b_find_alloc_info(obj);
    bool              managed = (ainfo.kind == n00b_alloc_oob
                                 || ainfo.kind == n00b_alloc_inline);

    if (managed) {
        // Check cached hash.
        n00b_uint128_t cached = 0;

        if (ainfo.kind == n00b_alloc_inline) {
            cached = ainfo.hdr.in_line->cached_hash;
        }
        else {
            cached = ainfo.hdr.oob->cached_hash;
        }

        if (cached) {
            return cached;
        }

        // If the caller didn't supply a hash function, try the vtable.
        if (!fn) {
            n00b_vtable_entry vt_fn = n00b_obj_core_method(obj, N00B_BI_HASH);

            if (vt_fn) {
                fn = (n00b_hash_fn)vt_fn;
            }
        }
    }

    if (!fn) {
        fn = n00b_hash_word;
    }

    n00b_uint128_t hv = (*fn)(obj);

    // Cache the result for managed objects.
    if (managed) {
        if (ainfo.kind == n00b_alloc_inline) {
            ainfo.hdr.in_line->cached_hash = hv;
        }
        else {
            ainfo.hdr.oob->cached_hash = hv;
        }
    }

    return hv;
}

n00b_uint128_t
n00b_hash_word(void *value)
{
    n00b_word_t w = (n00b_word_t)value;

    return n00b_xxh64_pair(&w, sizeof(n00b_word_t));
}

n00b_uint128_t
n00b_hash_cstring(void *value)
{
    char *s = (char *)value;

    return n00b_xxh64_pair(s, strlen(s));
}

n00b_uint128_t
n00b_hash_raw(const void *data, size_t len)
{
    return n00b_xxh_convert(XXH3_128bits(data, len));
}

n00b_uint128_t
n00b_string_hash(n00b_string_t s)
{
    if (!s.u8_bytes || !s.data) {
        return n00b_hash_word(0ULL);
    }

    return n00b_xxh64_pair(s.data, s.u8_bytes);
}

n00b_uint128_t
n00b_buffer_hash(n00b_buffer_t *b)
{
    if (!b || !b->byte_len) {
        return n00b_hash_word(0ULL);
    }

    return n00b_xxh64_pair(b->data, b->byte_len);
}
