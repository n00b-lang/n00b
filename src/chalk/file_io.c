/** @file src/chalk/file_io.c — VFS-backed file I/O for chalk codec
 *        file-mode entry points.
 *
 *  All file-mode operations run through a single process-wide VFS
 *  with the local-filesystem backend mounted at "/". The VFS is built
 *  lazily on first use. Sidecar codecs use a fixed ".chalk" suffix
 *  (matching the chalk-tool default). */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/alloc.h"
#include "core/sha256.h"
#include "vfs/vfs.h"
#include "vfs/backend_local.h"
#include "vfs/types.h"
#include "chalk/n00b_chalk.h"
#include "internal/chalk/file_io.h"

#include <string.h>

// -----------------------------------------------------------------------
// Lazy global VFS rooted at the host filesystem's root.
// -----------------------------------------------------------------------

static _Atomic(n00b_vfs_t *) s_vfs = nullptr;

static n00b_vfs_t *
get_vfs(void)
{
    n00b_vfs_t *v = n00b_atomic_load(&s_vfs);
    if (v) return v;

    auto root_str = n00b_string_from_cstr("/");
    auto br       = n00b_vfs_backend_local_new(root_str);
    if (n00b_result_is_err(br)) return nullptr;
    n00b_vfs_backend_t *be = n00b_result_get(br);

    auto vr = n00b_vfs_new();
    if (n00b_result_is_err(vr)) return nullptr;
    n00b_vfs_t *vfs = n00b_result_get(vr);

    auto mp = n00b_string_from_cstr("/");
    auto mr = n00b_vfs_mount(vfs, mp, be, 0);
    if (n00b_result_is_err(mr)) return nullptr;

    // First writer wins; lazy initialization is idempotent.
    n00b_vfs_t *expected = nullptr;
    atomic_compare_exchange_strong((_Atomic(n00b_vfs_t *) *)&s_vfs,
                                   &expected, vfs);
    return n00b_atomic_load(&s_vfs);
}

static n00b_string_t *
sidecar_path(n00b_string_t *path)
{
    size_t plen = path->u8_bytes;
    char  *buf  = n00b_alloc_array(char, plen + 6 + 1);
    memcpy(buf, path->data, plen);
    memcpy(buf + plen, ".chalk", 6);
    buf[plen + 6] = '\0';
    return n00b_string_from_raw(buf, (int64_t)(plen + 6));
}

// -----------------------------------------------------------------------
// Path-based read / write
// -----------------------------------------------------------------------

n00b_result_t(n00b_buffer_t *)
n00b_chalk_read_file(n00b_string_t *path)
{
    n00b_vfs_t *vfs = get_vfs();
    if (!vfs || !path) return n00b_result_err(n00b_buffer_t *, 1);

    auto sr = n00b_vfs_stat(vfs, path);
    if (n00b_result_is_err(sr)) return n00b_result_err(n00b_buffer_t *, 2);
    n00b_vfs_obj_stat_t st = n00b_result_get(sr);

    auto fr = n00b_vfs_open(vfs, path, N00B_VFS_O_R);
    if (n00b_result_is_err(fr)) return n00b_result_err(n00b_buffer_t *, 3);
    n00b_vfs_fh_t fh = n00b_result_get(fr);

    auto rr = n00b_vfs_read(vfs, fh, st.size);
    n00b_vfs_close(vfs, fh);
    if (n00b_result_is_err(rr)) return n00b_result_err(n00b_buffer_t *, 4);
    return n00b_result_ok(n00b_buffer_t *, n00b_result_get(rr));
}

n00b_result_t(bool)
n00b_chalk_write_file(n00b_string_t *path, n00b_buffer_t *bytes)
{
    n00b_vfs_t *vfs = get_vfs();
    if (!vfs || !path || !bytes) return n00b_result_err(bool, 1);

    auto fr = n00b_vfs_open(vfs, path, N00B_VFS_O_W);
    if (n00b_result_is_err(fr)) return n00b_result_err(bool, 2);
    n00b_vfs_fh_t fh = n00b_result_get(fr);

    auto wr = n00b_vfs_write(vfs, fh, bytes);
    n00b_vfs_close(vfs, fh);
    if (n00b_result_is_err(wr)) return n00b_result_err(bool, 3);
    return n00b_result_ok(bool, true);
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_read_sidecar(n00b_string_t *path)
{
    return n00b_chalk_read_file(sidecar_path(path));
}

n00b_result_t(bool)
n00b_chalk_write_sidecar(n00b_string_t *path, n00b_buffer_t *bytes)
{
    return n00b_chalk_write_file(sidecar_path(path), bytes);
}

n00b_result_t(bool)
n00b_chalk_delete_sidecar(n00b_string_t *path)
{
    n00b_vfs_t *vfs = get_vfs();
    if (!vfs) return n00b_result_err(bool, 1);
    auto r = n00b_vfs_delete(vfs, sidecar_path(path));
    if (n00b_result_is_err(r)) return n00b_result_err(bool, 2);
    return n00b_result_ok(bool, true);
}

// -----------------------------------------------------------------------
// Compose: buffer-mode → file-mode
// -----------------------------------------------------------------------

static n00b_result_t(bool)
write_io_result(n00b_string_t *path, n00b_chalk_io_result_t *io)
{
    if (io->kind == N00B_CHALK_OUT_SIDECAR) {
        if (io->bytes->byte_len == 0) {
            return n00b_chalk_delete_sidecar(path);
        }
        return n00b_chalk_write_sidecar(path, io->bytes);
    }
    return n00b_chalk_write_file(path, io->bytes);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_file_insert_via(n00b_string_t        *path,
                           n00b_chalk_mark_t    *mark,
                           n00b_chalk_buf_io_fn_t buf_insert)
{
    auto rr = n00b_chalk_read_file(path);
    if (n00b_result_is_err(rr)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 1);
    }
    auto ir = buf_insert(n00b_result_get(rr), mark);
    if (n00b_result_is_err(ir)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 2);
    }
    n00b_chalk_io_result_t *io = n00b_result_get(ir);
    auto wr = write_io_result(path, io);
    if (n00b_result_is_err(wr)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 3);
    }
    return n00b_result_ok(n00b_chalk_io_result_t *, io);
}

