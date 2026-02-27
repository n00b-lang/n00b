/**
 * @file test_codegen.c
 * @brief Tests for the codegen API with MIR backend.
 *
 * Tests the builder API (functions, constants, branches, loops),
 * imports, and both interpret and JIT execution modes.
 */

#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "slay/codegen.h"

// ============================================================================
// 1. Builder: add two i64 values, interpret
// ============================================================================

static void
test_builder_add_interp(void)
{
    n00b_codegen_t *cg = n00b_codegen_new(NULL);

    n00b_cg_begin_func(cg, "add",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x", "y"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64, N00B_CG_I64},
                        .n_params    = 2);

    n00b_cg_val_t x = n00b_cg_param(cg, 0);
    n00b_cg_val_t y = n00b_cg_param(cg, 1);

    n00b_cg_emit_ret(cg, n00b_cg_emit_add(cg, x, y));
    n00b_cg_end_func(cg);

    // Interpret.
    MIR_val_t result;
    MIR_val_t args[2] = {{.i = 3}, {.i = 4}};

    bool ok = n00b_codegen_interpret(cg, "add", &result, args, 2);
    assert(ok);
    assert(result.i == 7);

    // Another test.
    args[0].i = -10;
    args[1].i = 25;
    ok = n00b_codegen_interpret(cg, "add", &result, args, 2);
    assert(ok);
    assert(result.i == 15);

    n00b_codegen_free(cg);
    printf("  [PASS] builder_add_interp\n");
}

// ============================================================================
// 2. Builder: add two i64 values, JIT
// ============================================================================

static void
test_builder_add_jit(void)
{
    n00b_codegen_t *cg = n00b_codegen_new(NULL);

    n00b_cg_begin_func(cg, "add",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x", "y"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64, N00B_CG_I64},
                        .n_params    = 2);

    n00b_cg_val_t x = n00b_cg_param(cg, 0);
    n00b_cg_val_t y = n00b_cg_param(cg, 1);

    n00b_cg_emit_ret(cg, n00b_cg_emit_add(cg, x, y));
    n00b_cg_end_func(cg);

    typedef int64_t (*add_fn)(int64_t, int64_t);
    add_fn compiled = (add_fn)n00b_codegen_jit(cg, "add");
    assert(compiled != NULL);

    assert(compiled(3, 4) == 7);
    assert(compiled(-10, 25) == 15);
    assert(compiled(0, 0) == 0);
    assert(compiled(INT64_MAX, 0) == INT64_MAX);

    n00b_codegen_free(cg);
    printf("  [PASS] builder_add_jit\n");
}

// ============================================================================
// 3. Builder: arithmetic operations
// ============================================================================

