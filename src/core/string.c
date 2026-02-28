#include "core/string.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "internal/text/unicode/raw.h"
#include <string.h>

n00b_string_t
n00b_string_from_raw(const char *src, int64_t byte_len)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
        int64_t          *cp_count  = nullptr;
    }
{
    (void)allocator;
    (void)cp_count;
    n00b_string_t result = {};

    n00b_ensure_allocator(kargs->allocator);
    result.data = n00b_alloc_array_with_opts(char,
                                             (size_t)byte_len + 1,
                                             &(n00b_alloc_opts_t){.allocator = kargs->allocator});
    if (byte_len > 0 && src) {
        memcpy(result.data, src, (size_t)byte_len);
    }
    result.data[byte_len] = '\0';
    result.u8_bytes       = byte_len;
    result.codepoints     = n00b_unicode_utf8_count_codepoints_raw(src,
                                                                    (uint32_t)byte_len);

    if (kargs->cp_count) {
        *kargs->cp_count = (int64_t)result.codepoints;
    }

    return result;
}

n00b_string_t
n00b_string_from_cstr(const char *src)
    _kargs { n00b_allocator_t *allocator = nullptr; }
{
    (void)allocator;
    if (!src) {
        return n00b_string_empty(.allocator = kargs->allocator);
    }

    const char *p = src;
    while (*p) {
        p++;
    }
    int64_t byte_len = (int64_t)(p - src);

    return n00b_string_from_raw(src, byte_len, .allocator = kargs->allocator);
}

n00b_string_t
n00b_string_empty()
    _kargs { n00b_allocator_t *allocator = nullptr; }
{
    (void)allocator;
    return n00b_string_from_raw("", 0, .allocator = kargs->allocator);
}
