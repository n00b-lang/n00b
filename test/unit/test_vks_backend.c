#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "core/buffer.h"
#include "vfs/vfs.h"
#include "vfs/backend_memory.h"
#include "vks/vks.h"
#include "vks/backend_snapshot.h"

// ============================================================================
// Helpers
// ============================================================================

typedef n00b_vks_store_t(n00b_string_t *, n00b_string_t *) str_store_t;

static bool
str_eq(n00b_string_t *a, const char *b)
{
    if (a == nullptr) {
        return false;
    }
    size_t bl = strlen(b);
    return a->u8_bytes == bl && memcmp(a->data, b, bl) == 0;
}

// The vks op macros take the address of their key/value arguments (they
// delegate to dict put/get, which need lvalues), so keys and values must be
// named locals — these helpers provide that binding.
static void
put_kv(str_store_t *s, const char *k, const char *v)
{
    n00b_string_t *ks = n00b_string_from_cstr(k);
    n00b_string_t *vs = n00b_string_from_cstr(v);
    n00b_vks_put(s, ks, vs);
}

static n00b_string_t *
get_k(str_store_t *s, const char *k, bool *found)
{
    n00b_string_t *ks = n00b_string_from_cstr(k);
    return n00b_vks_get(s, ks, found);
}

// A vfs with a memory backend mounted at "/", matching test_vfs_core's setup.
static n00b_vfs_t *
make_vfs(void)
{
    n00b_result_t(n00b_vfs_t *) vr = n00b_vfs_new();
    assert(n00b_result_is_ok(vr));
    n00b_vfs_t *vfs = n00b_result_get(vr);

    n00b_result_t(n00b_vfs_backend_t *) br = n00b_vfs_backend_memory_new();
    assert(n00b_result_is_ok(br));
    n00b_vfs_backend_t *be = n00b_result_get(br);

    n00b_string_t *root = n00b_string_from_cstr("/");
    n00b_result_t(n00b_vfs_mount_t *) mr = n00b_vfs_mount(vfs, root, be, 0);
    assert(n00b_result_is_ok(mr));

    return vfs;
}

