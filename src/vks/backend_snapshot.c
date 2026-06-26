#include "vks/backend_snapshot.h"
#include "vks/vks.h"
#include "core/alloc.h"
#include "core/string.h"
#include "core/buffer.h"
#include "core/type_info.h"
#include "util/marshal.h"

#include <string.h>

// ============================================================================
// Internal context
// ============================================================================

typedef struct {
    n00b_vfs_t       *vfs;
    n00b_string_t    *path;
    n00b_allocator_t *allocator;
} snapshot_ctx_t;

// ============================================================================
// Path helper
// ============================================================================

/*
 * Build "<path>.tmp" as a plain (unstyled) string. We do this by hand rather
 * than via n00b_cformat so the temp path carries no style metadata — it is a
 * pure filesystem path passed straight to the vfs.
 */
static n00b_string_t *
temp_path(snapshot_ctx_t *sc)
{
    static const char suffix[] = ".tmp";
    size_t            plen     = sc->path->u8_bytes;
    size_t            slen     = sizeof(suffix) - 1;

    char *buf = n00b_alloc_array(char, plen + slen + 1,
                                 .allocator = sc->allocator);
    memcpy(buf, sc->path->data, plen);
    memcpy(buf + plen, suffix, slen);
    buf[plen + slen] = '\0';

    return n00b_string_from_cstr(buf, .allocator = sc->allocator);
}

// ============================================================================
// Vtable: name / init / cleanup
// ============================================================================

static n00b_string_t *
snapshot_name(void)
{
    return r"snapshot-vfs";
}

/*
 * The owning context is built up front by n00b_vks_backend_snapshot_new (it
 * needs the vfs + path the caller supplied), so init just returns the
 * already-installed ctx that the lifecycle helper will store back.
 */
static void *
snapshot_init(n00b_vks_backend_t *be)
{
    return be->ctx;
}

static void
snapshot_cleanup(void *ctx)
{
    (void)ctx;
}

// ============================================================================
// Vtable: load
// ============================================================================

/*
 * Read every byte of the object at path into a single buffer. Returns nullptr
 * with *ok=false on a hard I/O error, or a buffer (possibly empty) with
 * *ok=true otherwise. A not-found path is reported via *not_found=true.
 */
static n00b_buffer_t *
read_all(snapshot_ctx_t *sc, bool *not_found, bool *ok)
{
    *not_found = false;
    *ok        = false;

    n00b_result_t(n00b_vfs_obj_stat_t) sr = n00b_vfs_stat(sc->vfs, sc->path);
    if (n00b_result_is_err(sr)) {
        if (n00b_result_get_err(sr) == N00B_VFS_ERR_NOT_FOUND) {
            *not_found = true;
            *ok        = true;
            return nullptr;
        }
        return nullptr;
    }

    n00b_result_t(n00b_vfs_fh_t) o = n00b_vfs_open(sc->vfs,
                                                   sc->path,
                                                   N00B_VFS_O_R);
    if (n00b_result_is_err(o)) {
        if (n00b_result_get_err(o) == N00B_VFS_ERR_NOT_FOUND) {
            *not_found = true;
            *ok        = true;
            return nullptr;
        }
        return nullptr;
    }

    n00b_vfs_fh_t  fh  = n00b_result_get(o);
    n00b_buffer_t *acc = n00b_buffer_new(0, .allocator = sc->allocator);

    // Read in chunks until EOF (a zero-length read). Each backend may return
    // fewer bytes than requested, so loop rather than assume one read suffices.
    for (;;) {
        n00b_result_t(n00b_buffer_t *) rr = n00b_vfs_read(sc->vfs,
                                                          fh,
                                                          65536,
                                                          .allocator = sc->allocator);
        if (n00b_result_is_err(rr)) {
            (void)n00b_vfs_close(sc->vfs, fh);
            return nullptr;
        }

        n00b_buffer_t *chunk = n00b_result_get(rr);
        if (chunk == nullptr || n00b_buffer_len(chunk) == 0) {
            break;
        }

        n00b_buffer_concat(acc, chunk);
    }

    (void)n00b_vfs_close(sc->vfs, fh);

    *ok = true;
    return acc;
}