static void
test_builder_arithmetic(void)
{
    n00b_codegen_t *cg = n00b_codegen_new(NULL);

    // sub(x, y) = x - y
    n00b_cg_begin_func(cg, "sub",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x", "y"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64, N00B_CG_I64},
                        .n_params    = 2);
    n00b_cg_emit_ret(cg, n00b_cg_emit_sub(cg, n00b_cg_param(cg, 0),
                                             n00b_cg_param(cg, 1)));
    n00b_cg_end_func(cg);

    // mul(x, y) = x * y
    n00b_cg_begin_func(cg, "mul",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x", "y"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64, N00B_CG_I64},
                        .n_params    = 2);
    n00b_cg_emit_ret(cg, n00b_cg_emit_mul(cg, n00b_cg_param(cg, 0),
                                             n00b_cg_param(cg, 1)));
    n00b_cg_end_func(cg);

    // div(x, y) = x / y
    n00b_cg_begin_func(cg, "div_fn",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x", "y"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64, N00B_CG_I64},
                        .n_params    = 2);
    n00b_cg_emit_ret(cg, n00b_cg_emit_div(cg, n00b_cg_param(cg, 0),
                                             n00b_cg_param(cg, 1)));
    n00b_cg_end_func(cg);

    // mod(x, y) = x % y
    n00b_cg_begin_func(cg, "mod_fn",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x", "y"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64, N00B_CG_I64},
                        .n_params    = 2);
    n00b_cg_emit_ret(cg, n00b_cg_emit_mod(cg, n00b_cg_param(cg, 0),
                                             n00b_cg_param(cg, 1)));
    n00b_cg_end_func(cg);

    // neg(x) = -x
    n00b_cg_begin_func(cg, "neg_fn",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64},
                        .n_params    = 1);
    n00b_cg_emit_ret(cg, n00b_cg_emit_neg(cg, n00b_cg_param(cg, 0)));
    n00b_cg_end_func(cg);

    typedef int64_t (*binop_fn)(int64_t, int64_t);
    typedef int64_t (*unop_fn)(int64_t);

    binop_fn sub = (binop_fn)n00b_codegen_jit(cg, "sub");
    binop_fn mul = (binop_fn)n00b_codegen_jit(cg, "mul");
    binop_fn div_fn = (binop_fn)n00b_codegen_jit(cg, "div_fn");
    binop_fn mod_fn = (binop_fn)n00b_codegen_jit(cg, "mod_fn");
    unop_fn  neg_fn = (unop_fn)n00b_codegen_jit(cg, "neg_fn");

    assert(sub(10, 3) == 7);
    assert(mul(6, 7) == 42);
    assert(div_fn(20, 4) == 5);
    assert(mod_fn(17, 5) == 2);
    assert(neg_fn(42) == -42);
    assert(neg_fn(-1) == 1);

    n00b_codegen_free(cg);
    printf("  [PASS] builder_arithmetic\n");
}

// ============================================================================
// 4. Builder: comparisons
// ============================================================================

static void
test_builder_comparisons(void)
{
    n00b_codegen_t *cg = n00b_codegen_new(NULL);

    n00b_cg_begin_func(cg, "lt",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x", "y"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64, N00B_CG_I64},
                        .n_params    = 2);
    n00b_cg_emit_ret(cg, n00b_cg_emit_lt(cg, n00b_cg_param(cg, 0),
                                            n00b_cg_param(cg, 1)));
    n00b_cg_end_func(cg);

    n00b_cg_begin_func(cg, "eq",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x", "y"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64, N00B_CG_I64},
                        .n_params    = 2);
    n00b_cg_emit_ret(cg, n00b_cg_emit_eq(cg, n00b_cg_param(cg, 0),
                                            n00b_cg_param(cg, 1)));
    n00b_cg_end_func(cg);

    typedef int64_t (*cmp_fn)(int64_t, int64_t);

    cmp_fn lt = (cmp_fn)n00b_codegen_jit(cg, "lt");
    cmp_fn eq = (cmp_fn)n00b_codegen_jit(cg, "eq");

    assert(lt(1, 2) == 1);
    assert(lt(2, 1) == 0);
    assert(lt(1, 1) == 0);

    assert(eq(5, 5) == 1);
    assert(eq(5, 6) == 0);

    n00b_codegen_free(cg);
    printf("  [PASS] builder_comparisons\n");
}

// ============================================================================
// 5. Builder: branches (if/else via labels)
// ============================================================================

