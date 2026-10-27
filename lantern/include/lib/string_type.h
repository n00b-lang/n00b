#pragma once

#include <stddef.h>

typedef struct core_string_t core_string_t;

struct core_string_t {
    char  *data;
    size_t u8_bytes;
};
