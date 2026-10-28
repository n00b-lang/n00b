#include <stdio.h>
#include <stdlib.h>

#include "ir/ir.h"
#include "ir/text.h"
#include "ir/walk.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static const core_string_t SRC = S(
    "module m {\n"
    "  profile bce\n"
    "  global @g: i32\n"
    "  global @h: i32\n"
    "  func @f(%a: i32, %b: i32) -> i32 {\n"
    "    local $t: i32\n"
    "    block entry:\n"
    "      %c: bool = lt %a, %b\n"
    "      branch %c, yes, no\n"
    "    block yes:\n"
    "      return %a\n"
    "    block no:\n"
    "      return %b\n"
    "  }\n"
    "}\n");

typedef struct {
    int globals, funcs, params, locals, blocks, instrs, operands, returns;
    int func_enter, func_leave, block_enter, block_leave;
    char order[256];
    size_t at;
} tally;

static bool order_is(const tally *t, core_string_t want)
{
    return core_string_eq(core_string_borrow(t->order, t->at), want);
}

static void note(tally *t, char c)
{
    if (t->at + 1 < sizeof(t->order)) {
        t->order[t->at++] = c;
    }
}

static bool on_global(ir_visit_t *v) { ((tally *)v->user)->globals++; return true; }
static bool on_param(ir_visit_t *v)  { ((tally *)v->user)->params++;  return true; }
static bool on_local(ir_visit_t *v)  { ((tally *)v->user)->locals++;  return true; }
static bool on_operand(ir_visit_t *v){ ((tally *)v->user)->operands++; return true; }

static bool func_in(ir_visit_t *v)
{
    tally *t = v->user;

    t->funcs++;
    t->func_enter++;
    note(t, 'F');

    return true;
}

static bool func_out(ir_visit_t *v)
{
    tally *t = v->user;

    t->func_leave++;
    note(t, 'f');

    return true;
}

static bool block_in(ir_visit_t *v)
{
    tally *t = v->user;

    t->blocks++;
    t->block_enter++;
    note(t, 'B');

    return true;
}

static bool block_out(ir_visit_t *v)
{
    tally *t = v->user;

    t->block_leave++;
    note(t, 'b');

    return true;
}

static bool on_instr(ir_visit_t *v)
{
    tally *t = v->user;

    t->instrs++;
    note(t, 'i');

    return true;
}

static bool on_return(ir_visit_t *v)
{
    ((tally *)v->user)->returns++;

    return true;
}

static bool skip_children(ir_visit_t *v)
{
    (void)v;

    return false;
}

static bool stop_here(ir_visit_t *v)
{
    ir_walk_stop(v->walk);

    return true;
}

static ir_module_t *load(ir_ctx_t **ctx)
{
    const ir_profile_t *const ps[] = {&ir_profile_bce, &ir_profile_synth};

    *ctx = ir_ctx_new();

    ir_diags_t *d = ir_diags_new(*ctx, nullptr);

    return ir_module_parse(*ctx, SRC, 0, ps, 2, d);
}

static void test_the_walk_reaches_every_node(void)
{
    ir_ctx_t    *ctx;
    ir_module_t *m = load(&ctx);

    if (!m) {
        check("the fixture parses", false);

        return;
    }

    tally      t = {0};
    ir_walk_t *w = ir_walk_new(&t);

    ir_walk_on_global(w, on_global, nullptr);
    ir_walk_on_func(w, func_in, func_out, nullptr);
    ir_walk_on_param(w, on_param, nullptr);
    ir_walk_on_local(w, on_local, nullptr);
    ir_walk_on_block(w, block_in, block_out, nullptr);
    ir_walk_on_instr(w, on_instr, nullptr, nullptr);
    ir_walk_on_operand(w, on_operand, nullptr);
    ir_walk_on_op(w, IR_OP_RETURN, on_return, nullptr, nullptr);

    ir_walk_run(w, m);
    ir_walk_free(w);

    check("every global is visited", t.globals == 2);
    check("every function is visited", t.funcs == 1);
    check("every parameter is visited", t.params == 2);
    check("every local is visited", t.locals == 1);
    check("every block is visited", t.blocks == 3);
    check("every instruction is visited", t.instrs == 4);
    check("operands are visited", t.operands > 0);
    check("a selector by opcode fires only for that opcode", t.returns == 2);
    check("entering and leaving are balanced",
          t.func_enter == t.func_leave && t.block_enter == t.block_leave);
    check("blocks nest inside their function",
          order_is(&t, S("FBiibBibBibf")));

    ir_ctx_free(ctx);
}

