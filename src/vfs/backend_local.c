#include "vfs/backend_local.h"
#include "core/alloc.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

// ============================================================================
// Internal context
// ============================================================================

typedef struct {
    n00b_string_t *root;
} local_ctx_t;

// ============================================================================
// Path helpers
// ============================================================================

/**
 * Join root directory and relative path with a '/'.
 * Returns a heap-allocated NUL-terminated C string.
 * Handles empty @p rel as "the root directory itself".
 */
static char *
join_path(local_ctx_t *lc, n00b_string_t *rel)
{
    size_t rlen = lc->root->u8_bytes;
    size_t plen = rel->u8_bytes;

    // Empty relative path -> return root directory (no trailing slash).
    if (plen == 0) {
        char *buf = n00b_alloc_array(char, rlen + 1);
        memcpy(buf, lc->root->data, rlen);
        buf[rlen] = '\0';
        return buf;
    }

    // root + '/' + path + '\0'
    size_t total = rlen + 1 + plen + 1;
    char  *buf   = n00b_alloc_array(char, total);

    memcpy(buf, lc->root->data, rlen);
    buf[rlen] = '/';
    memcpy(buf + rlen + 1, rel->data, plen);
    buf[rlen + 1 + plen] = '\0';

    return buf;
}

static n00b_err_t
errno_to_vfs_err(int e)
{
    switch (e) {
    case ENOENT:  return N00B_VFS_ERR_NOT_FOUND;
    case EEXIST:  return N00B_VFS_ERR_EXISTS;
    case EISDIR:  return N00B_VFS_ERR_IS_DIR;
    case ENOTDIR: return N00B_VFS_ERR_NOT_DIR;
    case EACCES:
    case EPERM:   return N00B_VFS_ERR_PERMISSION;
    case ENOSPC:  return N00B_VFS_ERR_NO_SPACE;
    case EXDEV:   return N00B_VFS_ERR_CROSS_DEVICE;
    default:      return N00B_VFS_ERR_IO;
    }
}

// ============================================================================
// Vtable implementations
// ============================================================================

static n00b_string_t *
local_name(void)
{
    return n00b_string_from_cstr("local");
}

static void *
local_init(n00b_vfs_backend_t *be)
{
    local_ctx_t *lc = n00b_alloc(local_ctx_t);
    lc->root = be->root;
    return lc;
}

static void
local_cleanup(void *ctx)
{
    (void)ctx;
}

static n00b_result_t(n00b_buffer_t *)
local_get(void *ctx, n00b_string_t *path)
{
    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, path);

    int fd = open(full, O_RDONLY);
    if (fd < 0) {
        return n00b_result_err(n00b_buffer_t *, errno_to_vfs_err(errno));
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        int e = errno;
        close(fd);
        return n00b_result_err(n00b_buffer_t *, errno_to_vfs_err(e));
    }

    size_t         size = (size_t)st.st_size;
    n00b_buffer_t *buf  = n00b_buffer_new((int64_t)size);

    if (size > 0) {
        n00b_buffer_resize(buf, size);
        char *dst = n00b_buffer_to_c(buf, nullptr);

        size_t  total = 0;
        while (total < size) {
            ssize_t r = read(fd, dst + total, size - total);
            if (r <= 0) {
                break;
            }
            total += (size_t)r;
        }
    }

    close(fd);
    return n00b_result_ok(n00b_buffer_t *, buf);
}

static n00b_result_t(n00b_buffer_t *)
local_get_range(void *ctx, n00b_string_t *path, uint64_t offset,
                uint64_t length)
{
    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, path);

    int fd = open(full, O_RDONLY);
    if (fd < 0) {
        return n00b_result_err(n00b_buffer_t *, errno_to_vfs_err(errno));
    }

    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        int e = errno;
        close(fd);
        return n00b_result_err(n00b_buffer_t *, errno_to_vfs_err(e));
    }

    n00b_buffer_t *buf = n00b_buffer_new((int64_t)length);
    n00b_buffer_resize(buf, length);
    char *dst = n00b_buffer_to_c(buf, nullptr);

    size_t total = 0;
    while (total < length) {
        ssize_t r = read(fd, dst + total, length - total);
        if (r <= 0) {
            break;
        }
        total += (size_t)r;
    }

    // Shrink if we read less than requested.
    if (total < length) {
        n00b_buffer_resize(buf, total);
    }

    close(fd);
    return n00b_result_ok(n00b_buffer_t *, buf);
}

