#include <stdio.h>
#include <stdlib.h>

#include "ir/text.h"
#include "ir/doms.h"
#include "ir/verify.h"
#include "harness.h"

static bool produced[IR_DIAG_COUNT];

typedef struct {
    ir_ctx_t    *ctx;
    ir_module_t *m;
    ir_diags_t  *d;
    ir_props_t   held;
    core_string_t report;
    core_string_t src;
} run_t;

#define S(x) CORE_STRING_STATIC(x)

static const ir_profile_t *const PROFILES[] = {&ir_profile_bce,
                                               &ir_profile_synth};

static core_string_t body(void *u, int32_t file)
{
    (void)file;

    return *(const core_string_t *)u;
}

static run_t *verify(core_string_t src, ir_props_t claim)
{
    run_t *r = calloc(1, sizeof(run_t));

    r->src = src;

    r->ctx = ir_ctx_new();
    r->d   = ir_diags_new(r->ctx, nullptr);
    r->m   = ir_module_parse(r->ctx, src, 0, PROFILES, 2, r->d);

    if (!r->m) {
        ir_render_t rd = {0};

        r->report = ir_diags_render_text(r->d, &rd);
        r->held   = 0;

        return r;
    }
    ir_module_establish(r->m, claim);
    r->held = ir_verify(r->m, r->d);

    for (uint32_t i = 0; i < ir_diags_count(r->d); i++) {
        produced[ir_diags_at(r->d, i)->code] = true;
    }

    ir_render_t rd = {.module = r->m, .source = body,
                      .state  = (void *)&r->src};

    r->report = ir_diags_render_text(r->d, &rd);

    return r;
}

static void done(run_t *r)
{
    free(r->report.data);
    ir_ctx_free(r->ctx);
    free(r);
}

static bool contains(core_string_t haystack, core_string_t needle)
{
    return haystack.data && core_string_find(haystack, needle) >= 0;
}

static bool told(ir_diags_t *d, core_string_t text)
{
    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        const ir_diag_t *dg = ir_diags_at(d, i);

        if (contains(ir_diag_item(d, dg)->label, text) || contains(ir_diag_item(d, dg)->message, text)) {
            return true;
        }
    }

    return false;
}

static bool said(run_t *r, core_string_t rule)
{
    return told(r->d, rule);
}

static bool every_diag_is_placed_and_named(ir_diags_t *d)
{
    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        const ir_diag_t *dg = ir_diags_at(d, i);

        if (!ir_span_known(dg->span) || !ir_diag_item(d, dg)->label.data
            || !ir_diag_item(d, dg)->message.data) {
            return false;
        }
    }

    return true;
}

static bool parsed(run_t *r)
{
    return (r->held & IR_PROP_PARSED) != 0;
}

static bool typed(run_t *r)
{
    return (r->held & IR_PROP_TYPED) != 0;
}

static bool ssa(run_t *r)
{
    return (r->held & IR_PROP_SSA) != 0;
}

#define MOD(...) S("module m {\n  profile bce\n  target t\n\n" __VA_ARGS__ "}\n")

static const core_string_t CLEAN = MOD(
    "  func @f(%a: i32, %c: bool) -> i32 {\n"
    "    block entry:\n"
    "      branch %c, l, r\n"
    "    block l:\n"
    "      %x: i32 = add wrap %a, %a\n"
    "      jump r\n"
    "    block r:\n"
    "      %p: i32 = phi [entry: %a], [l: %x]\n"
    "      return %p\n"
    "  }\n");

static void test_graded(void)
{
    run_t *ok = verify(CLEAN, IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);

    check("a well formed SSA module holds all three properties",
          parsed(ok) && typed(ok) && ssa(ok));
    check("and reports nothing",
          ok->report.data && !contains(ok->report, S("error")));
    done(ok);

    run_t *nonssa = verify(MOD("  func @f(%a: i32) -> i32 {\n"
                               "    local $t: i32\n"
                               "    block entry:\n"
                               "      $t = copy %a\n"
                               "      return %a\n"
                               "  }\n"),
                           IR_PROP_PARSED | IR_PROP_TYPED);

    check("a non-SSA module is parsed and typed",
          parsed(nonssa) && typed(nonssa));
    check("but is not in SSA form", !ssa(nonssa));
    check("and is not diagnosed for it, since non-SSA is not degraded",
          nonssa->report.data && !contains(nonssa->report, S("error")));
    done(nonssa);
}

static void test_failure_channel_ops(void)
{
    run_t *valid = verify(MOD(
        "  func @f(%e: i32) -> i32 fail: i32 {\n"
        "    block entry:\n      fail %e\n  }\n"),
        IR_PROP_PARSED | IR_PROP_TYPED);
    check("a failure terminator matches the function failure channel",
          parsed(valid) && typed(valid)
              && !contains(valid->report, S("error")));
    done(valid);

}

static void test_parsed_rules(void)
{
    struct {
        core_string_t what;
        core_string_t rule;
        core_string_t src;
    } cases[] = {
        {S("a block with two terminators"), S("parsed.terminator"),
         MOD("  func @f() -> void {\n    block entry:\n"
             "      unreachable\n      unreachable\n  }\n")},
        {S("a block with no terminator"), S("parsed.terminator"),
         MOD("  func @f(%a: i32) -> void {\n    block entry:\n"
             "      %s: i32 = add wrap %a, %a\n  }\n")},
        {S("a terminator that is not last"), S("parsed.terminator"),
         MOD("  func @f(%a: i32) -> void {\n    block entry:\n"
             "      unreachable\n      %s: i32 = add wrap %a, %a\n  }\n")},
        {S("a value defined twice"), S("parsed.one-def"),
         MOD("  func @f(%a: i32) -> void {\n    block entry:\n"
             "      %s: i32 = add wrap %a, %a\n"
             "      %s: i32 = add wrap %a, %a\n      unreachable\n  }\n")},
        {S("a destination that shadows a parameter"), S("parsed.one-def"),
         MOD("  func @f(%a: i32) -> void {\n    block entry:\n"
             "      %a: i32 = add wrap %a, %a\n      unreachable\n  }\n")},
        {S("a phi after a non-phi"), S("parsed.phi.leading"),
         MOD("  func @f(%a: i32, %c: bool) -> void {\n    block entry:\n"
             "      branch %c, l, l\n    block l:\n"
             "      %y: i32 = add wrap %a, %a\n"
             "      %p: i32 = phi [entry: %a]\n      unreachable\n  }\n")},
        {S("a phi that misses a predecessor"), S("parsed.phi.arms"),
         MOD("  func @f(%a: i32, %c: bool) -> void {\n    block entry:\n"
             "      branch %c, l, r\n    block l:\n      jump r\n"
             "    block r:\n      %p: i32 = phi [entry: %a]\n"
             "      unreachable\n  }\n")},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_t *r = verify(cases[i].src, IR_PROP_PARSED);

        if (parsed(r) || !said(r, cases[i].rule)) {
            printf("FAIL  %.*s is caught and names %.*s\n",
                   (int)cases[i].what.u8_bytes, cases[i].what.data,
                   (int)cases[i].rule.u8_bytes, cases[i].rule.data);
            fails++;
        }

        if (!every_diag_is_placed_and_named(r->d)) {
            printf("FAIL  %.*s is reported with a location and a rule\n",
                   (int)cases[i].what.u8_bytes, cases[i].what.data);
            fails++;
        }
        done(r);
    }
}

