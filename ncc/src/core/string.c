// string.c — Minimal n00b_string_t implementations.

#include "core/string.h"
#include "core/alloc.h"
#include <string.h>
#include <stdlib.h>

n00b_string_t
n00b_string_from_raw(const char *src, int64_t byte_len)
{
    n00b_string_t s = {0};

    if (!src || byte_len <= 0) {
        return s;
    }

    s.data = (char *)calloc(1, (size_t)byte_len + 1);
    memcpy(s.data, src, (size_t)byte_len);
    s.u8_bytes   = (size_t)byte_len;
    s.codepoints = (size_t)byte_len; // Approximate; fine for parser.
    s.styling    = NULL;

    return s;
}

n00b_string_t
n00b_string_from_cstr(const char *src)
{
    n00b_string_t s = {0};

    if (!src) {
        return s;
    }

    size_t len = strlen(src);

    s.data = (char *)calloc(1, len + 1);
    memcpy(s.data, src, len);
    s.u8_bytes   = len;
    s.codepoints = len; // Approximate.
    s.styling    = NULL;

    return s;
}

n00b_string_t
n00b_string_empty(void)
{
    n00b_string_t s = {0};

    s.data       = (char *)calloc(1, 1);
    s.u8_bytes   = 0;
    s.codepoints = 0;
    s.styling    = NULL;

    return s;
}

bool
n00b_string_eq(n00b_string_t a, n00b_string_t b)
{
    if (a.u8_bytes != b.u8_bytes) {
        return false;
    }

    if (!a.data && !b.data) {
        return true;
    }

    if (!a.data || !b.data) {
        return false;
    }

    return memcmp(a.data, b.data, a.u8_bytes) == 0;
}
