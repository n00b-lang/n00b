#include <stdio.h>
#include <stdlib.h>

#include "ir/analyze.h"
#include "ir/pass.h"
#include "ir/pdg.h"
#include "ir/text.h"
#include "ir/doms.h"
#include "ir/verify.h"
#include "lib/buffer.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static bool produced[IR_DIAG_COUNT];

static void same(core_string_t what, core_string_t got, core_string_t want)
{
    if (!got.data || !core_string_eq(got, want)) {
        core_string_t shown = got.data ? got : S("(null)");

        fails++;
        printf("FAIL  %.*s\n  want: %.*s\n  got:  %.*s\n", (int)what.u8_bytes,
               what.data, (int)want.u8_bytes, want.data,
               (int)shown.u8_bytes, shown.data);
    }
}

static const ir_profile_t *const PROFILES[] = {&ir_profile_bce};

typedef struct {
    ir_ctx_t    *ctx;
    ir_module_t *m;
    ir_diags_t  *d;
} run_t;

static run_t *load(core_string_t src)
{
    run_t *r = calloc(1, sizeof(run_t));

    r->ctx = ir_ctx_new();
    r->d   = ir_diags_new(r->ctx, nullptr);
    r->m   = ir_module_parse(r->ctx, src, 0, PROFILES, 1, r->d);

    if (r->m) {
        ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
    }

    return r;
}

static void done(run_t *r)
{
    for (uint32_t i = 0; i < ir_diags_count(r->d); i++) {
        produced[ir_diags_at(r->d, i)->code] = true;
    }
    ir_ctx_free(r->ctx);
    free(r);
}

static uint32_t counted(run_t *r, ir_diagcode_t code)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < ir_diags_count(r->d); i++) {
        n += ir_diags_at(r->d, i)->code == code;
    }

    return n;
}

static bool contains(core_string_t hay, core_string_t needle)
{
    return hay.data && core_string_find(hay, needle) >= 0;
}

static bool told(run_t *r, core_string_t text)
{
    for (uint32_t i = 0; i < ir_diags_count(r->d); i++) {
        const ir_diag_t *dg = ir_diags_at(r->d, i);

        if (contains(dg->primary.label, text) || contains(dg->message, text)) {
            return true;
        }
    }

    return false;
}

static ir_func_t *func_of(run_t *r)
{
    return r->m ? ir_func_at(r->m, 0) : nullptr;
}

static void tag(core_buffer_t *b, ir_instr_t *in)
{
    core_string_t name = in->dest_kind == IR_DEST_VALUE && in->dest
                           ? in->dest->name
                       : in->dest_kind == IR_DEST_LOCAL && in->dest_local
                           ? in->dest_local->name
                           : ir_op_spelling(in->op);

    core_buffer_printf(b, "%.*s#%u:%.*s", (int)in->block->name.u8_bytes,
                       in->block->name.data, in->index, (int)name.u8_bytes,
                       name.data);
}

static core_string_t sketch_result(core_buffer_t *b)
{
    if (!b->data) {
        return core_string_empty();
    }

    return (core_string_t){.data = b->data, .u8_bytes = b->byte_len};
}

static core_string_t data_sketch(ir_pdg_t *g)
{
    core_buffer_t b = {0};

    for (uint32_t i = 0; i < ir_pdg_node_count(g); i++) {
        ir_instr_t *in = ir_pdg_node_at(g, i);

        for (uint32_t k = 0; k < ir_pdg_in_count(g, in); k++) {
            const ir_dep_edge_t *e = ir_pdg_in_at(g, in, k);

            if (e->kind != IR_DEP_DATA) {
                continue;
            }
            tag(&b, e->from);
            core_buffer_append_string(&b, e->certainty == IR_DEP_EXACT
                                              ? S(" => ")
                                              : S(" ~> "));
            tag(&b, e->to);
            core_buffer_puts(&b, "\n");
        }
    }

    return sketch_result(&b);
}

static bool controlled_by(const ir_pdg_t *g, ir_block_t *b, ir_block_t *on)
{
    for (uint32_t i = 0; i < ir_controllers_of(g, b); i++) {
        if (ir_controller_at(g, b, i) == on) {
            return true;
        }
    }

    return false;
}

static core_string_t cdg_sketch(ir_pdg_t *g, ir_func_t *f)
{
    core_buffer_t b = {0};

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_block_t *bl = f->blocks[i];

        for (uint32_t k = 0; k < ir_controllers_of(g, bl); k++) {
            core_buffer_append_string(&b, bl->name);
            core_buffer_puts(&b, " on ");
            core_buffer_append_string(&b, ir_controller_at(g, bl, k)->name);
            core_buffer_puts(&b, "\n");
        }
    }

    return sketch_result(&b);
}

