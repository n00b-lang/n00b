#pragma once
/**
 * base_alloc_shim.h - Bootstrap compatibility shim for base allocator API
 *
 * When building ncc-bootstrap with plain clang, we can't link libbase.a
 * (it requires ncc's typeid). This header provides inline wrappers that
 * map base_alloc/base_dealloc to stdlib malloc/free.
 *
 * The main ncc build uses real base/alloc.h instead.
 */

#include <stdlib.h>
#include <string.h>

static inline void *
base_alloc(size_t size)
{
    return malloc(size);
}

static inline void *
base_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}

static inline void *
base_realloc(void *ptr, size_t size)
{
    return realloc(ptr, size);
}

static inline void
base_dealloc(void *ptr)
{
    free(ptr);
}

static inline char *
base_strdup(const char *s)
{
    return strdup(s);
}
