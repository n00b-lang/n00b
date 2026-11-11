#include "ir/ir.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static void test_branch_paths(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, S("liveness"), &ir_profile_bce,
                                         S("test"));
    ir_func_t *function = ir_func_add(module, S("branch"),
        ir_type_parse(module, S("(bool, bool) -> bool")), IR_LINK_EXTERNAL);
    ir_value_t *value = ir_param_add(function, S("value"),
                                      ir_type_parse(module, S("bool")));
    ir_value_t *condition = ir_param_add(function, S("condition"),
                                          ir_type_parse(module, S("bool")));
    ir_block_t *entry = ir_block_add(function, S("entry"));
    ir_block_t *left = ir_block_add(function, S("left"));
    ir_block_t *right = ir_block_add(function, S("right"));
    ir_instr_t *point = ir_instr_add(entry, IR_OP_CONST);
    ir_instr_set_dest(point, S("marker"), ir_type_parse(module, S("bool")));
    ir_instr_operand_int(point, 0);
    ir_instr_t *branch = ir_instr_add(entry, IR_OP_BRANCH);
    ir_instr_operand_value(branch, condition);
    ir_instr_operand_block(branch, left);
    ir_instr_operand_block(branch, right);
    ir_instr_t *left_return = ir_instr_add(left, IR_OP_RETURN);
    ir_instr_t *right_return = ir_instr_add(right, IR_OP_RETURN);
    ir_instr_operand_value(right_return, value);

    check("a use on any reachable successor path keeps a value live",
          ir_value_liveness_after(value, point) == IR_LIVENESS_LIVE);
    check("a sibling branch is not considered after a returning path",
          ir_value_liveness_after(value, left_return) == IR_LIVENESS_DEAD);
    check("the return operand is consumed at its own instruction",
          ir_value_liveness_after(value, right_return) == IR_LIVENESS_DEAD);
    check("edge liveness distinguishes a dead branch from its live sibling",
          ir_value_liveness_on_edge(value, entry, left) == IR_LIVENESS_DEAD
              && ir_value_liveness_on_edge(value, entry, right)
                     == IR_LIVENESS_LIVE);
    check("edge liveness rejects a pair that is not a CFG edge",
          ir_value_liveness_on_edge(value, left, right)
              == IR_LIVENESS_UNKNOWN);
    ir_ctx_free(ctx);
}

static void test_unreachable_use(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, S("liveness"), &ir_profile_bce,
                                         S("test"));
    ir_func_t *function = ir_func_add(module, S("unreachable"),
        ir_type_parse(module, S("(bool) -> bool")), IR_LINK_EXTERNAL);
    ir_value_t *value = ir_param_add(function, S("value"),
                                      ir_type_parse(module, S("bool")));
    ir_block_t *entry = ir_block_add(function, S("entry"));
    ir_block_t *dead = ir_block_add(function, S("dead"));
    ir_instr_t *point = ir_instr_add(entry, IR_OP_RETURN);
    ir_instr_t *dead_return = ir_instr_add(dead, IR_OP_RETURN);
    ir_instr_operand_value(dead_return, value);

    check("uses in unreachable blocks do not extend liveness",
          ir_value_liveness_after(value, point) == IR_LIVENESS_DEAD);
    ir_ctx_free(ctx);
}

static void test_incomplete_facts(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, S("liveness"), &ir_profile_bce,
                                         S("test"));
    ir_func_t *function = ir_func_add(module, S("non_dominating"),
        ir_type_parse(module, S("(bool) -> bool")), IR_LINK_EXTERNAL);
    ir_value_t *condition = ir_param_add(function, S("condition"),
                                          ir_type_parse(module, S("bool")));
    ir_block_t *entry = ir_block_add(function, S("entry"));
    ir_block_t *definition = ir_block_add(function, S("definition"));
    ir_block_t *use = ir_block_add(function, S("use"));
    ir_instr_t *branch = ir_instr_add(entry, IR_OP_BRANCH);
    ir_instr_operand_value(branch, condition);
    ir_instr_operand_block(branch, definition);
    ir_instr_operand_block(branch, use);
    ir_instr_t *defined = ir_instr_add(definition, IR_OP_CONST);
    ir_instr_set_dest(defined, S("defined"), ir_type_parse(module, S("bool")));
    ir_instr_operand_int(defined, 1);
    ir_instr_t *definition_return = ir_instr_add(definition, IR_OP_RETURN);
    ir_instr_operand_value(definition_return, defined->dest);
    ir_instr_t *use_return = ir_instr_add(use, IR_OP_RETURN);
    ir_instr_operand_value(use_return, defined->dest);

    check("a definition that does not dominate the query is unknown",
          ir_value_liveness_after(defined->dest, use_return)
              == IR_LIVENESS_UNKNOWN
              && ir_value_liveness_on_edge(defined->dest, entry, use)
                     == IR_LIVENESS_UNKNOWN);

    ir_func_t *incomplete = ir_func_add(module, S("incomplete"),
        ir_type_parse(module, S("() -> void")), IR_LINK_EXTERNAL);
    ir_block_t *open_block = ir_block_add(incomplete, S("open"));
    ir_instr_t *open_point = ir_instr_add(open_block, IR_OP_CONST);
    ir_instr_set_dest(open_point, S("open_value"),
                      ir_type_parse(module, S("i:32")));
    ir_instr_operand_int(open_point, 1);
    check("a reachable unterminated block has unknown liveness",
          ir_value_liveness_after(open_point->dest, open_point)
              == IR_LIVENESS_UNKNOWN);
    ir_ctx_free(ctx);
}