static void test_ssa_rules(void)
{
    run_t *loc = verify(MOD("  func @f(%a: i32) -> i32 {\n    local $t: i32\n"
                            "    block entry:\n      $t = copy %a\n"
                            "      return %a\n  }\n"),
                        IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);

    check("a local in a module claiming SSA is caught",
          !ssa(loc) && said(loc, S("ssa.locals")));
    check("and the false claim is reported as such",
          said(loc, S("claims ssa, which the verifier finds does not hold")));
    done(loc);

    run_t *dom = verify(MOD("  func @f(%c: bool) -> i32 {\n    block entry:\n"
                            "      branch %c, l, r\n    block l:\n"
                            "      %x: i32 = const 1\n      jump r\n"
                            "    block r:\n      return %x\n  }\n"),
                        IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);

    check("a use its definition does not dominate is caught",
          !ssa(dom) && said(dom, S("ssa.dominance")));
    done(dom);

    run_t *order = verify(MOD("  func @f(%a: i32) -> i32 {\n    block entry:\n"
                              "      %y: i32 = add wrap %x, %a\n"
                              "      %x: i32 = const 1\n      return %y\n  }\n"),
                          IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);

    check("a use before its definition in one block is caught",
          !ssa(order) && said(order, S("ssa.dominance")));
    done(order);

    run_t *arm = verify(MOD("  func @f(%a: i32, %c: bool) -> i32 {\n"
                            "    block entry:\n      branch %c, l, r\n"
                            "    block l:\n      %x: i32 = const 1\n"
                            "      jump r\n    block r:\n"
                            "      %p: i32 = phi [entry: %a], [l: %x]\n"
                            "      return %p\n  }\n"),
                        IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);

    check("a phi arm is judged at the end of its own predecessor, not at "
          "the phi", ssa(arm));
    done(arm);

}

static void test_extension_opcodes(void)
{
    core_string_t src = S("module m {\n  profile synth\n  target t\n\n"
                          "  func @f(%a: str, %b: str) -> str {\n"
                          "    block entry:\n"
                          "      %s: str = synth.concat %a, %b\n"
                          "      return %s\n  }\n}\n");

    run_t *r = verify(src, IR_PROP_PARSED | IR_PROP_TYPED);

    check("an extension opcode its profile supplies verifies",
          parsed(r) && typed(r));
    done(r);

    ir_ctx_t    *ctx = ir_ctx_new();
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
    ir_module_t *m   = ir_module_parse(ctx, src, 0, PROFILES, 2, d);

    check("the module parses under the profile that supplies the opcode",
          m != nullptr);

    if (m) {
        ir_module_set_profile(m, &ir_profile_bce);
        ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED);

        ir_props_t held = ir_verify(m, d);

        for (uint32_t i = 0; i < ir_diags_count(d); i++) {
            produced[ir_diags_at(d, i)->code] = true;
        }

        ir_render_t rd = {.module = m};
        core_string_t rp = ir_diags_render_text(d, &rd);

        check("rebound to a profile without it, the module is still parsed",
              (held & IR_PROP_PARSED) != 0);
        check("but is no longer typed", !(held & IR_PROP_TYPED));
        check("and it is reported as a missing profile, not an unknown opcode",
              told(d, S("not one profile bce supplies")));
        free(rp.data);
    }
    ir_ctx_free(ctx);

}

static ir_module_t *built(ir_ctx_t *ctx, ir_func_t **out)
{
    ir_module_t *m = ir_module_new(ctx, S("m"), &ir_profile_bce, S("t"));
    ir_func_t   *f = ir_func_add(m, S("f"), ir_type_parse(m, S("() -> void")),
                                 IR_LINK_EXTERNAL);
    ir_block_t  *b = ir_block_add(f, S("entry"));
    ir_instr_t  *u = ir_instr_add(b, IR_OP_UNREACHABLE);

    ir_module_set_span(m, ir_span_make(0, 0, 6, 1, 1, 1, 7));
    f->span = ir_span_make(0, 10, 14, 2, 3, 2, 7);
    b->span = ir_span_make(0, 20, 24, 3, 3, 3, 7);
    u->span = ir_span_make(0, 30, 34, 4, 3, 4, 7);

    if (out) {
        *out = f;
    }

    return m;
}

