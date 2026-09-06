#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/ir.h"
#include "ir/text.h"
#include "ir/walk.h"

static int fails;

static void check(const char *what, bool ok)
{
    if (!ok) {
        fails++;
        printf("FAIL  %s\n", what);
    }
}

static const char *const SRC =
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
    "}\n";

typedef struct {
    int globals, funcs, params, locals, blocks, instrs, operands, returns;
    int func_enter, func_leave, block_enter, block_leave;
    char order[256];
    size_t at;
} tally;

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
          !strcmp(t.order, "FBiibBibBibf"));

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
          !strcmp(t.order, "abcabcabcabc"));

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

int main(void)
{
    test_the_walk_reaches_every_node();
    test_a_pre_visitor_can_skip_children();
    test_a_walk_can_be_stopped();
    test_a_function_can_be_walked_alone();
    test_visitors_stack_in_one_pass();
    test_one_visitor_refusing_children_affects_all();

    if (!fails) {
        printf("ir walk: all checks passed\n");
    }

    return fails ? 1 : 0;
}
