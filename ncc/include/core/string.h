#pragma once

#include "n00b.h"

struct n00b_string_t {
    char  *data;
    size_t u8_bytes;
    size_t codepoints;
    void  *styling;
};

#define N00B_STRING_STATIC(lit) \
    ((n00b_string_t){ .data = (char *)(lit), .u8_bytes = sizeof(lit) - 1, \
                      .codepoints = sizeof(lit) - 1, .styling = nullptr })

extern n00b_string_t n00b_string_from_raw(const char *src, int64_t byte_len);
extern n00b_string_t n00b_string_from_cstr(const char *src);
extern n00b_string_t n00b_string_empty(void);
extern bool          n00b_string_eq(n00b_string_t a, n00b_string_t b);