#define MOD(...) \
    S("module m {\n  profile bce\n  target t\n\n" __VA_ARGS__ "}\n")

static void test_the_cfg_is_the_blocks_and_the_terminators(void)
{
    run_t *r = load(MOD("  func @f(%c: bool) -> void {\n"
                        "    block entry:\n      branch %c, t, e\n"
                        "    block t:\n      jump join\n"
                        "    block e:\n      jump join\n"
                        "    block join:\n      return\n"
                        "  }\n"));
    ir_func_t *f = func_of(r);

    check("a parsed module has its blocks with no construction step",
          f && f->n_blocks == 4);

    ir_instr_t *t = ir_block_terminator(f->blocks[0]);

    check("successors come from the terminator", ir_succ_count(t) == 2);
    check("and they are the blocks themselves",
          ir_succ_at(t, 0) == f->blocks[1] && ir_succ_at(t, 1) == f->blocks[2]);
    done(r);
}

static void test_all_seven_edge_kinds(void)
{
    run_t *r = load(MOD(
        "  func @f(%c: bool, %n: i32) -> void {\n"
        "    block entry:\n      jump head\n"
        "    block head:\n      branch %c, body, out\n"
        "    block body:\n"
        "      switch %n, default: other, [0: one], [1: two]\n"
        "    block one:\n      jump latch\n"
        "    block two:\n      jump latch\n"
        "    block other:\n      jump latch\n"
        "    block latch:\n      jump head\n"
        "    block out:\n      branch %c, yes, no\n"
        "    block yes:\n      jump end\n"
        "    block no:\n      jump end\n"
        "    block end:\n      return\n"
        "  }\n"));
    ir_func_t *f    = func_of(r);
    ir_pdg_t  *g    = ir_pdg_of(f);
    bool          saw[7] = {0};
    core_buffer_t b      = {0};

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_instr_t *t = ir_block_terminator(f->blocks[i]);

        for (uint32_t s = 0; t && s < t->n_operands; s++) {
            if (!ir_succ_of(&t->operands[s])) {
                continue;
            }

            ir_edge_kind_t k = ir_edge_kind(g, f->blocks[i], s);

            saw[k] = true;
            core_buffer_append_string(&b, f->blocks[i]->name);
            core_buffer_puts(&b, " -> ");
            core_buffer_append_string(&b, ir_succ_of(&t->operands[s])->name);
            core_buffer_puts(&b, " ");
            core_buffer_append_string(&b, ir_edge_kind_name(k));
            core_buffer_puts(&b, "\n");
        }
    }

    same(S("every edge kind is derived from the shape, not recorded"),
         core_buffer_string(&b),
         S("entry -> head fallthrough\n"
         "head -> body branch-true\n"
         "head -> out loop-exit\n"
         "body -> other case\n"
         "body -> one case\n"
         "body -> two case\n"
         "one -> latch jump\n"
         "two -> latch jump\n"
         "other -> latch fallthrough\n"
         "latch -> head loop-back\n"
         "out -> yes branch-true\n"
         "out -> no branch-false\n"
         "yes -> end jump\n"
         "no -> end fallthrough\n"));

    for (uint32_t k = 0; k < 7; k++) {
        if (!saw[k]) {
            core_string_t kind = ir_edge_kind_name((ir_edge_kind_t)k);

            printf("FAIL  no edge of kind %.*s occurs in the fixture\n",
                   (int)kind.u8_bytes, kind.data);
            fails++;
        }
    }
    core_buffer_release(&b);
    done(r);
}

