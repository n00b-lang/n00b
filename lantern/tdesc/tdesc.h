#pragma once

#include <stdint.h>

typedef struct th_ctx  th_ctx;
typedef struct th_type th_type;

typedef struct tdesc_t {
    th_type *identity;

    uint64_t hash;

    void *layout;

    void *ext;
} tdesc_t;
