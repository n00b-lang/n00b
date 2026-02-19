#include "core/string.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include <string.h>

n00b_string_t
n00b_string_from_raw(n00b_allocator_t *allocator, const char *src,
                     int64_t byte_len, int64_t cp_count)
{
    n00b_string_t result = {0};

    n00b_ensure_allocator(allocator);
    result.data = n00b_alloc_array(char, (size_t)byte_len + 1,
                                   .allocator = allocator);
    if (byte_len > 0 && src) {
        memcpy(result.data, src, (size_t)byte_len);
    }
    result.data[byte_len] = '\0';
    result.u8_bytes       = byte_len;
    result.codepoints     = cp_count;

    return result;
}

n00b_string_t
n00b_string_empty(n00b_allocator_t *allocator)
{
    return n00b_string_from_raw(allocator, "", 0, 0);
}