static void test_parsed_rules_the_builder_prevents(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, S("m"), &ir_profile_bce, S("t"));

    ir_module_set_span(m, ir_span_make(0, 0, 6, 1, 1, 1, 7));

    ir_func_t  *f  = ir_func_add(m, S("f"), ir_type_parse(m, S("(i32) -> void")),
                                 IR_LINK_EXTERNAL);
    ir_value_t *a  = ir_param_add(f, S("a"), ir_type_parse(m, S("i32")));
    ir_block_t *b  = ir_block_add(f, S("entry"));
    ir_func_t  *g  = ir_func_add(m, S("g"), ir_type_parse(m, S("() -> void")),
                                 IR_LINK_EXTERNAL);
    ir_block_t *gb = ir_block_add(g, S("elsewhere"));

    ir_instr_add(gb, IR_OP_UNREACHABLE);

    ir_instr_t *few = ir_instr_add(b, IR_OP_ADD);

    ir_instr_set_dest(few, S("s"), ir_type_parse(m, S("i32")));
    ir_instr_add_mod(few, S("wrap"));
    ir_instr_operand_value(few, a);

    ir_instr_t *kind = ir_instr_add(b, IR_OP_ADD);

    ir_instr_set_dest(kind, S("k"), ir_type_parse(m, S("i32")));
    ir_instr_add_mod(kind, S("wrap"));

    check("the builder refuses an operand of the wrong kind",
          !ir_instr_operand_type(kind, ir_type_parse(m, S("i32"))));
    ir_instr_operand_value(kind, a);
    ir_instr_operand_value(kind, a);
    kind->operands[0].kind = IR_OPD_TYPE;
    kind->operands[0].type = ir_type_parse(m, S("i32"));

    ir_instr_t *away = ir_instr_add(b, IR_OP_JUMP);

    ir_instr_operand_block(away, gb);

    ir_instr_t *ld = ir_instr_add(b, IR_OP_LOAD);

    check("the builder refuses a modifier the opcode does not take",
          !ir_instr_add_mod(ld, S("sat")));

    ld->mods   = ir_alloc(ctx, sizeof(core_string_t));
    ld->mods[0] = S("sat");
    ld->n_mods  = 1;

    ir_diags_t *d = ir_diags_new(ctx, nullptr);

    ir_module_establish(m, IR_PROP_PARSED);

    ir_props_t held = ir_verify(m, d);

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        produced[ir_diags_at(d, i)->code] = true;
    }

    check("a module in this state is not structurally well formed",
          !(held & IR_PROP_PARSED));
    check("an instruction with too few operands is caught",
          told(d, S("takes 2 to 2 operands and has 1")));
    check("an operand of the wrong kind is caught",
          told(d, S("is a type where the table says value")));
    check("a branch to a block of another function is caught",
          told(d, S("parsed.block-ref")));
    check("a modifier the opcode does not take is caught",
          told(d, S("does not take the modifier sat")));
    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        const ir_diag_t *dg = ir_diags_at(d, i);

        if (!ir_span_known(dg->span) || !ir_diag_item(d, dg)->label.data) {
            core_string_t code    = ir_diag_code(dg->code);
            core_string_t message = ir_diag_item(d, dg)->message.data ? ir_diag_item(d, dg)->message
                                                     : S("(no message)");

            printf("FAIL  unplaced or unnamed: %.*s / %.*s\n",
                   (int)code.u8_bytes, code.data, (int)message.u8_bytes,
                   message.data);
            fails++;
        }
    }
    ir_ctx_free(ctx);

}

static void test_value_references_resolve(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, S("m"), &ir_profile_bce, S("t"));

    ir_module_set_span(m, ir_span_make(0, 0, 6, 1, 1, 1, 7));

    ir_func_t  *f = ir_func_add(m, S("f"), ir_type_parse(m, S("() -> void")),
                                IR_LINK_EXTERNAL);
    ir_func_t  *g = ir_func_add(m, S("g"), ir_type_parse(m, S("(i32) -> void")),
                                IR_LINK_EXTERNAL);
    ir_value_t *a = ir_param_add(g, S("a"), ir_type_parse(m, S("i32")));
    ir_block_t *b = ir_block_add(f, S("entry"));
    ir_instr_t *c = ir_instr_add(b, IR_OP_COPY);

    ir_block_add(g, S("entry"));
    ir_instr_set_dest(c, S("s"), ir_type_parse(m, S("i32")));
    ir_instr_operand_value(c, a);
    ir_instr_add(b, IR_OP_UNREACHABLE);

    ir_diags_t *d = ir_diags_new(ctx, nullptr);

    ir_module_establish(m, IR_PROP_PARSED);

    ir_props_t held = ir_verify(m, d);

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        produced[ir_diags_at(d, i)->code] = true;
    }

    check("a use of a value belonging to another function is caught",
          !(held & IR_PROP_PARSED) && told(d, S("parsed.value-ref")));
    check("and is placed and named", every_diag_is_placed_and_named(d));
    ir_ctx_free(ctx);
}

static void test_entry_block_rules(void)
{
    run_t *r = verify(MOD("  func @f(%c: bool) -> void {\n"
                          "    block entry:\n"
                          "      branch %c, entry, exit\n"
                          "    block exit:\n      unreachable\n  }\n"),
                      IR_PROP_PARSED);

    check("a branch back to entry gives it a predecessor, which is caught",
          !parsed(r) && said(r, S("parsed.entry")));
    done(r);

    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, S("m"), &ir_profile_bce, S("t"));
    ir_func_t   *f   = ir_func_add(m, S("f"), ir_type_parse(m, S("() -> void")),
                                   IR_LINK_EXTERNAL);
    ir_block_t  *e   = ir_block_add(f, S("entry"));

    ir_block_add(f, S("other"));

    check("the entry block is the first block by construction, so there is "
          "nothing to verify", ir_block_entry(f) == f->blocks[0]
                                   && ir_block_entry(f) == e);

    ir_block_t *front = ir_block_add_entry(f, S("front"));
    ir_instr_t *jump  = ir_instr_add(front, IR_OP_JUMP);

    ir_instr_operand_block(jump, e);
    check("a block added as the entry comes first and the rest shift down",
          ir_block_entry(f) == front && front->index == 0
              && f->blocks[1] == e && e->index == 1
              && f->blocks[2]->index == 2
              && ir_block_find(f, S("front")) == front);
    check("a duplicate entry name is refused",
          !ir_block_add_entry(f, S("front")) && f->n_blocks == 3);
    ir_ctx_free(ctx);
}

static void test_phi_names_its_predecessors(void)
{
    core_string_t twice = MOD("  func @f(%a: i32, %c: bool) -> void {\n"
                            "    block entry:\n      branch %c, l, l\n"
                            "    block l:\n"
                            "      %p: i32 = phi [entry: %a], [entry: %a]\n"
                            "      unreachable\n  }\n");
    core_string_t stranger = MOD("  func @f(%a: i32, %c: bool) -> void {\n"
                               "    block entry:\n      branch %c, l, r\n"
                               "    block l:\n      jump r\n"
                               "    block r:\n"
                               "      %p: i32 = phi [entry: %a], [l: %a], "
                               "[r: %a]\n      unreachable\n  }\n");

    run_t *t = verify(twice, IR_PROP_PARSED);

    check("a phi that names one predecessor twice is caught",
          !parsed(t) && said(t, S("parsed.phi.arms")));
    done(t);

    run_t *sx = verify(stranger, IR_PROP_PARSED);

    check("a phi that names a block that is not a predecessor is caught",
          !parsed(sx) && said(sx, S("parsed.phi.arms")));
    done(sx);
}

static void test_values_carry_types(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, S("m"), &ir_profile_bce, S("t"));

    ir_module_set_span(m, ir_span_make(0, 0, 6, 1, 1, 1, 7));

    ir_func_t  *f = ir_func_add(m, S("f"), ir_type_parse(m, S("() -> void")),
                                IR_LINK_EXTERNAL);
    ir_block_t *b = ir_block_add(f, S("entry"));

    ir_instr_add(b, IR_OP_UNREACHABLE);

    ir_value_t *p = ir_param_add(f, S("a"), ir_type_parse(m, S("i32")));

    p->type = nullptr;

    ir_diags_t *d = ir_diags_new(ctx, nullptr);

    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED);

    ir_props_t held = ir_verify(m, d);

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        produced[ir_diags_at(d, i)->code] = true;
    }

    check("a value with no type is caught",
          !(held & IR_PROP_TYPED) && told(d, S("has no type")));
    ir_ctx_free(ctx);
}

