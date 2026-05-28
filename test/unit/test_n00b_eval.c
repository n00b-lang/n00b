/**
 * @file test_n00b_eval.c
 * @brief WP-009 Phase 3 — libn00b embedded-eval API smoke.
 *
 * Three cases:
 *   1. `r"true"`  on `r"int"`  → returns true regardless of arg.
 *   2. `r"false"` on `r"int"`  → returns false regardless of arg.
 *   3. `r"test_double_int(arg) > 10"` on `r"int"`, after
 *      `n00b_ffi_install_simple` of a C `int64_t -> int64_t`
 *      doubler. arg=6 → 12 > 10 → true; arg=4 → 8 > 10 → false.
 *
 * Case 3 exercises the libn00b plumbing that the naudit-side test
 * also relies on: an `n00b_ffi_install_simple`-registered C symbol
 * being callable from a JIT'd predicate expression.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"
#include "n00b/embed_ffi.h"
#include "n00b/eval.h"

// ============================================================================
// Helper FFI target
//
// `n00b_ffi_install_simple` resolves the C symbol via `dlsym(RTLD_DEFAULT, ...)`
// at install time, so this function must have external linkage. Using
// `static` linkage made the symbol invisible to `dlsym` and broke case 3.
// The function is referenced at install time by name, so the linker keeps
// it without further hints.
// ============================================================================

int64_t
test_double_int(int64_t x)
{
    return x * 2;
}

// ============================================================================
// Cases
// ============================================================================

static void
test_compile_true(n00b_eval_session_t *s)
{
    auto r = n00b_eval_compile_predicate(s, r"true", r"int");

    if (n00b_result_is_err(r)) {
        n00b_eval_err_t e = (n00b_eval_err_t)n00b_result_get_err(r);
        fprintf(stderr,
                "  [FAIL] compile true: code=%d (%.*s)\n",
                (int)e,
                (int)n00b_eval_err_str(e)->u8_bytes,
                n00b_eval_err_str(e)->data);
    }
    assert(n00b_result_is_ok(r));

    n00b_eval_predicate_fn_t fn = n00b_result_get(r);
    assert(fn);

    bool got = fn((void *)(intptr_t)0);
    assert(got == true);

    printf("  [PASS] compile_true (returns true)\n");
}

static void
test_compile_false(n00b_eval_session_t *s)
{
    auto r = n00b_eval_compile_predicate(s, r"false", r"int");

    if (n00b_result_is_err(r)) {
        n00b_eval_err_t e = (n00b_eval_err_t)n00b_result_get_err(r);
        fprintf(stderr,
                "  [FAIL] compile false: code=%d (%.*s)\n",
                (int)e,
                (int)n00b_eval_err_str(e)->u8_bytes,
                n00b_eval_err_str(e)->data);
    }
    assert(n00b_result_is_ok(r));

    n00b_eval_predicate_fn_t fn = n00b_result_get(r);
    assert(fn);

    bool got = fn((void *)(intptr_t)0);
    assert(got == false);

    printf("  [PASS] compile_false (returns false)\n");
}

static void
test_ffi_double_then_compare(n00b_eval_session_t *s)
{
    // Install the doubler under the n00b name `test_double_int`.
    // Signature: (int) -> int, matching the C signature for an
    // (int64_t)->int64_t.
    n00b_cg_session_t *cg = n00b_eval_session_cg(s);
    const char *param_types[] = {"int"};

    bool installed = n00b_ffi_install_simple(cg,
                                              "test_double_int",
                                              "test_double_int",
                                              param_types,
                                              1,
                                              "int");

    if (!installed) {
        fprintf(stderr, "  [FAIL] n00b_ffi_install_simple "
                        "could not register test_double_int\n");
    }
    assert(installed);

    // Compile a predicate referencing the registered symbol.
    auto r = n00b_eval_compile_predicate(s,
                                         r"test_double_int(arg) > 10",
                                         r"int");

    if (n00b_result_is_err(r)) {
        n00b_eval_err_t e = (n00b_eval_err_t)n00b_result_get_err(r);
        fprintf(stderr,
                "  [FAIL] compile ffi expression: code=%d (%.*s)\n",
                (int)e,
                (int)n00b_eval_err_str(e)->u8_bytes,
                n00b_eval_err_str(e)->data);
    }
    assert(n00b_result_is_ok(r));

    n00b_eval_predicate_fn_t fn = n00b_result_get(r);
    assert(fn);

    // 6 → 12 > 10 → true
    bool got_true = fn((void *)(intptr_t)6);
    assert(got_true == true);

    // 4 → 8 > 10 → false
    bool got_false = fn((void *)(intptr_t)4);
    assert(got_false == false);

    printf("  [PASS] ffi_double_then_compare (6→true, 4→false)\n");
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    auto sr = n00b_eval_session_new();

    if (n00b_result_is_err(sr)) {
        n00b_eval_err_t e = (n00b_eval_err_t)n00b_result_get_err(sr);
        fprintf(stderr,
                "  [FAIL] session_new: code=%d (%.*s)\n",
                (int)e,
                (int)n00b_eval_err_str(e)->u8_bytes,
                n00b_eval_err_str(e)->data);
        return 2;
    }

    n00b_eval_session_t *s = n00b_result_get(sr);
    assert(s);

    // Case 3 installs an FFI binding via `n00b_ffi_install_simple`,
    // which emits a fresh MIR wrapper into the session's currently
    // active module. After `n00b_eval_compile_predicate` has run,
    // that module is `MIR_finish_module`'d and MIR will refuse new
    // imports (the error surface is "import outside module" on stderr).
    // Running the FFI-bound case FIRST is the conservative ordering;
    // the trivial true/false cases still exercise the post-FFI session.
    test_ffi_double_then_compare(s);
    test_compile_true(s);
    test_compile_false(s);

    n00b_eval_session_free(s);

    printf("All n00b_eval smoke tests passed.\n");
    return 0;
}
