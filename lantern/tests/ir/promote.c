#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/pass.h"
#include "ir/rule.h"
#include "ir/text.h"
#include "ir/verify.h"
#include "harness.h"

static bool produced[IR_DIAG_COUNT];

static const ir_profile_t *const PROFILES[] = {&ir_profile_bce};

typedef struct {
    ir_ctx_t    *ctx;
    ir_module_t *m;
    ir_diags_t  *d;
    char        *out;
} run_t;

static run_t *load(const char *src)
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
    free(r->out);
    ir_ctx_free(r->ctx);
    free(r);
}

static void snap(run_t *r)
{
    free(r->out);
    r->out = ir_module_print(r->m);
}

#define MOD(...) "module m {\n  profile bce\n  target t\n\n" __VA_ARGS__ "}\n"

static const char *DIAMOND = MOD(
    "  func @f(%a: i32, %c: bool) -> i32 {\n"
    "    local $x: i32\n"
    "    block entry:\n"
    "      $x = copy %a\n"
    "      branch %c, l, r\n"
    "    block l:\n"
    "      $x = add wrap %a, %a\n"
    "      jump join\n"
    "    block r:\n"
    "      jump join\n"
    "    block join:\n"
    "      %res: i32 = copy $x\n"
    "      return %res\n"
    "  }\n");

static void test_promote_establishes_ssa(void)
{
    run_t *r = load(DIAMOND);

    check("the module loads and is not yet SSA",
          r->m && !(ir_verify(r->m, r->d) & IR_PROP_SSA));
    check("promote runs", ir_promote(r->m, nullptr));
    check("and establishes ssa",
          (ir_module_props(r->m) & IR_PROP_SSA) != 0);
    check("which the verifier confirms",
          (ir_verify(r->m, r->d) & IR_PROP_SSA) != 0);
    check("the function has no locals left",
          ir_func_at(r->m, 0)->n_locals == 0);

    snap(r);
    check("and no local destination survives in the text",
          r->out && !strstr(r->out, "$x ="));
    check("the pass declares requires parsed and typed, establishes ssa",
          ir_pass_promote.requires == (IR_PROP_PARSED | IR_PROP_TYPED)
              && ir_pass_promote.establishes == IR_PROP_SSA
              && ir_pass_declares(&ir_pass_promote));
    done(r);
}

static void test_promote_requires_what_it_declares(void)
{
    run_t *r = load(DIAMOND);

    ir_module_invalidate(r->m, IR_PROP_TYPED);

    ir_props_t unmet = 0;

    check("promote refuses to run on a module that is not typed",
          ir_pass_run(&ir_pass_promote, r->m, nullptr, &unmet, r->d)
              == IR_PASS_UNMET);
    check("and says which property was missing", unmet == IR_PROP_TYPED);
    done(r);
}

static void test_names_follow_the_source(void)
{
    run_t *r = load(DIAMOND);

    ir_promote(r->m, nullptr);
    snap(r);

    check("the first definition is named after the local",
          r->out && strstr(r->out, "%x_0"));
    check("the second follows it in dominance order",
          r->out && strstr(r->out, "%x_1"));
    check("and the phi takes the next number",
          r->out && strstr(r->out, "%x_2: i32 = phi"));
    check("numbering starts at zero, so a reader can follow the variable",
          r->out && !strstr(r->out, "%x_3"));
    done(r);
}

static void test_a_colliding_name_is_disambiguated(void)
{
    run_t *r = load(MOD("  func @f(%a: i32) -> i32 {\n"
                        "    local $x: i32\n"
                        "    block entry:\n"
                        "      %x_0: i32 = copy %a\n"
                        "      $x = copy %a\n"
                        "      %res: i32 = add wrap $x, %x_0\n"
                        "      return %res\n"
                        "  }\n"));

    check("a module that already uses the derived name loads", r->m);
    ir_promote(r->m, nullptr);
    snap(r);

    check("the existing %x_0 is left alone",
          r->out && strstr(r->out, "%x_0: i32 = copy %a"));
    check("and the promoted one takes a disambiguating suffix",
          r->out && strstr(r->out, "%x_0.1"));
    check("the module is still well formed",
          (ir_verify(r->m, r->d) & IR_PROP_SSA) != 0);
    done(r);
}

static const char *shout(ir_ctx_t *ctx, const char *local, uint32_t nth,
                         void *state)
{
    size_t n    = strlen(local) + 24;
    char  *name = ir_alloc(ctx, n);

    snprintf(name, n, "%s_%s_%u", (const char *)state, local, nth);

    return name;
}