static void
test_builder_branches(void)
{
    n00b_codegen_t *cg = n00b_codegen_new(NULL);

    // max(x, y) = x > y ? x : y
    n00b_cg_begin_func(cg, "max",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x", "y"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64, N00B_CG_I64},
                        .n_params    = 2);

    n00b_cg_val_t x = n00b_cg_param(cg, 0);
    n00b_cg_val_t y = n00b_cg_param(cg, 1);

    n00b_cg_val_t cond       = n00b_cg_emit_gt(cg, x, y);
    n00b_cg_val_t else_label = n00b_cg_label_new(cg);
    n00b_cg_val_t end_label  = n00b_cg_label_new(cg);

    n00b_cg_emit_bf(cg, cond, else_label);
    n00b_cg_emit_ret(cg, x);

    n00b_cg_label_here(cg, else_label);
    n00b_cg_emit_ret(cg, y);

    n00b_cg_label_here(cg, end_label); // Not reached but valid.
    n00b_cg_end_func(cg);

    typedef int64_t (*max_fn)(int64_t, int64_t);
    max_fn maxfn = (max_fn)n00b_codegen_jit(cg, "max");

    assert(maxfn(3, 7) == 7);
    assert(maxfn(10, 2) == 10);
    assert(maxfn(5, 5) == 5);

    n00b_codegen_free(cg);
    printf("  [PASS] builder_branches\n");
}

// ============================================================================
// 6. Builder: loops
// ============================================================================

static void
test_builder_loops(void)
{
    n00b_codegen_t *cg = n00b_codegen_new(NULL);

    // sum(n) = 1 + 2 + ... + n
    n00b_cg_begin_func(cg, "sum",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"n"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64},
                        .n_params    = 1);

    n00b_cg_val_t n   = n00b_cg_param(cg, 0);
    n00b_cg_val_t acc = n00b_cg_local(cg, "acc");
    n00b_cg_val_t i   = n00b_cg_local(cg, "i");

    n00b_cg_store(cg, acc, _n00b_cg_const_i64(cg, 0));
    n00b_cg_store(cg, i,   _n00b_cg_const_i64(cg, 1));

    n00b_cg_val_t loop_top  = n00b_cg_label_new(cg);
    n00b_cg_val_t loop_exit = n00b_cg_label_new(cg);

    n00b_cg_label_here(cg, loop_top);

    // while (i <= n)
    n00b_cg_val_t cond = n00b_cg_emit_le(cg, n00b_cg_load(cg, i),
                                           n00b_cg_load(cg, n));
    n00b_cg_emit_bf(cg, cond, loop_exit);

    // acc += i
    n00b_cg_store(cg, acc, n00b_cg_emit_add(cg, n00b_cg_load(cg, acc),
                                               n00b_cg_load(cg, i)));
    // i++
    n00b_cg_store(cg, i, n00b_cg_emit_add(cg, n00b_cg_load(cg, i),
                                             _n00b_cg_const_i64(cg, 1)));

    n00b_cg_emit_jmp(cg, loop_top);
    n00b_cg_label_here(cg, loop_exit);

    n00b_cg_emit_ret(cg, n00b_cg_load(cg, acc));
    n00b_cg_end_func(cg);

    typedef int64_t (*sum_fn)(int64_t);
    sum_fn sumfn = (sum_fn)n00b_codegen_jit(cg, "sum");

    assert(sumfn(10) == 55);
    assert(sumfn(0) == 0);
    assert(sumfn(1) == 1);
    assert(sumfn(100) == 5050);

    n00b_codegen_free(cg);
    printf("  [PASS] builder_loops\n");
}

// ============================================================================
// 7. Builder: constants via typed functions
// ============================================================================

static void
test_builder_constants(void)
{
    n00b_codegen_t *cg = n00b_codegen_new(NULL);

    // const42() = 42
    n00b_cg_begin_func(cg, "const42", .ret = N00B_CG_I64);
    n00b_cg_emit_ret(cg, _n00b_cg_const_i64(cg, 42));
    n00b_cg_end_func(cg);

    // const_bool() = true (1)
    n00b_cg_begin_func(cg, "const_bool", .ret = N00B_CG_I64);
    n00b_cg_emit_ret(cg, _n00b_cg_const_bool(cg, true));
    n00b_cg_end_func(cg);

    // const_ptr() = NULL pointer
    n00b_cg_begin_func(cg, "const_ptr", .ret = N00B_CG_I64);
    n00b_cg_emit_ret(cg, _n00b_cg_const_ptr(cg, NULL));
    n00b_cg_end_func(cg);

    typedef int64_t (*i64_fn)(void);

    i64_fn c42   = (i64_fn)n00b_codegen_jit(cg, "const42");
    i64_fn cbool = (i64_fn)n00b_codegen_jit(cg, "const_bool");
    i64_fn cptr  = (i64_fn)n00b_codegen_jit(cg, "const_ptr");

    assert(c42() == 42);
    assert(cbool() == 1);
    assert(cptr() == 0);

    n00b_codegen_free(cg);
    printf("  [PASS] builder_constants\n");
}