static void test_dominators(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
    ir_module_t *m   = ir_module_parse(ctx,
                                       MOD("  func @f(%c: bool) -> void {\n"
                                           "    block entry:\n"
                                           "      branch %c, l, r\n"
                                           "    block l:\n      jump join\n"
                                           "    block r:\n      jump join\n"
                                           "    block join:\n"
                                           "      unreachable\n"
                                           "    block orphan:\n"
                                           "      unreachable\n  }\n"),
                                       0, PROFILES, 2, d);

    check("the diamond parses", m != nullptr);

    if (!m) {
        ir_ctx_free(ctx);

        return;
    }

    ir_func_t  *f    = ir_func_at(m, 0);
    ir_doms_t  *dom  = ir_doms_of(f);
    ir_block_t *e    = ir_block_find(f, S("entry"));
    ir_block_t *l    = ir_block_find(f, S("l"));
    ir_block_t *r    = ir_block_find(f, S("r"));
    ir_block_t *j    = ir_block_find(f, S("join"));
    ir_block_t *orph = ir_block_find(f, S("orphan"));

    check("every block dominates itself", ir_dominates(dom, l, l));
    check("entry dominates every reachable block",
          ir_dominates(dom, e, l) && ir_dominates(dom, e, r)
              && ir_dominates(dom, e, j));
    check("a branch arm does not dominate the join",
          !ir_dominates(dom, l, j) && !ir_dominates(dom, r, j));
    check("and the arms do not dominate each other",
          !ir_dominates(dom, l, r) && !ir_dominates(dom, r, l));
    check("the join's immediate dominator is the block that branched",
          ir_idom_of(dom, j) == e);
    check("an arm's immediate dominator is entry too",
          ir_idom_of(dom, l) == e && ir_idom_of(dom, r) == e);
    check("entry has no immediate dominator",
          ir_idom_of(dom, e) == nullptr);
    check("an unreachable block dominates nothing and nothing dominates it",
          !ir_dominates(dom, e, orph) && !ir_dominates(dom, orph, j));
    check("and it has no immediate dominator",
          ir_idom_of(dom, orph) == nullptr);
    ir_ctx_free(ctx);
}

static void test_origins(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_func_t   *f   = nullptr;
    ir_module_t *m   = built(ctx, &f);
    ir_instr_t  *in  = ir_block_entry(f)->instrs[0];

    check("an origin that is not SOURCE and names no agent cannot be built",
          ir_origin_new(ctx, IR_ORIGIN_INLINED_FROM, in->span,
                        (core_string_t){}, nullptr) == nullptr);
    check("a SOURCE origin needs no agent",
          ir_origin_new(ctx, IR_ORIGIN_SOURCE, in->span, (core_string_t){},
                        nullptr)
              != nullptr);

    ir_module_establish(m, IR_PROP_PARSED);

    ir_origin_t *a = ir_origin_new(ctx, IR_ORIGIN_INLINED_FROM, in->span,
                                   S("p"), nullptr);
    ir_origin_t *b = ir_origin_new(ctx, IR_ORIGIN_INLINED_FROM, in->span,
                                   S("p"), a);

    a->parent  = b;
    in->origin = a;

    ir_diags_t *d2 = ir_diags_new(ctx, nullptr);
    ir_props_t  h2 = ir_verify(m, d2);

    for (uint32_t i = 0; i < ir_diags_count(d2); i++) {
        produced[ir_diags_at(d2, i)->code] = true;
    }

    check("an origin chain that loops is caught rather than followed forever",
          !(h2 & IR_PROP_PARSED) && told(d2, S("origin.acyclic")));
    check("and the verifier places and names every diagnostic it makes",
          every_diag_is_placed_and_named(d2));
    ir_ctx_free(ctx);
}

static void test_annotations(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
    ir_func_t   *f   = nullptr;
    ir_module_t *m   = built(ctx, &f);
    ir_instr_t  *in  = ir_block_entry(f)->instrs[0];

    ir_annot_set(ctx, &in->annots,
                 (ir_annot_t){.key = S("bare"), .kind = IR_ANNOT_BOOL, .b = true});
    ir_module_establish(m, IR_PROP_PARSED);

    ir_props_t held = ir_verify(m, d);

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        produced[ir_diags_at(d, i)->code] = true;
    }

    ir_render_t rd = {.module = m};
    core_string_t rp = ir_diags_render_text(d, &rd);

    check("an annotation key with no namespace is caught",
          !(held & IR_PROP_PARSED) && told(d, S("annot.namespace")));
    free(rp.data);
    ir_ctx_free(ctx);
}

static void test_annotation_invalidation(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_func_t   *f   = nullptr;
    ir_module_t *m   = built(ctx, &f);
    ir_instr_t  *in  = ir_block_entry(f)->instrs[0];
    ir_block_t  *b   = ir_block_entry(f);

    ir_annots_t *maps[] = {&in->annots, &b->annots, &f->annots,
                           ir_module_annots(m)};

    for (size_t i = 0; i < 4; i++) {
        ir_annot_set(ctx, maps[i],
                     (ir_annot_t){.key          = S("ir.mark"),
                                  .kind         = IR_ANNOT_BOOL,
                                  .b            = true,
                                  .derived_from = IR_PROP_TYPED});
        ir_annot_set(ctx, maps[i],
                     (ir_annot_t){.key  = S("bce.kept"),
                                  .kind = IR_ANNOT_BOOL,
                                  .b    = true});
    }

    for (size_t i = 0; i < 4; i++) {
        ir_annot_invalidate(maps[i], IR_PROP_TYPED);
    }

    bool dropped = true;
    bool kept    = true;

    for (size_t i = 0; i < 4; i++) {
        dropped = dropped && !ir_annot_get(maps[i], S("ir.mark"));
        kept    = kept && ir_annot_get(maps[i], S("bce.kept"));
    }

    check("invalidating a property drops what was derived from it, on every "
          "node kind", dropped);
    check("and leaves what a pass did not derive alone", kept);
    ir_ctx_free(ctx);
}

static bool drop_spans(ir_module_t *m, ir_diags_t *d, void *state)
{
    (void)d;
    (void)state;

    ir_func_t *f = ir_func_at(m, 0);

    ir_block_entry(f)->instrs[0]->span = IR_SPAN_UNKNOWN;

    return true;
}

static bool keep_spans(ir_module_t *m, ir_diags_t *d, void *state)
{
    (void)m;
    (void)d;
    (void)state;

    return true;
}