static void test_naming_is_overridable(void)
{
    run_t            *r    = load(DIAMOND);
    ir_promote_opts_t opts = {.rename = shout, .state = (void *)"mine"};

    ir_promote(r->m, &opts);
    snap(r);

    check("a supplied naming function is used instead of the default",
          r->out && strstr(r->out, "%mine_x_0"));
    check("and the default numbering does not appear",
          r->out && !strstr(r->out, "%x_0"));
    check("the result is still SSA",
          (ir_verify(r->m, r->d) & IR_PROP_SSA) != 0);
    done(r);
}

static void test_the_phi_carries_its_provenance(void)
{
    run_t *r = load(DIAMOND);

    ir_promote(r->m, nullptr);

    ir_func_t  *f    = ir_func_at(r->m, 0);
    ir_block_t *join = ir_block_find(f, "join");
    ir_instr_t *phi  = join ? join->instrs[0] : nullptr;

    check("a phi is placed at the join", phi && phi->op == IR_OP_PHI);
    check("it takes the span of the local's declaration",
          phi && ir_span_known(phi->span) && phi->span.start_line == 6);
    check("and an origin naming the pass that made it",
          phi && phi->origin && phi->origin->kind == IR_ORIGIN_GENERATED_BY
              && !strcmp(phi->origin->agent, "promote"));
    check("it names each predecessor once",
          phi && phi->n_operands == 2);
    done(r);
}

static void test_promote_does_not_touch_memory(void)
{
    run_t *r = load(MOD("  func @f(%a: i32) -> i32 {\n"
                        "    block entry:\n"
                        "      %p: ptr[i32] = alloca i32\n"
                        "      store %p, %a\n"
                        "      %v: i32 = load %p\n"
                        "      return %v\n"
                        "  }\n"));

    check("a module holding its value in memory loads", r->m);

    char *before = ir_module_print(r->m);

    ir_promote(r->m, nullptr);
    snap(r);

    check("promote leaves alloca, store and load exactly as they were",
          before && r->out && !strcmp(before, r->out));
    free(before);
    done(r);
}

static void test_a_local_has_no_address(void)
{
    run_t *r = load(MOD("  func @f() -> void {\n"
                        "    local $t: i32\n"
                        "    block entry:\n"
                        "      unreachable\n"
                        "  }\n"));

    ir_func_t  *f = ir_func_at(r->m, 0);
    ir_local_t *l = ir_local_find(f, "t");
    ir_block_t *b = ir_block_entry(f);
    ir_instr_t *o = ir_instr_add(b, IR_OP_OFFSET);

    ir_instr_operand_local(o, l);
    ir_instr_operand_value(o, ir_param_add(f, "n", ir_type_parse(r->m, "u32")));

    ir_rules_t *rs = ir_module_rules(r->m);

    check("a local read is its value, so pointer arithmetic on it is rejected",
          ir_rule_apply(r->m, rs, o, r->d).verdict == IR_RULE_REJECT);
    check("so there is no address-taken local for promote to skip",
          ir_local_find(f, "t") != nullptr);
    done(r);
}