/*
 * Rehydrate the store's in-memory dict from the snapshot.
 *
 * On a usable snapshot, store->mem points at the restored dict. A missing,
 * empty, corrupt, or wrong-typed image leaves store->mem unchanged and returns
 * ok(false).
 */
static n00b_result_t(bool)
snapshot_load(void *ctx, void *store)
{
    if (ctx == nullptr || store == nullptr) {
        return n00b_result_err(bool, N00B_VKS_ERR_NULL_ARG);
    }

    snapshot_ctx_t             *sc = ctx;
    _n00b_vks_store_internal_t *s  = store;

    bool           not_found = false;
    bool           ok        = false;
    n00b_buffer_t *bytes     = read_all(sc, &not_found, &ok);

    if (!ok) {
        return n00b_result_err(bool, N00B_VKS_ERR_IO);
    }
    if (not_found || bytes == nullptr || n00b_buffer_len(bytes) == 0) {
        // Nothing durable yet (or an empty file) — keep the empty dict.
        return n00b_result_ok(bool, false);
    }

    // A corrupt / truncated / garbage image must NOT crash: unmarshal returns
    // nullptr on a bad stream (header/magic/length mismatch all set an error
    // status and yield no roots). Treat that as "nothing to load".
    void *obj = n00b_unmarshal_one(bytes);
    if (obj == nullptr) {
        return n00b_result_ok(bool, false);
    }

    // Trust boundary: only vks writes these snapshots, but a stale or foreign
    // image could still carry an object of the wrong type. Verify the
    // unmarshaled object's runtime type matches the store's embedded dict before
    // installing it; on any mismatch fall back to the existing empty dict (same
    // as the corrupt-load path) rather than installing a wrong-typed object.
    //
    // We compare the type identity recorded in each object's GC/allocation
    // header via n00b_obj_typehash, which reads only the header (it never derefs
    // obj as a dict, so a tiny non-dict image cannot trigger an out-of-bounds
    // field read). The freshly built empty dict at s->mem (created by
    // n00b_vks_store_new for this exact K/V) carries the authoritative
    // n00b_dict(K, V) typehash; that fully parameterized type encodes the dict
    // identity plus both element types, so a single typehash comparison subsumes
    // the per-element key/value type check.
    if (s->mem == nullptr) {
        return n00b_result_ok(bool, false);
    }
    if (n00b_obj_typehash(obj) != n00b_obj_typehash(s->mem)) {
        return n00b_result_ok(bool, false);
    }

    s->mem = obj;
    return n00b_result_ok(bool, true);
}

// ============================================================================
// Vtable: snapshot
// ============================================================================

/*
 * Persist the store's in-memory dict as one marshaled object graph.
 *
 * Atomic: the bytes are written to a sibling temp path, which is then renamed
 * over the live path so a concurrent / interrupted reader never sees a partial
 * image. On success the object at path is a complete snapshot of the store.
 */
