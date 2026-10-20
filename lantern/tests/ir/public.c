#include <stdio.h>
#include <stdlib.h>

#include "ir/libir.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static bool has(core_string_t hay, core_string_t needle)
{
    return hay.data && core_string_find(hay, needle) >= 0;
}

static ir_module_t *build(ir_ctx_t *ctx)
{
    ir_module_t *m = ir_module_new(ctx, S("m"), &ir_profile_bce, S("host"));

    if (!m) {
        return nullptr;
    }

    th_type   *i32 = ir_type_parse(m, S("i32"));
    th_type   *sig = ir_type_parse(m, S("(i32) -> i32"));
    ir_func_t *f   = ir_func_add(m, S("twice"), sig, IR_LINK_EXTERNAL);

    if (!f || !i32) {
        return nullptr;
    }

    ir_value_t *n = ir_param_add(f, S("n"), i32);
    ir_block_t *b = ir_block_add(f, S("entry"));
    ir_instr_t *a = ir_instr_add(b, IR_OP_ADD);

    ir_instr_set_dest(a, S("s"), i32);
    ir_instr_operand_value(a, n);
    ir_instr_operand_value(a, n);
    ir_instr_add_mod(a, S("wrap"));

    ir_instr_t *r = ir_instr_add(b, IR_OP_RETURN);

    ir_instr_operand_value(r, a->dest);
    ir_module_establish(m, IR_PROP_PARSED);

    return m;
}

int main(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = build(ctx);

    check("a module can be built through the public header", m != nullptr);

    if (!m) {
        return 1;
    }

    ir_diags_t *d = ir_diags_new(ctx, nullptr);
    ir_props_t  p = ir_verify(m, d);

    check("and it verifies as typed", (p & IR_PROP_TYPED) != 0);

    if (!(p & IR_PROP_TYPED)) {
        core_string_t why   = ir_diags_render_text(d, &(ir_render_t){0});
        core_string_t shown = why.data ? why : S("(no diagnostics)\n");

        printf("%.*s", (int)shown.u8_bytes, shown.data);
        core_free(why.data);
    }

    core_string_t text = ir_module_print(m);

    check("and prints", text.data != nullptr);
    check("and what it prints is what was built",
          has(text, S("func @twice(%n: i32) -> i32"))
              && has(text, S("%s: i32 = add wrap %n, %n")));

    const ir_profile_t *const profiles[] = {&ir_profile_bce};
    ir_diags_t               *d2 = ir_diags_new(ctx, nullptr);
    ir_module_t              *back
        = text.data ? ir_module_parse(ctx, text, 0, profiles, 1, d2) : nullptr;

    check("and reads back", back != nullptr);

    core_string_t again = back ? ir_module_print(back) : (core_string_t){};

    check("to the same module",
          again.data && text.data && core_string_eq(text, again));

    ir_module_t *extern_module = ir_module_new(ctx, S("externs"),
        &ir_profile_bce, S("host"));
    th_type *extern_signature = ir_type_parse(extern_module,
        S("(i32, bool) -> i32"));
    ir_func_t *external = extern_signature
        ? ir_func_add(extern_module, S("external"), extern_signature,
                      IR_LINK_EXTERN) : nullptr;
    core_string_t extern_text = extern_module ? ir_module_print(extern_module)
                                              : (core_string_t){};
    check("an external signature prints its positional parameter types",
        external && has(extern_text,
            S("extern func @external(%arg0: i32, %arg1: bool) -> i32")));

    check("the type target is reachable from the umbrella header",
          ir_type_parse(m, S("(i32, *u8) -> bool")) != nullptr);

    th_ctx *types = ir_module_types(m);
    th_type *open_type = th_var(types);
    th_type *closed_type = ir_type_parse(m, S("i32"));
    uint64_t open_id = ir_type_id(m, open_type);
    uint64_t closed_id = ir_type_id(m, closed_type);
    check("closed type IDs remain cached", closed_id
          && ir_type_id(m, closed_type) == closed_id);
    check("open type IDs refine after unification", open_id
          && th_unify_in_place(types, open_type, closed_type)
          && ir_type_id(m, open_type) == closed_id);

    core_free(text.data);
    core_free(again.data);
    core_free(extern_text.data);
    ir_ctx_free(ctx);

    return report("ir public");
}