static void test_split_edges(void)
{
    run_t *clean = load(DIAMOND);

    ir_promote(clean->m, nullptr);

    char *before = ir_module_print(clean->m);

    ir_split_edges(clean->m, clean->d);
    snap(clean);
    check("a diamond has no critical edge, so nothing is split",
          before && clean->out && !strcmp(before, clean->out));
    free(before);
    done(clean);

    run_t *r = load(MOD("  func @f(%a: i32, %c: bool) -> i32 {\n"
                        "    local $x: i32\n"
                        "    block entry:\n"
                        "      $x = copy %a\n"
                        "      branch %c, l, join\n"
                        "    block l:\n"
                        "      $x = add wrap %a, %a\n"
                        "      jump join\n"
                        "    block join:\n"
                        "      %res: i32 = copy $x\n"
                        "      return %res\n"
                        "  }\n"));

    check("a module with a critical edge loads", r->m);
    ir_promote(r->m, nullptr);
    check("split-edges runs", ir_split_edges(r->m, r->d));
    snap(r);

    check("the critical edge is split by a block named for the edge",
          r->out && strstr(r->out, "block entry.to.join"));
    check("and the block is not numbered",
          r->out && !strstr(r->out, "block bb"));
    check("the phi now names the new block instead of the old predecessor",
          r->out && strstr(r->out, "[entry.to.join:"));
    check("the module is still SSA afterwards",
          (ir_verify(r->m, r->d) & IR_PROP_SSA) != 0);
    check("the pass declares requires parsed, establishes no-critical-edges",
          ir_pass_split_edges.requires == IR_PROP_PARSED
              && ir_pass_split_edges.establishes == IR_PROP_NO_CRIT_EDGE
              && ir_pass_declares(&ir_pass_split_edges));
    check("and the new block carries an origin naming the pass",
          ir_block_find(ir_func_at(r->m, 0), "entry.to.join") != nullptr);
    done(r);

    run_t *two = load(MOD("  func @f(%a: i32, %c: bool) -> i32 {\n"
                          "    block entry:\n"
                          "      branch %c, join, join\n"
                          "    block join:\n"
                          "      %p: i32 = phi [entry: %a], [entry: %a]\n"
                          "      return %p\n"
                          "  }\n"));

    check("a block that branches both ways to one target loads", two->m);
    check("both edges are critical and both split",
          ir_split_edges(two->m, two->d));
    snap(two);
    check("each edge gets its own block, named apart",
          two->out && strstr(two->out, "block entry.to.join")
              && strstr(two->out, "block entry.to.join.1"));
    check("and each new block takes one of the phi's two arms",
          two->out && strstr(two->out, "[entry.to.join: %a]")
              && strstr(two->out, "[entry.to.join.1: %a]"));
    check("so the module still verifies",
          (ir_verify(two->m, two->d) & IR_PROP_PARSED) != 0);
    done(two);
}

static void test_location_propagation(void)
{
    run_t      *r  = load(DIAMOND);
    ir_func_t  *f  = ir_func_at(r->m, 0);
    ir_block_t *b  = ir_block_entry(f);
    ir_instr_t *in = b->instrs[0];

    check("the source instruction has a location", ir_span_known(in->span));

    ir_span_t   was = in->span;
    ir_instr_t *rep = ir_instr_derived(b, IR_OP_COPY, in, 0);

    check("a replacement inherits its span and origin unchanged",
          rep && rep->span.start_byte == was.start_byte
              && rep->span.end_byte == was.end_byte
              && rep->origin == in->origin);

    ir_instr_t *one = b->instrs[1];
    ir_instr_t *two = b->instrs[2];
    ir_span_t   fold = ir_span_cover(one->span, two->span);

    check("a fold covers both spans",
          fold.start_byte <= one->span.start_byte
              && fold.end_byte >= two->span.end_byte);
    check("and stays in the one file", fold.file == one->span.file);

    ir_span_t backwards = ir_span_cover(two->span, one->span);

    check("a fold covers both spans whichever order it is given",
          backwards.start_byte == fold.start_byte
              && backwards.end_byte == fold.end_byte);

    ir_span_t nothing = ir_span_cover(IR_SPAN_UNKNOWN, one->span);

    check("and an unknown span contributes nothing rather than poisoning it",
          nothing.start_byte == one->span.start_byte
              && ir_span_known(nothing));

    ir_span_t elsewhere = ir_span_make(9, 900, 910, 90, 1, 90, 11);
    ir_span_t across    = ir_span_cover(one->span, elsewhere);

    check("across files a fold takes the primary operand's span",
          across.file == one->span.file
              && across.start_byte == one->span.start_byte);

    ir_instr_t *ex1 = ir_instr_derived(b, IR_OP_COPY, in, 0);
    ir_instr_t *ex2 = ir_instr_derived(b, IR_OP_COPY, in, 0);

    check("every result of an expansion inherits the original's span",
          ex1 && ex2 && ir_span_known(ex1->span) && ir_span_known(ex2->span)
              && ex1->span.start_byte == was.start_byte
              && ex2->span.start_byte == was.start_byte);

    ir_instr_t *made = ir_instr_generated(b, IR_OP_UNREACHABLE,
                                          IR_SPAN_UNKNOWN, "a-pass", 0);

    check("a pass-created node takes the nearest enclosing span",
          made && ir_span_known(made->span));
    check("and an origin naming the pass",
          made && made->origin
              && made->origin->kind == IR_ORIGIN_GENERATED_BY
              && !strcmp(made->origin->agent, "a-pass"));

    ir_block_t *other = ir_block_find(f, "l");
    ir_span_t   kept  = rep->span;

    check("a move succeeds", ir_instr_move(rep, other, 0));
    check("and the moved instruction keeps its own span",
          rep->span.start_byte == kept.start_byte
              && rep->block == other);

    ir_span_t call = ir_span_make(0, 500, 510, 50, 1, 50, 11);

    check("an inlined instruction gains an origin",
          ir_instr_inlined_from(rep, call, "inline"));
    check("keeps its own span",
          rep->span.start_byte == kept.start_byte);
    check("and the origin names the call site",
          rep->origin && rep->origin->kind == IR_ORIGIN_INLINED_FROM
              && rep->origin->span.start_line == 50);
    done(r);
}