n00b_result_t(n00b_chalk_io_result_t *)
n00b_chalk_file_delete_via(n00b_string_t            *path,
                           n00b_chalk_buf_io_unary_t buf_delete)
{
    auto rr = n00b_chalk_read_file(path);
    if (n00b_result_is_err(rr)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 1);
    }
    auto ir = buf_delete(n00b_result_get(rr));
    if (n00b_result_is_err(ir)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 2);
    }
    n00b_chalk_io_result_t *io = n00b_result_get(ir);
    auto wr = write_io_result(path, io);
    if (n00b_result_is_err(wr)) {
        return n00b_result_err(n00b_chalk_io_result_t *, 3);
    }
    return n00b_result_ok(n00b_chalk_io_result_t *, io);
}

n00b_result_t(n00b_chalk_extract_result_t *)
n00b_chalk_file_extract_via(n00b_string_t              *path,
                            n00b_chalk_buf_extract_fn_t buf_extract)
{
    auto rr = n00b_chalk_read_file(path);
    if (n00b_result_is_err(rr)) {
        return n00b_result_err(n00b_chalk_extract_result_t *, 1);
    }
    return buf_extract(n00b_result_get(rr));
}

n00b_result_t(n00b_buffer_t *)
n00b_chalk_file_hash_via(n00b_string_t            *path,
                         n00b_chalk_buf_hash_fn_t  buf_hash)
{
    auto rr = n00b_chalk_read_file(path);
    if (n00b_result_is_err(rr)) return n00b_result_err(n00b_buffer_t *, 1);
    return buf_hash(n00b_result_get(rr));
}

// -----------------------------------------------------------------------
// Streaming SHA-256 of a file. Reads in fixed 64 KiB chunks and feeds
// each chunk to n00b_sha256_update. The maximum resident chunk is the
// chunk size itself, regardless of file size.
//
// This is the right primitive for codecs whose unchalked hash is
// "sha256 of the file contents" (certs, model sidecar, elf_fallback).
// For codecs that compute a canonical-form hash (chalk-aware
// transformation of the bytes), this won't apply.
// -----------------------------------------------------------------------

#define N00B_CHALK_HASH_CHUNK (64 * 1024)

n00b_result_t(n00b_buffer_t *)
n00b_chalk_hash_file_stream(n00b_string_t *path)
{
    n00b_vfs_t *vfs = get_vfs();
    if (!vfs || !path) return n00b_result_err(n00b_buffer_t *, 1);

    auto sr = n00b_vfs_stat(vfs, path);
    if (n00b_result_is_err(sr)) return n00b_result_err(n00b_buffer_t *, 2);
    n00b_vfs_obj_stat_t st = n00b_result_get(sr);

    auto fr = n00b_vfs_open(vfs, path, N00B_VFS_O_R);
    if (n00b_result_is_err(fr)) return n00b_result_err(n00b_buffer_t *, 3);
    n00b_vfs_fh_t fh = n00b_result_get(fr);

    n00b_sha256_ctx_t ctx;
    n00b_sha256_init(&ctx);

    uint64_t remaining = st.size;
    while (remaining > 0) {
        uint64_t want = remaining > N00B_CHALK_HASH_CHUNK
                            ? N00B_CHALK_HASH_CHUNK : remaining;
        auto rr = n00b_vfs_read(vfs, fh, want);
        if (n00b_result_is_err(rr)) {
            n00b_vfs_close(vfs, fh);
            return n00b_result_err(n00b_buffer_t *, 4);
        }
        n00b_buffer_t *chunk = n00b_result_get(rr);
        if (!chunk || chunk->byte_len == 0) break;
        n00b_sha256_update(&ctx, chunk->data, chunk->byte_len);
        remaining -= chunk->byte_len;
        if (chunk->byte_len < want) break;
    }
    n00b_vfs_close(vfs, fh);

    n00b_sha256_digest_t words;
    n00b_sha256_finalize(&ctx, words);
    uint8_t bytes[32];
    for (int i = 0; i < 8; i++) {
        uint32_t w   = words[i];
        bytes[i * 4]     = (uint8_t)((w >> 24) & 0xff);
        bytes[i * 4 + 1] = (uint8_t)((w >> 16) & 0xff);
        bytes[i * 4 + 2] = (uint8_t)((w >> 8) & 0xff);
        bytes[i * 4 + 3] = (uint8_t)(w & 0xff);
    }
    return n00b_result_ok(n00b_buffer_t *,
                          n00b_buffer_from_bytes((char *)bytes, 32));
}
