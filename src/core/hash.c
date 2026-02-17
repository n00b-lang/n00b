// IWYU pragma: no_include <sys/_endian.h>

#define XXH_INLINE_ALL
#define N00B_HASH_INTERNAL

#include "n00b.h"

#if defined __linux__
#include <byteswap.h>
#define n00b_bswap64(x) bswap_64(x)
#else
#define n00b_bswap64(x) htonll(x)
#endif

#include <string.h> // IWYU pragma: keep ; for strlen
#include "core/alloc.h"
#include "core/hash.h"
#include "vendor/xxhash.h"

static inline n00b_uint128_t
n00b_xxh_convert(XXH128_hash_t hv)
{
    union {
        XXH128_hash_t  xxh;
        n00b_uint128_t n00b;
    } u = {.xxh = hv};

    return u.n00b;
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

    return n00b_xxh_convert(XXH3_128bits(&w, sizeof(n00b_word_t)));
}

n00b_uint128_t
n00b_hash_cstring(void *value)
{
    char *s = (char *)value;

    return n00b_xxh_convert(XXH3_128bits(s, strlen(s)));
}

#if 0 // TODO
n00b_uint128_t
n00b_string_hash(n00b_string_t *s)
{
    if (!s || !s->u8_bytes) {
        return n00b_hash_word(0ULL);
    }

    return n00b_xxh_convert(XXH3_128bits(s->data, s->u8_bytes));
}

n00b_uint128_t
n00b_buffer_hash(n00b_buf_t *b)
{
    if (!b || !b->byte_len) {
        return n00b_hash_word(0ULL);
    }

    return n00b_xxh_convert(XXH3_128bits(b->data, b->byte_len));
}

#endif
