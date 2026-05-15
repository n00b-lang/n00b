/**
 * @file macho_stream.c
 * @brief Stream constructors + free. Trimmed to just what chalk needs.
 *
 *  The original file had a `macho_stream_from_file(const char *path)`
 *  that opened the path via POSIX `open()`/`read()`. libchalk lifts
 *  this codec under n00b's no-libc policy and only needs the
 *  buffer-mode constructor; file I/O is handled by the n00b file API
 *  in the codec-level wrapper. The from-file path is dropped.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "internal/chalk/macho_stream.h"

#include <string.h>

macho_stream_t *
macho_stream_new(n00b_buffer_t *buf)
{
    if (!buf) {
        return NULL;
    }

    macho_stream_t *s = n00b_alloc(macho_stream_t);
    if (!s) {
        return NULL;
    }

    s->buf         = buf;
    s->pos         = 0;
    s->swap_endian = false;
    return s;
}

void
macho_stream_free(macho_stream_t *s)
{
    (void)s;
    /* GC handles cleanup. */
}