static n00b_result_t(bool)
snapshot_snapshot(void *ctx, void *store)
{
    if (ctx == nullptr || store == nullptr) {
        return n00b_result_err(bool, N00B_VKS_ERR_NULL_ARG);
    }

    snapshot_ctx_t             *sc = ctx;
    _n00b_vks_store_internal_t *s  = store;

    if (s->mem == nullptr) {
        return n00b_result_err(bool, N00B_VKS_ERR_NULL_ARG);
    }

    n00b_buffer_t *bytes = n00b_marshal(s->mem);
    if (bytes == nullptr) {
        return n00b_result_err(bool, N00B_VKS_ERR_BACKEND);
    }

    n00b_string_t *tmp = temp_path(sc);

    n00b_result_t(n00b_vfs_fh_t) o = n00b_vfs_open(sc->vfs,
                                                   tmp,
                                                   N00B_VFS_O_W);
    if (n00b_result_is_err(o)) {
        return n00b_result_err(bool, N00B_VKS_ERR_IO);
    }

    n00b_vfs_fh_t fh = n00b_result_get(o);

    n00b_result_t(uint64_t) wr = n00b_vfs_write(sc->vfs, fh, bytes);
    if (n00b_result_is_err(wr)) {
        (void)n00b_vfs_close(sc->vfs, fh);
        // Best-effort: drop the partial temp so we don't orphan it. The live
        // sc->path is untouched, so ignoring the delete result is safe.
        (void)n00b_vfs_delete(sc->vfs, tmp);
        return n00b_result_err(bool, N00B_VKS_ERR_IO);
    }

    // A short write would leave a truncated temp; treat any byte count below the
    // full payload as a failure so the snapshot stays all-or-nothing regardless
    // of the underlying vfs substrate.
    if (n00b_result_get(wr) != (uint64_t)n00b_buffer_len(bytes)) {
        (void)n00b_vfs_close(sc->vfs, fh);
        // Best-effort: drop the partial temp (live sc->path is untouched).
        (void)n00b_vfs_delete(sc->vfs, tmp);
        return n00b_result_err(bool, N00B_VKS_ERR_IO);
    }

    n00b_result_t(bool) cr = n00b_vfs_close(sc->vfs, fh);
    if (n00b_result_is_err(cr)) {
        // Best-effort: drop the temp on a failed close (live sc->path is
        // untouched), ignoring the delete result.
        (void)n00b_vfs_delete(sc->vfs, tmp);
        return n00b_result_err(bool, N00B_VKS_ERR_IO);
    }

    // Atomic publish: rename the fully-written temp over the live path.
    n00b_result_t(bool) mvr = n00b_vfs_rename(sc->vfs, tmp, sc->path);
    if (n00b_result_is_err(mvr)) {
        // Best-effort: the rename failed so the temp is still a sibling of the
        // (untouched) live sc->path; drop it, ignoring the delete result.
        (void)n00b_vfs_delete(sc->vfs, tmp);
        return n00b_result_err(bool, N00B_VKS_ERR_IO);
    }

    return n00b_result_ok(bool, true);
}

// ============================================================================
// Vtable instance
// ============================================================================

const n00b_vks_backend_ops_t n00b_vks_backend_snapshot_ops = {
    .name     = snapshot_name,
    .init     = snapshot_init,
    .cleanup  = snapshot_cleanup,
    .load     = snapshot_load,
    .snapshot = snapshot_snapshot,
};

// ============================================================================
// Constructor
// ============================================================================

n00b_result_t(n00b_vks_backend_t *)
n00b_vks_backend_snapshot_new(n00b_vfs_t *vfs, n00b_string_t *path) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (vfs == nullptr || path == nullptr) {
        return n00b_result_err(n00b_vks_backend_t *, N00B_VKS_ERR_NULL_ARG);
    }

    snapshot_ctx_t *sc = n00b_alloc(snapshot_ctx_t, .allocator = allocator);
    sc->vfs            = vfs;
    sc->path           = path;
    sc->allocator      = allocator;

    n00b_vks_backend_t *be = n00b_alloc(n00b_vks_backend_t,
                                        .allocator = allocator);
    be->ops       = &n00b_vks_backend_snapshot_ops;
    be->ctx       = sc;
    be->allocator = allocator;

    // init() returns the ctx we just installed; the lifecycle helper stores it
    // back into be->ctx (a no-op here, but keeps the contract uniform).
    n00b_result_t(bool) r = n00b_vks_backend_init(be);
    if (n00b_result_is_err(r)) {
        return n00b_result_err(n00b_vks_backend_t *, n00b_result_get_err(r));
    }

    return n00b_result_ok(n00b_vks_backend_t *, be);
}