static void test_a_pre_visitor_can_skip_children(void)
{
    ir_ctx_t    *ctx;
    ir_module_t *m = load(&ctx);

    if (!m) {
        return;
    }

    tally      t = {0};
    ir_walk_t *w = ir_walk_new(&t);

    ir_walk_on_block(w, skip_children, nullptr, nullptr);
    ir_walk_on_instr(w, on_instr, nullptr, nullptr);

    ir_walk_run(w, m);
    ir_walk_free(w);

    check("the only visitor refusing children skips them", t.instrs == 0);

    ir_ctx_free(ctx);
}

static void test_a_walk_can_be_stopped(void)
{
    ir_ctx_t    *ctx;
    ir_module_t *m = load(&ctx);

    if (!m) {
        return;
    }

    tally      t = {0};
    ir_walk_t *w = ir_walk_new(&t);

    ir_walk_on_block(w, stop_here, nullptr, nullptr);
    ir_walk_on_instr(w, on_instr, nullptr, nullptr);

    ir_walk_run(w, m);

    check("stopping in the first block reaches no instruction", t.instrs == 0);
    check("and the walk reports it stopped", ir_walk_stopped(w));

    ir_walk_free(w);
    ir_ctx_free(ctx);
}

static void test_a_function_can_be_walked_alone(void)
{
    ir_ctx_t    *ctx;
    ir_module_t *m = load(&ctx);

    if (!m) {
        return;
    }

    tally      t = {0};
    ir_walk_t *w = ir_walk_new(&t);

    ir_walk_on_global(w, on_global, nullptr);
    ir_walk_on_block(w, block_in, block_out, nullptr);

    ir_walk_run_func(w, ir_func_at(m, 0));
    ir_walk_free(w);

    check("walking one function reaches its blocks", t.blocks == 3);
    check("and does not reach the module's globals", t.globals == 0);

    ir_ctx_free(ctx);
}


static bool count_a(ir_visit_t *v)
{
    tally *t = v->user;

    note(t, 'a');
    t->instrs++;

    return true;
}

static bool count_b(ir_visit_t *v)
{
    note(v->user, 'b');

    return true;
}

static bool count_c(ir_visit_t *v)
{
    note(v->user, 'c');

    return true;
}

static void test_visitors_stack_in_one_pass(void)
{
    ir_ctx_t    *ctx;
    ir_module_t *m = load(&ctx);

    if (!m) {
        return;
    }

    tally      t = {0};
    ir_walk_t *w = ir_walk_new(&t);

    ir_walk_on_instr(w, count_a, nullptr, nullptr);
    ir_walk_on_instr(w, count_b, nullptr, nullptr);
    ir_walk_on_instr(w, count_c, nullptr, nullptr);

    ir_walk_run(w, m);
    ir_walk_free(w);

    check("three visitors on one node kind all run", t.instrs == 4);
    check("in one pass, in registration order, per node",
          order_is(&t, S("abcabcabcabc")));

    ir_ctx_free(ctx);
}

