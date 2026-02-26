/**
 * @file test_mir.c
 * @brief Smoke test for the MIR JIT compiler subproject.
 *
 * Verifies that MIR compiles, links, and can build + interpret a
 * trivial function (add two i64 values).
 */

#include <stdio.h>
#include <assert.h>
#include <stdint.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"

#include "mir.h"
#include "mir-gen.h"

// ============================================================================
// 1. Build an "add(x, y) → x + y" function via the MIR API and interpret it.
// ============================================================================

static void
test_mir_interp_add(void)
{
    MIR_context_t ctx = MIR_init();

    // Create module.
    MIR_module_t m = MIR_new_module(ctx, "test_add");

    // Create function: int64_t add(int64_t x, int64_t y)
    MIR_type_t res_type = MIR_T_I64;
    MIR_item_t func = MIR_new_func(ctx, "add", 1, &res_type, 2,
                                   MIR_T_I64, "x", MIR_T_I64, "y");

    // Get registers for parameters (auto-created by MIR_new_func).
    MIR_reg_t x   = MIR_reg(ctx, "x", func->u.func);
    MIR_reg_t y   = MIR_reg(ctx, "y", func->u.func);
    MIR_reg_t res = MIR_new_func_reg(ctx, func->u.func, MIR_T_I64, "res");

    // ADD res, x, y
    MIR_append_insn(ctx, func,
                    MIR_new_insn(ctx, MIR_ADD,
                                 MIR_new_reg_op(ctx, res),
                                 MIR_new_reg_op(ctx, x),
                                 MIR_new_reg_op(ctx, y)));

    // RET res
    MIR_append_insn(ctx, func,
                    MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, res)));

    MIR_finish_func(ctx);
    MIR_finish_module(ctx);

    // Load module and set up interpreter.
    MIR_load_module(ctx, m);
    MIR_link(ctx, MIR_set_interp_interface, NULL);

    // Interpret: add(3, 4) should yield 7.
    MIR_val_t result;
    MIR_interp(ctx, func, &result, 2, (int64_t)3, (int64_t)4);
    assert(result.i == 7);

    // Interpret: add(-10, 25) should yield 15.
    MIR_interp(ctx, func, &result, 2, (int64_t)-10, (int64_t)25);
    assert(result.i == 15);

    MIR_finish(ctx);
    printf("  [PASS] mir_interp_add\n");
}

// ============================================================================
// 2. Build and JIT-compile the same function, call via function pointer.
// ============================================================================

static void
test_mir_gen_add(void)
{
    MIR_context_t ctx = MIR_init();

    MIR_module_t m = MIR_new_module(ctx, "test_gen");

    MIR_type_t res_type = MIR_T_I64;
    MIR_item_t func = MIR_new_func(ctx, "add", 1, &res_type, 2,
                                   MIR_T_I64, "x", MIR_T_I64, "y");

    MIR_reg_t x   = MIR_reg(ctx, "x", func->u.func);
    MIR_reg_t y   = MIR_reg(ctx, "y", func->u.func);
    MIR_reg_t res = MIR_new_func_reg(ctx, func->u.func, MIR_T_I64, "res");

    MIR_append_insn(ctx, func,
                    MIR_new_insn(ctx, MIR_ADD,
                                 MIR_new_reg_op(ctx, res),
                                 MIR_new_reg_op(ctx, x),
                                 MIR_new_reg_op(ctx, y)));
    MIR_append_insn(ctx, func,
                    MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, res)));

    MIR_finish_func(ctx);
    MIR_finish_module(ctx);

    MIR_load_module(ctx, m);

    // Initialize generator and compile.
    MIR_gen_init(ctx);
    MIR_link(ctx, MIR_set_gen_interface, NULL);

    typedef int64_t (*add_fn)(int64_t, int64_t);
    add_fn compiled = (add_fn)MIR_gen(ctx, func);
    assert(compiled != NULL);

    assert(compiled(3, 4) == 7);
    assert(compiled(-10, 25) == 15);
    assert(compiled(0, 0) == 0);
    assert(compiled(INT64_MAX, 0) == INT64_MAX);

    MIR_gen_finish(ctx);
    MIR_finish(ctx);
    printf("  [PASS] mir_gen_add\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running MIR smoke tests...\n");

    test_mir_interp_add();
    test_mir_gen_add();

    printf("All MIR tests passed.\n");
    n00b_shutdown();
    return 0;
}
