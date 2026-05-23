// IWYU pragma: no_include <sys/_endian.h>

#define XXH_INLINE_ALL
#define N00B_HASH_INTERNAL
#ifdef _WIN32
#define XXH_VECTOR 0
#endif

#include "n00b.h"

#if defined __linux__
#include <byteswap.h>
#define n00b_bswap64(x) bswap_64(x)
#elif defined(_WIN32)
#define n00b_bswap64(x) __builtin_bswap64(x)
#else
#define n00b_bswap64(x) htonll(x)
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
    n00b_alloc_info_t ainfo = n00b_find_alloc_info(obj);
    bool managed            = (ainfo.kind == n00b_alloc_oob || ainfo.kind == n00b_alloc_inline);

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
            n00b_option_t(n00b_vtable_entry) vt_opt =
                n00b_obj_core_method(obj, N00B_BI_HASH);

            if (n00b_option_is_set(vt_opt)) {
                fn = (n00b_hash_fn)n00b_option_get(vt_opt);
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

    return n00b_xxh_convert(XXH3_128bits(&w, sizeof(n00b_word_t)));
}

n00b_uint128_t
n00b_hash_cstring(void *value)
{
    char *s = (char *)value;

    return n00b_xxh_convert(XXH3_128bits(s, strlen(s)));
}

n00b_uint128_t
n00b_hash_raw(const void *data, size_t len)
{
    return n00b_xxh_convert(XXH3_128bits(data, len));
}

n00b_uint128_t
n00b_string_hash(void *key)
{
    n00b_string_t *s = (n00b_string_t *)key;

    if (!s || !s->u8_bytes || !s->data) {
        return n00b_hash_word(0ULL);
    }

    return n00b_xxh_convert(XXH3_128bits(s->data, s->u8_bytes));
}

n00b_uint128_t
n00b_buffer_hash(n00b_buffer_t *b)
{
    if (!b || !b->byte_len) {
        return n00b_hash_word(0ULL);
    }

    return n00b_xxh_convert(XXH3_128bits(b->data, b->byte_len));
}
