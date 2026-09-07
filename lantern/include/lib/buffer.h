#pragma once

#include "core.h"
#include "lib/alloc.h"

#include <stdarg.h>
#include <stdio.h>

struct core_buffer_t {
    char  *data;
    size_t byte_len;
    size_t alloc_len;
};

static inline core_buffer_t *
core_buffer_empty(void)
{
    core_buffer_t *buf = core_alloc(core_buffer_t);
    return buf;
}

static inline core_buffer_t *
core_buffer_from_cstr(const char *s)
{
    core_buffer_t *buf = core_alloc(core_buffer_t);
    size_t len = strlen(s);

    buf->data      = (char *)core_alloc_size(1, len + 1);
    memcpy(buf->data, s, len);
    buf->byte_len  = len;
    buf->alloc_len = len + 1;

    return buf;
}

static inline core_buffer_t *
core_buffer_from_bytes(const char *data, int64_t len)
{
    core_buffer_t *buf = core_alloc(core_buffer_t);

    buf->data      = (char *)core_alloc_size(1, (size_t)len + 1);
    memcpy(buf->data, data, (size_t)len);
    buf->byte_len  = (size_t)len;
    buf->alloc_len = (size_t)len + 1;

    return buf;
}

static inline void
core_buffer_ensure(core_buffer_t *buf, size_t extra)
{
    size_t needed = buf->byte_len + extra;

    if (needed < buf->alloc_len) {
        return;
    }

    size_t new_alloc = buf->alloc_len ? buf->alloc_len : 64;

    while (new_alloc <= needed) {
        new_alloc *= 2;
    }

    buf->data      = (char *)core_realloc(buf->data, new_alloc);
    buf->alloc_len = new_alloc;
}

static inline void
core_buffer_append(core_buffer_t *buf, const char *data, size_t len)
{
    if (!data || !len) {
        return;
    }

    core_buffer_ensure(buf, len + 1);
    memcpy(buf->data + buf->byte_len, data, len);
    buf->byte_len += len;
    buf->data[buf->byte_len] = '\0';
}

static inline void
core_buffer_putc(core_buffer_t *buf, char c)
{
    core_buffer_ensure(buf, 2);
    buf->data[buf->byte_len++] = c;
    buf->data[buf->byte_len]   = '\0';
}

static inline void
core_buffer_puts(core_buffer_t *buf, const char *s)
{
    if (s) {
        core_buffer_append(buf, s, strlen(s));
    }
}

static inline void
core_buffer_vprintf(core_buffer_t *buf, const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);

    int needed = vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);

    if (needed < 0) {
        return;
    }

    core_buffer_ensure(buf, (size_t)needed + 1);

    va_copy(copy, ap);
    int written = vsnprintf(buf->data + buf->byte_len,
                            buf->alloc_len - buf->byte_len, fmt, copy);
    va_end(copy);

    if (written > 0) {
        buf->byte_len += (size_t)written;
    }
}

static inline void
core_buffer_printf(core_buffer_t *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    core_buffer_vprintf(buf, fmt, ap);
    va_end(ap);
}

static inline char *
core_buffer_take(core_buffer_t *buf)
{
    char *result = buf->data;

    if (!result) {
        result    = (char *)core_alloc_size(1, 1);
        result[0] = '\0';
    }

    core_free(buf);
    return result;
}

static inline void
core_buffer_release(core_buffer_t *buf)
{
    if (!buf) {
        return;
    }

    core_free(buf->data);
    buf->data      = nullptr;
    buf->byte_len  = 0;
    buf->alloc_len = 0;
}

static inline void
core_buffer_free(core_buffer_t *buf)
{
    if (!buf) {
        return;
    }

    core_buffer_release(buf);
    core_free(buf);
}
