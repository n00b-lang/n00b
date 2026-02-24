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
#include "core/hash.h"
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
// TODO -- new vtables
// TODO -- convert to new n00b_object_header
#if 0
    bool                          cache  = false;
    [[maybe_unused]] n00b_hash_fn obj_fn = n00b_hash_word;
    n00b_inline_hdr_t            *alloc  = n00b_get_object_header(obj);

    if (alloc && alloc->cached_hash) {
        return alloc->cached_hash;
    }


    if (alloc) {
        if (alloc->n00b_type) {
            n00b_vtable_t *vt = n00b_vtable_from_alloc(alloc);

            if (vt && vt->methods[N00B_BI_HASH]) {
                obj_fn = (n00b_hash_fn)vt->methods[N00B_BI_HASH];
            }
            if (!fn) {
                fn = obj_fn;
            }
        }

        if (!fn) {
            fn = obj_fn;
        }

        cache = obj_fn == fn;
    }
    else {
        cache = false;

        if (!fn) {
            fn = obj_fn;
        }
    }
#endif
    n00b_uint128_t hv = (*fn)(obj);

#if 0
    if (cache) {
        alloc->cached_hash = hv;

#endif

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
