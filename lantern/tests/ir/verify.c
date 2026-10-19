#include <stdio.h>
#include <stdlib.h>

#include "ir/rule.h"
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

        if (contains(dg->primary.label, text) || contains(dg->message, text)) {
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

        if (dg->severity == IR_SEV_NOTE || dg->severity == IR_SEV_HELP) {
            continue;
        }

        if (!ir_span_known(dg->primary.span) || !dg->primary.label.data
            || !dg->message.data) {
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

    run_t *wrong_type = verify(MOD(
        "  func @f(%e: i32) -> i32 fail: bool {\n"
        "    block entry:\n      fail %e\n  }\n"),
        IR_PROP_PARSED | IR_PROP_TYPED);
    check("a failure terminator rejects a mismatched failure value",
          parsed(wrong_type) && !typed(wrong_type)
              && said(wrong_type, S("failure value")));
    done(wrong_type);

    run_t *no_channel = verify(MOD(
        "  func @f(%e: i32) -> i32 {\n"
        "    block entry:\n      fail %e\n  }\n"),
        IR_PROP_PARSED | IR_PROP_TYPED);
    check("a failure terminator requires a function failure channel",
          parsed(no_channel) && !typed(no_channel)
              && said(no_channel, S("failure exit")));
    done(no_channel);

    run_t *unhandled_call = verify(MOD(
        "  func @callee(%e: i32) -> i32 fail: i32 {\n"
        "    block entry:\n      fail %e\n  }\n"
        "  func @caller() -> i32 {\n"
        "    block entry:\n      %r: i32 = call @callee\n"
        "      return %r\n  }\n"),
        IR_PROP_PARSED | IR_PROP_TYPED);
    check("a call must represent the callee error channel",
          parsed(unhandled_call) && !typed(unhandled_call)
              && said(unhandled_call, S("error channel")));
    done(unhandled_call);
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

static void test_typed_rules(void)
{
    struct {
        core_string_t what;
        core_string_t rule;
        core_string_t src;
    } cases[] = {
        {S("an instruction that breaks its rule"), S("arithmetic operands"),
         MOD("  func @f(%a: i32, %b: u64) -> void {\n    block entry:\n"
             "      %s: i32 = add wrap %a, %b\n      unreachable\n  }\n")},
        {S("a member name that does not resolve"), S("has no member"),
         MOD("  func @f(%p: ptr[(x: i32)]) -> void {\n    block entry:\n"
             "      %q: ptr[i32] = field %p, \"nope\"\n      unreachable\n  }\n")},
        {S("a call whose argument is wrong"), S("and the argument is"),
         MOD("  func @g(%x: i32) -> void {\n    block entry:\n"
             "      unreachable\n  }\n"
             "  func @f(%a: u64) -> void {\n    block entry:\n"
             "      %r: void = call @g, %a\n      unreachable\n  }\n")},
        {S("a return that does not match"),
         S("returns i32, and this value is"),
         MOD("  func @f(%a: u64) -> i32 {\n    block entry:\n"
             "      return %a\n  }\n")},
        {S("a switch on a non-integer"), S("scrutinee"),
         MOD("  func @f(%a: f64) -> void {\n    block entry:\n"
             "      switch %a, default: out\n    block out:\n"
             "      unreachable\n  }\n")},
        {S("an opaque named type used as a scalar"), S("arithmetic needs"),
         MOD("  func @f(%a: Widget) -> void {\n    block entry:\n"
             "      %s: Widget = add wrap %a, %a\n      unreachable\n  }\n")},
        {S("a constant that does not fit"), S("does not fit in"),
         MOD("  func @f() -> void {\n    block entry:\n"
             "      %k: i8 = const 300\n      unreachable\n  }\n")},
        {S("an extract name that does not resolve"), S("has no member"),
         MOD("  func @f(%v: (x: i32)) -> void {\n    block entry:\n"
             "      %q: i32 = extract %v, \"nope\"\n"
             "      unreachable\n  }\n")},
        {S("a case value the scrutinee cannot hold"),
         S("not representable in i8"),
         MOD("  func @f(%a: i8) -> void {\n    block entry:\n"
             "      switch %a, default: out, [300: out]\n"
             "    block out:\n      unreachable\n  }\n")},
        {S("a duplicated switch case"), S("two cases both match"),
         MOD("  func @f(%a: i32) -> void {\n    block entry:\n"
             "      switch %a, default: out, [1: out], [1: out]\n"
             "    block out:\n      unreachable\n  }\n")},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_t *r = verify(cases[i].src, IR_PROP_PARSED | IR_PROP_TYPED);

        if (typed(r) || !said(r, cases[i].rule)) {
            printf("FAIL  %.*s is caught and says %.*s\n",
                   (int)cases[i].what.u8_bytes, cases[i].what.data,
                   (int)cases[i].rule.u8_bytes, cases[i].rule.data);
            fails++;
        }

        if (!parsed(r)) {
            printf("FAIL  %.*s is still structurally well formed\n",
                   (int)cases[i].what.u8_bytes, cases[i].what.data);
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

    run_t *bad = verify(MOD("  func @f(%a: i32, %c: bool) -> i32 {\n"
                            "    block entry:\n      branch %c, l, r\n"
                            "    block l:\n      jump r\n    block r:\n"
                            "      %p: u64 = phi [entry: %a], [l: %a]\n"
                            "      unreachable\n  }\n"),
                        IR_PROP_PARSED | IR_PROP_TYPED);

    check("a phi arm of the wrong type is a typing failure",
          !typed(bad) && said(bad, S("incoming value")));
    done(bad);
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

        if (!ir_span_known(dg->primary.span) || !dg->primary.label.data) {
            core_string_t code    = ir_diag_code(dg->code);
            core_string_t message = dg->message.data ? dg->message
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

static void test_no_rule_resolves(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
    ir_func_t   *f   = nullptr;
    ir_module_t *m   = built(ctx, &f);

    ir_module_set_rules(m, ir_rules_new(ctx));
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED);

    ir_props_t held = ir_verify(m, d);

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        produced[ir_diags_at(d, i)->code] = true;
    }

    check("with no rule for an opcode the module is still parsed",
          (held & IR_PROP_PARSED) != 0);
    check("but is not typed", !(held & IR_PROP_TYPED));
    check("and the verifier says no rule resolved rather than staying quiet",
          told(d, S("no typing rule resolves for unreachable")));
    check("every diagnostic is placed and names its rule",
          every_diag_is_placed_and_named(d));
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

    ir_diag_begin(d, IR_DIAG_INTERNAL, IR_SPAN_UNKNOWN, (core_string_t){});
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

static void test_address_of_local(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, S("m"), &ir_profile_bce, S("t"));
    ir_func_t   *f   = ir_func_add(m, S("f"), ir_type_parse(m, S("() -> void")),
                                   IR_LINK_EXTERNAL);
    ir_block_t  *b   = ir_block_add(f, S("entry"));
    ir_local_t  *l   = ir_local_add(f, S("t"), ir_type_parse(m, S("i32")));
    ir_instr_t  *address = ir_instr_add(b, IR_OP_ADDRESS_OF);

    ir_instr_operand_local(address, l);

    ir_diags_t *d = ir_diags_new(ctx, nullptr);
    ir_rules_t *rs = ir_module_rules(m);

    ir_rule_result_t typed = ir_rule_apply(m, rs, address, d);
    check("address.of yields a pointer to the local's type",
          typed.verdict == IR_RULE_ACCEPT
              && ir_type_eq(m, typed.result, ir_type_parse(m, S("ref[i32]"))));
    ir_ctx_free(ctx);
}

static bool set_alias_analysis_complete(ir_ctx_t *ctx, ir_value_t *value,
                                        ir_value_t *alias,
                                        bool is_alias)
{
    ir_annots_t *annots = ir_value_annots(value);
    if (!ir_annot_set(ctx, annots, (ir_annot_t){
            .key = S("ir.alias_analysis_complete"), .kind = IR_ANNOT_BOOL,
            .b = true,
        })
        || !ir_annot_set(ctx, annots, (ir_annot_t){
            .key = S("ir.lifetime_escapes"), .kind = IR_ANNOT_BOOL,
            .b = false,
        })
        || !ir_annot_set(ctx, annots, (ir_annot_t){
            .key = S("ir.alias_unresolved"), .kind = IR_ANNOT_BOOL,
            .b = false,
        })) return false;

    if (is_alias) {
        return ir_annot_set(ctx, annots, (ir_annot_t){
                   .key = S("ir.alias_count"), .kind = IR_ANNOT_INT,
                   .i = {.mag = 0},
               })
            && ir_annot_set(ctx, annots, (ir_annot_t){
                   .key = S("ir.aliased_by_count"), .kind = IR_ANNOT_INT,
                   .i = {.mag = 1},
               })
            && ir_annot_set(ctx, annots, (ir_annot_t){
                   .key = S("ir.aliased_by0"), .kind = IR_ANNOT_VALUE,
                   .v = alias,
               })
            && ir_annot_set(ctx, annots, (ir_annot_t){
                   .key = S("ir.aliased_by0_escapes"), .kind = IR_ANNOT_BOOL,
                   .b = false,
               });
    }

    return ir_annot_set(ctx, annots, (ir_annot_t){
               .key = S("ir.alias_count"), .kind = IR_ANNOT_INT,
               .i = {.mag = 1},
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = S("ir.aliased_by_count"), .kind = IR_ANNOT_INT,
               .i = {.mag = 0},
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = S("ir.alias0"), .kind = IR_ANNOT_VALUE, .v = alias,
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = S("ir.alias0_escapes"), .kind = IR_ANNOT_BOOL,
               .b = false,
           });
}

static bool set_empty_alias_analysis_complete(ir_ctx_t *ctx,
                                             ir_value_t *value)
{
    ir_annots_t *annots = ir_value_annots(value);
    return ir_annot_set(ctx, annots, (ir_annot_t){
               .key = S("ir.alias_analysis_complete"), .kind = IR_ANNOT_BOOL,
               .b = true,
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = S("ir.lifetime_escapes"), .kind = IR_ANNOT_BOOL,
               .b = false,
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = S("ir.alias_unresolved"), .kind = IR_ANNOT_BOOL,
               .b = false,
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = S("ir.alias_count"), .kind = IR_ANNOT_INT,
               .i = {.mag = 0},
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = S("ir.aliased_by_count"), .kind = IR_ANNOT_INT,
               .i = {.mag = 0},
           });
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

    run_t *alias = verify(MOD(
        "  func @f(%owner: ptr[i32], %alias: ptr[i32]) -> void {\n"
        "    block entry:\n"
        "      finalize %owner\n"
        "      dealloc %alias\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED | IR_PROP_TYPED);
    ir_func_t *function = ir_func_find(alias->m, S("f"));
    ir_value_t *owner = function ? ir_value_find(function, S("owner")) : nullptr;
    ir_value_t *other = function ? ir_value_find(function, S("alias")) : nullptr;
    bool facts = owner && other
        && set_alias_analysis_complete(alias->ctx, owner, other, false)
        && set_alias_analysis_complete(alias->ctx, other, owner, true);
    alias->held = ir_verify(alias->m, alias->d);
    check("finalize rejects a later use through a complete alias component",
          facts && !typed(alias) && said(alias, S("lifetime.no-live-alias")));
    done(alias);

    run_t *unknown = verify(MOD(
        "  func @f(%p: ptr[i32]) -> void {\n"
        "    block entry:\n"
        "      dealloc %p\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED | IR_PROP_TYPED);
    check("dealloc rejects incomplete alias information",
          !typed(unknown) && said(unknown, S("lifetime.unknown-alias")));
    done(unknown);

    core_string_t safe_source = MOD(
        "  func @f(%p: ptr[i32]) -> void {\n"
        "    block entry:\n"
        "      dealloc %p\n"
        "      return\n"
        "  }\n");
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *diags = ir_diags_new(ctx, nullptr);
    ir_module_t *module = ir_module_parse(ctx, safe_source, 0, PROFILES, 2,
                                          diags);
    ir_func_t *safe_function = module
        ? ir_func_find(module, S("f")) : nullptr;
    ir_value_t *safe_value = safe_function
        ? ir_value_find(safe_function, S("p")) : nullptr;
    bool safe_facts = safe_value
        && set_empty_alias_analysis_complete(ctx, safe_value);
    if (module && safe_facts) {
        ir_module_establish(module, IR_PROP_PARSED | IR_PROP_TYPED);
        ir_props_t held = ir_verify(module, diags);
        check("dealloc accepts a dead value with complete alias facts",
              (held & IR_PROP_TYPED) && !ir_diags_failed(diags));
    }
    else {
        check("dealloc test fixture receives complete alias facts", false);
    }
    ir_ctx_free(ctx);
}

static void test_global_phase_access(void)
{
    core_string_t source = MOD(
        "  global @runtime_only: i32\n\n"
        "  func @init() -> void !ir.execution_phase(1) {\n"
        "    block entry:\n"
        "      %value: i32 = load @runtime_only\n"
        "      return\n"
        "  }\n");
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *diags = ir_diags_new(ctx, nullptr);
    ir_module_t *module = ir_module_parse(ctx, source, 0, PROFILES, 2,
                                          diags);
    ir_global_t *global = module
        ? ir_global_find(module, S("runtime_only")) : nullptr;
    bool partial_contract = global && ir_annot_set(ctx, &global->annots,
        (ir_annot_t){.key = S("ir.global_available_phases"),
            .kind = IR_ANNOT_INT, .i = {.mag = IR_PHASE_ACCESS_RUNTIME}})
        && !ir_global_can_read(global, IR_PHASE_RUNTIME);
    check("partial global phase metadata fails conservatively",
          partial_contract);
    bool contract = global && ir_global_set_phase_access(ctx, global,
        IR_PHASE_ACCESS_RUNTIME, IR_PHASE_ACCESS_RUNTIME,
        IR_PHASE_ACCESS_COMPTIME);
    check("global phase contract distinguishes availability, mutation, and initialization",
          contract
              && !ir_global_can_read(global, IR_PHASE_COMPTIME)
              && ir_global_can_read(global, IR_PHASE_RUNTIME)
              && !ir_global_can_write(global, IR_PHASE_COMPTIME)
              && ir_global_can_write(global, IR_PHASE_RUNTIME)
              && ir_global_can_initialize(global, IR_PHASE_COMPTIME));
    bool inconsistent_contract = contract
        && ir_global_set_phase_access(ctx, global,
            IR_PHASE_ACCESS_RUNTIME, IR_PHASE_ACCESS_COMPTIME,
            IR_PHASE_ACCESS_COMPTIME);
    check("mutability does not make an unavailable global writable",
          inconsistent_contract
              && !ir_global_can_write(global, IR_PHASE_COMPTIME));
    contract = contract && ir_global_set_phase_access(ctx, global,
        IR_PHASE_ACCESS_RUNTIME, IR_PHASE_ACCESS_RUNTIME,
        IR_PHASE_ACCESS_COMPTIME);
    if (module && contract) {
        ir_module_establish(module, IR_PROP_PARSED | IR_PROP_TYPED);
        ir_props_t held = ir_verify(module, diags);
        bool rejected = !(held & IR_PROP_TYPED);
        for (uint32_t i = 0; i < ir_diags_count(diags); i++) {
            const ir_diag_t *diagnostic = ir_diags_at(diags, i);
            produced[diagnostic->code] = true;
        }
        bool phase_diagnostic = false;
        for (uint32_t i = 0; i < ir_diags_count(diags); i++) {
            const ir_diag_t *diagnostic = ir_diags_at(diags, i);
            phase_diagnostic |= diagnostic->code == IR_DIAG_VERIFY_PHASE;
        }
        check("typed IR rejects reading a runtime-only global at compile time",
              rejected && phase_diagnostic);
    }
    else {
        check("global phase access test fixture is valid", false);
    }
    ir_ctx_free(ctx);
}

static void test_binding_phase_access(void)
{
    core_string_t source = MOD(
        "  func @init(%input: i32) -> i32 !ir.execution_phase(1) {\n"
        "    block entry:\n"
        "      return %input\n"
        "  }\n");
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *diags = ir_diags_new(ctx, nullptr);
    ir_module_t *module = ir_module_parse(ctx, source, 0, PROFILES, 2,
                                          diags);
    ir_func_t *function = module ? ir_func_find(module, S("init")) : nullptr;
    ir_value_t *input = function ? ir_value_find(function, S("input")) : nullptr;
    bool access = input && ir_value_set_phase_access(ctx, input,
        IR_PHASE_ACCESS_RUNTIME, IR_PHASE_ACCESS_NONE,
        IR_PHASE_ACCESS_NONE);
    check("binding phase access exposes independent read/write/init queries",
          access && !ir_value_can_read(input, IR_PHASE_COMPTIME)
              && ir_value_can_read(input, IR_PHASE_RUNTIME)
              && !ir_value_can_write(input, IR_PHASE_RUNTIME)
              && !ir_value_can_initialize(input, IR_PHASE_RUNTIME));
    if (module && access) {
        ir_module_establish(module, IR_PROP_PARSED | IR_PROP_TYPED);
        ir_props_t held = ir_verify(module, diags);
        bool phase_diagnostic = false;
        for (uint32_t i = 0; i < ir_diags_count(diags); i++) {
            const ir_diag_t *diagnostic = ir_diags_at(diags, i);
            produced[diagnostic->code] = true;
            phase_diagnostic |= diagnostic->code == IR_DIAG_VERIFY_PHASE;
        }
        check("typed IR rejects reading a runtime-only binding at compile time",
              !(held & IR_PROP_TYPED) && phase_diagnostic);
    }
    else {
        check("binding phase access test fixture is valid", false);
    }
    ir_ctx_free(ctx);
}

static void test_store_through_address_phase_access(void)
{
    run_t *r = verify(MOD(
        "  func @f(%input: i32) -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      %address: ref[i32] = address.of $x\n"
        "      store %address, %input\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    ir_func_t *function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
    ir_local_t *local = function ? ir_local_find(function, S("x")) : nullptr;
    bool configured = local && ir_value_set_phase_access(r->ctx,
        ir_local_value(function, local), IR_PHASE_ACCESS_RUNTIME,
        IR_PHASE_ACCESS_NONE, IR_PHASE_ACCESS_NONE);
    if (configured) {
        ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
        r->held = ir_verify(r->m, r->d);
    }
    bool phase_diagnostic = false;
    for (uint32_t i = 0; i < ir_diags_count(r->d); i++) {
        const ir_diag_t *diagnostic = ir_diags_at(r->d, i);
        produced[diagnostic->code] = true;
        phase_diagnostic |= diagnostic->code == IR_DIAG_VERIFY_PHASE;
    }
    check("typed IR checks phase permissions through address.of",
          configured && !typed(r) && phase_diagnostic);
    done(r);

    r = verify(MOD(
        "  global @x: i32\n"
        "  func @f(%input: i32) -> void {\n"
        "    block entry:\n"
        "      %address: ref[i32] = address.of @x\n"
        "      store %address, %input\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    ir_global_t *global = r->m ? ir_global_find(r->m, S("x")) : nullptr;
    configured = global && ir_global_set_phase_access(r->ctx, global,
        IR_PHASE_ACCESS_RUNTIME, IR_PHASE_ACCESS_NONE,
        IR_PHASE_ACCESS_NONE);
    if (configured) {
        ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
        r->held = ir_verify(r->m, r->d);
    }
    phase_diagnostic = false;
    for (uint32_t i = 0; i < ir_diags_count(r->d); i++) {
        const ir_diag_t *diagnostic = ir_diags_at(r->d, i);
        produced[diagnostic->code] = true;
        phase_diagnostic |= diagnostic->code == IR_DIAG_VERIFY_PHASE;
    }
    check("typed IR checks phase permissions through a global address",
          configured && !typed(r) && phase_diagnostic);
    done(r);

    r = verify(MOD(
        "  global @x: i32\n"
        "  func @f(%input: i32) -> void !ir.execution_phase(1) {\n"
        "    block entry:\n"
        "      %address: ref[i32] = address.of @x\n"
        "      store %address, %input\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    global = r->m ? ir_global_find(r->m, S("x")) : nullptr;
    function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
    ir_block_t *entry = function ? ir_block_entry(function) : nullptr;
    ir_instr_t *store = nullptr;
    for (uint32_t i = 0; entry && i < entry->n_instrs; i++)
        if (entry->instrs[i]->op == IR_OP_STORE) store = entry->instrs[i];
    configured = global && function && store
        && ir_global_set_phase_access(r->ctx, global,
            IR_PHASE_ACCESS_BOTH, IR_PHASE_ACCESS_RUNTIME,
            IR_PHASE_ACCESS_COMPTIME)
        && ir_instr_set_binding_initialization(store, true);
    if (configured) {
        ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
        r->held = ir_verify(r->m, r->d);
    }
    check("typed IR accepts an initialization-marked store through a global address",
          configured && typed(r));
    done(r);
}

static void test_store_through_forwarded_addresses(void)
{
    run_t *option = verify(MOD(
        "  func @f(%maybe: Option[ref[i32]], %input: i32) -> void {\n"
        "    block entry:\n"
        "      %address: ref[i32] = option.get %maybe\n"
        "      store %address, %input\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    ir_func_t *option_function = option->m
        ? ir_func_find(option->m, S("f")) : nullptr;
    ir_value_t *extracted = option_function
        ? ir_value_find(option_function, S("address")) : nullptr;
    bool option_configured = extracted
        && ir_value_set_indirect_access(option->ctx, extracted,
                                        IR_INDIRECT_ACCESS_READ_WRITE);
    if (option_configured) {
        ir_module_establish(option->m, IR_PROP_PARSED | IR_PROP_TYPED);
        option->held = ir_verify(option->m, option->d);
    }
    check("typed IR accepts a writable reference extracted from an option parameter",
          option_configured && typed(option));
    done(option);

    run_t *r = verify(MOD(
        "  func @f(%address: ptr[i32], %input: i32) -> void {\n"
        "    block entry:\n"
        "      store %address, %input\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    ir_func_t *function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
    ir_value_t *parameter = function
        ? ir_value_find(function, S("address")) : nullptr;
    bool configured = parameter && ir_value_set_indirect_access(r->ctx,
        parameter, IR_INDIRECT_ACCESS_WRITE);
    if (configured) {
        ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
        r->held = ir_verify(r->m, r->d);
    }
    check("typed IR accepts writes through explicit indirect-access effects",
          configured && typed(r)
              && ir_value_indirect_access(parameter)
                     == IR_INDIRECT_ACCESS_WRITE);
    done(r);

    r = verify(MOD(
        "  func @f(%address: ref[i32], %input: i32) -> void {\n"
        "    block entry:\n"
        "      store %address, %input\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
    parameter = function ? ir_value_find(function, S("address")) : nullptr;
    configured = parameter
        && ir_value_set_indirect_access(r->ctx, parameter,
                                        IR_INDIRECT_ACCESS_READ_WRITE)
        && set_empty_alias_analysis_complete(r->ctx, parameter);
    if (configured) {
        ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
        r->held = ir_verify(r->m, r->d);
    }
    check("complete local aliases do not hide writable parameter storage",
          configured && typed(r));
    done(r);

    r = verify(MOD(
        "  func @f(%input: i32) -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      %address: ref[i32] = address.of $x\n"
        "      %copy: ref[i32] = copy %address\n"
        "      store %copy, %input\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
    ir_local_t *local = function ? ir_local_find(function, S("x")) : nullptr;
    configured = local && ir_value_set_phase_access(r->ctx,
        ir_local_value(function, local), IR_PHASE_ACCESS_RUNTIME,
        IR_PHASE_ACCESS_NONE, IR_PHASE_ACCESS_NONE);
    if (configured) {
        ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
        r->held = ir_verify(r->m, r->d);
    }
    check("typed IR follows copied addresses to their binding phase",
          configured && !typed(r) && said(r, S("phase.binding-write")));
    done(r);

    const core_string_t element_store = MOD(
        "  func @f(%items: array[i32], %slot: i64, %input: i32) -> void {\n"
        "    local $x: array[i32]\n"
        "    block entry:\n"
        "      copy $x, %items\n"
        "      %address: ref[array[i32]] = address.of $x\n"
        "      %element: ref[i32] = index %address, %slot\n"
        "      store %element, %input\n"
        "      return\n"
        "  }\n");
    const core_string_t binding_store = MOD(
        "  func @f(%items: array[i32], %slot: i64, %input: i32) -> void {\n"
        "    local $x: array[i32]\n"
        "    block entry:\n"
        "      copy $x, %items\n"
        "      %address: ref[array[i32]] = address.of $x\n"
        "      store %address, %items\n"
        "      return\n"
        "  }\n");
    const core_string_t sources[] = {element_store, binding_store};
    bool held[2] = {0};
    for (size_t i = 0; i < 2; i++) {
        r = verify(sources[i], IR_PROP_PARSED);
        function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
        local = function ? ir_local_find(function, S("x")) : nullptr;
        ir_instr_t *initializer = function && function->n_blocks
            && function->blocks[0]->n_instrs
            ? function->blocks[0]->instrs[0] : nullptr;
        configured = local && initializer
            && ir_instr_set_binding_initialization(initializer, true)
            && ir_value_set_phase_access(r->ctx,
                ir_local_value(function, local), IR_PHASE_ACCESS_RUNTIME,
                IR_PHASE_ACCESS_NONE, IR_PHASE_ACCESS_RUNTIME);
        if (configured) {
            ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
            r->held = ir_verify(r->m, r->d);
        }
        held[i] = configured && typed(r);
        if (i == 1)
            check("a store over an immutable array binding is a binding write",
                  configured && !held[i] && said(r, S("phase.binding-write")));
        done(r);
    }
    check("an element store writes array contents, not the immutable binding",
          held[0]);

    const core_string_t global_sources[] = {
        MOD("  const global @g: array[i32]\n"
            "  func @f(%slot: i64, %input: i32) -> void {\n"
            "    block entry:\n"
            "      %address: ref[array[i32]] = address.of @g\n"
            "      %element: ref[i32] = index %address, %slot\n"
            "      store %element, %input\n"
            "      return\n"
            "  }\n"),
        MOD("  immutable global @g: array[i32]\n"
            "  func @f(%slot: i64, %input: i32) -> void {\n"
            "    block entry:\n"
            "      %address: ref[array[i32]] = address.of @g\n"
            "      %element: ref[i32] = index %address, %slot\n"
            "      store %element, %input\n"
            "      return\n"
            "  }\n"),
    };
    bool constant_rejected = false, immutable_held = false;
    for (size_t i = 0; i < 2; i++) {
        r = verify(global_sources[i], IR_PROP_PARSED);
        ir_global_t *global = r->m ? ir_global_find(r->m, S("g")) : nullptr;
        configured = global && ir_global_set_phase_access(r->ctx, global,
            IR_PHASE_ACCESS_RUNTIME, IR_PHASE_ACCESS_NONE,
            IR_PHASE_ACCESS_RUNTIME);
        if (configured) {
            ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
            r->held = ir_verify(r->m, r->d);
        }
        if (i == 0)
            constant_rejected = configured && !typed(r)
                && said(r, S("phase.constant-contents-write"));
        else
            immutable_held = configured && typed(r);
        done(r);
    }
    check("an element store into a constant array is rejected",
          constant_rejected);
    check("an element store into an immutable global array is accepted",
          immutable_held);

    r = verify(MOD(
        "  func @f(%condition: bool, %input: i32) -> void {\n"
        "    local $x: i32\n"
        "    local $y: i32\n"
        "    block entry:\n"
        "      branch %condition, left, right\n"
        "    block left:\n"
        "      %left_address: ref[i32] = address.of $x\n"
        "      jump join\n"
        "    block right:\n"
        "      %right_address: ref[i32] = address.of $y\n"
        "      jump join\n"
        "    block join:\n"
        "      %address: ref[i32] = phi [left: %left_address], "
        "[right: %right_address]\n"
        "      store %address, %input\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
    ir_local_t *left = function ? ir_local_find(function, S("x")) : nullptr;
    ir_local_t *right = function ? ir_local_find(function, S("y")) : nullptr;
    configured = left && right
        && ir_value_set_phase_access(r->ctx,
            ir_local_value(function, left), IR_PHASE_ACCESS_RUNTIME,
            IR_PHASE_ACCESS_RUNTIME, IR_PHASE_ACCESS_NONE)
        && ir_value_set_phase_access(r->ctx,
            ir_local_value(function, right), IR_PHASE_ACCESS_RUNTIME,
            IR_PHASE_ACCESS_NONE, IR_PHASE_ACCESS_NONE);
    if (configured) {
        ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
        r->held = ir_verify(r->m, r->d);
    }
    check("typed IR checks every binding reached through a phi address",
          configured && !typed(r) && said(r, S("phase.binding-write")));
    done(r);
}

typedef struct {
    ir_value_t *targets[4];
    size_t count;
} store_target_capture_t;

static bool capture_store_target(void *context, ir_value_t *target)
{
    store_target_capture_t *capture = context;
    if (capture->count == sizeof(capture->targets) / sizeof(*capture->targets))
        return false;
    capture->targets[capture->count++] = target;
    return true;
}

static void test_store_target_visitor(void)
{
    run_t *r = verify(MOD(
        "  global @shared: i32\n"
        "  func @f(%input: i32) -> void {\n"
        "    block entry:\n"
        "      %address: ref[i32] = address.of @shared\n"
        "      %forwarded: ref[i32] = copy %address\n"
        "      store %forwarded, %input\n"
        "      return\n"
        "    }\n"
        ), IR_PROP_PARSED);
    ir_func_t *function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
    ir_instr_t *store = nullptr;
    ir_block_t *entry = function ? ir_block_entry(function) : nullptr;
    for (uint32_t i = 0; entry && i < entry->n_instrs; i++)
        if (entry->instrs[i]->op == IR_OP_STORE) store = entry->instrs[i];
    store_target_capture_t capture = {0};
    bool complete = store && ir_value_visit_store_targets(
        ir_operand_value(&store->operands[0]), capture_store_target, &capture);
    ir_global_t *shared = r->m ? ir_global_find(r->m, S("shared")) : nullptr;
    check("the store-target visitor fixture parses and finds its store",
          store && shared);
    check("the store-target visitor completes with one binding target",
          complete && capture.count == 1);
    check("the store-target visitor follows forwarded addresses to globals",
          complete && capture.count == 1
              && capture.targets[0]->kind == IR_VAL_GLOBAL
              && capture.targets[0]->global == shared);

    run_t *allocated = verify(MOD(
        "  func @f(%allocator: Allocator, %input: i32) -> void {\n"
        "    block entry:\n"
        "      %storage: ptr[i32] = alloc.using i32, %allocator\n"
        "      store %storage, %input\n"
        "      return\n"
        "    }\n"
        ), IR_PROP_PARSED);
    ir_func_t *allocated_function = allocated->m
        ? ir_func_find(allocated->m, S("f")) : nullptr;
    ir_block_t *allocated_entry = allocated_function
        ? ir_block_entry(allocated_function) : nullptr;
    ir_instr_t *allocated_store = nullptr;
    for (uint32_t i = 0; allocated_entry
         && i < allocated_entry->n_instrs; i++)
        if (allocated_entry->instrs[i]->op == IR_OP_STORE)
            allocated_store = allocated_entry->instrs[i];
    capture = (store_target_capture_t){0};
    complete = allocated_store && ir_value_visit_store_targets(
        ir_operand_value(&allocated_store->operands[0]),
        capture_store_target, &capture);
    check("allocator-backed storage is a known store target",
          complete && capture.count == 1
              && capture.targets[0] == allocated_store->operands[0].value);
    done(allocated);

    ir_local_t *returned = ir_local_add(function, S("returned"),
                                        ir_type_parse(r->m, S("ref[i32]")));
    ir_value_t *returned_value = returned
        ? ir_local_value(function, returned) : nullptr;
    ir_value_t *shared_value = ir_global_address(r->m, shared);
    ir_alias_target_t returned_alias = {.value = shared_value};
    bool aliased = returned_value && shared_value
        && ir_value_set_alias_targets(r->ctx, returned_value,
                                      &returned_alias, 1, true);
    capture = (store_target_capture_t){0};
    complete = aliased && ir_value_visit_store_targets(returned_value,
        capture_store_target, &capture);
    check("known aliases can be attached to an indirect value",
          complete && capture.count == 1
              && capture.targets[0]->kind == IR_VAL_GLOBAL
              && capture.targets[0]->global == shared);
    ir_global_t *items = ir_global_add(r->m, S("items"),
        ir_type_parse(r->m, S("array[i32]")));
    ir_value_t *items_address = items
        ? ir_global_address(r->m, items) : nullptr;
    ir_local_t *view = ir_local_add(function, S("view"),
        ir_type_parse(r->m, S("view[i32]")));
    ir_value_t *view_value = view ? ir_local_value(function, view) : nullptr;
    ir_alias_target_t view_alias = {
        .value = items_address,
        .escapes = true,
    };
    bool view_aliased = view_value && items_address
        && ir_value_set_alias_targets(r->ctx, view_value, &view_alias, 1,
                                      true);
    ir_annot_t *view_target = view_value
        ? ir_annot_get(ir_value_annots(view_value), S("ir.alias0"))
        : nullptr;
    ir_annot_t *view_escape = view_value
        ? ir_annot_get(ir_value_annots(view_value), S("ir.alias0_escapes"))
        : nullptr;
    check("non-pointer view values retain backing-storage provenance",
          view_aliased && view_target
              && view_target->kind == IR_ANNOT_VALUE
              && view_target->v == items_address && view_escape
              && view_escape->kind == IR_ANNOT_BOOL && view_escape->b);
    ir_local_t *projected_view = ir_local_add(function, S("projected_view"),
        ir_type_parse(r->m, S("view[i32]")));
    ir_value_t *projected_view_value = projected_view
        ? ir_local_value(function, projected_view) : nullptr;
    bool copied_view_alias = projected_view_value
        && ir_value_copy_alias_targets(r->ctx, projected_view_value,
                                      view_value);
    ir_annot_t *projected_target = projected_view_value
        ? ir_annot_get(ir_value_annots(projected_view_value), S("ir.alias0"))
        : nullptr;
    ir_annot_t *projected_escape = projected_view_value
        ? ir_annot_get(ir_value_annots(projected_view_value),
                       S("ir.alias0_escapes")) : nullptr;
    check("alias provenance survives value-preserving result projections",
          copied_view_alias && projected_target
              && projected_target->kind == IR_ANNOT_VALUE
              && projected_target->v == items_address && projected_escape
              && projected_escape->kind == IR_ANNOT_BOOL
              && projected_escape->b);
    check("view provenance is not treated as an indirect store address",
          view_value && !ir_value_visit_store_targets(view_value,
              capture_store_target, &capture));
    ir_global_t *other_items = ir_global_add(r->m, S("other_items"),
        ir_type_parse(r->m, S("array[i32]")));
    ir_value_t *other_items_address = other_items
        ? ir_global_address(r->m, other_items) : nullptr;
    bool appended_alias = other_items_address
        && ir_value_add_alias_target(r->ctx, view_value,
            (ir_alias_target_t){.value = other_items_address});
    bool duplicate_alias = appended_alias
        && ir_value_add_alias_target(r->ctx, view_value,
            (ir_alias_target_t){.value = other_items_address,
                                .escapes = true});
    ir_annot_t *alias_count = view_value
        ? ir_annot_get(ir_value_annots(view_value), S("ir.alias_count"))
        : nullptr;
    ir_annot_t *second_alias = view_value
        ? ir_annot_get(ir_value_annots(view_value), S("ir.alias1")) : nullptr;
    ir_annot_t *second_escape = view_value
        ? ir_annot_get(ir_value_annots(view_value), S("ir.alias1_escapes"))
        : nullptr;
    ir_annot_t *alias_complete = view_value
        ? ir_annot_get(ir_value_annots(view_value),
                       S("ir.alias_analysis_complete")) : nullptr;
    ir_annot_t *reverse_count = other_items_address
        ? ir_annot_get(ir_value_annots(other_items_address),
                       S("ir.aliased_by_count")) : nullptr;
    check("incremental alias discovery retains distinct targets and reverse edges",
          duplicate_alias && alias_count && alias_count->kind == IR_ANNOT_INT
              && alias_count->i.mag == 2 && second_alias
              && second_alias->kind == IR_ANNOT_VALUE
              && second_alias->v == other_items_address && reverse_count
              && reverse_count->kind == IR_ANNOT_INT
              && reverse_count->i.mag == 1);
    ir_annot_t *reverse_escape = other_items_address
        ? ir_annot_get(ir_value_annots(other_items_address),
                       S("ir.aliased_by0_escapes")) : nullptr;
    check("duplicate alias discovery upgrades escape provenance",
          second_escape && second_escape->kind == IR_ANNOT_BOOL
              && second_escape->b && reverse_escape
              && reverse_escape->kind == IR_ANNOT_BOOL
              && reverse_escape->b && alias_complete
              && alias_complete->kind == IR_ANNOT_BOOL
              && !alias_complete->b);
    bool closed_alias = duplicate_alias
        && ir_value_complete_alias_targets(r->ctx, view_value);
    ir_annot_t *alias_unresolved = view_value
        ? ir_annot_get(ir_value_annots(view_value),
                       S("ir.alias_unresolved")) : nullptr;
    check("incremental alias sets can be closed when all origins are known",
          closed_alias && alias_complete && alias_complete->b
              && alias_unresolved && alias_unresolved->kind == IR_ANNOT_BOOL
              && !alias_unresolved->b);
    done(r);

    r = verify(MOD(
        "  func @f(%address: ptr[i32], %input: i32) -> void {\n"
        "    block entry:\n"
        "      store %address, %input\n"
        "      return\n"
        "    }\n"
        ), IR_PROP_PARSED);
    function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
    entry = function ? ir_block_entry(function) : nullptr;
    store = nullptr;
    for (uint32_t i = 0; entry && i < entry->n_instrs; i++)
        if (entry->instrs[i]->op == IR_OP_STORE) store = entry->instrs[i];
    capture = (store_target_capture_t){0};
    complete = store && ir_value_visit_store_targets(
        ir_operand_value(&store->operands[0]), capture_store_target, &capture);
    check("the store-target visitor reports unresolved pointer targets",
          !complete);
    ir_value_t *formal = function && function->n_params
        ? function->params[0] : nullptr;
    bool unknown_alias = formal
        && ir_value_set_alias_targets(r->ctx, formal, nullptr, 0, false)
        && ir_value_set_indirect_access(r->ctx, formal,
                                        IR_INDIRECT_ACCESS_READ_WRITE);
    capture = (store_target_capture_t){0};
    complete = unknown_alias && ir_value_visit_store_targets(
        formal, capture_store_target, &capture);
    check("a writable formal permits an unknown but incomplete alias set",
          complete && capture.count == 0);
    capture = (store_target_capture_t){0};
    complete = store && ir_value_visit_store_origins(
        ir_operand_value(&store->operands[0]), capture_store_target,
        &capture);
    check("the store-origin visitor retains an unresolved formal parameter",
          complete && capture.count == 1
              && capture.targets[0]->kind == IR_VAL_PARAM
              && capture.targets[0]->index == 0);

    ir_global_t *origin = ir_global_add(r->m, S("origin"),
                                        ir_type_parse(r->m, S("i32")));
    ir_value_t *origin_value = origin ? ir_global_address(r->m, origin)
                                      : nullptr;
    ir_value_t *address = function && function->n_params
        ? function->params[0] : nullptr;
    ir_alias_target_t parameter_alias = {.value = origin_value};
    aliased = address && origin_value
        && ir_value_set_alias_targets(r->ctx, address, &parameter_alias, 1,
                                      true);
    capture = (store_target_capture_t){0};
    complete = aliased && ir_value_visit_store_targets(address,
        capture_store_target, &capture);
    check("the effective store-target visitor follows parameter aliases",
          complete && capture.count == 1
              && capture.targets[0]->kind == IR_VAL_GLOBAL
              && capture.targets[0]->global == origin);
    capture = (store_target_capture_t){0};
    complete = aliased && ir_value_visit_store_origins(address,
        capture_store_target, &capture);
    check("the store-origin visitor keeps formal identity under aliases",
          complete && capture.count == 1
              && capture.targets[0] == address);
    capture = (store_target_capture_t){0};
    complete = origin_value && ir_value_visit_store_origins(origin_value,
        capture_store_target, &capture);
    check("the store-origin visitor accepts direct global bindings",
          complete && capture.count == 1
              && capture.targets[0] == origin_value);
    done(r);
}

static bool configure_indirect_initializations(run_t *r)
{
    ir_func_t *function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
    ir_local_t *local = function ? ir_local_find(function, S("x")) : nullptr;
    if (!local || !ir_value_set_phase_access(r->ctx,
            ir_local_value(function, local), IR_PHASE_ACCESS_BOTH,
            IR_PHASE_ACCESS_NONE, IR_PHASE_ACCESS_RUNTIME)) return false;

    size_t events = 0;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if (instruction->op != IR_OP_STORE) continue;
            if (!ir_instr_set_binding_initialization(instruction, true))
                return false;
            events++;
        }
    }
    return events > 0;
}

static bool set_single_alias_target(ir_ctx_t *ctx, ir_value_t *holder,
                                    ir_value_t *target, bool complete)
{
    ir_annots_t *annots = ir_value_annots(holder);
    return annots && target
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = S("ir.alias_count"), .kind = IR_ANNOT_INT,
               .i = {.mag = 1},
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = S("ir.alias0"), .kind = IR_ANNOT_VALUE, .v = target,
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = S("ir.alias_analysis_complete"), .kind = IR_ANNOT_BOOL,
               .b = complete,
           })
        && ir_annot_set(ctx, annots, (ir_annot_t){
               .key = S("ir.alias_unresolved"), .kind = IR_ANNOT_BOOL,
               .b = !complete,
           });
}

static void test_alias_store_target_phase_access(void)
{
    run_t *r = verify(MOD(
        "  func @f(%p: ref[i32], %input: i32) -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      store %p, %input\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    ir_func_t *function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
    ir_local_t *local = function ? ir_local_find(function, S("x")) : nullptr;
    ir_value_t *target = function && local
        ? ir_local_value(function, local) : nullptr;
    ir_value_t *pointer = nullptr;
    bool configured = target
        && ir_value_set_phase_access(r->ctx, target,
            IR_PHASE_ACCESS_RUNTIME, IR_PHASE_ACCESS_NONE,
            IR_PHASE_ACCESS_NONE)
        && set_single_alias_target(r->ctx,
            function ? ir_value_find(function, S("p")) : nullptr,
            target, true);
    if (configured) {
        ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
        r->held = ir_verify(r->m, r->d);
    }
    check("typed IR applies write permissions through a complete alias target",
          configured && !typed(r) && said(r, S("phase.binding-write")));
    done(r);

    r = verify(MOD(
        "  func @f(%p: ref[i32], %input: i32) -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      store %p, %input\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
    local = function ? ir_local_find(function, S("x")) : nullptr;
    target = function && local ? ir_local_value(function, local) : nullptr;
    configured = target
        && set_single_alias_target(r->ctx,
            function ? ir_value_find(function, S("p")) : nullptr,
            target, false);
    if (configured) {
        ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
        r->held = ir_verify(r->m, r->d);
    }
    check("typed IR rejects stores through an incomplete alias set",
          configured && !typed(r)
              && said(r, S("phase.store-alias-incomplete")));
    done(r);

    r = verify(MOD(
        "  func @f(%p: ref[i32], %input: i32) -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      store %p, %input\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
    local = function ? ir_local_find(function, S("x")) : nullptr;
    target = function && local ? ir_local_value(function, local) : nullptr;
    pointer = function ? ir_value_find(function, S("p")) : nullptr;
    configured = pointer && target
        && set_single_alias_target(r->ctx, pointer, target, false)
        && ir_value_set_indirect_access(r->ctx, pointer,
                                        IR_INDIRECT_ACCESS_READ_WRITE);
    if (configured) {
        ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
        r->held = ir_verify(r->m, r->d);
    }
    check("writable references admit an unresolved external alias",
          configured && typed(r));
    done(r);

    r = verify(MOD(
        "  func @f(%p: ref[i32], %input: i32) -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      store %p, %input\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
    local = function ? ir_local_find(function, S("x")) : nullptr;
    target = function && local ? ir_local_value(function, local) : nullptr;
    pointer = function ? ir_value_find(function, S("p")) : nullptr;
    configured = pointer && target
        && ir_value_set_phase_access(r->ctx, target,
            IR_PHASE_ACCESS_RUNTIME, IR_PHASE_ACCESS_NONE,
            IR_PHASE_ACCESS_NONE)
        && set_single_alias_target(r->ctx, pointer, target, false)
        && ir_value_set_indirect_access(r->ctx, pointer,
                                        IR_INDIRECT_ACCESS_READ_WRITE);
    if (configured) {
        ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
        r->held = ir_verify(r->m, r->d);
    }
    check("writable references still reject known read-only aliases",
          configured && !typed(r) && said(r, S("phase.binding-write")));
    done(r);

    r = verify(MOD(
        "  func @f(%p: ref[i32], %input: i32) -> void {\n"
        "    block entry:\n"
        "      store %p, %input\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    function = r->m ? ir_func_find(r->m, S("f")) : nullptr;
    pointer = function ? ir_value_find(function, S("p")) : nullptr;
    configured = pointer && ir_annot_set(r->ctx,
        ir_value_annots(pointer), (ir_annot_t){
            .key = S("ir.alias_count"), .kind = IR_ANNOT_INT,
            .i = {.mag = UINT32_MAX},
        }) && ir_annot_set(r->ctx, ir_value_annots(pointer),
            (ir_annot_t){.key = S("ir.alias_analysis_complete"),
                         .kind = IR_ANNOT_BOOL, .b = true})
        && ir_annot_set(r->ctx, ir_value_annots(pointer),
            (ir_annot_t){.key = S("ir.alias_unresolved"),
                         .kind = IR_ANNOT_BOOL, .b = false});
    if (configured) {
        ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
        r->held = ir_verify(r->m, r->d);
    }
    check("typed IR bounds malformed alias target counts",
          configured && !typed(r)
              && said(r, S("phase.store-alias-incomplete")));
    done(r);
}

static bool has_multiple_initialization_diagnostic(ir_diags_t *diagnostics)
{
    for (uint32_t i = 0; i < ir_diags_count(diagnostics); i++) {
        const ir_diag_t *diagnostic = ir_diags_at(diagnostics, i);
        if (diagnostic->code == IR_DIAG_VERIFY_PHASE
            && contains(diagnostic->message,
                        S("initializes it more than once"))) return true;
    }
    return false;
}

static void test_indirect_local_initialization(void)
{
    run_t *multiple = verify(MOD(
        "  func @f() -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      %a: ref[i32] = address.of $x\n"
        "      %one: i32 = const 1\n"
        "      store %a, %one\n"
        "      %b: ref[i32] = address.of $x\n"
        "      %two: i32 = const 2\n"
        "      store %b, %two\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    bool configured = configure_indirect_initializations(multiple);
    if (configured) {
        ir_module_establish(multiple->m, IR_PROP_PARSED | IR_PROP_TYPED);
        multiple->held = ir_verify(multiple->m, multiple->d);
    }
    check("typed IR rejects distinct indirect initialization sites on one path",
          configured && !typed(multiple)
              && has_multiple_initialization_diagnostic(multiple->d));
    done(multiple);

    run_t *branches = verify(MOD(
        "  func @f(%condition: bool) -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      branch %condition, left, right\n"
        "    block left:\n"
        "      %a: ref[i32] = address.of $x\n"
        "      %one: i32 = const 1\n"
        "      store %a, %one\n"
        "      jump join\n"
        "    block right:\n"
        "      %b: ref[i32] = address.of $x\n"
        "      %two: i32 = const 2\n"
        "      store %b, %two\n"
        "      jump join\n"
        "    block join:\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    configured = configure_indirect_initializations(branches);
    if (configured) {
        ir_module_establish(branches->m, IR_PROP_PARSED | IR_PROP_TYPED);
        branches->held = ir_verify(branches->m, branches->d);
    }
    check("typed IR permits branch-exclusive indirect initialization sites",
          configured && typed(branches) && !ir_diags_failed(branches->d));
    done(branches);

    run_t *loop = verify(MOD(
        "  func @f(%condition: bool) -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      jump loop\n"
        "    block loop:\n"
        "      %a: ref[i32] = address.of $x\n"
        "      %one: i32 = const 1\n"
        "      store %a, %one\n"
        "      branch %condition, loop, exit\n"
        "    block exit:\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    configured = configure_indirect_initializations(loop);
    if (configured) {
        ir_module_establish(loop->m, IR_PROP_PARSED | IR_PROP_TYPED);
        loop->held = ir_verify(loop->m, loop->d);
    }
    check("typed IR counts repeated execution of one indirect init site once",
          configured && typed(loop) && !ir_diags_failed(loop->d));
    done(loop);

    run_t *aliases = verify(MOD(
        "  func @f(%p: ref[i32], %q: ref[i32]) -> void {\n"
        "    local $x: i32\n"
        "    block entry:\n"
        "      %one: i32 = const 1\n"
        "      store %p, %one\n"
        "      %two: i32 = const 2\n"
        "      store %q, %two\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    ir_func_t *function = aliases->m ? ir_func_find(aliases->m, S("f")) : nullptr;
    ir_local_t *local = function ? ir_local_find(function, S("x")) : nullptr;
    ir_value_t *target = function && local
        ? ir_local_value(function, local) : nullptr;
    configured = function && target
        && set_single_alias_target(aliases->ctx,
                                   ir_value_find(function, S("p")), target, true)
        && set_single_alias_target(aliases->ctx,
                                   ir_value_find(function, S("q")), target, true)
        && configure_indirect_initializations(aliases);
    if (configured) {
        ir_module_establish(aliases->m, IR_PROP_PARSED | IR_PROP_TYPED);
        aliases->held = ir_verify(aliases->m, aliases->d);
    }
    check("typed IR counts complete alias targets in initialization dataflow",
          configured && !typed(aliases)
              && has_multiple_initialization_diagnostic(aliases->d));
    done(aliases);

    run_t *allocated = verify(MOD(
        "  func @f() -> void {\n"
        "    block entry:\n"
        "      %address: ref[i32] = alloc i32\n"
        "      %one: i32 = const 1\n"
        "      store %address, %one\n"
        "      %two: i32 = const 2\n"
        "      store %address, %two\n"
        "      return\n"
        "  }\n"), IR_PROP_PARSED);
    function = allocated->m ? ir_func_find(allocated->m, S("f")) : nullptr;
    ir_block_t *entry = function ? ir_block_entry(function) : nullptr;
    ir_instr_t *allocation = nullptr;
    size_t stores = 0;
    configured = true;
    for (uint32_t i = 0; entry && i < entry->n_instrs; i++) {
        if (entry->instrs[i]->op == IR_OP_ALLOC)
            allocation = entry->instrs[i];
        if (entry->instrs[i]->op == IR_OP_STORE) {
            stores++;
            configured = configured && ir_instr_set_binding_initialization(
                entry->instrs[i], true);
        }
    }
    configured = allocation && allocation->dest && stores == 2
        && configured && ir_value_set_phase_access(allocated->ctx,
            allocation->dest, IR_PHASE_ACCESS_BOTH,
            IR_PHASE_ACCESS_NONE, IR_PHASE_ACCESS_RUNTIME);
    if (configured) {
        ir_module_establish(allocated->m, IR_PROP_PARSED | IR_PROP_TYPED);
        allocated->held = ir_verify(allocated->m, allocated->d);
    }
    check("typed IR enforces single initialization for allocated bindings",
          configured && !typed(allocated)
              && has_multiple_initialization_diagnostic(allocated->d));
    done(allocated);
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
    bool configured = global && function && entry
        && ir_global_set_phase_access(r->ctx, global,
            IR_PHASE_ACCESS_RUNTIME, IR_PHASE_ACCESS_NONE,
            IR_PHASE_ACCESS_RUNTIME)
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
        bool configured = binding && header
            && ir_value_set_phase_access(r->ctx, binding,
                IR_PHASE_ACCESS_BOTH, IR_PHASE_ACCESS_NONE,
                IR_PHASE_ACCESS_RUNTIME);
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
    bool configured = binding && header
        && ir_value_set_phase_access(same_iteration->ctx, binding,
            IR_PHASE_ACCESS_BOTH, IR_PHASE_ACCESS_NONE,
            IR_PHASE_ACCESS_RUNTIME)
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
        && ir_value_set_phase_access(ctx, ir_local_value(function, local),
            IR_PHASE_ACCESS_BOTH, IR_PHASE_ACCESS_NONE,
            IR_PHASE_ACCESS_RUNTIME)
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
    setup = local && copy
        && ir_value_set_phase_access(ctx, ir_local_value(function, local),
            IR_PHASE_ACCESS_BOTH, IR_PHASE_ACCESS_NONE,
            IR_PHASE_ACCESS_RUNTIME);
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
            duplicate_diagnostic |= diagnostic->code == IR_DIAG_VERIFY_PHASE
                && contains(diagnostic->message,
                            S("initializes it more than once"));
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
    setup = local && ir_value_set_phase_access(ctx,
        ir_local_value(function, local), IR_PHASE_ACCESS_BOTH,
        IR_PHASE_ACCESS_NONE, IR_PHASE_ACCESS_RUNTIME);
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
    setup = local && copy
        && ir_value_set_phase_access(ctx, ir_local_value(function, local),
            IR_PHASE_ACCESS_BOTH, IR_PHASE_ACCESS_NONE,
            IR_PHASE_ACCESS_RUNTIME)
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
    test_typed_rules();
    test_ssa_rules();
    test_extension_opcodes();
    test_parsed_rules_the_builder_prevents();
    test_value_references_resolve();
    test_entry_block_rules();
    test_phi_names_its_predecessors();
    test_values_carry_types();
    test_dominators();
    test_no_rule_resolves();
    test_origins();
    test_annotations();
    test_annotation_invalidation();
    test_a_pass_leaves_foreign_annotations_alone();
    test_passes();
    test_undeclared_invalidation();
    test_spanless_diagnostic_is_a_bug();
    test_address_of_local();
    test_release_liveness();
    test_global_phase_access();
    test_binding_phase_access();
    test_store_through_address_phase_access();
    test_store_through_forwarded_addresses();
    test_store_target_visitor();
    test_alias_store_target_phase_access();
    test_indirect_local_initialization();
    test_global_single_initialization();
    test_loop_binding_reentry();
    test_local_phase_writes();
    test_global_storage_requirement();
    test_allocation_storage_requirement();
    test_a_phase_can_be_asked_for_on_its_own();

    dump_produced_codes("produced-codes-verify.txt", produced);

    return report("ir verify");
}
