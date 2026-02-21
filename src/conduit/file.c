/*
 * file.c - File-to-FD binding with managed I/O
 *
 * Opens files by name, wraps the FD via n00b_conduit_fd_manage(), and
 * creates a string-URI topic "file:<mode>:<path>" for identification.
 */

#include "conduit/conduit.h"
#include "conduit/file.h"
#include "conduit/io.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define N00B_FILE_OPEN(path, flags, mode)  _open((path), (flags) | _O_BINARY, (mode))
#define N00B_FILE_CLOSE(fd)                _close((fd))
#else
#include <fcntl.h>
#include <unistd.h>
#define N00B_FILE_OPEN(path, flags, mode)  open((path), (flags), (mode))
#define N00B_FILE_CLOSE(fd)                close((fd))
#endif

// ============================================================================
// Mode helpers
// ============================================================================

static int
mode_to_oflags(uint32_t mode)
{
    int oflags = 0;
    bool r     = (mode & N00B_CONDUIT_FILE_READ) != 0;
    bool w     = (mode & N00B_CONDUIT_FILE_WRITE) != 0;

    if (r && w) {
        oflags = O_RDWR;
    }
    else if (w) {
        oflags = O_WRONLY;
    }
    else {
        oflags = O_RDONLY;
    }

    if (mode & N00B_CONDUIT_FILE_CREATE) {
        oflags |= O_CREAT;
    }
    if (mode & N00B_CONDUIT_FILE_TRUNCATE) {
        oflags |= O_TRUNC;
    }
    if (mode & N00B_CONDUIT_FILE_APPEND) {
        oflags |= O_APPEND;
    }

    return oflags;
}

static const char *
mode_to_str(uint32_t mode)
{
    bool r = (mode & N00B_CONDUIT_FILE_READ) != 0;
    bool w = (mode & N00B_CONDUIT_FILE_WRITE) != 0;
    bool a = (mode & N00B_CONDUIT_FILE_APPEND) != 0;

    if (r && w && a) {
        return "ra";
    }
    if (r && w) {
        return "rw";
    }
    if (w && a) {
        return "a";
    }
    if (w) {
        return "w";
    }
    return "r";
}

// ============================================================================
// FD owner cleanup helper
// ============================================================================

static void
fd_owner_remove(n00b_conduit_t *c, int fd)
{
    n00b_dict_untyped_remove(&c->fd_owners, (void *)(intptr_t)fd);
}

// ============================================================================
// File API
// ============================================================================

n00b_result_t(n00b_conduit_file_t *)
n00b_conduit_file_open(n00b_conduit_t *c, n00b_conduit_io_backend_t *io,
                       const char *path, uint32_t mode)
{
    if (!c || !io || !path) {
        return n00b_result_err(n00b_conduit_file_t *, EINVAL);
    }

    int oflags = mode_to_oflags(mode);
    int fd     = N00B_FILE_OPEN(path, oflags, 0666);
    if (fd < 0) {
        return n00b_result_err(n00b_conduit_file_t *, errno);
    }

    // Create managed FD owner
    auto manage_r = n00b_conduit_fd_manage(c, io, fd, false);
    if (n00b_result_is_err(manage_r)) {
        N00B_FILE_CLOSE(fd);
        return n00b_result_err(n00b_conduit_file_t *, n00b_result_get_err(manage_r));
    }
    n00b_conduit_fd_owner_t *owner = n00b_result_get(manage_r);

    // Build URI string "file:<mode>:<path>"
    const char *mode_str = mode_to_str(mode);
    size_t uri_len       = 5 + strlen(mode_str) + 1 + strlen(path) + 1;
    char  *uri_buf       = n00b_alloc_array(char, uri_len);

    snprintf(uri_buf, uri_len, "file:%s:%s", mode_str, path);

    // Create string-URI topic
    n00b_string_t uri_str;
    uri_str.data     = uri_buf;
    uri_str.u8_bytes = uri_len - 1;

    n00b_result_t(n00b_conduit_topic_base_t *) res =
        n00b_conduit_topic_get(c, n00b_conduit_str_uri(uri_str), 0);
    if (n00b_result_is_err(res)) {
        N00B_FILE_CLOSE(fd);
        return n00b_result_err(n00b_conduit_file_t *, ENOMEM);
    }

    // Allocate file handle
    n00b_conduit_file_t *f = n00b_alloc(n00b_conduit_file_t);

    f->conduit    = c;
    f->io         = io;
    f->owner      = owner;
    f->file_topic = n00b_result_get(res);
    f->path       = n00b_string_from_raw(path, (int64_t)strlen(path));
    f->fd         = fd;
    f->mode       = mode;

    // For read-only files, activate reads immediately
    if ((mode & N00B_CONDUIT_FILE_READ) && !(mode & N00B_CONDUIT_FILE_WRITE)) {
        n00b_conduit_fd_activate_reads(owner);
    }

    return n00b_result_ok(n00b_conduit_file_t *, f);
}

void
n00b_conduit_file_close(n00b_conduit_file_t *f)
{
    if (!f) {
        return;
    }

    // Close the file topic
    if (f->file_topic) {
        n00b_conduit_topic_close(f->file_topic);
    }

    // Close the 4 managed FD sub-topics
    if (f->owner) {
        n00b_conduit_topic_close(f->owner->read_topic);
        n00b_conduit_topic_close(f->owner->write_topic);
        n00b_conduit_topic_close(f->owner->status_topic);
        n00b_conduit_topic_close(f->owner->wreq_topic);
    }

    // Unwatch from I/O backend
    n00b_conduit_io_unwatch(f->io, f->fd);

    // Remove fd_owner from hash table
    fd_owner_remove(f->conduit, f->fd);

    // Close the FD
    N00B_FILE_CLOSE(f->fd);
}

n00b_option_t(n00b_conduit_fd_owner_t *)
n00b_conduit_file_fd_owner(n00b_conduit_file_t *f)
{
    return n00b_option_from_nullable(n00b_conduit_fd_owner_t *,
                                     f ? f->owner : nullptr);
}

n00b_option_t(n00b_conduit_topic_base_t *)
n00b_conduit_file_topic(n00b_conduit_file_t *f)
{
    return n00b_option_from_nullable(n00b_conduit_topic_base_t *,
                                     f ? f->file_topic : nullptr);
}
