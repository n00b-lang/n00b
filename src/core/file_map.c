/*
 * file_map.c — implementation of n00b_file_mmap / n00b_file_mmap_advise.
 *
 * Wraps an mmap'd region in an n00b_buffer_t. The buffer carries the
 * N00B_BUF_F_MMAP flag, which n00b_buffer_free reads to dispatch to
 * munmap(2) instead of n00b_free on collection.
 */

#include "n00b.h"
#include "core/file_map.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/string.h"

#include <errno.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

n00b_result_t(n00b_buffer_t *)
n00b_file_mmap(n00b_string_t *path) _kargs
{
    bool writable = false;
    bool populate = false;
}
{
    if (!path || !path->data) {
        return n00b_result_err(n00b_buffer_t *, EINVAL);
    }
#ifdef _WIN32
    (void)writable;
    (void)populate;
    return n00b_result_err(n00b_buffer_t *, ENOSYS);
#else
    const char *cpath = (const char *)path->data;

    int oflags = writable ? O_RDWR : O_RDONLY;
    int fd     = open(cpath, oflags);
    if (fd < 0) {
        return n00b_result_err(n00b_buffer_t *, errno);
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        return n00b_result_err(n00b_buffer_t *, e);
    }

    n00b_buffer_t *buf = n00b_alloc(n00b_buffer_t);

    if (st.st_size == 0) {
        // Empty file — close the fd and return a regular empty buffer.
        // No mapping is needed; the resulting buffer behaves like any
        // other empty n00b_buffer_t (and is freed via n00b_free, not
        // munmap).
        close(fd);
        n00b_buffer_init(buf, .length = 0);
        return n00b_result_ok(n00b_buffer_t *, buf);
    }

    int prot  = PROT_READ | (writable ? PROT_WRITE : 0);
    int mflag = writable ? MAP_SHARED : MAP_PRIVATE;
#ifdef MAP_POPULATE
    if (populate) {
        mflag |= MAP_POPULATE;
    }
#else
    (void)populate;
#endif

    void *addr = mmap(NULL, (size_t)st.st_size, prot, mflag, fd, 0);
    int   merr = errno;
    close(fd);
    if (addr == MAP_FAILED) {
        return n00b_result_err(n00b_buffer_t *, merr);
    }

    // Initialize the buffer to alias the mapping. We can't use
    // n00b_buffer_init's .ptr path here because it leaves alloc_len
    // at zero (correct) but doesn't flag the buffer as mmap-backed —
    // and we need n00b_buffer_free to call munmap, not n00b_free.
    buf->data      = (char *)addr;
    buf->byte_len  = (size_t)st.st_size;
    buf->alloc_len = 0;
    buf->allocator = nullptr;
    buf->flags     = N00B_BUF_F_MMAP;
    buf->lock      = n00b_data_lock_new();

    return n00b_result_ok(n00b_buffer_t *, buf);
#endif
}

void
n00b_file_mmap_advise(n00b_buffer_t *buf, n00b_file_mmap_advice_t advice)
{
    if (!buf || !buf->data || !(buf->flags & N00B_BUF_F_MMAP)) {
        return;
    }
#ifdef _WIN32
    (void)advice;
#else
    int a;
    switch (advice) {
    case N00B_MMAP_ADVICE_SEQUENTIAL:
        a = MADV_SEQUENTIAL;
        break;
    case N00B_MMAP_ADVICE_RANDOM:
        a = MADV_RANDOM;
        break;
    case N00B_MMAP_ADVICE_WILLNEED:
        a = MADV_WILLNEED;
        break;
    case N00B_MMAP_ADVICE_DONTNEED:
        a = MADV_DONTNEED;
        break;
    case N00B_MMAP_ADVICE_NORMAL:
    default:
        a = MADV_NORMAL;
        break;
    }
    (void)madvise(buf->data, buf->byte_len, a);
#endif
}
