#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "vfs/backend_memory.h"

// ============================================================================
// Helpers
// ============================================================================

static n00b_vfs_backend_t *
setup(void)
{
    n00b_result_t(n00b_vfs_backend_t *) r = n00b_vfs_backend_memory_new();
    assert(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

// ============================================================================
// 1. Put + Get round-trip
// ============================================================================

static void
test_put_get(void)
{
    n00b_vfs_backend_t *be  = setup();
    n00b_string_t      *key = n00b_string_from_cstr("hello.txt");
    n00b_buffer_t      *val = n00b_buffer_from_cstr("world");

    n00b_result_t(bool) pr = be->ops->put(be->ctx, key, val);
    assert(n00b_result_is_ok(pr));

    n00b_result_t(n00b_buffer_t *) gr = be->ops->get(be->ctx, key);
    assert(n00b_result_is_ok(gr));

    n00b_buffer_t *got = n00b_result_get(gr);
    assert(n00b_buffer_len(got) == 5);

    int64_t    len;
    char      *data = n00b_buffer_to_c(got, &len);
    assert(len == 5);
    assert(memcmp(data, "world", 5) == 0);

    n00b_vfs_backend_cleanup(be);
    printf("  [PASS] put_get\n");
}

// ============================================================================
// 2. Get not found
// ============================================================================

static void
test_get_not_found(void)
{
    n00b_vfs_backend_t *be  = setup();
    n00b_string_t      *key = n00b_string_from_cstr("missing.txt");

    n00b_result_t(n00b_buffer_t *) r = be->ops->get(be->ctx, key);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_VFS_ERR_NOT_FOUND);

    n00b_vfs_backend_cleanup(be);
    printf("  [PASS] get_not_found\n");
}

// ============================================================================
// 3. Delete
// ============================================================================

static void
test_delete(void)
{
    n00b_vfs_backend_t *be  = setup();
    n00b_string_t      *key = n00b_string_from_cstr("del.txt");
    n00b_buffer_t      *val = n00b_buffer_from_cstr("data");

    be->ops->put(be->ctx, key, val);

    n00b_result_t(bool) dr = be->ops->del(be->ctx, key);
    assert(n00b_result_is_ok(dr));

    // Should be gone now.
    n00b_result_t(n00b_buffer_t *) gr = be->ops->get(be->ctx, key);
    assert(n00b_result_is_err(gr));
    assert(n00b_result_get_err(gr) == N00B_VFS_ERR_NOT_FOUND);

    n00b_vfs_backend_cleanup(be);
    printf("  [PASS] delete\n");
}

// ============================================================================
// 4. Delete not found
// ============================================================================

static void
test_delete_not_found(void)
{
    n00b_vfs_backend_t *be  = setup();
    n00b_string_t      *key = n00b_string_from_cstr("nope.txt");

    n00b_result_t(bool) r = be->ops->del(be->ctx, key);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_VFS_ERR_NOT_FOUND);

    n00b_vfs_backend_cleanup(be);
    printf("  [PASS] delete_not_found\n");
}

// ============================================================================
// 5. Stat
// ============================================================================

static void
test_stat(void)
{
    n00b_vfs_backend_t *be  = setup();
    n00b_string_t      *key = n00b_string_from_cstr("stat.txt");
    n00b_buffer_t      *val = n00b_buffer_from_cstr("twelve bytes");

    be->ops->put(be->ctx, key, val);

    n00b_result_t(n00b_vfs_obj_stat_t) r = be->ops->stat(be->ctx, key);
    assert(n00b_result_is_ok(r));

    n00b_vfs_obj_stat_t st = n00b_result_get(r);
    assert(st.kind == N00B_VFS_OBJ_FILE);
    assert(st.size == 12);
    assert(st.mtime_ns > 0);
    assert(st.mode == 0644);

    n00b_vfs_backend_cleanup(be);
    printf("  [PASS] stat\n");
}

// ============================================================================
// 6. Stat not found
// ============================================================================

static void
test_stat_not_found(void)
{
    n00b_vfs_backend_t *be  = setup();
    n00b_string_t      *key = n00b_string_from_cstr("ghost.txt");

    n00b_result_t(n00b_vfs_obj_stat_t) r = be->ops->stat(be->ctx, key);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_VFS_ERR_NOT_FOUND);

    n00b_vfs_backend_cleanup(be);
    printf("  [PASS] stat_not_found\n");
}

// ============================================================================
// 7. Overwrite
// ============================================================================

static void
test_overwrite(void)
{
    n00b_vfs_backend_t *be  = setup();
    n00b_string_t      *key = n00b_string_from_cstr("over.txt");

    be->ops->put(be->ctx, key, n00b_buffer_from_cstr("first"));
    be->ops->put(be->ctx, key, n00b_buffer_from_cstr("second"));

    n00b_result_t(n00b_buffer_t *) gr = be->ops->get(be->ctx, key);
    assert(n00b_result_is_ok(gr));

    n00b_buffer_t *got = n00b_result_get(gr);
    int64_t        len;
    char          *data = n00b_buffer_to_c(got, &len);
    assert(len == 6);
    assert(memcmp(data, "second", 6) == 0);

    n00b_vfs_backend_cleanup(be);
    printf("  [PASS] overwrite\n");
}

// ============================================================================
// 8. List with prefix
// ============================================================================