static void test_a_loop_inside_a_loop(void)
{
    run_t *r = load(MOD("  func @f(%c: bool, %d: bool) -> void {\n"
                        "    block entry:\n      jump outer\n"
                        "    block outer:\n      branch %c, inner, done\n"
                        "    block inner:\n      branch %d, spin, latch\n"
                        "    block spin:\n      jump inner\n"
                        "    block latch:\n      jump outer\n"
                        "    block done:\n      return\n"
                        "  }\n"));
    ir_func_t *f = func_of(r);
    ir_pdg_t     *g = ir_pdg_of(f);
    core_buffer_t b = {0};

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_instr_t *t = ir_block_terminator(f->blocks[i]);

        for (uint32_t s = 0; t && s < t->n_operands; s++) {
            if (!ir_succ_of(&t->operands[s])) {
                continue;
            }
            core_buffer_append_string(&b, f->blocks[i]->name);
            core_buffer_puts(&b, " -> ");
            core_buffer_append_string(&b, ir_succ_of(&t->operands[s])->name);
            core_buffer_puts(&b, " ");
            core_buffer_append_string(
                &b, ir_edge_kind_name(ir_edge_kind(g, f->blocks[i], s)));
            core_buffer_puts(&b, "\n");
        }
    }
    same(S("leaving the inner loop is an exit, and staying in it is not"),
         core_buffer_string(&b),
         S("entry -> outer fallthrough\n"
           "outer -> inner branch-true\n"
           "outer -> done loop-exit\n"
           "inner -> spin branch-true\n"
           "inner -> latch loop-exit\n"
           "spin -> inner loop-back\n"
           "latch -> outer loop-back\n"));
    core_buffer_release(&b);
    done(r);
}

static void test_keyed_by_identity_not_by_name(void)
{
    run_t *r = load(MOD("  func @f(%a: i32) -> i32 {\n"
                        "    local $x: i32\n"
                        "    block entry:\n"
                        "      $x = copy %a\n"
                        "      %res: i32 = add wrap $x, %a\n"
                        "      return %res\n"
                        "  }\n"));
    ir_func_t   *f  = func_of(r);
    ir_pdg_t    *g  = ir_pdg_of(f);
    ir_block_t  *e  = f->blocks[0];
    ir_instr_t  *dx = e->instrs[0];
    ir_instr_t  *ad = e->instrs[1];
    ir_instr_t  *rt = e->instrs[2];
    bool         by_local = false;
    bool         by_value = false;

    for (uint32_t k = 0; k < ir_pdg_in_count(g, ad); k++) {
        const ir_dep_edge_t *ed = ir_pdg_in_at(g, ad, k);

        by_local = by_local
                || (ed->kind == IR_DEP_DATA && ed->from == dx
                    && ed->via == f->locals[0]);
    }

    for (uint32_t k = 0; k < ir_pdg_in_count(g, rt); k++) {
        const ir_dep_edge_t *ed = ir_pdg_in_at(g, rt, k);

        by_value = by_value
                || (ed->kind == IR_DEP_DATA && ed->from == ad
                    && ed->via == nullptr);
    }
    check("a local edge names the ir_local_t it flows through", by_local);
    check("a value edge is the defining instruction itself", by_value);
    check("the local is found by identity, not by its spelling",
          f->locals[0] == ir_local_find(f, S("x")));
    done(r);
}

static void test_reaching_definitions_are_the_data_edges(void)
{
    run_t *r = load(MOD("  func @f(%a: i32) -> i32 {\n"
                        "    local $x: i32\n"
                        "    block entry:\n"
                        "      $x = copy %a\n"
                        "      $x = add wrap %a, %a\n"
                        "      %res: i32 = copy $x\n"
                        "      return %res\n"
                        "  }\n"));
    core_string_t s = data_sketch(ir_pdg_of(func_of(r)));

    same(S("the killed definition does not reach the use"), s,
         S("entry#1:x => entry#2:res\n"
           "entry#2:res => entry#3:return\n"));
    core_free(s.data);
    done(r);

    r = load(MOD("  func @f(%c: bool, %a: i32) -> i32 {\n"
                 "    local $x: i32\n"
                 "    block entry:\n"
                 "      $x = copy %a\n"
                 "      jump head\n"
                 "    block head:\n"
                 "      branch %c, body, out\n"
                 "    block body:\n"
                 "      $x = add wrap %a, %a\n"
                 "      jump head\n"
                 "    block out:\n"
                 "      %res: i32 = copy $x\n"
                 "      return %res\n"
                 "  }\n"));
    s = data_sketch(ir_pdg_of(func_of(r)));
    same(S("both the entry definition and the loop-carried one reach"), s,
         S("entry#0:x ~> out#0:res\n"
           "body#0:x ~> out#0:res\n"
           "out#0:res => out#1:return\n"));
    core_free(s.data);
    done(r);
}

