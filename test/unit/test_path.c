/*
 * test_path.c — Tests for path resolution, joining, classification, etc.
 */

#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/file.h"
#include "core/runtime.h"
#include "text/strings/string_ops.h"
#include "util/assert.h"
#include "util/path.h"

#define N00B_TEST_REQUIRE(expr) n00b_require((expr), #expr)

static n00b_string_t *
fixture_dir(n00b_string_t *prefix)
{
    auto r = n00b_new_temp_dir(prefix, n00b_string_from_cstr("_dir"));
    N00B_TEST_REQUIRE(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

static n00b_string_t *
fixture_child(n00b_string_t *dir, n00b_string_t *name)
{
    return n00b_path_simple_join(dir, name);
}

static void
fixture_write(n00b_string_t *path, n00b_string_t *contents)
{
    auto open_r = n00b_file_open(path, .mode = N00B_FILE_W);
    N00B_TEST_REQUIRE(n00b_result_is_ok(open_r));
    n00b_file_t *file = n00b_result_get(open_r);

    n00b_buffer_t *buffer = n00b_buffer_from_bytes(
        contents->data, (int64_t)contents->u8_bytes);
    auto write_r = n00b_file_write_all(file, buffer);
    N00B_TEST_REQUIRE(n00b_result_is_ok(write_r));
    N00B_TEST_REQUIRE(n00b_result_get(write_r) == contents->u8_bytes);

    auto close_r = n00b_file_close_result(file);
    N00B_TEST_REQUIRE(n00b_result_is_ok(close_r));
}

static void
fixture_assert_contents(n00b_string_t *path, n00b_string_t *expected)
{
    auto open_r = n00b_file_open(path, .kind = N00B_FILE_KIND_MMAP);
    N00B_TEST_REQUIRE(n00b_result_is_ok(open_r));
    n00b_file_t *file = n00b_result_get(open_r);

    auto buffer_r = n00b_file_as_buffer(file);
    N00B_TEST_REQUIRE(n00b_result_is_ok(buffer_r));
    n00b_buffer_t *buffer = n00b_result_get(buffer_r);
    N00B_TEST_REQUIRE(buffer->byte_len == expected->u8_bytes);
    N00B_TEST_REQUIRE(memcmp(buffer->data,
                             expected->data,
                             expected->u8_bytes) == 0);
    n00b_file_close(file);
}

static void
fixture_unlink(n00b_string_t *path)
{
    auto unlink_r = n00b_file_unlink(path, .ignore_missing = true);
    N00B_TEST_REQUIRE(n00b_result_is_ok(unlink_r));
}

// ============================================================================
// 1. resolve_path: absolute passthrough
// ============================================================================

static void
test_resolve_absolute(void)
{
    n00b_string_t *p = n00b_resolve_path(n00b_string_from_cstr("/usr/bin"));
    assert(p != nullptr);
    assert(strcmp(p->data, "/usr/bin") == 0);

    printf("  [PASS] resolve_absolute\n");
}

// ============================================================================
// 2. resolve_path: dot removal
// ============================================================================

static void
test_resolve_dot(void)
{
    n00b_string_t *p = n00b_resolve_path(n00b_string_from_cstr("/usr/./bin/../lib"));
    assert(p != nullptr);
    assert(strcmp(p->data, "/usr/lib") == 0);

    printf("  [PASS] resolve_dot\n");
}

// ============================================================================
// 3. resolve_path: relative (prepends cwd)
// ============================================================================

static void
test_resolve_relative(void)
{
    n00b_string_t *cwd = n00b_get_current_directory();
    n00b_string_t *p   = n00b_resolve_path(n00b_string_from_cstr("foo/bar"));
    assert(p != nullptr);

    // Should start with cwd
    assert(strncmp(p->data, cwd->data, cwd->u8_bytes) == 0);
    // Should end with /foo/bar
    size_t plen = p->u8_bytes;
    assert(plen > 8);
    assert(strcmp(p->data + plen - 8, "/foo/bar") == 0);

    printf("  [PASS] resolve_relative\n");
}

// ============================================================================
// 4. resolve_path: tilde expansion
// ============================================================================

static void
test_resolve_tilde(void)
{
    n00b_string_t *home = n00b_get_user_dir(nullptr);
    n00b_string_t *p    = n00b_resolve_path(n00b_string_from_cstr("~/test"));
    assert(p != nullptr);

    // Should start with home directory
    assert(strncmp(p->data, home->data, home->u8_bytes) == 0);
    // Should end with /test
    size_t plen = p->u8_bytes;
    assert(plen > 5);
    assert(strcmp(p->data + plen - 5, "/test") == 0);

    printf("  [PASS] resolve_tilde\n");
}

// ============================================================================
// 5. resolve_path: null/empty returns home
// ============================================================================

static void
test_resolve_empty(void)
{
    n00b_string_t *home = n00b_get_user_dir(nullptr);
    n00b_string_t *p1   = n00b_resolve_path(nullptr);
    n00b_string_t *p2   = n00b_resolve_path(n00b_string_from_cstr(""));

    assert(p1 != nullptr);
    assert(p2 != nullptr);
    assert(strcmp(p1->data, home->data) == 0);
    assert(strcmp(p2->data, home->data) == 0);

    printf("  [PASS] resolve_empty\n");
}

// ============================================================================
// 6. path_join
// ============================================================================

static void
test_path_join(void)
{
    n00b_list_t(n00b_string_t *) *parts = n00b_alloc(n00b_list_t(n00b_string_t *));
    *parts = n00b_list_new(n00b_string_t *);

    n00b_list_push(*parts, n00b_string_from_cstr("/usr"));
    n00b_list_push(*parts, n00b_string_from_cstr("local"));
    n00b_list_push(*parts, n00b_string_from_cstr("bin"));

    n00b_string_t *result = n00b_path_join(parts);
    assert(strcmp(result->data, "/usr/local/bin") == 0);

    printf("  [PASS] path_join\n");
}

// ============================================================================
// 7. path_simple_join
// ============================================================================

static void
test_path_simple_join(void)
{
    n00b_string_t *p = n00b_path_simple_join(
        n00b_string_from_cstr("/usr/local"),
        n00b_string_from_cstr("bin"));
    assert(strcmp(p->data, "/usr/local/bin") == 0);

    // Absolute second arg overrides
    n00b_string_t *p2 = n00b_path_simple_join(
        n00b_string_from_cstr("/usr"),
        n00b_string_from_cstr("/etc"));
    assert(strcmp(p2->data, "/etc") == 0);

    printf("  [PASS] path_simple_join\n");
}

// ============================================================================
// 8. get_file_kind
// ============================================================================

static void
test_get_file_kind(void)
{
    assert(n00b_get_file_kind(n00b_string_from_cstr("/")) == N00B_FK_IS_DIR);
    assert(n00b_get_file_kind(n00b_string_from_cstr("/nonexistent_xyzzy"))
           == N00B_FK_NOT_FOUND);

    // /bin/sh should be a file or link
    n00b_file_kind k = n00b_get_file_kind(n00b_string_from_cstr("/bin/sh"));
    assert(k == N00B_FK_IS_REG_FILE || k == N00B_FK_IS_FLINK);

    printf("  [PASS] get_file_kind\n");
}

// ============================================================================
// 9. path_exists / path_is_file / path_is_directory
// ============================================================================

static void
test_path_predicates(void)
{
    assert(n00b_path_exists(n00b_string_from_cstr("/")));
    assert(!n00b_path_exists(n00b_string_from_cstr("/nonexistent_xyzzy")));
    assert(n00b_path_is_directory(n00b_string_from_cstr("/")));
    assert(!n00b_path_is_file(n00b_string_from_cstr("/")));

    printf("  [PASS] path_predicates\n");
}

// ============================================================================
// 10. filename_from_path
// ============================================================================

static void
test_filename_from_path(void)
{
    n00b_string_t *f = n00b_filename_from_path(
        n00b_string_from_cstr("/usr/local/bin/ncc"));
    assert(strcmp(f->data, "ncc") == 0);

    // No slashes — returns as-is
    n00b_string_t *f2 = n00b_filename_from_path(
        n00b_string_from_cstr("just_a_name"));
    assert(strcmp(f2->data, "just_a_name") == 0);

    printf("  [PASS] filename_from_path\n");
}

// ============================================================================
// 11. path_get_extension / path_remove_extension
// ============================================================================

static void
test_extensions(void)
{
    n00b_string_t *ext = n00b_path_get_extension(
        n00b_string_from_cstr("/foo/bar.txt"));
    assert(strcmp(ext->data, ".txt") == 0);

    n00b_string_t *no_ext = n00b_path_remove_extension(
        n00b_string_from_cstr("/foo/bar.txt"));
    assert(strcmp(no_ext->data, "/foo/bar") == 0);

    // No extension
    n00b_string_t *ext2 = n00b_path_get_extension(
        n00b_string_from_cstr("/foo/bar"));
    assert(ext2->u8_bytes == 0);

    printf("  [PASS] extensions\n");
}

// ============================================================================
// 12. path_parts
// ============================================================================

static void
test_path_parts(void)
{
    n00b_list_t(n00b_string_t *) *parts =
        n00b_path_parts(n00b_string_from_cstr("/usr/local/test.txt"));

    assert(n00b_list_len(*parts) == 3);

    n00b_string_t *dir  = n00b_list_get(*parts, 0);
    n00b_string_t *base = n00b_list_get(*parts, 1);
    n00b_string_t *ext  = n00b_list_get(*parts, 2);

    assert(strcmp(dir->data, "/usr/local") == 0);
    assert(strcmp(base->data, "test") == 0);
    assert(strcmp(ext->data, "txt") == 0);

    printf("  [PASS] path_parts\n");
}

// ============================================================================
// 13. path_trim_trailing_slashes
// ============================================================================

static void
test_trim_slashes(void)
{
    n00b_string_t *p = n00b_path_trim_trailing_slashes(
        n00b_string_from_cstr("/usr/local///"));
    assert(strcmp(p->data, "/usr/local") == 0);

    // No trailing slash — unchanged
    n00b_string_t *p2 = n00b_path_trim_trailing_slashes(
        n00b_string_from_cstr("/usr/local"));
    assert(strcmp(p2->data, "/usr/local") == 0);

    printf("  [PASS] trim_slashes\n");
}

// ============================================================================
// 14. get_current_directory / set_current_directory
// ============================================================================

static void
test_cwd(void)
{
    n00b_string_t *orig = n00b_get_current_directory();
    assert(orig != nullptr);
    assert(orig->u8_bytes > 0);

    bool ok = n00b_set_current_directory(n00b_string_from_cstr("/tmp"));
    assert(ok);

    n00b_string_t *tmp = n00b_get_current_directory();
    // /tmp might resolve to /private/tmp on macOS
    assert(strstr(tmp->data, "tmp") != nullptr);

    // Restore
    n00b_set_current_directory(orig);

    printf("  [PASS] cwd\n");
}

// ============================================================================
// 15. new_temp_dir
// ============================================================================

static void
test_new_temp_dir(void)
{
    n00b_result_t(n00b_string_t *) r = n00b_new_temp_dir(
        n00b_string_from_cstr("test_"),
        n00b_string_from_cstr("_dir"));
    assert(n00b_result_is_ok(r));

    n00b_string_t *dir = n00b_result_get(r);
    assert(dir != nullptr);

    struct stat st;
    assert(stat(dir->data, &st) == 0);
    assert(S_ISDIR(st.st_mode));

    // Cleanup
    rmdir(dir->data);

    printf("  [PASS] new_temp_dir\n");
}

// ============================================================================
// 16. new_temp_path
// ============================================================================

static void
test_new_temp_path_prefix_suffix(void)
{
    n00b_string_t *path = n00b_new_temp_path(r"wp011_", r"_suffix");

    N00B_TEST_REQUIRE(path != nullptr);

    n00b_list_t(n00b_string_t *) *parts = n00b_path_parts(path);
    N00B_TEST_REQUIRE((int)n00b_list_len(*parts) >= 3);

    n00b_string_t *name = n00b_list_get(*parts, 1);

    N00B_TEST_REQUIRE(n00b_unicode_str_starts_with(name, r"wp011_"));
    N00B_TEST_REQUIRE(n00b_unicode_str_ends_with(name, r"_suffix"));
    N00B_TEST_REQUIRE(!n00b_path_exists(path));
}

// ============================================================================
// 17. same-directory sibling temp file
// ============================================================================

static void
test_sibling_temp_file(void)
{
    n00b_string_t *dir = fixture_dir(n00b_string_from_cstr("test_sibling_"));
    n00b_string_t *dst = fixture_child(dir, n00b_string_from_cstr("artifact.o"));
    fixture_write(dst, n00b_string_from_cstr("original"));

    auto temp_r = n00b_new_sibling_temp_file(dst);
    N00B_TEST_REQUIRE(n00b_result_is_ok(temp_r));
    n00b_sibling_temp_file_t *temp = n00b_result_get(temp_r);
    N00B_TEST_REQUIRE(temp->path != nullptr);
    N00B_TEST_REQUIRE(temp->file != nullptr);

    n00b_list_t(n00b_string_t *) *dst_parts = n00b_path_parts(dst);
    n00b_list_t(n00b_string_t *) *tmp_parts = n00b_path_parts(temp->path);
    n00b_string_t *dst_dir = n00b_list_get(*dst_parts, 0);
    n00b_string_t *tmp_dir = n00b_list_get(*tmp_parts, 0);
    N00B_TEST_REQUIRE(strcmp(dst_dir->data, tmp_dir->data) == 0);
    N00B_TEST_REQUIRE(strcmp(dst->data, temp->path->data) != 0);
    N00B_TEST_REQUIRE(n00b_path_exists(temp->path));
    fixture_assert_contents(dst, n00b_string_from_cstr("original"));

    auto close_r = n00b_file_close_result(temp->file);
    N00B_TEST_REQUIRE(n00b_result_is_ok(close_r));
    fixture_unlink(temp->path);

    n00b_string_t *hidden_dst =
        fixture_child(dir, n00b_string_from_cstr(".artifact"));
    auto hidden_temp_r = n00b_new_sibling_temp_path(hidden_dst);
    N00B_TEST_REQUIRE(n00b_result_is_ok(hidden_temp_r));
    n00b_string_t *hidden_temp = n00b_result_get(hidden_temp_r);
    n00b_list_t(n00b_string_t *) *hidden_parts = n00b_path_parts(hidden_temp);
    n00b_string_t *hidden_dir = n00b_list_get(*hidden_parts, 0);
    N00B_TEST_REQUIRE(strcmp(dst_dir->data, hidden_dir->data) == 0);
    N00B_TEST_REQUIRE(strcmp(hidden_dst->data, hidden_temp->data) != 0);

    fixture_unlink(dst);
    rmdir(dir->data);
}

// ============================================================================
// 18. exact commit replace and reject-existing
// ============================================================================

static void
test_exact_commit(void)
{
    n00b_string_t *dir = fixture_dir(n00b_string_from_cstr("test_commit_"));
    n00b_string_t *dst = fixture_child(dir, n00b_string_from_cstr("artifact"));
    n00b_string_t *src = fixture_child(dir, n00b_string_from_cstr("artifact.tmp"));
    fixture_write(dst, n00b_string_from_cstr("old"));
    fixture_write(src, n00b_string_from_cstr("new"));

    auto reject_r = n00b_path_commit_exact(
        src, dst, .policy = N00B_PATH_COMMIT_REJECT_EXISTING);
    if (n00b_result_is_err(reject_r) && n00b_result_get_err(reject_r) == ENOSYS) {
        fixture_unlink(src);
        fixture_unlink(dst);
        rmdir(dir->data);
        return;
    }
    N00B_TEST_REQUIRE(n00b_result_is_err(reject_r));
    N00B_TEST_REQUIRE(n00b_result_get_err(reject_r) == EEXIST);
    N00B_TEST_REQUIRE(n00b_path_exists(src));
    fixture_assert_contents(dst, n00b_string_from_cstr("old"));

    auto replace_r = n00b_path_commit_exact(
        src, dst, .policy = N00B_PATH_COMMIT_REPLACE_EXISTING);
    N00B_TEST_REQUIRE(n00b_result_is_ok(replace_r));
    N00B_TEST_REQUIRE(!n00b_path_exists(src));
    fixture_assert_contents(dst, n00b_string_from_cstr("new"));

    n00b_string_t *src2 = fixture_child(dir, n00b_string_from_cstr("fresh.tmp"));
    n00b_string_t *dst2 = fixture_child(dir, n00b_string_from_cstr("fresh"));
    fixture_write(src2, n00b_string_from_cstr("fresh"));
    auto create_r = n00b_path_commit_exact(
        src2, dst2, .policy = N00B_PATH_COMMIT_REJECT_EXISTING);
    N00B_TEST_REQUIRE(n00b_result_is_ok(create_r));
    N00B_TEST_REQUIRE(!n00b_path_exists(src2));
    fixture_assert_contents(dst2, n00b_string_from_cstr("fresh"));

    fixture_unlink(dst);
    fixture_unlink(dst2);
    rmdir(dir->data);
}

// ============================================================================
// 18. cleanup visibility
// ============================================================================

static void
test_cleanup_visibility(void)
{
    n00b_string_t *dir = fixture_dir(n00b_string_from_cstr("test_cleanup_"));
    n00b_string_t *existing = fixture_child(dir, n00b_string_from_cstr("temp"));
    n00b_string_t *missing = fixture_child(dir, n00b_string_from_cstr("missing"));
    fixture_write(existing, n00b_string_from_cstr("data"));

    auto remove_r = n00b_file_unlink(existing);
    N00B_TEST_REQUIRE(n00b_result_is_ok(remove_r));
    N00B_TEST_REQUIRE(n00b_result_get(remove_r));
    N00B_TEST_REQUIRE(!n00b_path_exists(existing));

    auto missing_ok = n00b_file_unlink(missing, .ignore_missing = true);
    N00B_TEST_REQUIRE(n00b_result_is_ok(missing_ok));
    N00B_TEST_REQUIRE(!n00b_result_get(missing_ok));

    auto missing_err = n00b_file_unlink(missing);
    N00B_TEST_REQUIRE(n00b_result_is_err(missing_err));
    N00B_TEST_REQUIRE(n00b_result_get_err(missing_err) == ENOENT);

    rmdir(dir->data);
}

// ============================================================================
// 19. path mode get/set
// ============================================================================

static void
test_path_mode_helpers(void)
{
    n00b_string_t *dir = fixture_dir(n00b_string_from_cstr("test_mode_"));
    n00b_string_t *path = fixture_child(dir, n00b_string_from_cstr("mode-file"));
    fixture_write(path, n00b_string_from_cstr("mode"));

    auto set_r = n00b_path_set_mode(path, 0600);
    if (n00b_result_is_err(set_r) && n00b_result_get_err(set_r) == ENOSYS) {
        fixture_unlink(path);
        rmdir(dir->data);
        return;
    }
    N00B_TEST_REQUIRE(n00b_result_is_ok(set_r));
    N00B_TEST_REQUIRE(n00b_result_get(set_r) == 0600);

    auto get_r = n00b_path_get_mode(path);
    N00B_TEST_REQUIRE(n00b_result_is_ok(get_r));
    N00B_TEST_REQUIRE(n00b_result_get(get_r) == 0600);

    auto bad_r = n00b_path_set_mode(path, 010000);
    N00B_TEST_REQUIRE(n00b_result_is_err(bad_r));
    N00B_TEST_REQUIRE(n00b_result_get_err(bad_r) == EINVAL);

    fixture_unlink(path);
    rmdir(dir->data);
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_init_simple(argc, argv);

    printf("Running path tests...\n");

    test_resolve_absolute();
    test_resolve_dot();
    test_resolve_relative();
    test_resolve_tilde();
    test_resolve_empty();
    test_path_join();
    test_path_simple_join();
    test_get_file_kind();
    test_path_predicates();
    test_filename_from_path();
    test_extensions();
    test_path_parts();
    test_trim_slashes();
    test_cwd();
    test_new_temp_dir();
    test_new_temp_path_prefix_suffix();
    test_sibling_temp_file();
    test_exact_commit();
    test_cleanup_visibility();
    test_path_mode_helpers();

    printf("All path tests passed.\n");
    n00b_shutdown();
    return 0;
}