static void
test_list(void)
{
    n00b_vfs_backend_t *be = setup();

    be->ops->put(be->ctx, n00b_string_from_cstr("dir/a.txt"),
                 n00b_buffer_from_cstr("a"));
    be->ops->put(be->ctx, n00b_string_from_cstr("dir/b.txt"),
                 n00b_buffer_from_cstr("bb"));
    be->ops->put(be->ctx, n00b_string_from_cstr("other/c.txt"),
                 n00b_buffer_from_cstr("ccc"));

    n00b_string_t *prefix = n00b_string_from_cstr("dir/");

    n00b_result_t(n00b_vfs_list_result_t *) r =
        be->ops->list(be->ctx, prefix, nullptr, 100);
    assert(n00b_result_is_ok(r));

    n00b_vfs_list_result_t *lr = n00b_result_get(r);
    assert(lr->count == 2);
    assert(!lr->truncated);
    assert(lr->continuation == nullptr);

    n00b_vfs_backend_cleanup(be);
    printf("  [PASS] list\n");
}

// ============================================================================
// 9. Rename
// ============================================================================

static void
test_rename(void)
{
    n00b_vfs_backend_t *be      = setup();
    n00b_string_t      *old_key = n00b_string_from_cstr("old.txt");
    n00b_string_t      *new_key = n00b_string_from_cstr("new.txt");

    be->ops->put(be->ctx, old_key, n00b_buffer_from_cstr("data"));

    n00b_result_t(bool) rr = be->ops->rename(be->ctx, old_key, new_key);
    assert(n00b_result_is_ok(rr));

    // Old key should be gone.
    n00b_result_t(n00b_buffer_t *) gr = be->ops->get(be->ctx, old_key);
    assert(n00b_result_is_err(gr));

    // New key should have the data.
    gr = be->ops->get(be->ctx, new_key);
    assert(n00b_result_is_ok(gr));

    int64_t len;
    char   *data = n00b_buffer_to_c(n00b_result_get(gr), &len);
    assert(len == 4);
    assert(memcmp(data, "data", 4) == 0);

    n00b_vfs_backend_cleanup(be);
    printf("  [PASS] rename\n");
}

// ============================================================================
// 10. Rename not found
// ============================================================================

static void
test_rename_not_found(void)
{
    n00b_vfs_backend_t *be = setup();

    n00b_result_t(bool) r = be->ops->rename(
        be->ctx,
        n00b_string_from_cstr("nope.txt"),
        n00b_string_from_cstr("dest.txt"));
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_VFS_ERR_NOT_FOUND);

    n00b_vfs_backend_cleanup(be);
    printf("  [PASS] rename_not_found\n");
}

// ============================================================================
// 11. Mkdir
// ============================================================================

static void
test_mkdir(void)
{
    n00b_vfs_backend_t *be  = setup();
    n00b_string_t      *dir = n00b_string_from_cstr("mydir");

    n00b_result_t(bool) r = be->ops->mkdir(be->ctx, dir);
    assert(n00b_result_is_ok(r));

    n00b_result_t(n00b_vfs_obj_stat_t) sr = be->ops->stat(be->ctx, dir);
    assert(n00b_result_is_ok(sr));
    assert(n00b_result_get(sr).kind == N00B_VFS_OBJ_DIR);
    assert(n00b_result_get(sr).mode == 0755);

    // Duplicate mkdir should fail.
    r = be->ops->mkdir(be->ctx, dir);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_VFS_ERR_EXISTS);

    n00b_vfs_backend_cleanup(be);
    printf("  [PASS] mkdir\n");
}

// ============================================================================
// 12. Get range
// ============================================================================

static void
test_get_range(void)
{
    n00b_vfs_backend_t *be  = setup();
    n00b_string_t      *key = n00b_string_from_cstr("range.txt");

    be->ops->put(be->ctx, key, n00b_buffer_from_cstr("hello world"));

    n00b_result_t(n00b_buffer_t *) r = be->ops->get_range(be->ctx, key, 6, 5);
    assert(n00b_result_is_ok(r));

    n00b_buffer_t *got = n00b_result_get(r);
    int64_t        len;
    char          *data = n00b_buffer_to_c(got, &len);
    assert(len == 5);
    assert(memcmp(data, "world", 5) == 0);

    n00b_vfs_backend_cleanup(be);
    printf("  [PASS] get_range\n");
}

// ============================================================================
// 13. Capability probes
// ============================================================================

static void
test_capabilities(void)
{
    n00b_vfs_backend_t *be = setup();

    assert(be->ops->supports_range_read(be->ctx) == true);
    assert(be->ops->supports_rename(be->ctx) == true);
    assert(be->ops->supports_link(be->ctx) == false);

    n00b_vfs_backend_cleanup(be);
    printf("  [PASS] capabilities\n");
}

// ============================================================================
// 14. Error code names
// ============================================================================

static void
test_err_names(void)
{
    assert(strcmp(n00b_vfs_err_name(N00B_VFS_ERR_NOT_FOUND), "NOT_FOUND") == 0);
    assert(strcmp(n00b_vfs_err_name(N00B_VFS_ERR_EXISTS), "EXISTS") == 0);
    assert(strcmp(n00b_vfs_err_name(99999), "UNKNOWN") == 0);

    printf("  [PASS] err_names\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running VFS backend tests...\n");

    test_put_get();
    test_get_not_found();
    test_delete();
    test_delete_not_found();
    test_stat();
    test_stat_not_found();
    test_overwrite();
    test_list();
    test_rename();
    test_rename_not_found();
    test_mkdir();
    test_get_range();
    test_capabilities();
    test_err_names();

    printf("All VFS backend tests passed.\n");
    n00b_shutdown();
    return 0;
}