static void test_under_ssa_def_use_is_immediate(void)
{
    run_t *r = load(MOD("  func @f(%c: bool, %a: i32) -> i32 {\n"
                        "    local $x: i32\n"
                        "    block entry:\n"
                        "      $x = copy %a\n"
                        "      branch %c, body, out\n"
                        "    block body:\n"
                        "      $x = add wrap %a, %a\n"
                        "      jump out\n"
                        "    block out:\n"
                        "      %res: i32 = copy $x\n"
                        "      return %res\n"
                        "  }\n"));
    ir_pdg_t *before = ir_pdg_of(func_of(r));

    check("with a local live across a join the graph is not exact",
          !ir_pdg_is_exact(before));

    ir_promote_opts_t o = {.diags = r->d};

    check("promote succeeds", ir_promote(r->m, &o));
    check("the module is in SSA form",
          ir_verify(r->m, r->d) & IR_PROP_SSA);

    ir_pdg_t  *g = ir_pdg_of(func_of(r));
    ir_func_t *f = func_of(r);
    bool       every_edge_is_a_definition = true;
    uint32_t   data = 0;

    check("under ssa the graph is exact", ir_pdg_is_exact(g));

    for (uint32_t i = 0; i < ir_pdg_edge_count(g); i++) {
        const ir_dep_edge_t *e = ir_pdg_edge_at(g, i);

        if (e->kind != IR_DEP_DATA) {
            continue;
        }
        data++;
        every_edge_is_a_definition = every_edge_is_a_definition
                                  && e->certainty == IR_DEP_EXACT
                                  && e->via == nullptr;
    }
    check("every data edge is exact and carries no local", data
          && every_edge_is_a_definition);
    check("no local survives promotion", f->n_locals == 0);

    for (uint32_t i = 0; i < ir_pdg_node_count(g); i++) {
        ir_instr_t *in = ir_pdg_node_at(g, i);

        for (uint32_t o2 = 0; o2 < in->n_operands; o2++) {
            ir_value_t *v = ir_operand_value(&in->operands[o2]);
            bool        linked = v == nullptr || v->kind != IR_VAL_INSTR;

            for (uint32_t k = 0; !linked && k < ir_pdg_in_count(g, in); k++) {
                linked = ir_pdg_in_at(g, in, k)->from == v->def;
            }
            check("each operand's definition is an edge into the use",
                  linked);
        }
    }
    done(r);
}

static void test_a_definition_reaching_through_two_joins(void)
{
    run_t *r = load(MOD("  func @f(%c: bool, %d: bool, %a: i32) -> i32 {\n"
                        "    local $x: i32\n"
                        "    block entry:\n"
                        "      $x = copy %a\n"
                        "      jump head\n"
                        "    block head:\n"
                        "      branch %c, body, out\n"
                        "    block body:\n"
                        "      branch %d, then, else\n"
                        "    block then:\n"
                        "      $x = add wrap %a, %a\n"
                        "      jump join\n"
                        "    block else:\n"
                        "      jump join\n"
                        "    block join:\n"
                        "      jump head\n"
                        "    block out:\n"
                        "      %res: i32 = copy $x\n"
                        "      return %res\n"
                        "  }\n"));
    core_string_t s = data_sketch(ir_pdg_of(func_of(r)));

    same(S("a definition reaches through a join inside a loop"), s,
         S("entry#0:x ~> out#0:res\n"
           "then#0:x ~> out#0:res\n"
           "out#0:res => out#1:return\n"));
    core_free(s.data);
    done(r);
}

static void test_the_two_directions_are_the_same_edges(void)
{
    run_t *r = load(MOD("  func @f(%c: bool, %a: i32) -> i32 {\n"
                        "    local $x: i32\n"
                        "    block entry:\n"
                        "      $x = copy %a\n"
                        "      branch %c, body, out\n"
                        "    block body:\n"
                        "      $x = add wrap %a, %a\n"
                        "      jump out\n"
                        "    block out:\n"
                        "      %res: i32 = copy $x\n"
                        "      return %res\n"
                        "  }\n"));
    ir_pdg_t *g    = ir_pdg_of(func_of(r));
    uint32_t  out  = 0;
    uint32_t  in   = 0;
    bool      both = true;

    for (uint32_t i = 0; i < ir_pdg_node_count(g); i++) {
        ir_instr_t *n = ir_pdg_node_at(g, i);

        out += ir_pdg_out_count(g, n);
        in += ir_pdg_in_count(g, n);

        for (uint32_t k = 0; k < ir_pdg_out_count(g, n); k++) {
            both = both && ir_pdg_out_at(g, n, k)->from == n;
        }

        for (uint32_t k = 0; k < ir_pdg_in_count(g, n); k++) {
            both = both && ir_pdg_in_at(g, n, k)->to == n;
        }
    }
    check("every edge is once in an out list and once in an in list",
          out == ir_pdg_edge_count(g) && in == ir_pdg_edge_count(g));
    check("and each list holds the edges that end at its own node", both);

    uint32_t seen = 0;

    for (uint32_t i = 0; i < ir_pdg_edge_count(g); i++) {
        const ir_dep_edge_t *e = ir_pdg_edge_at(g, i);

        for (uint32_t k = 0; k < ir_pdg_out_count(g, e->from); k++) {
            seen += ir_pdg_out_at(g, e->from, k) == e;
        }
    }
    check("and the two views hold the same edges, not copies",
          seen == ir_pdg_edge_count(g));
    done(r);
}