static void test_one_visitor_refusing_children_affects_all(void)
{
    ir_ctx_t    *ctx;
    ir_module_t *m = load(&ctx);

    if (!m) {
        return;
    }

    tally      t = {0};
    ir_walk_t *w = ir_walk_new(&t);

    ir_walk_on_block(w, skip_children, nullptr, nullptr);
    ir_walk_on_block(w, block_in, nullptr, nullptr);
    ir_walk_on_instr(w, on_instr, nullptr, nullptr);

    ir_walk_run(w, m);
    ir_walk_free(w);

    check("a visitor that refuses children still lets its peers run",
          t.blocks == 3);
    check("and one visitor's refusal does not prune another's traversal",
          t.instrs == 4);

    ir_ctx_free(ctx);
}

static void test_an_instruction_visitor_can_skip_operands(void)
{
    ir_ctx_t    *ctx;
    ir_module_t *m = load(&ctx);

    if (!m) {
        return;
    }

    tally      t = {0};
    ir_walk_t *w = ir_walk_new(&t);

    ir_walk_on_instr(w, skip_children, nullptr, nullptr);
    ir_walk_on_operand(w, on_operand, nullptr);

    ir_walk_run(w, m);
    ir_walk_free(w);

    check("an instruction visitor refusing children skips the operands",
          t.operands == 0);

    tally      u  = {0};
    ir_walk_t *w2 = ir_walk_new(&u);

    ir_walk_on_instr(w2, skip_children, nullptr, nullptr);
    ir_walk_on_op(w2, IR_OP_RETURN, on_return, nullptr, nullptr);
    ir_walk_on_operand(w2, on_operand, nullptr);

    ir_walk_run(w2, m);
    ir_walk_free(w2);

    check("an opcode visitor that wants children overrides the refusal",
          u.operands == 2 && u.returns == 2);

    ir_ctx_free(ctx);
}

typedef struct {
    ir_func_t *functions[4];
    size_t count;
} closure_t;

static bool collect_reachable_function(ir_func_t *function, void *user)
{
    closure_t *closure = user;
    if (closure->count == 4) return false;
    closure->functions[closure->count++] = function;
    return true;
}

static void test_reachable_function_walk_follows_live_function_references(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, S("closure"), &ir_profile_bce,
                                        S("host"));
    th_type *signature = ir_type_parse(module, S("() -> void"));
    ir_func_t *root = ir_func_add(module, S("root"), signature,
                                  IR_LINK_EXTERNAL);
    ir_func_t *middle = ir_func_add(module, S("middle"), signature,
                                    IR_LINK_INTERNAL);
    ir_func_t *leaf = ir_func_add(module, S("leaf"), signature,
                                  IR_LINK_INTERNAL);
    ir_func_t *referenced = ir_func_add(module, S("referenced"), signature,
                                        IR_LINK_INTERNAL);
    ir_func_t *dead = ir_func_add(module, S("dead"), signature,
                                  IR_LINK_INTERNAL);
    ir_block_t *root_entry = ir_block_add(root, S("entry"));
    ir_block_t *root_dead = ir_block_add(root, S("dead"));
    ir_block_t *middle_entry = ir_block_add(middle, S("entry"));
    ir_block_t *leaf_entry = ir_block_add(leaf, S("entry"));
    ir_block_t *referenced_entry = ir_block_add(referenced, S("entry"));
    ir_block_t *dead_entry = ir_block_add(dead, S("entry"));
    ir_instr_t *root_call = ir_instr_add(root_entry, IR_OP_CALL);
    ir_instr_t *root_reference = ir_instr_add(root_entry, IR_OP_COPY);
    ir_instr_t *dead_call = ir_instr_add(root_dead, IR_OP_CALL);
    ir_instr_t *middle_call = ir_instr_add(middle_entry, IR_OP_CALL);
    ir_value_t *function_reference = ir_func_reference(module, referenced);
    bool built = root_call && root_reference && dead_call && middle_call
        && function_reference
        && ir_instr_operand_value(root_call, ir_func_address(module, middle))
        && ir_instr_set_dest(root_reference, S("callback"),
                             function_reference->type)
        && ir_instr_operand_value(root_reference, function_reference)
        && ir_instr_operand_value(dead_call, ir_func_address(module, dead))
        && ir_instr_operand_value(middle_call, ir_func_address(module, leaf))
        && ir_instr_add(root_entry, IR_OP_RETURN)
        && ir_instr_add(root_dead, IR_OP_RETURN)
        && ir_instr_add(middle_entry, IR_OP_RETURN)
        && ir_instr_add(leaf_entry, IR_OP_RETURN)
        && ir_instr_add(referenced_entry, IR_OP_RETURN)
        && ir_instr_add(dead_entry, IR_OP_RETURN);
    closure_t closure = {0};
    check("reachable-function walking follows live calls and references",
          built && ir_walk_reachable_functions(&root, 1,
              collect_reachable_function, nullptr, &closure)
              && closure.count == 4
              && closure.functions[0] == root
              && closure.functions[1] == middle
              && closure.functions[2] == referenced
              && closure.functions[3] == leaf);
    ir_ctx_free(ctx);
}

