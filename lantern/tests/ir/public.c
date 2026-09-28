#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/libir.h"
#include "harness.h"

static ir_module_t *build(ir_ctx_t *ctx)
{
    ir_module_t *m = ir_module_new(ctx, "m", &ir_profile_bce, "host");

    if (!m) {
        return nullptr;
    }

    th_type   *i32 = ir_type_parse(m, "i32");
    th_type   *sig = ir_type_parse(m, "(i32) -> i32");
    ir_func_t *f   = ir_func_add(m, "twice", sig, IR_LINK_EXTERNAL);

    if (!f || !i32) {
        return nullptr;
    }

    ir_value_t *n = ir_param_add(f, "n", i32);
    ir_block_t *b = ir_block_add(f, "entry");
    ir_instr_t *a = ir_instr_add(b, IR_OP_ADD);

    ir_instr_set_dest(a, "s", i32);
    ir_instr_operand_value(a, n);
    ir_instr_operand_value(a, n);
    ir_instr_add_mod(a, "wrap");

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
        char *why = ir_diags_render_text(d, &(ir_render_t){0});

        printf("%s", why ? why : "(no diagnostics)\n");
        free(why);
    }

    char *text = ir_module_print(m);

    check("and prints", text != nullptr);
    check("and what it prints is what was built",
          text && strstr(text, "func @twice(%n: i32) -> i32")
              && strstr(text, "%s: i32 = add wrap %n, %n"));

    const ir_profile_t *const profiles[] = {&ir_profile_bce};
    ir_diags_t               *d2 = ir_diags_new(ctx, nullptr);
    ir_module_t              *back
        = text ? ir_module_parse(ctx, text, 0, profiles, 1, d2) : nullptr;

    check("and reads back", back != nullptr);

    char *again = back ? ir_module_print(back) : nullptr;

    check("to the same module", again && text && !strcmp(text, again));

    ir_module_t *extern_module = ir_module_new(ctx, "externs",
        &ir_profile_bce, "host");
    th_type *extern_signature = ir_type_parse(extern_module,
        "(i32, bool) -> i32");
    ir_func_t *external = extern_signature
        ? ir_func_add(extern_module, "external", extern_signature,
                      IR_LINK_EXTERN) : nullptr;
    char *extern_text = extern_module ? ir_module_print(extern_module) : nullptr;
    check("an external signature prints its positional parameter types",
        external && extern_text && strstr(extern_text,
            "extern func @external(%arg0: i32, %arg1: bool) -> i32"));

    check("the type target is reachable from the umbrella header",
          ir_type_parse(m, "(i32, *u8) -> bool") != nullptr);

    size_t np    = ir_tree_pass_count();
    bool   named = np > 0;
    bool   saw_symcheck = false, saw_type_diags = false;

    for (size_t i = 0; i < np; i++) {
        const ir_pass_t *tp = ir_tree_pass_at(i);

        named = named && tp && tp->name && *tp->name && tp->run;

        if (tp == &ir_pass_symcheck) {
            saw_symcheck = true;
        }

        if (tp == &ir_pass_type_diags) {
            saw_type_diags = true;
        }
    }

    check("every tree pass is enumerable and named", named);
    check("ir_tree_pass_at is exhaustive: symcheck is in it", saw_symcheck);
    check("ir_tree_pass_at is exhaustive: type-diags is in it",
          saw_type_diags);
    check("a pass past the end is nullptr, not out of bounds",
          ir_tree_pass_at(np) == nullptr);
    check("a tree pass on a module with no tree does not run",
          ir_tree_pass_run(&ir_pass_annot_scope, m, nullptr, nullptr, d)
              != IR_PASS_OK);

    free(text);
    free(again);
    free(extern_text);
    ir_ctx_free(ctx);

    return report("ir public");
}