// ============================================================================
// 8. Builder: double arithmetic
// ============================================================================

static void
test_builder_double(void)
{
    n00b_codegen_t *cg = n00b_codegen_new(NULL);

    n00b_cg_begin_func(cg, "dadd",
                        .ret         = N00B_CG_F64,
                        .param_names = (const char *[]){"x", "y"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_F64, N00B_CG_F64},
                        .n_params    = 2);

    n00b_cg_val_t x = n00b_cg_param(cg, 0);
    n00b_cg_val_t y = n00b_cg_param(cg, 1);
    n00b_cg_emit_ret(cg, n00b_cg_emit_add(cg, x, y));
    n00b_cg_end_func(cg);

    typedef double (*dadd_fn)(double, double);
    dadd_fn compiled = (dadd_fn)n00b_codegen_jit(cg, "dadd");
    assert(compiled != NULL);

    double r = compiled(1.5, 2.5);
    assert(fabs(r - 4.0) < 1e-10);

    r = compiled(-1.0, 1.0);
    assert(fabs(r) < 1e-10);

    n00b_codegen_free(cg);
    printf("  [PASS] builder_double\n");
}

// ============================================================================
// 9. Builder: import external function
// ============================================================================

static int64_t
external_add(int64_t a, int64_t b)
{
    return a + b + 100;
}

static void
test_builder_import(void)
{
    n00b_codegen_t *cg = n00b_codegen_new(NULL);

    n00b_cg_import_func(cg, "external_add", (void *)external_add,
                          .ret         = N00B_CG_I64,
                          .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64, N00B_CG_I64},
                          .n_params    = 2);

    n00b_cg_begin_func(cg, "use_ext",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x", "y"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64, N00B_CG_I64},
                        .n_params    = 2);

    n00b_cg_val_t x = n00b_cg_param(cg, 0);
    n00b_cg_val_t y = n00b_cg_param(cg, 1);

    n00b_cg_val_t args[2] = {x, y};
    n00b_cg_val_t result = n00b_cg_emit_call(cg, "external_add", args, 2,
                                               .ret = N00B_CG_I64);
    n00b_cg_emit_ret(cg, result);
    n00b_cg_end_func(cg);

    typedef int64_t (*use_ext_fn)(int64_t, int64_t);
    use_ext_fn fn = (use_ext_fn)n00b_codegen_jit(cg, "use_ext");
    assert(fn != NULL);

    assert(fn(1, 2) == 103);  // 1 + 2 + 100
    assert(fn(0, 0) == 100);
    assert(fn(-50, -50) == 0);

    n00b_codegen_free(cg);
    printf("  [PASS] builder_import\n");
}

// ============================================================================
// 10. Builder: internal function calls
// ============================================================================

