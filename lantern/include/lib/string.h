#pragma once

#include "core.h"

struct core_string_t {
    char  *data;
    size_t u8_bytes;
};

#define CORE_STRING_STATIC(lit) \
    ((core_string_t){.data = (char *)(lit), .u8_bytes = sizeof(lit) - 1})

extern core_string_t core_string_from_raw(const char *src, int64_t byte_len);
extern core_string_t core_string_from_cstr(const char *src);
extern core_string_t core_string_borrow(const char *src, size_t byte_len);
extern core_string_t core_string_dup(core_string_t s);
extern core_string_t core_string_empty(void);
extern bool         core_string_eq(core_string_t a, core_string_t b);
extern long long    core_string_strtoll(core_string_t s, int base);
extern unsigned long long core_string_strtoull(core_string_t s, int base);