static void test_promote_keeps_every_location(void)
{
    run_t        *r      = load(DIAMOND);
    ir_locsnap_t *before = ir_loc_snapshot(r->m);

    ir_promote(r->m, nullptr);
    check("promote turns no known location into an unknown one",
          ir_loc_monotone(before, r->m, "promote", r->d));

    ir_func_t *f = ir_func_at(r->m, 0);

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        for (uint32_t k = 0; k < f->blocks[i]->n_instrs; k++) {
            if (!ir_span_known(f->blocks[i]->instrs[k]->span)) {
                printf("FAIL  every instruction promote left has a location\n");
                fails++;
            }
        }
    }

    ir_locsnap_t *mid = ir_loc_snapshot(r->m);

    ir_split_edges(r->m, r->d);
    check("and neither does split-edges",
          ir_loc_monotone(mid, r->m, "split-edges", r->d));
    done(r);
}

static bool told(run_t *r, const char *text)
{
    for (uint32_t i = 0; i < ir_diags_count(r->d); i++) {
        const ir_diag_t *dg = ir_diags_at(r->d, i);

        if ((dg->primary.label && strstr(dg->primary.label, text))
            || (dg->message && strstr(dg->message, text))) {
            return true;
        }
    }

    return false;
}

static void test_a_local_read_before_it_is_set(void)
{
    run_t            *r    = load(MOD("  func @f(%a: i32, %c: bool) -> i32 {\n"
                                      "    local $x: i32\n"
                                      "    block entry:\n"
                                      "      branch %c, l, join\n"
                                      "    block l:\n"
                                      "      $x = copy %a\n"
                                      "      jump join\n"
                                      "    block join:\n"
                                      "      %res: i32 = copy $x\n"
                                      "      return %res\n  }\n"));
    ir_promote_opts_t opts = {.diags = r->d};

    check("promote succeeds rather than leaving the module half built",
          ir_promote(r->m, &opts));
    snap(r);

    check("the unassigned path is given undef, which is what undef is for",
          r->out && strstr(r->out, "= undef"));
    check("and the phi names both predecessors",
          r->out && strstr(r->out, "[entry: %x_0]")
              && strstr(r->out, "[l: %x_1]"));
    check("the result verifies as SSA",
          (ir_verify(r->m, r->d) & IR_PROP_SSA) != 0);
    check("and the reader is warned rather than left to find it later",
          told(r, "read on a path that does not assign it"));
    done(r);

    run_t            *n    = load(MOD("  func @f(%a: i32) -> i32 {\n"
                                      "    local $x: i32\n"
                                      "    block entry:\n"
                                      "      %res: i32 = copy $x\n"
                                      "      return %res\n  }\n"));
    ir_promote_opts_t nopts = {.diags = n->d};

    check("a local never assigned at all promotes too",
          ir_promote(n->m, &nopts));
    check("into valid SSA rather than a dangling operand",
          (ir_verify(n->m, n->d) & IR_PROP_SSA) != 0);
    snap(n);
    check("with no unresolved operand printed",
          n->out && !strstr(n->out, "%?"));
    done(n);
}

static void test_promote_handles_loops(void)
{
    run_t *r = load(MOD("  func @f(%a: i32, %c: bool) -> i32 {\n"
                        "    local $x: i32\n"
                        "    block entry:\n"
                        "      $x = copy %a\n"
                        "      jump head\n"
                        "    block head:\n"
                        "      branch %c, body, out\n"
                        "    block body:\n"
                        "      $x = add wrap $x, %a\n"
                        "      jump head\n"
                        "    block out:\n"
                        "      %res: i32 = copy $x\n"
                        "      return %res\n  }\n"));

    check("a loop promotes", ir_promote(r->m, nullptr));
    snap(r);
    check("with a phi at the loop header, where the back edge merges",
          r->out && strstr(r->out, "block head:\n      %x_")
              && strstr(r->out, "= phi"));
    check("and the result is SSA",
          (ir_verify(r->m, r->d) & IR_PROP_SSA) != 0);
    done(r);

    run_t *n = load(MOD("  func @f(%a: i32, %c: bool) -> i32 {\n"
                        "    local $x: i32\n"
                        "    block entry:\n"
                        "      $x = copy %a\n"
                        "      jump h1\n"
                        "    block h1:\n"
                        "      branch %c, h2, out\n"
                        "    block h2:\n"
                        "      branch %c, b2, l1\n"
                        "    block b2:\n"
                        "      $x = add wrap $x, %a\n"
                        "      jump h2\n"
                        "    block l1:\n"
                        "      jump h1\n"
                        "    block out:\n"
                        "      %res: i32 = copy $x\n"
                        "      return %res\n  }\n"));

    check("nested loops promote", ir_promote(n->m, nullptr));
    check("and verify as SSA",
          (ir_verify(n->m, n->d) & IR_PROP_SSA) != 0);
    done(n);
}