static void
test_builder_internal_call(void)
{
    n00b_codegen_t *cg = n00b_codegen_new(NULL);

    // Define helper: double_it(x) = x * 2
    n00b_cg_begin_func(cg, "double_it",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64},
                        .n_params    = 1);
    n00b_cg_val_t x = n00b_cg_param(cg, 0);
    n00b_cg_emit_ret(cg, n00b_cg_emit_mul(cg, x, _n00b_cg_const_i64(cg, 2)));
    n00b_cg_end_func(cg);

    // Caller: call_double(x) = double_it(x) + 1
    n00b_cg_begin_func(cg, "call_double",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64},
                        .n_params    = 1);
    n00b_cg_val_t arg = n00b_cg_param(cg, 0);
    n00b_cg_val_t call_args[1] = {arg};
    n00b_cg_val_t doubled = n00b_cg_emit_call(cg, "double_it", call_args, 1,
                                                .ret = N00B_CG_I64);
    n00b_cg_emit_ret(cg, n00b_cg_emit_add(cg, doubled,
                                             _n00b_cg_const_i64(cg, 1)));
    n00b_cg_end_func(cg);

    typedef int64_t (*call_fn)(int64_t);
    call_fn fn = (call_fn)n00b_codegen_jit(cg, "call_double");
    assert(fn != NULL);

    assert(fn(5) == 11);   // 5 * 2 + 1
    assert(fn(0) == 1);    // 0 * 2 + 1
    assert(fn(-3) == -5);  // -3 * 2 + 1

    n00b_codegen_free(cg);
    printf("  [PASS] builder_internal_call\n");
}

// ============================================================================
// 11. Builder: bitwise operations
// ============================================================================

static void
test_builder_bitwise(void)
{
    n00b_codegen_t *cg = n00b_codegen_new(NULL);

    // and(x, y) = x & y
    n00b_cg_begin_func(cg, "band",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x", "y"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64, N00B_CG_I64},
                        .n_params    = 2);
    n00b_cg_emit_ret(cg, n00b_cg_emit_and(cg, n00b_cg_param(cg, 0),
                                             n00b_cg_param(cg, 1)));
    n00b_cg_end_func(cg);

    // or(x, y) = x | y
    n00b_cg_begin_func(cg, "bor",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x", "y"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64, N00B_CG_I64},
                        .n_params    = 2);
    n00b_cg_emit_ret(cg, n00b_cg_emit_or(cg, n00b_cg_param(cg, 0),
                                            n00b_cg_param(cg, 1)));
    n00b_cg_end_func(cg);

    // shl(x, y) = x << y
    n00b_cg_begin_func(cg, "bshl",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"x", "y"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64, N00B_CG_I64},
                        .n_params    = 2);
    n00b_cg_emit_ret(cg, n00b_cg_emit_shl(cg, n00b_cg_param(cg, 0),
                                             n00b_cg_param(cg, 1)));
    n00b_cg_end_func(cg);

    typedef int64_t (*binop_fn)(int64_t, int64_t);

    binop_fn band = (binop_fn)n00b_codegen_jit(cg, "band");
    binop_fn bor  = (binop_fn)n00b_codegen_jit(cg, "bor");
    binop_fn bshl = (binop_fn)n00b_codegen_jit(cg, "bshl");

    assert(band(0xFF, 0x0F) == 0x0F);
    assert(bor(0xF0, 0x0F) == 0xFF);
    assert(bshl(1, 8) == 256);

    n00b_codegen_free(cg);
    printf("  [PASS] builder_bitwise\n");
}

// ============================================================================
// 12. Builder: recursive fibonacci via JIT
// ============================================================================

