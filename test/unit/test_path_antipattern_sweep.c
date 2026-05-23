/** @file test/unit/test_path_antipattern_sweep.c — libn00b path
 *  module anti-pattern sweep regression.
 *
 *  Pre-WP-005 P5 sweep replaced the `n00b_alloc(n00b_list_t(T)) +
 *  *p = n00b_list_new(T)` anti-pattern at seven sites in
 *  `include/util/path.h` + `src/util/path.c` with the canonical
 *  build-as-lvalue + struct-copy idiom. That anti-pattern skips
 *  the GC scan-info threading that `n00b_list_new`'s constructor
 *  establishes (`scan_kind` / `scan_cb` / `scan_user` / `allocator`)
 *  and is the same class of latent bug that caused the slay teardown
 *  SEGV (DF-023 substrate dispatch).
 *
 *  This test exercises each swept function with representative
 *  inputs and verifies the returned list is push-able (which forces
 *  `_n00b_list_ensure_cap` to read the list's stored scan-info /
 *  allocator fields to drive the grow correctly). If a future commit
 *  reintroduces the anti-pattern at any of these sites, the grow
 *  path will see a zero-initialized scan-info block and the test
 *  will either crash, misallocate, or produce wrong results.
 *
 *  Sites covered:
 *    [1]  `split_on_slash`               — internal helper, exercised
 *                                          transitively via
 *                                          `n00b_resolve_path`.
 *    [2]  `n00b_resolve_path`            — cwd-path construction;
 *                                          returns `n00b_string_t *`
 *                                          but internally relies on
 *                                          `split_on_slash` lists.
 *    [3]  `_n00b_path_walk`              — `@public` walker.
 *    [4]  `n00b_path_parts`              — splits resolved path into
 *                                          [dir, name, ext].
 *    [5]  `n00b_find_file_in_program_path` — PATH-lookup helper.
 *    [6]  `_n00b_list_directory`        — directory listing.
 *    [7]  `n00b_get_program_search_path` — PATH splitter (header
 *                                          inline).
 *
 *  Test-file conventions per D-030.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "n00b.h"
#include "util/path.h"
#include "core/string.h"
#include "core/runtime.h"

// ---------------------------------------------------------------------------
// Shared assertion: push-after-return then read-back verifies the
// returned list's scan-info / allocator fields are threaded — the
// grow path in `_n00b_list_ensure_cap` reads them directly.
//
// We push enough sentinel elements to force at least one grow past
// the default capacity (N00B_DEFAULT_LIST_SZ). If scan-info is
// zeroed, that grow either picks the wrong allocator (corruption) or
// the wrong scan_kind (GC walks bad pointers on a later collection
// — which we can't fully exercise here without a forced collect,
// but at minimum the allocation must succeed and the readback must
// recover identity).
// ---------------------------------------------------------------------------

static void
assert_push_and_readback(n00b_list_t(n00b_string_t *) *lst,
                         const char *tag)
{
    if (lst == NULL) {
        fprintf(stderr, "  [FAIL %s] returned list is null\n", tag);
        assert(0);
    }

    // Force a grow past default capacity by pushing more than
    // N00B_DEFAULT_LIST_SZ elements. We don't know that constant at
    // this layer; 64 is enough to force several power-of-two grows
    // from any reasonable default.
    const int N = 64;
    size_t before = n00b_list_len(*lst);

    for (int i = 0; i < N; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "sentinel-%02d", i);
        n00b_list_push(*lst, n00b_string_from_cstr(buf));
    }

    size_t after = n00b_list_len(*lst);
    if (after != before + (size_t)N) {
        fprintf(stderr,
                "  [FAIL %s] expected len %zu got %zu after %d pushes\n",
                tag, before + (size_t)N, after, N);
        assert(0);
    }

    // Read back each pushed element to confirm the data array is
    // well-formed. n00b_list_get on a corrupt list would abort
    // (via the bounds check) or return junk; a junk readback
    // surfaces as a mismatch here.
    for (int i = 0; i < N; i++) {
        n00b_string_t *got = n00b_list_get(*lst, before + (size_t)i);
        char expected[32];
        snprintf(expected, sizeof(expected), "sentinel-%02d", i);
        size_t elen = strlen(expected);
        if (got == NULL
            || (size_t)got->u8_bytes != elen
            || memcmp(got->data, expected, elen) != 0) {
            fprintf(stderr,
                    "  [FAIL %s] readback mismatch at idx %d: '%.*s'\n",
                    tag, i,
                    got ? (int)got->u8_bytes : 0,
                    got ? got->data : "<null>");
            assert(0);
        }
    }
}

// ---------------------------------------------------------------------------
// Test 1 — n00b_get_program_search_path (header inline).
// ---------------------------------------------------------------------------

static void
test_program_search_path(void)
{
    fprintf(stderr, "[sweep] n00b_get_program_search_path\n");

    // Both with-PATH and no-PATH branches exist; exercise with PATH
    // set (the typical case) since the no-PATH branch returns an
    // empty list — which our push-after-return still exercises.
    setenv("PATH", "/usr/bin:/bin:/usr/local/bin", 1);
    n00b_list_t(n00b_string_t *) *r = n00b_get_program_search_path();
    assert(r != NULL);
    assert(n00b_list_len(*r) == 3);
    assert_push_and_readback(r, "program_search_path");

    // No-PATH branch: still returns a well-formed (empty) list.
    unsetenv("PATH");
    r = n00b_get_program_search_path();
    assert(r != NULL);
    assert(n00b_list_len(*r) == 0);
    assert_push_and_readback(r, "program_search_path-empty");

    // Restore something reasonable for downstream tests.
    setenv("PATH", "/usr/bin:/bin", 1);

    fprintf(stderr, "  [PASS]\n");
}

// ---------------------------------------------------------------------------
// Test 2 — n00b_resolve_path (exercises split_on_slash transitively).
// ---------------------------------------------------------------------------

static void
test_resolve_path(void)
{
    fprintf(stderr, "[sweep] n00b_resolve_path (+ split_on_slash)\n");

    // Absolute path: goes through split_on_slash directly.
    n00b_string_t *abs = n00b_resolve_path(n00b_string_from_cstr("/usr/local/bin"));
    assert(abs != NULL);
    assert(abs->u8_bytes > 0);

    // Relative path: goes through split_on_slash for both cwd and
    // the relative tail, then merges.
    n00b_string_t *rel = n00b_resolve_path(n00b_string_from_cstr("foo/bar"));
    assert(rel != NULL);
    // Result is absolute (cwd-prefixed).
    assert(rel->data[0] == '/');

    // Null / empty path: returns home dir.
    n00b_string_t *e = n00b_resolve_path(nullptr);
    assert(e != NULL);

    fprintf(stderr, "  [PASS]\n");
}

// ---------------------------------------------------------------------------
// Test 3 — _n00b_path_walk (recursive directory walk).
// ---------------------------------------------------------------------------

static void
test_path_walk(void)
{
    fprintf(stderr, "[sweep] _n00b_path_walk\n");

    // /tmp is universally available and non-empty on test hosts.
    n00b_list_t(n00b_string_t *) *r =
        n00b_path_walk(n00b_string_from_cstr("/tmp"), .recurse = false);
    assert(r != NULL);
    // We don't assert on length (host-dependent), but the list
    // must be push-able post-return.
    assert_push_and_readback(r, "path_walk");

    fprintf(stderr, "  [PASS]\n");
}

// ---------------------------------------------------------------------------
// Test 4 — n00b_path_parts.
// ---------------------------------------------------------------------------

static void
test_path_parts(void)
{
    fprintf(stderr, "[sweep] n00b_path_parts\n");

    // Three branches: trailing-slash, no-extension, with-extension.
    // Use absolute paths to keep cwd-independence.
    n00b_list_t(n00b_string_t *) *r =
        n00b_path_parts(n00b_string_from_cstr("/tmp/foo.txt"));
    assert(r != NULL);
    assert(n00b_list_len(*r) == 3);
    assert_push_and_readback(r, "path_parts-with-ext");

    r = n00b_path_parts(n00b_string_from_cstr("/tmp/noext"));
    assert(r != NULL);
    assert(n00b_list_len(*r) == 3);
    assert_push_and_readback(r, "path_parts-no-ext");

    r = n00b_path_parts(n00b_string_from_cstr("/tmp/"));
    assert(r != NULL);
    assert(n00b_list_len(*r) == 3);
    assert_push_and_readback(r, "path_parts-trailing-slash");

    // Empty / null path: still returns a well-formed (empty) list.
    r = n00b_path_parts(nullptr);
    assert(r != NULL);
    assert(n00b_list_len(*r) == 0);
    assert_push_and_readback(r, "path_parts-null");

    fprintf(stderr, "  [PASS]\n");
}

// ---------------------------------------------------------------------------
// Test 5 — n00b_find_file_in_program_path.
// ---------------------------------------------------------------------------

static void
test_find_file_in_program_path(void)
{
    fprintf(stderr, "[sweep] n00b_find_file_in_program_path\n");

    // Looking for /bin/sh is universal on POSIX hosts.
    setenv("PATH", "/usr/bin:/bin", 1);
    n00b_list_t(n00b_string_t *) *r =
        n00b_find_file_in_program_path(n00b_string_from_cstr("sh"), nullptr);
    assert(r != NULL);
    // We expect at least one match (/bin/sh on Linux/macOS); but the
    // test only requires the list to be well-formed and push-able.
    assert_push_and_readback(r, "find_file_in_program_path");

    // Not-found branch: still a well-formed empty list.
    r = n00b_find_file_in_program_path(
        n00b_string_from_cstr("definitely-not-a-real-binary-xyzzy-12345"),
        nullptr);
    assert(r != NULL);
    assert_push_and_readback(r, "find_file_in_program_path-empty");

    fprintf(stderr, "  [PASS]\n");
}

// ---------------------------------------------------------------------------
// Test 6 — _n00b_list_directory.
// ---------------------------------------------------------------------------

static void
test_list_directory(void)
{
    fprintf(stderr, "[sweep] _n00b_list_directory\n");

    // /tmp is universally available.
    n00b_list_t(n00b_string_t *) *r =
        n00b_list_directory(n00b_string_from_cstr("/tmp"));
    assert(r != NULL);
    assert_push_and_readback(r, "list_directory");

    // Non-existent directory: opendir fails → early-return path,
    // which under the new shape still returns a well-formed empty
    // list via the struct-copy idiom at the end.
    r = n00b_list_directory(
        n00b_string_from_cstr("/this/path/should/not/exist/xyzzy"));
    assert(r != NULL);
    assert(n00b_list_len(*r) == 0);
    assert_push_and_readback(r, "list_directory-empty");

    fprintf(stderr, "  [PASS]\n");
}

// ---------------------------------------------------------------------------
int
main(int argc, char **argv)
{
    n00b_init_simple(argc, argv);

    test_program_search_path();
    test_resolve_path();
    test_path_walk();
    test_path_parts();
    test_find_file_in_program_path();
    test_list_directory();

    fprintf(stderr, "All path anti-pattern sweep regression tests passed.\n");
    return 0;
}