static void test_the_cdg_is_over_blocks(void)
{
    run_t *r = load(MOD("  func @f(%c: bool, %d: bool) -> void {\n"
                        "    block entry:\n      branch %c, t, join\n"
                        "    block t:\n      branch %d, u, join\n"
                        "    block u:\n      jump join\n"
                        "    block join:\n      return\n"
                        "  }\n"));
    ir_func_t *f = func_of(r);
    if (!f) {
        check("the nested-branch function parses", false);
        for (uint32_t i = 0; i < ir_diags_count(r->d); i++) {
            const ir_diag_t *diag = ir_diags_at(r->d, i);
            core_string_t    msg  = diag->message.data ? diag->message
                                                       : S("(no message)");
            printf("  parse diagnostic at %u:%u: %.*s\n",
                   diag->primary.span.start_line,
                   diag->primary.span.start_col,
                   (int)msg.u8_bytes, msg.data);
        }
        done(r);
        return;
    }
    ir_pdg_t     *g = ir_pdg_of(f);
    core_string_t s = cdg_sketch(g, f);

    same(S("control dependence is recorded per block"), s,
         S("t on entry\n"
           "u on t\n"));
    check("and asking directly agrees",
          controlled_by(g, f->blocks[1], f->blocks[0])
              && controlled_by(g, f->blocks[2], f->blocks[1])
              && !controlled_by(g, f->blocks[3], f->blocks[0]));

    uint32_t control = 0;

    for (uint32_t i = 0; i < ir_pdg_edge_count(g); i++) {
        control += ir_pdg_edge_at(g, i)->kind == IR_DEP_CONTROL;
    }
    check("the control edges are in the one graph, not a second structure",
          control > 0);
    core_free(s.data);
    done(r);
}

static void test_dead_code_is_reported(void)
{
    run_t *r = load(MOD("  func @f(%a: i32) -> i32 {\n"
                        "    block entry:\n      return %a\n"
                        "    block orphan:\n      return %a\n"
                        "  }\n"));

    ir_analyze(r->m, r->d);
    check("W001 an unreachable block is reported once",
          counted(r, IR_DIAG_DEAD_CODE) == 1);
    done(r);
}

static void test_use_before_definition_is_reported(void)
{
    run_t *r = load(MOD("  func @f(%c: bool, %a: i32) -> i32 {\n"
                        "    local $x: i32\n"
                        "    block entry:\n"
                        "      branch %c, set, use\n"
                        "    block set:\n"
                        "      $x = copy %a\n"
                        "      jump use\n"
                        "    block use:\n"
                        "      %res: i32 = copy $x\n"
                        "      return %res\n"
                        "  }\n"));

    ir_analyze(r->m, r->d);
    check("W002 a read on a path that does not assign is reported",
          counted(r, IR_DIAG_UNSET_LOCAL) == 1);
    done(r);

    r = load(MOD("  func @f(%a: i32) -> i32 {\n"
                 "    local $x: i32\n"
                 "    block entry:\n"
                 "      $x = copy %a\n"
                 "      %res: i32 = copy $x\n"
                 "      return %res\n"
                 "  }\n"));
    ir_analyze(r->m, r->d);
    check("and a local assigned on every path is not reported",
          counted(r, IR_DIAG_UNSET_LOCAL) == 0);
    done(r);
}

