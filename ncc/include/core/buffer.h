#pragma once

#include "n00b.h"
#include "core/alloc.h"

struct n00b_buffer_t {
    char  *data;
    size_t byte_len;
    size_t alloc_len;
};

static inline n00b_buffer_t *
n00b_buffer_empty(void)
{
    n00b_buffer_t *buf = n00b_alloc(n00b_buffer_t);
    return buf;
}

static inline n00b_buffer_t *
n00b_buffer_from_cstr(const char *s)
{
    n00b_buffer_t *buf = n00b_alloc(n00b_buffer_t);
    size_t len = strlen(s);

    buf->data      = (char *)calloc(1, len + 1);
    memcpy(buf->data, s, len);
    buf->byte_len  = len;
    buf->alloc_len = len + 1;

    return buf;
}

static inline n00b_buffer_t *
n00b_buffer_from_bytes(const char *data, int64_t len)
{
    n00b_buffer_t *buf = n00b_alloc(n00b_buffer_t);

    buf->data      = (char *)calloc(1, (size_t)len + 1);
    memcpy(buf->data, data, (size_t)len);
    buf->byte_len  = (size_t)len;
    buf->alloc_len = (size_t)len + 1;

    return buf;
}

static inline void
n00b_buffer_free(n00b_buffer_t *buf)
{
    if (!buf) {
        return;
    }

    free(buf->data);
    free(buf);
}