static bool add_a_local(ir_module_t *m, ir_diags_t *d, void *state)
{
    (void)d;
    (void)state;

    ir_local_add(ir_func_at(m, 0), S("t"), ir_type_parse(m, S("i32")));

    return true;
}

static bool a_pass_that_gives_up(ir_module_t *m, ir_diags_t *d, void *state)
{
    (void)m;
    (void)d;
    (void)state;

    return false;
}

static bool a_pass_that_edits(ir_module_t *m, ir_diags_t *d, void *state)
{
    (void)d;
    (void)state;

    ir_func_t *f = ir_func_at(m, 0);

    ir_instr_add(ir_block_entry(f), IR_OP_UNREACHABLE);

    return true;
}

static void test_a_pass_leaves_foreign_annotations_alone(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_func_t   *f   = nullptr;
    ir_module_t *m   = built(ctx, &f);
    ir_instr_t  *in  = ir_block_entry(f)->instrs[0];

    ir_annot_set(ctx, &in->annots,
                 (ir_annot_t){.key  = S("bce.mine"),
                              .kind = IR_ANNOT_STR,
                              .s    = S("keep me")});
    ir_annot_set(ctx, &f->annots,
                 (ir_annot_t){.key  = S("bce.mine"),
                              .kind = IR_ANNOT_STR,
                              .s    = S("keep me")});
    ir_module_establish(m, IR_PROP_PARSED);

    ir_pass_t edits = {.name        = S("edits"),
                       .requires    = IR_PROP_PARSED,
                       .invalidates = IR_PROP_SSA,
                       .run         = a_pass_that_edits};

    check("a pass that does not know a namespace still runs",
          ir_pass_run(&edits, m, nullptr, nullptr, nullptr) == IR_PASS_OK);

    ir_annot_t *kept  = ir_annot_get(&in->annots, S("bce.mine"));
    ir_annot_t *kept2 = ir_annot_get(&f->annots, S("bce.mine"));

    check("and leaves an annotation in a namespace it does not understand",
          kept && kept->s.data && core_string_eq(kept->s, S("keep me"))
              && kept2);
    ir_ctx_free(ctx);
}

static void test_passes(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
    ir_func_t   *f   = nullptr;
    ir_module_t *m   = built(ctx, &f);

    ir_module_establish(m, IR_PROP_PARSED);

    ir_pass_t losing = {.name        = S("loses-locations"),
                        .requires    = IR_PROP_PARSED,
                        .run         = drop_spans};
    ir_pass_t honest = {.name        = S("keeps-locations"),
                        .requires    = IR_PROP_PARSED,
                        .run         = keep_spans};

    check("a pass that keeps its locations runs",
          ir_pass_run(&honest, m, nullptr, nullptr, d) == IR_PASS_OK);

    check("a pass that discards a known location fails",
          ir_pass_run(&losing, m, nullptr, nullptr, d)
              == IR_PASS_FAILED);

    ir_pass_t mute = {.name     = S("gives-up"),
                      .requires = IR_PROP_PARSED,
                      .run      = a_pass_that_gives_up};
    uint32_t  said = ir_diags_count(d);

    check("a pass that fails is a failure",
          ir_pass_run(&mute, m, nullptr, nullptr, d)
              == IR_PASS_FAILED);
    check("and failing without saying why is itself reported",
          ir_diags_count(d) > said
              && ir_diags_at(d, said)->code == IR_DIAG_INTERNAL);
    check("and it names the pass that went quiet",
          told(d, S("gives-up failed and said nothing")));

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        produced[ir_diags_at(d, i)->code] = true;
    }

    ir_render_t rd = {.module = m};
    core_string_t rp = ir_diags_render_text(d, &rd);

    check("and says what it lost",
          told(d, S("had a known location before loses-locations ran")));
    free(rp.data);
    ir_ctx_free(ctx);
}

static void test_undeclared_invalidation(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
    ir_func_t   *f   = nullptr;
    ir_module_t *m   = built(ctx, &f);

    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);
    check("the module holds ssa to begin with",
          (ir_verify(m, d) & IR_PROP_SSA) != 0);

    ir_pass_t sneaky = {.name     = S("adds-a-local"),
                        .requires = IR_PROP_PARSED,
                        .run      = add_a_local};

    ir_pass_t honest = {.name     = S("does-nothing"),
                        .requires = IR_PROP_PARSED,
                        .run      = keep_spans};

    check("the pass runs and declares nothing",
          ir_pass_run(&sneaky, m, nullptr, nullptr, d) == IR_PASS_OK);
    check("the verifier runs between that pass and the next",
          (ir_verify(m, d) & IR_PROP_PARSED) != 0);
    check("the next pass runs after it",
          ir_pass_run(&honest, m, nullptr, nullptr, d) == IR_PASS_OK);
    check("and the verifier runs again after that one",
          (ir_verify(m, d) & IR_PROP_PARSED) != 0);
    check("and finds ssa no longer holds",
          !(ir_verify(m, d) & IR_PROP_SSA));

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        produced[ir_diags_at(d, i)->code] = true;
    }

    ir_render_t rd = {.module = m};
    core_string_t rp = ir_diags_render_text(d, &rd);

    check("a pass that edited without declaring invalidation is caught",
          told(d, S("claims ssa, which the verifier finds does not hold")));
    free(rp.data);
    ir_ctx_free(ctx);
}

static void test_spanless_diagnostic_is_a_bug(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
    ir_func_t   *f   = nullptr;
    ir_module_t *m   = built(ctx, &f);

    ir_diag_begin(d, IR_DIAG_INTERNAL, IR_SPAN_UNKNOWN, IR_NO_LABEL);
    ir_verify(m, d);

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        produced[ir_diags_at(d, i)->code] = true;
    }

    ir_render_t rd = {.module = m};
    core_string_t rp = ir_diags_render_text(d, &rd);

    check("a diagnostic reported with no location is itself reported as a bug",
          told(d, S("no primary span")));
    free(rp.data);
    ir_ctx_free(ctx);
}

