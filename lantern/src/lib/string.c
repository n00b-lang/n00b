#include "lib/string.h"
#include "lib/alloc.h"
#include <stdlib.h>
#include <string.h>

core_string_t
core_string_from_raw(const char *src, int64_t byte_len)
{
    core_string_t s = {0};

    if (!src || byte_len <= 0) {
        return s;
    }

    s.data = (char *)core_alloc_size(1, (size_t)byte_len + 1);
    memcpy(s.data, src, (size_t)byte_len);
    s.u8_bytes = (size_t)byte_len;

    return s;
}

core_string_t
core_string_from_cstr(const char *src)
{
    core_string_t s = {0};

    if (!src) {
        return s;
    }

    size_t len = strlen(src);

    s.data = (char *)core_alloc_size(1, len + 1);
    memcpy(s.data, src, len);
    s.u8_bytes = len;

    return s;
}

core_string_t
core_string_borrow(const char *src, size_t byte_len)
{
    return (core_string_t){.data = (char *)src, .u8_bytes = byte_len};
}

core_string_t
core_string_dup(core_string_t s)
{
    if (!s.data) {
        return s;
    }

    if (s.u8_bytes == 0) {
        return core_string_empty();
    }

    return core_string_from_raw(s.data, (int64_t)s.u8_bytes);
}

core_string_t
core_string_empty(void)
{
    core_string_t s = {0};

    s.data = (char *)core_alloc_size(1, 1);

    return s;
}

bool
core_string_eq(core_string_t a, core_string_t b)
{
    if (a.u8_bytes != b.u8_bytes) {
        return false;
    }

    if (!a.data || !b.data) {
        return a.data == b.data;
    }

    return memcmp(a.data, b.data, a.u8_bytes) == 0;
}

static char *
bounded_copy(core_string_t s, char *small, size_t small_size)
{
    char *text = s.u8_bytes < small_size ? small
                                         : core_alloc_size(1, s.u8_bytes + 1);
    if (s.u8_bytes && s.data) {
        memcpy(text, s.data, s.u8_bytes);
    }
    text[s.data ? s.u8_bytes : 0] = '\0';
    return text;
}

long long
core_string_strtoll(core_string_t s, int base)
{
    char small[64];
    char *text = bounded_copy(s, small, sizeof(small));
    long long value = strtoll(text, nullptr, base);
    if (text != small) {
        core_free(text);
    }
    return value;
}

unsigned long long
core_string_strtoull(core_string_t s, int base)
{
    char small[64];
    char *text = bounded_copy(s, small, sizeof(small));
    unsigned long long value = strtoull(text, nullptr, base);
    if (text != small) {
        core_free(text);
    }
    return value;
}