static void test_unused_assignment_is_reported(void)
{
    run_t *r = load(MOD("  func @f(%a: i32) -> i32 {\n"
                        "    local $x: i32\n"
                        "    local $y: i32\n"
                        "    block entry:\n"
                        "      $x = copy %a\n"
                        "      $y = copy %a\n"
                        "      %res: i32 = copy $y\n"
                        "      return %res\n"
                        "  }\n"));

    ir_analyze(r->m, r->d);
    check("W003 the assignment nothing reads is reported, and only it",
          counted(r, IR_DIAG_UNUSED_LOCAL) == 1);
    done(r);

    r = load(MOD("  func @f(%c: bool, %a: i32) -> i32 {\n"
                 "    local $x: i32\n"
                 "    block entry:\n"
                 "      $x = copy %a\n"
                 "      branch %c, again, out\n"
                 "    block again:\n"
                 "      $x = add wrap %a, %a\n"
                 "      jump out\n"
                 "    block out:\n"
                 "      %res: i32 = copy $x\n"
                 "      return %res\n"
                 "  }\n"));
    ir_analyze(r->m, r->d);
    check("an assignment that reaches a read through a join is not reported",
          counted(r, IR_DIAG_UNUSED_LOCAL) == 0);
    done(r);
}

static void test_where_the_other_three_live(void)
{
    run_t *r = load(MOD("  func @f(%a: i32) -> i32 {\n"
                        "    block entry:\n"
                        "      return %a\n"
                        "      %dead: i32 = copy %a\n"
                        "  }\n"));

    check("W004 an instruction after a terminator is a verifier error, "
          "not a warning",
          !(ir_verify(r->m, r->d) & IR_PROP_PARSED));
    check("and the verifier says which instruction",
          counted(r, IR_DIAG_VERIFY_PARSED) == 1
              && told(r, S("is not its last instruction")));
    check("so ir_analyze has nothing of its own to say",
          ir_diags_count(r->d) == 3 && !ir_analyze(r->m, r->d)
              && ir_diags_count(r->d) == 3);
    done(r);

    r = load(MOD("  func @f(%a: i32) -> i32 {\n"
                 "    block entry:\n"
                 "      %res: i32 = copy $nope\n"
                 "      return %res\n"
                 "  }\n"));
    check("E001 a read of an undeclared local never becomes a module",
          r->m == nullptr && counted(r, IR_DIAG_PARSE) == 1
              && told(r, S("unknown local")));
    done(r);

    r = load(MOD("  func @f(%a: i32) -> i32 {\n"
                 "    local $x: i32\n"
                 "    local $x: i64\n"
                 "    block entry:\n"
                 "      $x = copy %a\n"
                 "      return %a\n"
                 "  }\n"));
    check("W005 shadowing has no IR form: a second $x is a parse error",
          r->m == nullptr && counted(r, IR_DIAG_PARSE) == 1
              && told(r, S("duplicate local")));
    done(r);
}

static void test_every_analysis_diagnostic_has_a_place(void)
{
    run_t *r = load(MOD("  func @f(%c: bool, %a: i32) -> i32 {\n"
                        "    local $x: i32\n"
                        "    local $y: i32\n"
                        "    block entry:\n"
                        "      $y = copy %a\n"
                        "      branch %c, set, use\n"
                        "    block set:\n"
                        "      $x = copy %a\n"
                        "      jump use\n"
                        "    block use:\n"
                        "      %res: i32 = copy $x\n"
                        "      return %res\n"
                        "    block orphan:\n"
                        "      return %a\n"
                        "  }\n"));

    ir_analyze(r->m, r->d);
    check("the fixture raises all three", ir_diags_count(r->d) == 3);

    for (uint32_t i = 0; i < ir_diags_count(r->d); i++) {
        const ir_diag_t *dg = ir_diags_at(r->d, i);

        check("the diagnostic has a code",
              ir_diag_code(dg->code).data != nullptr);
        check("and a span that names a place in the source",
              dg->primary.span.file >= 0
                  && dg->primary.span.start_line > 0);
    }

    core_string_t text = ir_diags_render_text(r->d, nullptr);

    check("and it renders with that place",
          contains(text, S("orphan")) && contains(text, S("$x"))
              && contains(text, S("$y")));
    core_free(text.data);
    done(r);
}


int main(void)
{
    test_the_cfg_is_the_blocks_and_the_terminators();
    test_all_seven_edge_kinds();
    test_a_loop_inside_a_loop();
    test_keyed_by_identity_not_by_name();
    test_reaching_definitions_are_the_data_edges();
    test_under_ssa_def_use_is_immediate();
    test_a_definition_reaching_through_two_joins();
    test_the_two_directions_are_the_same_edges();
    test_the_cdg_is_over_blocks();
    test_dead_code_is_reported();
    test_use_before_definition_is_reported();
    test_unused_assignment_is_reported();
    test_where_the_other_three_live();
    test_every_analysis_diagnostic_has_a_place();

    dump_produced_codes("produced-codes-pdg.txt", produced);

    return report("ir pdg");
}