static void test_passes_take_one_kind_of_state(void)
{
    run_t *r = load(DIAMOND);

    check("promote through the pass manager takes the diagnostics sink",
          ir_pass_run(&ir_pass_promote, r->m, nullptr, nullptr, r->d)
              == IR_PASS_OK);
    check("and split-edges takes the same",
          ir_pass_run(&ir_pass_split_edges, r->m, nullptr, nullptr, r->d)
              == IR_PASS_OK);
    check("split-edges invalidates the region overlay it may have broken",
          ir_pass_split_edges.invalidates == IR_PROP_STRUCTURED);
    done(r);
}

static const char *CRITICAL = MOD(
    "  func @f(%a: i32, %c: bool) -> i32 {\n"
    "    block entry:\n"
    "      branch %c, l, join\n"
    "    block l:\n"
    "      jump join\n"
    "    block join:\n"
    "      return %a\n"
    "  }\n");

static void test_critical_edges_are_verified(void)
{
    run_t *r = load(CRITICAL);

    ir_module_establish(r->m, IR_PROP_NO_CRIT_EDGE);

    ir_props_t held = ir_verify(r->m, r->d);

    check("a module claiming no-critical-edges while having one is caught",
          !(held & IR_PROP_NO_CRIT_EDGE));
    check("and the rule is named",
          told(r, "no-critical-edges.edge"));
    check("and the false claim is reported",
          told(r, "claims no-critical-edges"));
    check("the module is otherwise well formed",
          (held & IR_PROP_PARSED) && (held & IR_PROP_TYPED));
    done(r);

    run_t *s2 = load(CRITICAL);

    check("split-edges runs", ir_split_edges(s2->m, s2->d));
    check("and afterwards the property holds",
          (ir_verify(s2->m, s2->d) & IR_PROP_NO_CRIT_EDGE) != 0);
    done(s2);

    run_t *never = load(DIAMOND);

    check("a module that never claimed it is not diagnosed for it",
          !told(never, "no-critical-edges"));
    done(never);
}

static void test_verified_is_the_summary(void)
{
    run_t *r = load(DIAMOND);

    check("a module claiming only parsed and typed, and passing, is verified",
          (ir_verify(r->m, r->d) & IR_PROP_VERIFIED) != 0);
    done(r);

    run_t *lying = load(DIAMOND);

    ir_module_establish(lying->m, IR_PROP_SSA);
    check("a module claiming ssa it does not have is not verified",
          !(ir_verify(lying->m, lying->d) & IR_PROP_VERIFIED));
    done(lying);

    run_t *promoted = load(DIAMOND);

    ir_promote(promoted->m, nullptr);
    check("once the claim is true again, so is verified",
          (ir_verify(promoted->m, promoted->d) & IR_PROP_VERIFIED) != 0);
    done(promoted);

    run_t *broken = load(MOD("  func @f() -> void {\n    block entry:\n"
                             "      unreachable\n      unreachable\n  }\n"));

    check("a module that is not even parsed is not verified",
          !(ir_verify(broken->m, broken->d) & IR_PROP_VERIFIED));
    done(broken);
}


int main(void)
{
    test_promote_establishes_ssa();
    test_promote_requires_what_it_declares();
    test_names_follow_the_source();
    test_a_colliding_name_is_disambiguated();
    test_naming_is_overridable();
    test_the_phi_carries_its_provenance();
    test_promote_does_not_touch_memory();
    test_a_local_has_no_address();
    test_split_edges();
    test_a_local_read_before_it_is_set();
    test_promote_handles_loops();
    test_passes_take_one_kind_of_state();
    test_critical_edges_are_verified();
    test_verified_is_the_summary();
    test_location_propagation();
    test_promote_keeps_every_location();

    dump_produced_codes("produced-codes-promote.txt", produced);

    return report("ir promote");
}
