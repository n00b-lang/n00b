#pragma once

#include "core.h"

struct core_string_t {
    char  *data;
    size_t u8_bytes;
};

#define CORE_STRING_STATIC(lit) \
    ((core_string_t){.data = (char *)("" lit), .u8_bytes = sizeof(lit) - 1})

extern core_string_t core_string_from_raw(const char *src, int64_t byte_len);
extern core_string_t core_string_from_cstr(const char *src);
extern char         *core_string_to_cstr(core_string_t s);
extern core_string_t core_string_borrow(const char *src, size_t byte_len);
extern core_string_t core_string_dup(core_string_t s);
extern core_string_t core_string_empty(void);
extern core_string_t core_string_slice(core_string_t s, size_t start,
                                       size_t byte_len);
extern bool         core_string_eq(core_string_t a, core_string_t b);
extern int          core_string_cmp(core_string_t a, core_string_t b);
extern bool         core_string_starts_with(core_string_t s,
                                            core_string_t prefix);
extern bool         core_string_ends_with(core_string_t s,
                                          core_string_t suffix);
extern int64_t      core_string_find_byte(core_string_t s, char byte);
extern int64_t      core_string_rfind_byte(core_string_t s, char byte);
extern int64_t      core_string_find(core_string_t s, core_string_t needle);
extern bool         core_string_parse_i64(core_string_t s, int base,
                                          int64_t *out, size_t *consumed);
extern bool         core_string_parse_u64(core_string_t s, int base,
                                          uint64_t *out, size_t *consumed);
extern bool         core_string_parse_f64(core_string_t s, double *out,
                                          size_t *consumed);
extern long long    core_string_strtoll(core_string_t s, int base);
extern unsigned long long core_string_strtoull(core_string_t s, int base);