static n00b_vks_backend_t *
make_snapshot_backend(n00b_vfs_t *vfs, n00b_string_t *path)
{
    n00b_result_t(n00b_vks_backend_t *) r =
        n00b_vks_backend_snapshot_new(vfs, path);
    assert(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

static bool
vfs_path_exists(n00b_vfs_t *vfs, n00b_string_t *path)
{
    n00b_result_t(n00b_vfs_obj_stat_t) sr = n00b_vfs_stat(vfs, path);
    return n00b_result_is_ok(sr);
}

// ============================================================================
// VKS-04: snapshot survives marshal->unmarshal reopen
// ============================================================================

static void
test_reopen_round_trip(void)
{
    n00b_vfs_t    *vfs  = make_vfs();
    n00b_string_t *path = n00b_string_from_cstr("/snap.vks");

    // Store A: write two entries, then flush to durable storage.
    str_store_t *a = n00b_vks_store_new(
        n00b_string_t *, n00b_string_t *,
        .backend = make_snapshot_backend(vfs, path));

    put_kv(a, "a", "1");
    put_kv(a, "b", "2");

    n00b_result_t(bool) fr = n00b_vks_flush(a);
    assert(n00b_result_is_ok(fr));
    assert(n00b_result_get(fr) == true);
    assert(vfs_path_exists(vfs, path));

    // Store B: a fresh store with a NEW backend on the SAME vfs+path. Opening
    // it must rehydrate from the snapshot A wrote.
    str_store_t *b = n00b_vks_store_new(
        n00b_string_t *, n00b_string_t *,
        .backend = make_snapshot_backend(vfs, path));

    bool           found = false;
    n00b_string_t *va    = get_k(b, "a", &found);
    assert(found);
    assert(str_eq(va, "1"));

    found             = false;
    n00b_string_t *vb = get_k(b, "b", &found);
    assert(found);
    assert(str_eq(vb, "2"));

    // A key that was never stored is still absent after reopen.
    found = true;
    (void)get_k(b, "zzz", &found);
    assert(!found);

    printf("  [PASS] VKS-04 reopen_round_trip\n");
}

// ============================================================================
// VKS-05: snapshot fires exactly at dirty_max
// ============================================================================

static void
test_dirty_threshold(void)
{
    n00b_vfs_t    *vfs  = make_vfs();
    n00b_string_t *path = n00b_string_from_cstr("/threshold.vks");

    str_store_t *s = n00b_vks_store_new(
        n00b_string_t *, n00b_string_t *,
        .backend   = make_snapshot_backend(vfs, path),
        .dirty_max = 4);

    // 3 puts: below the threshold => nothing persisted yet.
    put_kv(s, "k0", "v");
    put_kv(s, "k1", "v");
    put_kv(s, "k2", "v");
    assert(!vfs_path_exists(vfs, path));

    _n00b_vks_store_internal_t *si = (_n00b_vks_store_internal_t *)s;
    assert(si->dirty == 3);

    // 4th put crosses dirty_max => exactly one snapshot fires and dirty resets.
    put_kv(s, "k3", "v");
    assert(vfs_path_exists(vfs, path));
    assert(si->dirty == 0);

    printf("  [PASS] VKS-05 dirty_threshold\n");
}

// ============================================================================
// VKS-06: corrupt snapshot loads gracefully (empty, no crash)
// ============================================================================

static void
write_raw(n00b_vfs_t *vfs, n00b_string_t *path, const char *bytes, size_t len)
{
    n00b_result_t(n00b_vfs_fh_t) o = n00b_vfs_open(vfs, path, N00B_VFS_O_W);
    assert(n00b_result_is_ok(o));
    n00b_vfs_fh_t fh = n00b_result_get(o);

    n00b_result_t(uint64_t) wr = n00b_vfs_write(
        vfs, fh, n00b_buffer_from_bytes((char *)bytes, (int64_t)len));
    assert(n00b_result_is_ok(wr));

    n00b_result_t(bool) cr = n00b_vfs_close(vfs, fh);
    assert(n00b_result_is_ok(cr));
}

static void
test_corrupt_load(void)
{
    n00b_vfs_t    *vfs  = make_vfs();
    n00b_string_t *path = n00b_string_from_cstr("/corrupt.vks");

    // Garbage / truncated bytes at the snapshot path: not a valid marshal
    // stream (wrong magic, far too short).
    static const char garbage[] =
        "this is definitely not a marshal stream\x01\x02\x03";
    write_raw(vfs, path, garbage, sizeof(garbage) - 1);
    assert(vfs_path_exists(vfs, path));

    // Opening a store on it must NOT crash; the store comes up empty.
    str_store_t *s = n00b_vks_store_new(
        n00b_string_t *, n00b_string_t *,
        .backend = make_snapshot_backend(vfs, path));

    bool found = true;
    (void)get_k(s, "anything", &found);
    assert(!found);

    // The store is usable post-recovery: a put then get works.
    put_kv(s, "k", "ok");
    found             = false;
    n00b_string_t *gv = get_k(s, "k", &found);
    assert(found);
    assert(str_eq(gv, "ok"));

    printf("  [PASS] VKS-06 corrupt_load\n");
}

// ============================================================================
// VKS-07: pluggable fake backend (no real storage) proves the wiring
// ============================================================================

typedef struct {
    uint32_t load_count;
    uint32_t snapshot_count;
} fake_ctx_t;

static n00b_string_t *
fake_name(void)
{
    return r"fake";
}

static void *
fake_init(n00b_vks_backend_t *be)
{
    return be->ctx;
}

static void
fake_cleanup(void *ctx)
{
    (void)ctx;
}

static n00b_result_t(bool)
fake_load(void *ctx, void *store)
{
    (void)store;
    ((fake_ctx_t *)ctx)->load_count++;
    return n00b_result_ok(bool, false); // nothing to load; keep empty dict
}

static n00b_result_t(bool)
fake_snapshot(void *ctx, void *store)
{
    (void)store;
    ((fake_ctx_t *)ctx)->snapshot_count++;
    return n00b_result_ok(bool, true);
}

static const n00b_vks_backend_ops_t fake_ops = {
    .name     = fake_name,
    .init     = fake_init,
    .cleanup  = fake_cleanup,
    .load     = fake_load,
    .snapshot = fake_snapshot,
};

static void
test_fake_backend_pluggability(void)
{
    fake_ctx_t *fc = n00b_alloc(fake_ctx_t);

    n00b_vks_backend_t *be = n00b_alloc(n00b_vks_backend_t);
    be->ops       = &fake_ops;
    be->ctx       = fc;
    be->allocator = nullptr;

    n00b_result_t(bool) ir = n00b_vks_backend_init(be);
    assert(n00b_result_is_ok(ir));

    // Open => load fires exactly once.
    str_store_t *s = n00b_vks_store_new(
        n00b_string_t *, n00b_string_t *, .backend = be, .dirty_max = 2);
    assert(fc->load_count == 1);
    assert(fc->snapshot_count == 0);

    // Explicit flush => snapshot increments.
    n00b_result_t(bool) fr = n00b_vks_flush(s);
    assert(n00b_result_is_ok(fr));
    assert(fc->snapshot_count == 1);

    // Cross dirty_max (2 puts) => one threshold snapshot.
    put_kv(s, "a", "1");
    assert(fc->snapshot_count == 1); // first put: below threshold
    put_kv(s, "b", "2");
    assert(fc->snapshot_count == 2); // second put: threshold crossed

    // Close => final snapshot via flush.
    n00b_vks_close(s);
    assert(fc->snapshot_count == 3);

    // load was only ever called the once (at open).
    assert(fc->load_count == 1);

    printf("  [PASS] VKS-07 fake_backend_pluggability\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running VKS backend tests...\n");

    test_reopen_round_trip();
    test_dirty_threshold();
    test_corrupt_load();
    test_fake_backend_pluggability();

    printf("All VKS backend tests passed.\n");
    n00b_shutdown();
    return 0;
}
