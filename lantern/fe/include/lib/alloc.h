#pragma once

#include "ncc.h"
#include "lib/macros.h"
#include "lib/align.h"

static inline void *
_ncc_calloc(size_t count, size_t size)
{
    void *p = calloc(count, size);

    if (!p) {
        abort();
    }

    return p;
}

#define ncc_alloc(T) \
    ((T *)_ncc_calloc(1, sizeof(T)))

#define ncc_alloc_array(T, N) \
    ((T *)_ncc_calloc((size_t)(N) ? (size_t)(N) : 1, sizeof(T)))

#define ncc_alloc_size(n, sz) \
    _ncc_calloc((size_t)(n) ? (size_t)(n) : 1, (size_t)(sz) ? (size_t)(sz) : 1)

#define ncc_alloc_flex(T1, T2, N2) \
    _ncc_calloc(1, sizeof(T1) + sizeof(T2) * (size_t)(N2))

static inline void *
ncc_realloc(void *ptr, size_t sz)
{
    void *p = realloc(ptr, sz);

    if (!p) {
        abort();
    }

    return p;
}

static inline void
ncc_free(void *ptr)
{
    free(ptr);
}

static inline char *
ncc_strndup(const char *s, size_t n)
{
    char *d = (char *)_ncc_calloc(1, n + 1);

    memcpy(d, s, n);

    return d;
}

static inline char *
ncc_strdup(const char *s)
{
    return s ? ncc_strndup(s, strlen(s)) : nullptr;
}