static n00b_result_t(bool)
local_put(void *ctx, n00b_string_t *path, n00b_buffer_t *data)
{
    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, path);

    int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return n00b_result_err(bool, errno_to_vfs_err(errno));
    }

    int64_t len;
    char   *src = n00b_buffer_to_c(data, &len);

    size_t total = 0;
    while (total < (size_t)len) {
        ssize_t w = write(fd, src + total, (size_t)len - total);
        if (w < 0) {
            int e = errno;
            close(fd);
            return n00b_result_err(bool, errno_to_vfs_err(e));
        }
        total += (size_t)w;
    }

    close(fd);
    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
local_del(void *ctx, n00b_string_t *path)
{
    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, path);

    if (unlink(full) < 0) {
        return n00b_result_err(bool, errno_to_vfs_err(errno));
    }

    return n00b_result_ok(bool, true);
}

static n00b_result_t(n00b_vfs_obj_stat_t)
local_stat(void *ctx, n00b_string_t *path)
{
    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, path);

    struct stat st;
    if (stat(full, &st) < 0) {
        return n00b_result_err(n00b_vfs_obj_stat_t, errno_to_vfs_err(errno));
    }

    n00b_vfs_obj_kind_t kind;
    if (S_ISDIR(st.st_mode)) {
        kind = N00B_VFS_OBJ_DIR;
    }
    else if (S_ISLNK(st.st_mode)) {
        kind = N00B_VFS_OBJ_SYMLINK;
    }
    else {
        kind = N00B_VFS_OBJ_FILE;
    }

#ifdef __APPLE__
    uint64_t atime_ns = (uint64_t)st.st_atimespec.tv_sec * 1000000000ULL
                      + (uint64_t)st.st_atimespec.tv_nsec;
    uint64_t mtime_ns = (uint64_t)st.st_mtimespec.tv_sec * 1000000000ULL
                      + (uint64_t)st.st_mtimespec.tv_nsec;
    uint64_t ctime_ns = (uint64_t)st.st_ctimespec.tv_sec * 1000000000ULL
                      + (uint64_t)st.st_ctimespec.tv_nsec;
#else
    uint64_t atime_ns = (uint64_t)st.st_atim.tv_sec * 1000000000ULL
                      + (uint64_t)st.st_atim.tv_nsec;
    uint64_t mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ULL
                      + (uint64_t)st.st_mtim.tv_nsec;
    uint64_t ctime_ns = (uint64_t)st.st_ctim.tv_sec * 1000000000ULL
                      + (uint64_t)st.st_ctim.tv_nsec;
#endif

    return n00b_result_ok(n00b_vfs_obj_stat_t, ((n00b_vfs_obj_stat_t){
        .kind     = kind,
        .size     = (uint64_t)st.st_size,
        .atime_ns = atime_ns,
        .mtime_ns = mtime_ns,
        .ctime_ns = ctime_ns,
        .mode     = st.st_mode & 07777,
    }));
}

static n00b_result_t(n00b_vfs_list_result_t *)
local_list(void *ctx, n00b_string_t *prefix, n00b_string_t *continuation,
           uint32_t max_keys)
{
    (void)continuation;

    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, prefix);

    DIR *dp = opendir(full);
    if (dp == nullptr) {
        return n00b_result_err(n00b_vfs_list_result_t *,
                               errno_to_vfs_err(errno));
    }

    // Single-pass: collect entries into a growable array.
    uint32_t cap = 32;
    uint32_t ix  = 0;
    n00b_vfs_list_entry_t *entries = n00b_alloc_array(n00b_vfs_list_entry_t, cap);
    bool truncated = false;

    struct dirent *ent;
    size_t flen = strlen(full);

    while ((ent = readdir(dp)) != nullptr) {
        if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0'
            || (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
            continue;
        }

        if (max_keys > 0 && ix >= max_keys) {
            truncated = true;
            break;
        }

        // Grow array if needed.
        if (ix >= cap) {
            uint32_t new_cap = cap * 2;
            n00b_vfs_list_entry_t *new_arr =
                n00b_alloc_array(n00b_vfs_list_entry_t, new_cap);
            memcpy(new_arr, entries, ix * sizeof(n00b_vfs_list_entry_t));
            entries = new_arr;
            cap     = new_cap;
        }

        entries[ix].name = n00b_string_from_cstr(ent->d_name);

        // Stat each entry for metadata.
        size_t nlen  = strlen(ent->d_name);
        char  *epath = n00b_alloc_array(char, flen + 1 + nlen + 1);
        memcpy(epath, full, flen);
        epath[flen] = '/';
        memcpy(epath + flen + 1, ent->d_name, nlen);
        epath[flen + 1 + nlen] = '\0';

        struct stat st;
        if (stat(epath, &st) == 0) {
            entries[ix].kind = S_ISDIR(st.st_mode)
                                   ? N00B_VFS_OBJ_DIR
                                   : N00B_VFS_OBJ_FILE;
            entries[ix].size = (uint64_t)st.st_size;
#ifdef __APPLE__
            entries[ix].mtime_ns =
                (uint64_t)st.st_mtimespec.tv_sec * 1000000000ULL
                + (uint64_t)st.st_mtimespec.tv_nsec;
#else
            entries[ix].mtime_ns =
                (uint64_t)st.st_mtim.tv_sec * 1000000000ULL
                + (uint64_t)st.st_mtim.tv_nsec;
#endif
        }

        ix++;
    }

    closedir(dp);

    n00b_vfs_list_result_t *res = n00b_alloc(n00b_vfs_list_result_t);
    res->entries      = (ix > 0) ? entries : nullptr;
    res->count        = ix;
    res->continuation = nullptr;
    res->truncated    = truncated;

    return n00b_result_ok(n00b_vfs_list_result_t *, res);
}

