#pragma once

#include "n00b.h"
#include "core/macros.h"
#include "core/align.h"

// Allocation options — ignored in this extraction (single allocator = libc).
typedef struct {
    ncc_allocator_t *allocator;
} ncc_alloc_opts_t;

#define NCC_ALLOC_OPTS(...) nullptr

// Core allocation: calloc-based, zero-filled.
#define ncc_alloc(T, ...) \
    ((T *)calloc(1, sizeof(T)))

#define ncc_alloc_array(T, N, ...) \
    ((T *)calloc((size_t)(N) ? (size_t)(N) : 1, sizeof(T)))

#define ncc_alloc_size(n, sz, ...) \
    calloc((size_t)(n) ? (size_t)(n) : 1, (size_t)(sz) ? (size_t)(sz) : 1)

#define ncc_alloc_with_opts(T, opts, ...) \
    ((T *)calloc(1, sizeof(T)))

#define ncc_alloc_array_with_opts(T, N, opts, ...) \
    ((T *)calloc((size_t)(N) ? (size_t)(N) : 1, sizeof(T)))

#define ncc_alloc_size_with_opts(n, sz, opts, ...) \
    calloc((size_t)(n) ? (size_t)(n) : 1, (size_t)(sz) ? (size_t)(sz) : 1)

#define ncc_alloc_flex(T1, T2, N2, ...) \
    calloc(1, sizeof(T1) + sizeof(T2) * (size_t)(N2))

static inline void
ncc_free(void *ptr)
{
    free(ptr);
}
