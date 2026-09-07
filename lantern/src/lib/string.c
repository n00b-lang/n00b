#include "lib/string.h"
#include "lib/alloc.h"
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
