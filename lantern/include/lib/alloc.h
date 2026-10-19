#pragma once

#include "core.h"
#include "lib/macros.h"
#include "lib/align.h"

static inline void *
_core_calloc(size_t count, size_t size)
{
    void *p = calloc(count, size);

    if (!p) {
        abort();
    }

    return p;
}

#define core_alloc(T) \
    ((T *)_core_calloc(1, sizeof(T)))

#define core_alloc_array(T, N) \
    ((T *)_core_calloc((size_t)(N) ? (size_t)(N) : 1, sizeof(T)))

#define core_alloc_size(n, sz) \
    _core_calloc((size_t)(n) ? (size_t)(n) : 1, (size_t)(sz) ? (size_t)(sz) : 1)

#define core_alloc_flex(T1, T2, N2) \
    _core_calloc(1, sizeof(T1) + sizeof(T2) * (size_t)(N2))

static inline void *
core_realloc(void *ptr, size_t sz)
{
    void *p = realloc(ptr, sz);

    if (!p) {
        abort();
    }

    return p;
}

static inline void *
core_try_realloc(void *ptr, size_t sz)
{
    return realloc(ptr, sz);
}

static inline void
core_free(void *ptr)
{
    free(ptr);
}

static inline char *
core_strndup(const char *s, size_t n)
{
    char *d = (char *)_core_calloc(1, n + 1);

    memcpy(d, s, n);

    return d;
}
