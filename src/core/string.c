#include "core/string.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "internal/text/unicode/raw.h"
#include <string.h>

void
n00b_string_init(n00b_string_t *self)
    _kargs {
        const char       *src       = nullptr;
        int64_t           byte_len  = -1;
        n00b_allocator_t *allocator = nullptr;
        int64_t          *cp_count  = nullptr;
    }
{
    (void)src;
    (void)byte_len;
    (void)allocator;
    (void)cp_count;

    n00b_ensure_allocator(kargs->allocator);

    int64_t len = kargs->byte_len;

    if (len < 0 && kargs->src) {
        const char *p = kargs->src;
        while (*p) {
            p++;
        }
        len = (int64_t)(p - kargs->src);
    }
    else if (len < 0) {
        len = 0;
    }

    self->data = n00b_alloc_array_with_opts(
        char,
        (size_t)len + 1,
        &(n00b_alloc_opts_t){.allocator = kargs->allocator});

    if (len > 0 && kargs->src) {
        memcpy(self->data, kargs->src, (size_t)len);
    }
    self->data[len] = '\0';
    self->u8_bytes  = len;
    self->codepoints =
        n00b_unicode_utf8_count_codepoints_raw(kargs->src, (uint32_t)len);

    if (kargs->cp_count) {
        *kargs->cp_count = (int64_t)self->codepoints;
    }
}

n00b_string_t *
n00b_string_from_raw(const char *src, int64_t byte_len)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
        int64_t          *cp_count  = nullptr;
    }
{
    (void)allocator;
    (void)cp_count;
    return n00b_new_kargs(n00b_string_t, string,
                          .src       = src,
                          .byte_len  = byte_len,
                          .allocator = kargs->allocator,
                          .cp_count  = kargs->cp_count);
}

n00b_string_t *
n00b_ncc_rstr(const char *src)
{
    return n00b_string_from_cstr(src);
}

n00b_string_t *
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

n00b_string_t *
n00b_string_empty()
    _kargs { n00b_allocator_t *allocator = nullptr; }
{
    (void)allocator;
    return n00b_string_from_raw("", 0, .allocator = kargs->allocator);
}