static void
test_builder_fibonacci(void)
{
    n00b_codegen_t *cg = n00b_codegen_new(NULL);

    // fib(n) = n <= 1 ? n : fib(n-1) + fib(n-2)
    n00b_cg_begin_func(cg, "fib",
                        .ret         = N00B_CG_I64,
                        .param_names = (const char *[]){"n"},
                        .param_types = (n00b_cg_type_tag_t[]){N00B_CG_I64},
                        .n_params    = 1);

    n00b_cg_val_t n = n00b_cg_param(cg, 0);

    // if n <= 1, return n
    n00b_cg_val_t cond      = n00b_cg_emit_le(cg, n, _n00b_cg_const_i64(cg, 1));
    n00b_cg_val_t recurse   = n00b_cg_label_new(cg);

    n00b_cg_emit_bf(cg, cond, recurse);
    n00b_cg_emit_ret(cg, n);

    n00b_cg_label_here(cg, recurse);

    // fib(n-1)
    n00b_cg_val_t nm1      = n00b_cg_emit_sub(cg, n, _n00b_cg_const_i64(cg, 1));
    n00b_cg_val_t args1[1] = {nm1};
    n00b_cg_val_t r1       = n00b_cg_emit_call(cg, "fib", args1, 1,
                                                 .ret = N00B_CG_I64);

    // fib(n-2)
    n00b_cg_val_t nm2      = n00b_cg_emit_sub(cg, n, _n00b_cg_const_i64(cg, 2));
    n00b_cg_val_t args2[1] = {nm2};
    n00b_cg_val_t r2       = n00b_cg_emit_call(cg, "fib", args2, 1,
                                                 .ret = N00B_CG_I64);

    n00b_cg_emit_ret(cg, n00b_cg_emit_add(cg, r1, r2));
    n00b_cg_end_func(cg);

    typedef int64_t (*fib_fn)(int64_t);
    fib_fn fib = (fib_fn)n00b_codegen_jit(cg, "fib");
    assert(fib != NULL);

    assert(fib(0) == 0);
    assert(fib(1) == 1);
    assert(fib(5) == 5);
    assert(fib(10) == 55);
    assert(fib(20) == 6765);

    n00b_codegen_free(cg);
    printf("  [PASS] builder_fibonacci\n");
}

// ============================================================================
// 13. Operator mapping
// ============================================================================

static void
test_operator_mapping(void)
{
    n00b_codegen_t *cg = n00b_codegen_new(NULL);

    // Default ops are pre-loaded.
    int32_t op = n00b_cg_lookup_op(cg, "+");
    assert(op == N00B_CG_OP_ADD);

    op = n00b_cg_lookup_op(cg, "==");
    assert(op == N00B_CG_OP_EQ);

    op = n00b_cg_lookup_op(cg, "&&");
    assert(op == N00B_CG_OP_LOGICAL_AND);

    // Custom op.
    n00b_codegen_map_operator(cg, "**", N00B_CG_OP_MUL); // Map ** to MUL for testing.
    op = n00b_cg_lookup_op(cg, "**");
    assert(op == N00B_CG_OP_MUL);

    // Unknown op.
    op = n00b_cg_lookup_op(cg, "???");
    assert(op == -1);

    n00b_codegen_free(cg);
    printf("  [PASS] operator_mapping\n");
}

// ============================================================================
// 14. Dump (smoke test)
// ============================================================================

static void
test_dump(void)
{
    n00b_codegen_t *cg = n00b_codegen_new(NULL);

    n00b_cg_begin_func(cg, "noop", .ret = N00B_CG_I64);
    n00b_cg_emit_ret(cg, _n00b_cg_const_i64(cg, 0));
    n00b_cg_end_func(cg);

    // JIT to trigger module finish + linking, then dump.
    void *fn = n00b_codegen_jit(cg, "noop");
    assert(fn != NULL);

    // Dump to /dev/null — just verify it doesn't crash.
    FILE *f = fopen("/dev/null", "w");

    if (f) {
        n00b_codegen_dump(cg, f);
        fclose(f);
    }

    n00b_codegen_free(cg);
    printf("  [PASS] dump\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running codegen tests...\n");

    test_builder_add_interp();
    test_builder_add_jit();
    test_builder_arithmetic();
    test_builder_comparisons();
    test_builder_branches();
    test_builder_loops();
    test_builder_constants();
    test_builder_double();
    test_builder_import();
    test_builder_internal_call();
    test_builder_bitwise();
    test_builder_fibonacci();
    test_operator_mapping();
    test_dump();

    printf("All codegen tests passed.\n");
    n00b_shutdown();
    return 0;
}