static n00b_result_t(bool)
local_rename(void *ctx, n00b_string_t *old_path, n00b_string_t *new_path)
{
    local_ctx_t *lc       = ctx;
    char        *old_full = join_path(lc, old_path);
    char        *new_full = join_path(lc, new_path);

    if (rename(old_full, new_full) < 0) {
        return n00b_result_err(bool, errno_to_vfs_err(errno));
    }

    return n00b_result_ok(bool, true);
}

static n00b_result_t(bool)
local_mkdir(void *ctx, n00b_string_t *path)
{
    local_ctx_t *lc   = ctx;
    char        *full = join_path(lc, path);

    if (mkdir(full, 0755) < 0) {
        return n00b_result_err(bool, errno_to_vfs_err(errno));
    }

    return n00b_result_ok(bool, true);
}

static bool
local_supports_range_read(void *ctx)
{
    (void)ctx;
    return true;
}

static bool
local_supports_rename(void *ctx)
{
    (void)ctx;
    return true;
}

static bool
local_supports_link(void *ctx)
{
    (void)ctx;
    return true;
}

static n00b_result_t(bool)
local_link(void *ctx, n00b_string_t *target, n00b_string_t *link_path)
{
    local_ctx_t *lc        = ctx;
    char        *tgt_full  = join_path(lc, target);
    char        *link_full = join_path(lc, link_path);

    if (link(tgt_full, link_full) < 0) {
        return n00b_result_err(bool, errno_to_vfs_err(errno));
    }

    return n00b_result_ok(bool, true);
}

// ============================================================================
// Vtable
// ============================================================================

const n00b_vfs_backend_ops_t n00b_vfs_backend_local_ops = {
    .name                = local_name,
    .init                = local_init,
    .cleanup             = local_cleanup,
    .get                 = local_get,
    .get_range           = local_get_range,
    .put                 = local_put,
    .del                 = local_del,
    .stat                = local_stat,
    .list                = local_list,
    .rename              = local_rename,
    .mkdir               = local_mkdir,
    .supports_range_read = local_supports_range_read,
    .supports_rename     = local_supports_rename,
    .supports_link       = local_supports_link,
    .link                = local_link,
};

// ============================================================================
// Constructor
// ============================================================================

n00b_result_t(n00b_vfs_backend_t *)
n00b_vfs_backend_local_new(n00b_string_t *root_dir)
{
    if (root_dir == nullptr) {
        return n00b_result_err(n00b_vfs_backend_t *, N00B_VFS_ERR_NULL_ARG);
    }

    // Verify root exists and is a directory.
    struct stat st;
    // NUL-terminate for stat().
    size_t rlen = root_dir->u8_bytes;
    char  *cstr = n00b_alloc_array(char, rlen + 1);
    memcpy(cstr, root_dir->data, rlen);
    cstr[rlen] = '\0';

    if (stat(cstr, &st) < 0 || !S_ISDIR(st.st_mode)) {
        return n00b_result_err(n00b_vfs_backend_t *, N00B_VFS_ERR_NOT_DIR);
    }

    n00b_vfs_backend_t *be = n00b_alloc(n00b_vfs_backend_t);
    be->ops  = &n00b_vfs_backend_local_ops;
    be->root = root_dir;

    n00b_result_t(bool) r = n00b_vfs_backend_init(be);
    if (n00b_result_is_err(r)) {
        return n00b_result_err(n00b_vfs_backend_t *, n00b_result_get_err(r));
    }

    return n00b_result_ok(n00b_vfs_backend_t *, be);
}