static void test_loop_backedge(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, S("liveness"), &ir_profile_bce,
                                         S("test"));
    ir_func_t *function = ir_func_add(module, S("loop"),
        ir_type_parse(module, S("(bool) -> bool")), IR_LINK_EXTERNAL);
    ir_value_t *value = ir_param_add(function, S("value"),
                                      ir_type_parse(module, S("bool")));
    ir_block_t *entry = ir_block_add(function, S("entry"));
    ir_block_t *loop = ir_block_add(function, S("loop"));
    ir_instr_t *jump = ir_instr_add(entry, IR_OP_JUMP);
    ir_instr_operand_block(jump, loop);
    ir_instr_t *select = ir_instr_add(loop, IR_OP_SELECT);
    ir_instr_set_dest(select, S("used"), ir_type_parse(module, S("bool")));
    ir_instr_operand_value(select, value);
    ir_instr_operand_value(select, value);
    ir_instr_operand_value(select, value);
    ir_instr_t *point = ir_instr_add(loop, IR_OP_CONST);
    ir_instr_set_dest(point, S("marker"), ir_type_parse(module, S("bool")));
    ir_instr_operand_int(point, 0);
    ir_instr_t *backedge = ir_instr_add(loop, IR_OP_JUMP);
    ir_instr_operand_block(backedge, loop);
    ir_func_t *other_function = ir_func_add(module, S("other"),
        ir_type_parse(module, S("(bool) -> bool")), IR_LINK_EXTERNAL);
    ir_value_t *other_value = ir_param_add(other_function, S("other_value"),
                                           ir_type_parse(module, S("bool")));

    check("a loop backedge revisits uses before the queried point",
          ir_value_liveness_after(value, point) == IR_LIVENESS_LIVE
              && ir_value_liveness_on_edge(value, loop, loop)
                     == IR_LIVENESS_LIVE);
    check("the query rejects values from another function",
          ir_value_liveness_after(other_value, point) == IR_LIVENESS_UNKNOWN);
    ir_ctx_free(ctx);
}

static void test_phi_edge_liveness(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, S("liveness"), &ir_profile_bce,
                                         S("test"));
    ir_func_t *function = ir_func_add(module, S("phi_backedge"),
        ir_type_parse(module, S("(bool, bool) -> bool")), IR_LINK_EXTERNAL);
    ir_value_t *seed = ir_param_add(function, S("seed"),
                                     ir_type_parse(module, S("bool")));
    ir_value_t *again = ir_param_add(function, S("again"),
                                      ir_type_parse(module, S("bool")));
    ir_block_t *entry = ir_block_add(function, S("entry"));
    ir_block_t *loop = ir_block_add(function, S("loop"));
    ir_block_t *exit = ir_block_add(function, S("exit"));
    ir_instr_t *enter = ir_instr_add(entry, IR_OP_JUMP);
    ir_instr_operand_block(enter, loop);
    ir_instr_t *phi = ir_instr_add(loop, IR_OP_PHI);
    ir_instr_set_dest(phi, S("carried"), ir_type_parse(module, S("bool")));
    ir_instr_operand_phi_arm(phi, entry, seed);
    ir_instr_operand_phi_arm(phi, loop, phi->dest);
    ir_instr_t *branch = ir_instr_add(loop, IR_OP_BRANCH);
    ir_instr_operand_value(branch, again);
    ir_instr_operand_block(branch, loop);
    ir_instr_operand_block(branch, exit);
    ir_instr_t *ret = ir_instr_add(exit, IR_OP_RETURN);
    ir_instr_operand_value(ret, seed);

    check("PHI self-use keeps its previous-iteration value live on the backedge",
          ir_value_liveness_on_edge(phi->dest, loop, loop)
              == IR_LIVENESS_LIVE);
    check("a value defined in the edge destination is not assumed available",
          ir_value_liveness_on_edge(phi->dest, entry, loop)
              == IR_LIVENESS_UNKNOWN);
    check("a sibling exit edge does not inherit the PHI backedge use",
          ir_value_liveness_on_edge(phi->dest, loop, exit)
              == IR_LIVENESS_DEAD);
    ir_ctx_free(ctx);
}

static void test_fresh_construction_storage(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, S("fresh_lifetime"),
        &ir_profile_bce, S("test"));
    ir_func_t *caller = ir_func_add(module, S("caller"),
        ir_type_parse(module, S("() -> void")), IR_LINK_INTERNAL);
    ir_block_t *entry = ir_block_add(caller, S("entry"));
    ir_instr_t *storage = ir_instr_add(entry, IR_OP_ALLOC);
    ir_instr_operand_type(storage, ir_type_parse(module, S("i32")));
    ir_instr_set_dest(storage, S("storage"),
        ir_type_parse(module, S("ptr[i32]")));
    ir_value_t *initializer = ir_alloc(ctx, sizeof(*initializer));
    initializer->kind = IR_VAL_FUNC;
    initializer->type = ir_type_parse(module, S("(inout i32) -> void"));
    ir_instr_t *call = ir_instr_add(entry, IR_OP_CALL);
    bool built = ir_instr_operand_value(call, initializer)
        && ir_instr_operand_fresh(call, storage->dest);
    ir_instr_t *load = ir_instr_add(entry, IR_OP_LOAD);
    built = built && ir_instr_operand_value(load, storage->dest);
    ir_instr_set_dest(load, S("result"), ir_type_parse(module, S("i32")));
    ir_instr_add(entry, IR_OP_RETURN);

    check("fresh modify construction does not make local storage escape",
          built && ir_allocation_has_only_local_uses(caller, storage));
    ir_ctx_free(ctx);
}

int main(void)
{
    test_branch_paths();
    test_unreachable_use();
    test_incomplete_facts();
    test_loop_backedge();
    test_phi_edge_liveness();
    test_fresh_construction_storage();
    return fails != 0;
}