typedef struct {
    closure_t closure;
    ir_func_t *declaration;
    ir_func_t *implementation;
} body_map_t;

static ir_func_t *resolve_body(ir_func_t *function, void *user)
{
    body_map_t *map = user;
    return function == map->declaration ? map->implementation : function;
}

static bool collect_through_bodies(ir_func_t *function, void *user)
{
    return collect_reachable_function(function, &((body_map_t *)user)->closure);
}

static void test_reachable_function_walk_scans_resolved_bodies(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, S("bodies"), &ir_profile_bce,
                                        S("host"));
    th_type *signature = ir_type_parse(module, S("() -> void"));
    ir_func_t *root = ir_func_add(module, S("root"), signature,
                                  IR_LINK_EXTERNAL);
    ir_func_t *declaration = ir_func_add(module, S("declaration"), signature,
                                         IR_LINK_EXTERN);
    ir_func_t *implementation = ir_func_add(module, S("implementation"),
                                            signature, IR_LINK_INTERNAL);
    ir_func_t *target = ir_func_add(module, S("target"), signature,
                                    IR_LINK_INTERNAL);
    ir_block_t *root_entry = ir_block_add(root, S("entry"));
    ir_block_t *implementation_entry = ir_block_add(implementation,
                                                    S("entry"));
    ir_block_t *target_entry = ir_block_add(target, S("entry"));
    ir_instr_t *root_call = ir_instr_add(root_entry, IR_OP_CALL);
    ir_instr_t *implementation_call = ir_instr_add(implementation_entry,
                                                   IR_OP_CALL);
    bool built = root_call && implementation_call
        && ir_instr_operand_value(root_call,
                                  ir_func_address(module, declaration))
        && ir_instr_operand_value(implementation_call,
                                  ir_func_address(module, target))
        && ir_instr_add(root_entry, IR_OP_RETURN)
        && ir_instr_add(implementation_entry, IR_OP_RETURN)
        && ir_instr_add(target_entry, IR_OP_RETURN);
    body_map_t map = {.declaration = declaration,
                      .implementation = implementation};
    check("reachable-function walking scans the body a declaration resolves to",
          built && ir_walk_reachable_functions(&root, 1,
              collect_through_bodies, resolve_body, &map)
              && map.closure.count == 3
              && map.closure.functions[0] == root
              && map.closure.functions[1] == declaration
              && map.closure.functions[2] == target);
    ir_ctx_free(ctx);
}

int main(void)
{
    test_the_walk_reaches_every_node();
    test_a_pre_visitor_can_skip_children();
    test_a_walk_can_be_stopped();
    test_a_function_can_be_walked_alone();
    test_visitors_stack_in_one_pass();
    test_one_visitor_refusing_children_affects_all();
    test_an_instruction_visitor_can_skip_operands();
    test_reachable_function_walk_follows_live_function_references();
    test_reachable_function_walk_scans_resolved_bodies();

    return report("ir walk");
}