static void test_release_liveness(void)
{
    run_t *direct = verify(MOD(
        "  func @f(%p: ptr[i32]) -> void {\n"
        "    block entry:\n"
        "      dealloc %p\n"
        "      dealloc %p\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED | IR_PROP_TYPED);
    check("dealloc rejects a later direct use of its address",
          !typed(direct) && said(direct, S("lifetime.no-live-use")));
    done(direct);
}

static bool immutable(ir_value_t *binding)
{
    if (!binding || !binding->local) return false;
    binding->local->mutability = IR_MUT_IMMUTABLE;
    return true;
}

static bool has_multiple_initialization_diagnostic(ir_diags_t *diagnostics)
{
    for (uint32_t i = 0; i < ir_diags_count(diagnostics); i++) {
        const ir_diag_t *diagnostic = ir_diags_at(diagnostics, i);
        if (diagnostic->code == IR_DIAG_IMMUTABLE_REINITIALIZED
            && contains(ir_diag_item(diagnostics, diagnostic)->message,
                        S("can assign it a second time"))) return true;
    }
    return false;
}

static void test_global_single_initialization(void)
{
    run_t *r = verify(MOD(
        "  global @x: i32\n"
        "  func @init() -> void {\n"
        "    block entry:\n"
        "      %one: i32 = const 1\n"
        "      global.store @x, %one\n"
        "      %two: i32 = const 2\n"
        "      global.store @x, %two\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    ir_global_t *global = r->m ? ir_global_find(r->m, S("x")) : nullptr;
    ir_func_t *function = r->m ? ir_func_find(r->m, S("init")) : nullptr;
    ir_block_t *entry = function ? ir_block_entry(function) : nullptr;
    if (global) global->mutability = IR_MUT_IMMUTABLE;
    bool configured = global && function && entry
        && ir_func_set_initializer(r->ctx, function, true);
    size_t stores = 0;
    for (uint32_t i = 0; configured && i < entry->n_instrs; i++) {
        ir_instr_t *instruction = entry->instrs[i];
        if (instruction->op != IR_OP_GLOBAL_STORE) continue;
        stores++;
        configured = ir_instr_set_binding_initialization(instruction, true);
    }
    configured = configured && stores == 2;
    if (configured) {
        ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
        r->held = ir_verify(r->m, r->d);
    }
    check("typed IR rejects multiple initialization sites for an immutable global",
          configured && !typed(r)
              && has_multiple_initialization_diagnostic(r->d));
    done(r);
}

static void test_loop_binding_reentry(void)
{
    core_string_t source = MOD(
        "  func @f(%condition: bool, %again: bool) -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      jump loop\n"
        "    block loop:\n"
        "      branch %condition, left, right\n"
        "    block left:\n"
        "      %one: i32 = const 1\n"
        "      $x = copy %one\n"
        "      jump latch\n"
        "    block right:\n"
        "      %two: i32 = const 2\n"
        "      $x = copy %two\n"
        "      jump latch\n"
        "    block latch:\n"
        "      branch %again, loop, exit\n"
        "    block exit:\n"
        "      return\n"
        "  }\n");
    for (bool reenters = false; ; reenters = true) {
        run_t *r = verify(source, IR_PROP_PARSED);
        ir_func_t *function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
        ir_local_t *local = function ? ir_local_find(function, S("x")) : nullptr;
        ir_block_t *header = function ? ir_block_find(function, S("loop"))
                                     : nullptr;
        ir_value_t *binding = function && local
            ? ir_local_value(function, local) : nullptr;
        if (local) local->mutability = IR_MUT_IMMUTABLE;
        bool configured = binding && header;
        if (configured && reenters)
            configured = ir_value_set_binding_reentry(binding, header);
        size_t events = 0;
        for (uint32_t b = 0; configured && b < function->n_blocks; b++) {
            ir_block_t *block = function->blocks[b];
            for (uint32_t i = 0; i < block->n_instrs; i++) {
                ir_instr_t *instruction = block->instrs[i];
                if (instruction->op != IR_OP_COPY
                    || instruction->dest_local != local) continue;
                events++;
                configured = ir_instr_set_binding_initialization(
                    instruction, true);
            }
        }
        configured = configured && events == 2;
        if (configured && reenters) {
            core_string_t serialized = ir_module_print(r->m);
            ir_module_t *copy = serialized.data
                ? ir_module_parse(r->ctx, serialized, 0, PROFILES, 2, r->d)
                : nullptr;
            free(serialized.data);
            r->m = copy;
            function = copy ? ir_func_find(copy, S("f")) : nullptr;
            local = function ? ir_local_find(function, S("x")) : nullptr;
            binding = function && local
                ? ir_local_value(function, local) : nullptr;
            ir_annot_t *reentry = binding
                ? ir_annot_get(ir_value_annots(binding),
                               S("ir.binding_reentry_header")) : nullptr;
            configured = reentry && reentry->kind == IR_ANNOT_STR
                && reentry->s.data && core_string_eq(reentry->s, S("loop"));
        }
        if (configured) {
            ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
            r->held = ir_verify(r->m, r->d);
        }
        if (reenters)
            check("loop re-entry resets initialization state between iterations",
                  configured && typed(r) && !ir_diags_failed(r->d));
        else
            check("distinct init sites across loop iterations require a re-entry contract",
                  configured && !typed(r)
                      && has_multiple_initialization_diagnostic(r->d));
        done(r);
        if (reenters) break;
    }

    run_t *same_iteration = verify(MOD(
        "  func @f(%again: bool) -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      jump loop\n"
        "    block loop:\n"
        "      %one: i32 = const 1\n"
        "      $x = copy %one\n"
        "      %two: i32 = const 2\n"
        "      $x = copy %two\n"
        "      branch %again, loop, exit\n"
        "    block exit:\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    ir_func_t *function = same_iteration->m
        ? ir_func_find(same_iteration->m, S("f")) : nullptr;
    ir_local_t *local = function ? ir_local_find(function, S("x")) : nullptr;
    ir_block_t *header = function ? ir_block_find(function, S("loop")) : nullptr;
    ir_value_t *binding = function && local
        ? ir_local_value(function, local) : nullptr;
    bool configured = binding && header && immutable(binding)
        && ir_value_set_binding_reentry(binding, header);
    for (uint32_t i = 0; configured && i < header->n_instrs; i++) {
        ir_instr_t *instruction = header->instrs[i];
        if (instruction->op == IR_OP_COPY
            && instruction->dest_local == local)
            configured = ir_instr_set_binding_initialization(
                instruction, true);
    }
    if (configured) {
        ir_module_establish(same_iteration->m,
                            IR_PROP_PARSED | IR_PROP_TYPED);
        same_iteration->held = ir_verify(same_iteration->m,
                                         same_iteration->d);
    }
    check("loop re-entry does not excuse duplicate sites in one iteration",
          configured && !typed(same_iteration)
              && has_multiple_initialization_diagnostic(
                  same_iteration->d));
    done(same_iteration);
}

static void test_local_phase_writes(void)
{
    core_string_t valid_source = MOD(
        "  func @f() -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      %one: i32 = const 1\n"
        "      $x = copy %one\n"
        "      return\n"
        "  }\n");
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *diags = ir_diags_new(ctx, nullptr);
    ir_module_t *module = ir_module_parse(ctx, valid_source, 0, PROFILES, 2,
                                          diags);
    ir_func_t *function = module ? ir_func_find(module, S("f")) : nullptr;
    ir_local_t *local = function ? ir_local_find(function, S("x")) : nullptr;
    ir_instr_t *copy = nullptr;
    ir_block_t *entry = function ? ir_block_entry(function) : nullptr;
    for (uint32_t i = 0; entry && i < entry->n_instrs; i++)
        if (entry->instrs[i]->op == IR_OP_COPY) copy = entry->instrs[i];
    bool setup = local && copy
        && immutable(ir_local_value(function, local))
        && ir_instr_set_binding_initialization(copy, true);
    if (module && setup) {
        ir_module_establish(module, IR_PROP_PARSED | IR_PROP_TYPED);
        ir_props_t held = ir_verify(module, diags);
        check("immutable local declaration initialization is allowed once",
              (held & IR_PROP_TYPED) && !ir_diags_failed(diags));
    }
    else {
        check("immutable local initialization test fixture is valid", false);
    }
    ir_ctx_free(ctx);

    core_string_t invalid_source = MOD(
        "  func @f() -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      %one: i32 = const 1\n"
        "      %two: i32 = const 2\n"
        "      $x = copy %one\n"
        "      $x = copy %two\n"
        "      return\n"
        "  }\n");
    ctx = ir_ctx_new();
    diags = ir_diags_new(ctx, nullptr);
    module = ir_module_parse(ctx, invalid_source, 0, PROFILES, 2, diags);
    function = module ? ir_func_find(module, S("f")) : nullptr;
    local = function ? ir_local_find(function, S("x")) : nullptr;
    entry = function ? ir_block_entry(function) : nullptr;
    copy = nullptr;
    for (uint32_t i = 0; entry && i < entry->n_instrs; i++)
        if (entry->instrs[i]->op == IR_OP_COPY) {
            copy = entry->instrs[i];
            break;
        }
    setup = local && copy && immutable(ir_local_value(function, local));
    for (uint32_t i = 0; setup && entry && i < entry->n_instrs; i++)
        if (entry->instrs[i]->op == IR_OP_COPY)
            setup = ir_instr_set_binding_initialization(entry->instrs[i],
                                                        true);
    if (module && setup) {
        ir_module_establish(module, IR_PROP_PARSED | IR_PROP_TYPED);
        ir_props_t held = ir_verify(module, diags);
        bool duplicate_diagnostic = false;
        for (uint32_t i = 0; i < ir_diags_count(diags); i++) {
            const ir_diag_t *diagnostic = ir_diags_at(diags, i);
            produced[diagnostic->code] = true;
            duplicate_diagnostic |=
                diagnostic->code == IR_DIAG_IMMUTABLE_REINITIALIZED
                && contains(ir_diag_item(diags, diagnostic)->message,
                            S("can assign it a second time"));
        }
        check("typed IR rejects multiple immutable initialization sites on one path",
              !(held & IR_PROP_TYPED) && duplicate_diagnostic);
    }
    else {
        check("immutable local reassignment test fixture is valid", false);
    }
    ir_ctx_free(ctx);

    core_string_t branch_source = MOD(
        "  func @f(%condition: bool) -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      branch %condition, left, right\n"
        "    block left:\n"
        "      %one: i32 = const 1\n"
        "      $x = copy %one\n"
        "      jump join\n"
        "    block right:\n"
        "      %two: i32 = const 2\n"
        "      $x = copy %two\n"
        "      jump join\n"
        "    block join:\n"
        "      return\n"
        "  }\n");
    ctx = ir_ctx_new();
    diags = ir_diags_new(ctx, nullptr);
    module = ir_module_parse(ctx, branch_source, 0, PROFILES, 2, diags);
    function = module ? ir_func_find(module, S("f")) : nullptr;
    local = function ? ir_local_find(function, S("x")) : nullptr;
    setup = local && immutable(ir_local_value(function, local));
    size_t branch_initializers = 0;
    for (uint32_t b = 0; setup && b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *instruction = function->blocks[b]->instrs[i];
            if (instruction->op != IR_OP_COPY
                || instruction->dest_kind != IR_DEST_LOCAL
                || instruction->dest_local != local) continue;
            setup = ir_instr_set_binding_initialization(instruction, true);
            branch_initializers++;
        }
    if (module && setup && branch_initializers == 2) {
        ir_module_establish(module, IR_PROP_PARSED | IR_PROP_TYPED);
        ir_props_t held = ir_verify(module, diags);
        check("mutually exclusive branches may initialize one immutable binding",
              (held & IR_PROP_TYPED) && !ir_diags_failed(diags));
    }
    else {
        check("branch initialization test fixture is valid", false);
    }
    ir_ctx_free(ctx);

    core_string_t loop_source = MOD(
        "  func @f(%condition: bool) -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      jump loop\n"
        "    block loop:\n"
        "      %one: i32 = const 1\n"
        "      $x = copy %one\n"
        "      branch %condition, loop, exit\n"
        "    block exit:\n"
        "      return\n"
        "  }\n");
    ctx = ir_ctx_new();
    diags = ir_diags_new(ctx, nullptr);
    module = ir_module_parse(ctx, loop_source, 0, PROFILES, 2, diags);
    function = module ? ir_func_find(module, S("f")) : nullptr;
    local = function ? ir_local_find(function, S("x")) : nullptr;
    entry = function ? ir_block_find(function, S("loop")) : nullptr;
    copy = nullptr;
    for (uint32_t i = 0; entry && i < entry->n_instrs; i++)
        if (entry->instrs[i]->op == IR_OP_COPY) copy = entry->instrs[i];
    setup = local && copy && immutable(ir_local_value(function, local))
        && ir_instr_set_binding_initialization(copy, true);
    if (module && setup) {
        ir_module_establish(module, IR_PROP_PARSED | IR_PROP_TYPED);
        ir_props_t held = ir_verify(module, diags);
        check("re-entering one static initialization site in a loop is not a second site",
              (held & IR_PROP_TYPED) && !ir_diags_failed(diags));
    }
    else {
        check("loop initialization test fixture is valid", false);
    }
    ir_ctx_free(ctx);
}

static void test_global_storage_requirement(void)
{
    core_string_t source = MOD(
        "  global @state: i32\n"
        "  func @f() -> void {\n"
        "    block entry:\n      return\n  }\n");
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *diags = ir_diags_new(ctx, nullptr);
    ir_module_t *module = ir_module_parse(ctx, source, 0, PROFILES, 2,
                                          diags);
    ir_global_t *global = module ? ir_global_find(module, S("state")) : nullptr;
    bool malformed = global
        && ir_global_set_storage_class(ctx, global, S("pool"), false,
                                       S("gc"))
        && ir_annot_set(ctx, &global->annots, (ir_annot_t){
            .key = S("ir.storage_required"), .kind = IR_ANNOT_BOOL, .b = true,
        });
    core_string_t serialized = malformed ? ir_module_print(module)
                                         : (core_string_t){};
    module = serialized.data
        ? ir_module_parse(ctx, serialized, 0, PROFILES, 2, diags) : nullptr;
    global = module ? ir_global_find(module, S("state")) : nullptr;
    malformed = malformed && module && global;
    free(serialized.data);
    if (module && malformed) {
        ir_module_establish(module, IR_PROP_PARSED | IR_PROP_TYPED);
        ir_props_t held = ir_verify(module, diags);
        bool storage_diagnostic = false;
        for (uint32_t i = 0; i < ir_diags_count(diags); i++) {
            const ir_diag_t *diagnostic = ir_diags_at(diags, i);
            produced[diagnostic->code] = true;
            storage_diagnostic |= diagnostic->code == IR_DIAG_VERIFY_STORAGE;
        }
        check("typed IR rejects an unhonored required global storage class",
              !(held & IR_PROP_TYPED) && storage_diagnostic);
    }
    else {
        check("global storage requirement test fixture is valid", false);
    }
    ir_ctx_free(ctx);

    run_t *malformed_requirement = verify(MOD(
        "  global @state: i32 !ir.storage_required(\"yes\")\n"
        "  func @f() -> void {\n"
        "    block entry:\n      return\n  }\n"),
        IR_PROP_PARSED | IR_PROP_TYPED);
    bool storage_diagnostic = false;
    for (uint32_t i = 0; i < ir_diags_count(malformed_requirement->d); i++)
        storage_diagnostic |= ir_diags_at(malformed_requirement->d, i)->code
            == IR_DIAG_VERIFY_STORAGE;
    check("typed IR rejects malformed global storage requirement metadata",
          !typed(malformed_requirement) && storage_diagnostic);
    done(malformed_requirement);
}

static void test_allocation_storage_requirement(void)
{
    run_t *missing = verify(MOD(
        "  func @f() -> void {\n"
        "    block entry:\n"
        "      %p: ptr[i32] = alloc i32 !ir.storage_required(true)\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED | IR_PROP_TYPED);
    check("typed IR rejects alloc with a missing required storage class",
          !typed(missing) && said(missing, S("storage.required")));
    done(missing);

    run_t *malformed = verify(MOD(
        "  func @f() -> void {\n"
        "    block entry:\n"
        "      %p: ptr[i32] = alloc i32 !ir.storage_required(\"yes\")\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED | IR_PROP_TYPED);
    check("typed IR rejects malformed required storage metadata",
          !typed(malformed) && said(malformed, S("storage.required")));
    done(malformed);

    run_t *valid = verify(MOD(
        "  func @f() -> void {\n"
        "    block entry:\n"
        "      %p: ptr[i32] = alloc i32\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    ir_func_t *function = valid->m
        ? ir_func_find(valid->m, S("f")) : nullptr;
    ir_block_t *entry = function ? ir_block_find(function, S("entry")) : nullptr;
    ir_instr_t *allocation = entry && entry->n_instrs
        ? entry->instrs[0] : nullptr;
    bool configured = allocation
        && ir_instr_set_storage_class(allocation, S("pool"), true);
    if (configured) {
        ir_module_establish(valid->m, IR_PROP_PARSED | IR_PROP_TYPED);
        valid->held = ir_verify(valid->m, valid->d);
    }
    check("typed IR accepts alloc with its required selected class",
          configured && typed(valid) && !ir_diags_failed(valid->d));
    done(valid);
}

static void test_a_phase_can_be_asked_for_on_its_own(void)
{
    core_string_t src = MOD("  func @f(%a: i32) -> i32 {\n"
                          "    local $t: i32\n    block entry:\n"
                          "      $t = copy %a\n      return %a\n  }\n");

    ir_ctx_t    *ctx = ir_ctx_new();
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
    ir_module_t *m   = ir_module_parse(ctx, src, 0, PROFILES, 2, d);

    check("the module for the single-phase checks parses", m != nullptr);

    if (!m) {
        ir_ctx_free(ctx);

        return;
    }

    ir_module_establish(m, IR_PROP_PARSED);

    ir_props_t one = ir_verify_only(m, IR_PROP_PARSED, d);

    check("asking for parsed alone answers parsed",
          (one & IR_PROP_PARSED) != 0);
    check("and answers nothing it was not asked",
          !(one & (IR_PROP_TYPED | IR_PROP_SSA | IR_PROP_LAID_OUT
                   | IR_PROP_NO_CRIT_EDGE | IR_PROP_STRUCTURED)));

    ir_diags_t *sd  = ir_diags_new(ctx, nullptr);
    ir_props_t  ssa = ir_verify_only(m, IR_PROP_SSA, sd);

    check("asking for ssa runs typed and parsed, which it needs",
          (ssa & IR_PROP_PARSED) && (ssa & IR_PROP_TYPED));
    check("the module is not in ssa form and is told so",
          !(ssa & IR_PROP_SSA) && told(sd, S("ssa.locals")));
    check("and it is told so though the module never claimed ssa",
          !(ir_module_props(m) & IR_PROP_SSA));
    check("asking for ssa does not run the phases ssa does not need",
          !(ssa & (IR_PROP_LAID_OUT | IR_PROP_NO_CRIT_EDGE
                   | IR_PROP_STRUCTURED)));

    ir_diags_t *nd = ir_diags_new(ctx, nullptr);

    check("asking for nothing runs nothing",
          ir_verify_only(m, 0, nd) == 0 && ir_diags_count(nd) == 0);

    ir_ctx_free(ctx);
}

int main(void)
{
    test_graded();
    test_failure_channel_ops();
    test_parsed_rules();
    test_ssa_rules();
    test_extension_opcodes();
    test_parsed_rules_the_builder_prevents();
    test_value_references_resolve();
    test_entry_block_rules();
    test_phi_names_its_predecessors();
    test_values_carry_types();
    test_dominators();
    test_origins();
    test_annotations();
    test_annotation_invalidation();
    test_a_pass_leaves_foreign_annotations_alone();
    test_passes();
    test_undeclared_invalidation();
    test_spanless_diagnostic_is_a_bug();
    test_release_liveness();
    test_global_single_initialization();
    test_loop_binding_reentry();
    test_local_phase_writes();
    test_global_storage_requirement();
    test_allocation_storage_requirement();
    test_a_phase_can_be_asked_for_on_its_own();

    dump_produced_codes("produced-codes-verify.txt", produced);

    return report("ir verify");
}
