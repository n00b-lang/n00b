#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    char        *report;
} run_t;

static const ir_profile_t *const PROFILES[] = {&ir_profile_bce,
                                               &ir_profile_synth};

static const char *body(void *u, int32_t file, size_t *n)
{
    (void)file;
    *n = strlen((const char *)u);

    return (const char *)u;
}

static run_t *verify(const char *src, ir_props_t claim)
{
    run_t *r = calloc(1, sizeof(run_t));

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
                      .state  = (void *)src};

    r->report = ir_diags_render_text(r->d, &rd);

    return r;
}

static void done(run_t *r)
{
    free(r->report);
    ir_ctx_free(r->ctx);
    free(r);
}

static bool told(ir_diags_t *d, const char *text)
{
    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        const ir_diag_t *dg = ir_diags_at(d, i);

        if ((dg->primary.label && strstr(dg->primary.label, text))
            || (dg->message && strstr(dg->message, text))) {
            return true;
        }
    }

    return false;
}

static bool said(run_t *r, const char *rule)
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

        if (!ir_span_known(dg->primary.span) || !dg->primary.label
            || !dg->message) {
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

#define MOD(...) "module m {\n  profile bce\n  target t\n\n" __VA_ARGS__ "}\n"

static const char *CLEAN = MOD(
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
    check("and reports nothing", ok->report && !strstr(ok->report, "error"));
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
          nonssa->report && !strstr(nonssa->report, "error"));
    done(nonssa);
}

static void test_failure_channel_ops(void)
{
    run_t *valid = verify(MOD(
        "  func @f(%e: i32) -> i32 fail: i32 {\n"
        "    block entry:\n      fail %e\n  }\n"),
        IR_PROP_PARSED | IR_PROP_TYPED);
    check("a failure terminator matches the function failure channel",
          parsed(valid) && typed(valid) && !strstr(valid->report, "error"));
    done(valid);

    run_t *wrong_type = verify(MOD(
        "  func @f(%e: i32) -> i32 fail: bool {\n"
        "    block entry:\n      fail %e\n  }\n"),
        IR_PROP_PARSED | IR_PROP_TYPED);
    check("a failure terminator rejects a mismatched failure value",
          parsed(wrong_type) && !typed(wrong_type)
              && said(wrong_type, "failure value"));
    done(wrong_type);

    run_t *no_channel = verify(MOD(
        "  func @f(%e: i32) -> i32 {\n"
        "    block entry:\n      fail %e\n  }\n"),
        IR_PROP_PARSED | IR_PROP_TYPED);
    check("a failure terminator requires a function failure channel",
          parsed(no_channel) && !typed(no_channel)
              && said(no_channel, "failure exit"));
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
              && said(unhandled_call, "error channel"));
    done(unhandled_call);
}

static void test_parsed_rules(void)
{
    struct {
        const char *what;
        const char *rule;
        const char *src;
    } cases[] = {
        {"a block with two terminators", "parsed.terminator",
         MOD("  func @f() -> void {\n    block entry:\n"
             "      unreachable\n      unreachable\n  }\n")},
        {"a block with no terminator", "parsed.terminator",
         MOD("  func @f(%a: i32) -> void {\n    block entry:\n"
             "      %s: i32 = add wrap %a, %a\n  }\n")},
        {"a terminator that is not last", "parsed.terminator",
         MOD("  func @f(%a: i32) -> void {\n    block entry:\n"
             "      unreachable\n      %s: i32 = add wrap %a, %a\n  }\n")},
        {"a value defined twice", "parsed.one-def",
         MOD("  func @f(%a: i32) -> void {\n    block entry:\n"
             "      %s: i32 = add wrap %a, %a\n"
             "      %s: i32 = add wrap %a, %a\n      unreachable\n  }\n")},
        {"a destination that shadows a parameter", "parsed.one-def",
         MOD("  func @f(%a: i32) -> void {\n    block entry:\n"
             "      %a: i32 = add wrap %a, %a\n      unreachable\n  }\n")},
        {"a phi after a non-phi", "parsed.phi.leading",
         MOD("  func @f(%a: i32, %c: bool) -> void {\n    block entry:\n"
             "      branch %c, l, l\n    block l:\n"
             "      %y: i32 = add wrap %a, %a\n"
             "      %p: i32 = phi [entry: %a]\n      unreachable\n  }\n")},
        {"a phi that misses a predecessor", "parsed.phi.arms",
         MOD("  func @f(%a: i32, %c: bool) -> void {\n    block entry:\n"
             "      branch %c, l, r\n    block l:\n      jump r\n"
             "    block r:\n      %p: i32 = phi [entry: %a]\n"
             "      unreachable\n  }\n")},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_t *r = verify(cases[i].src, IR_PROP_PARSED);

        if (parsed(r) || !said(r, cases[i].rule)) {
            printf("FAIL  %s is caught and names %s\n", cases[i].what,
                   cases[i].rule);
            fails++;
        }

        if (!every_diag_is_placed_and_named(r->d)) {
            printf("FAIL  %s is reported with a location and a rule\n",
                   cases[i].what);
            fails++;
        }
        done(r);
    }
}

static void test_typed_rules(void)
{
    struct {
        const char *what;
        const char *rule;
        const char *src;
    } cases[] = {
        {"an instruction that breaks its rule", "arithmetic operands",
         MOD("  func @f(%a: i32, %b: u64) -> void {\n    block entry:\n"
             "      %s: i32 = add wrap %a, %b\n      unreachable\n  }\n")},
        {"a member name that does not resolve", "has no member",
         MOD("  func @f(%p: ptr[(x: i32)]) -> void {\n    block entry:\n"
             "      %q: ptr[i32] = field %p, \"nope\"\n      unreachable\n  }\n")},
        {"a call whose argument is wrong", "and the argument is",
         MOD("  func @g(%x: i32) -> void {\n    block entry:\n"
             "      unreachable\n  }\n"
             "  func @f(%a: u64) -> void {\n    block entry:\n"
             "      %r: void = call @g, %a\n      unreachable\n  }\n")},
        {"a return that does not match", "returns i32, and this value is",
         MOD("  func @f(%a: u64) -> i32 {\n    block entry:\n"
             "      return %a\n  }\n")},
        {"a switch on a non-integer", "scrutinee",
         MOD("  func @f(%a: f64) -> void {\n    block entry:\n"
             "      switch %a, default: out\n    block out:\n"
             "      unreachable\n  }\n")},
        {"an opaque named type used as a scalar", "arithmetic needs",
         MOD("  func @f(%a: Widget) -> void {\n    block entry:\n"
             "      %s: Widget = add wrap %a, %a\n      unreachable\n  }\n")},
        {"a constant that does not fit", "does not fit in",
         MOD("  func @f() -> void {\n    block entry:\n"
             "      %k: i8 = const 300\n      unreachable\n  }\n")},
        {"an extract name that does not resolve", "has no member",
         MOD("  func @f(%v: (x: i32)) -> void {\n    block entry:\n"
             "      %q: i32 = extract %v, \"nope\"\n"
             "      unreachable\n  }\n")},
        {"a case value the scrutinee cannot hold", "not representable in i8",
         MOD("  func @f(%a: i8) -> void {\n    block entry:\n"
             "      switch %a, default: out, [300: out]\n"
             "    block out:\n      unreachable\n  }\n")},
        {"a duplicated switch case", "two cases both match",
         MOD("  func @f(%a: i32) -> void {\n    block entry:\n"
             "      switch %a, default: out, [1: out], [1: out]\n"
             "    block out:\n      unreachable\n  }\n")},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_t *r = verify(cases[i].src, IR_PROP_PARSED | IR_PROP_TYPED);

        if (typed(r) || !said(r, cases[i].rule)) {
            printf("FAIL  %s is caught and says %s\n", cases[i].what,
                   cases[i].rule);
            fails++;
        }

        if (!parsed(r)) {
            printf("FAIL  %s is still structurally well formed\n",
                   cases[i].what);
            fails++;
        }

        if (!every_diag_is_placed_and_named(r->d)) {
            printf("FAIL  %s is reported with a location and a rule\n",
                   cases[i].what);
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
          !ssa(loc) && said(loc, "ssa.locals"));
    check("and the false claim is reported as such",
          said(loc, "claims ssa, which the verifier finds does not hold"));
    done(loc);

    run_t *dom = verify(MOD("  func @f(%c: bool) -> i32 {\n    block entry:\n"
                            "      branch %c, l, r\n    block l:\n"
                            "      %x: i32 = const 1\n      jump r\n"
                            "    block r:\n      return %x\n  }\n"),
                        IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);

    check("a use its definition does not dominate is caught",
          !ssa(dom) && said(dom, "ssa.dominance"));
    done(dom);

    run_t *order = verify(MOD("  func @f(%a: i32) -> i32 {\n    block entry:\n"
                              "      %y: i32 = add wrap %x, %a\n"
                              "      %x: i32 = const 1\n      return %y\n  }\n"),
                          IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);

    check("a use before its definition in one block is caught",
          !ssa(order) && said(order, "ssa.dominance"));
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
          !typed(bad) && said(bad, "incoming value"));
    done(bad);
}

static void test_extension_opcodes(void)
{
    const char *src = "module m {\n  profile synth\n  target t\n\n"
                      "  func @f(%a: str, %b: str) -> str {\n"
                      "    block entry:\n"
                      "      %s: str = synth.concat %a, %b\n"
                      "      return %s\n  }\n}\n";

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
        char       *rp = ir_diags_render_text(d, &rd);

        check("rebound to a profile without it, the module is still parsed",
              (held & IR_PROP_PARSED) != 0);
        check("but is no longer typed", !(held & IR_PROP_TYPED));
        check("and it is reported as a missing profile, not an unknown opcode",
              told(d, "not one profile bce supplies"));
        free(rp);
    }
    ir_ctx_free(ctx);
}

static ir_module_t *built(ir_ctx_t *ctx, ir_func_t **out)
{
    ir_module_t *m = ir_module_new(ctx, "m", &ir_profile_bce, "t");
    ir_func_t   *f = ir_func_add(m, "f", ir_type_parse(m, "() -> void"),
                                 IR_LINK_EXTERNAL);
    ir_block_t  *b = ir_block_add(f, "entry");
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
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "t");

    ir_module_set_span(m, ir_span_make(0, 0, 6, 1, 1, 1, 7));

    ir_func_t  *f  = ir_func_add(m, "f", ir_type_parse(m, "(i32) -> void"),
                                 IR_LINK_EXTERNAL);
    ir_value_t *a  = ir_param_add(f, "a", ir_type_parse(m, "i32"));
    ir_block_t *b  = ir_block_add(f, "entry");
    ir_func_t  *g  = ir_func_add(m, "g", ir_type_parse(m, "() -> void"),
                                 IR_LINK_EXTERNAL);
    ir_block_t *gb = ir_block_add(g, "elsewhere");

    ir_instr_add(gb, IR_OP_UNREACHABLE);

    ir_instr_t *few = ir_instr_add(b, IR_OP_ADD);

    ir_instr_set_dest(few, "s", ir_type_parse(m, "i32"));
    ir_instr_add_mod(few, "wrap");
    ir_instr_operand_value(few, a);

    ir_instr_t *kind = ir_instr_add(b, IR_OP_ADD);

    ir_instr_set_dest(kind, "k", ir_type_parse(m, "i32"));
    ir_instr_add_mod(kind, "wrap");

    check("the builder refuses an operand of the wrong kind",
          !ir_instr_operand_type(kind, ir_type_parse(m, "i32")));
    ir_instr_operand_value(kind, a);
    ir_instr_operand_value(kind, a);
    kind->operands[0].kind = IR_OPD_TYPE;
    kind->operands[0].type = ir_type_parse(m, "i32");

    ir_instr_t *away = ir_instr_add(b, IR_OP_JUMP);

    ir_instr_operand_block(away, gb);

    ir_instr_t *ld = ir_instr_add(b, IR_OP_LOAD);

    check("the builder refuses a modifier the opcode does not take",
          !ir_instr_add_mod(ld, "sat"));

    ld->mods   = ir_alloc(ctx, sizeof(char *));
    ld->mods[0] = "sat";
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
          told(d, "takes 2 to 2 operands and has 1"));
    check("an operand of the wrong kind is caught",
          told(d, "is a type where the table says value"));
    check("a branch to a block of another function is caught",
          told(d, "parsed.block-ref"));
    check("a modifier the opcode does not take is caught",
          told(d, "does not take the modifier sat"));
    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        const ir_diag_t *dg = ir_diags_at(d, i);

        if (!ir_span_known(dg->primary.span) || !dg->primary.label) {
            printf("FAIL  unplaced or unnamed: %s / %s\n",
                   ir_diag_code(dg->code),
                   dg->message ? dg->message : "(no message)");
            fails++;
        }
    }
    ir_ctx_free(ctx);
}

static void test_value_references_resolve(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "t");

    ir_module_set_span(m, ir_span_make(0, 0, 6, 1, 1, 1, 7));

    ir_func_t  *f = ir_func_add(m, "f", ir_type_parse(m, "() -> void"),
                                IR_LINK_EXTERNAL);
    ir_func_t  *g = ir_func_add(m, "g", ir_type_parse(m, "(i32) -> void"),
                                IR_LINK_EXTERNAL);
    ir_value_t *a = ir_param_add(g, "a", ir_type_parse(m, "i32"));
    ir_block_t *b = ir_block_add(f, "entry");
    ir_instr_t *c = ir_instr_add(b, IR_OP_COPY);

    ir_block_add(g, "entry");
    ir_instr_set_dest(c, "s", ir_type_parse(m, "i32"));
    ir_instr_operand_value(c, a);
    ir_instr_add(b, IR_OP_UNREACHABLE);

    ir_diags_t *d = ir_diags_new(ctx, nullptr);

    ir_module_establish(m, IR_PROP_PARSED);

    ir_props_t held = ir_verify(m, d);

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        produced[ir_diags_at(d, i)->code] = true;
    }

    check("a use of a value belonging to another function is caught",
          !(held & IR_PROP_PARSED) && told(d, "parsed.value-ref"));
    check("and is placed and named", every_diag_is_placed_and_named(d));
    ir_ctx_free(ctx);
}

static void test_entry_block_rules(void)
{
    run_t *r = verify(MOD("  func @f(%c: bool) -> void {\n"
                          "    block entry:\n"
                          "      branch %c, entry, out\n"
                          "    block out:\n      unreachable\n  }\n"),
                      IR_PROP_PARSED);

    check("a branch back to entry gives it a predecessor, which is caught",
          !parsed(r) && said(r, "parsed.entry"));
    done(r);

    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "t");
    ir_func_t   *f   = ir_func_add(m, "f", ir_type_parse(m, "() -> void"),
                                   IR_LINK_EXTERNAL);
    ir_block_t  *e   = ir_block_add(f, "entry");

    ir_block_add(f, "other");

    check("the entry block is the first block by construction, so there is "
          "nothing to verify", ir_block_entry(f) == f->blocks[0]
                                   && ir_block_entry(f) == e);
    ir_ctx_free(ctx);
}

static void test_phi_names_its_predecessors(void)
{
    const char *twice = MOD("  func @f(%a: i32, %c: bool) -> void {\n"
                            "    block entry:\n      branch %c, l, l\n"
                            "    block l:\n"
                            "      %p: i32 = phi [entry: %a], [entry: %a]\n"
                            "      unreachable\n  }\n");
    const char *stranger = MOD("  func @f(%a: i32, %c: bool) -> void {\n"
                               "    block entry:\n      branch %c, l, r\n"
                               "    block l:\n      jump r\n"
                               "    block r:\n"
                               "      %p: i32 = phi [entry: %a], [l: %a], "
                               "[r: %a]\n      unreachable\n  }\n");

    run_t *t = verify(twice, IR_PROP_PARSED);

    check("a phi that names one predecessor twice is caught",
          !parsed(t) && said(t, "parsed.phi.arms"));
    done(t);

    run_t *sx = verify(stranger, IR_PROP_PARSED);

    check("a phi that names a block that is not a predecessor is caught",
          !parsed(sx) && said(sx, "parsed.phi.arms"));
    done(sx);
}

static void test_values_carry_types(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "t");

    ir_module_set_span(m, ir_span_make(0, 0, 6, 1, 1, 1, 7));

    ir_func_t  *f = ir_func_add(m, "f", ir_type_parse(m, "() -> void"),
                                IR_LINK_EXTERNAL);
    ir_block_t *b = ir_block_add(f, "entry");

    ir_instr_add(b, IR_OP_UNREACHABLE);

    ir_value_t *p = ir_param_add(f, "a", ir_type_parse(m, "i32"));

    p->type = nullptr;

    ir_diags_t *d = ir_diags_new(ctx, nullptr);

    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED);

    ir_props_t held = ir_verify(m, d);

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        produced[ir_diags_at(d, i)->code] = true;
    }

    check("a value with no type is caught",
          !(held & IR_PROP_TYPED) && told(d, "has no type"));
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
    ir_block_t *e    = ir_block_find(f, "entry");
    ir_block_t *l    = ir_block_find(f, "l");
    ir_block_t *r    = ir_block_find(f, "r");
    ir_block_t *j    = ir_block_find(f, "join");
    ir_block_t *orph = ir_block_find(f, "orphan");

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
          told(d, "no typing rule resolves for unreachable"));
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
          ir_origin_new(ctx, IR_ORIGIN_INLINED_FROM, in->span, nullptr,
                        nullptr) == nullptr);
    check("a SOURCE origin needs no agent",
          ir_origin_new(ctx, IR_ORIGIN_SOURCE, in->span, nullptr, nullptr)
              != nullptr);

    ir_module_establish(m, IR_PROP_PARSED);

    ir_origin_t *a = ir_origin_new(ctx, IR_ORIGIN_INLINED_FROM, in->span, "p",
                                   nullptr);
    ir_origin_t *b = ir_origin_new(ctx, IR_ORIGIN_INLINED_FROM, in->span, "p",
                                   a);

    a->parent  = b;
    in->origin = a;

    ir_diags_t *d2 = ir_diags_new(ctx, nullptr);
    ir_props_t  h2 = ir_verify(m, d2);

    for (uint32_t i = 0; i < ir_diags_count(d2); i++) {
        produced[ir_diags_at(d2, i)->code] = true;
    }

    check("an origin chain that loops is caught rather than followed forever",
          !(h2 & IR_PROP_PARSED) && told(d2, "origin.acyclic"));
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
                 (ir_annot_t){.key = "bare", .kind = IR_ANNOT_BOOL, .b = true});
    ir_module_establish(m, IR_PROP_PARSED);

    ir_props_t held = ir_verify(m, d);

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        produced[ir_diags_at(d, i)->code] = true;
    }

    ir_render_t rd = {.module = m};
    char       *rp = ir_diags_render_text(d, &rd);

    check("an annotation key with no namespace is caught",
          !(held & IR_PROP_PARSED) && told(d, "annot.namespace"));
    free(rp);
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
                     (ir_annot_t){.key          = "ir.mark",
                                  .kind         = IR_ANNOT_BOOL,
                                  .b            = true,
                                  .derived_from = IR_PROP_TYPED});
        ir_annot_set(ctx, maps[i],
                     (ir_annot_t){.key  = "bce.kept",
                                  .kind = IR_ANNOT_BOOL,
                                  .b    = true});
    }

    for (size_t i = 0; i < 4; i++) {
        ir_annot_invalidate(maps[i], IR_PROP_TYPED);
    }

    bool dropped = true;
    bool kept    = true;

    for (size_t i = 0; i < 4; i++) {
        dropped = dropped && !ir_annot_get(maps[i], "ir.mark");
        kept    = kept && ir_annot_get(maps[i], "bce.kept");
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

    ir_local_add(ir_func_at(m, 0), "t", ir_type_parse(m, "i32"));

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
                 (ir_annot_t){.key  = "bce.mine",
                              .kind = IR_ANNOT_STR,
                              .s    = "keep me"});
    ir_annot_set(ctx, &f->annots,
                 (ir_annot_t){.key  = "bce.mine",
                              .kind = IR_ANNOT_STR,
                              .s    = "keep me"});
    ir_module_establish(m, IR_PROP_PARSED);

    ir_pass_t edits = {.name        = "edits",
                       .requires    = IR_PROP_PARSED,
                       .invalidates = IR_PROP_SSA,
                       .run         = a_pass_that_edits};

    check("a pass that does not know a namespace still runs",
          ir_pass_run(&edits, m, nullptr, nullptr, nullptr) == IR_PASS_OK);

    ir_annot_t *kept  = ir_annot_get(&in->annots, "bce.mine");
    ir_annot_t *kept2 = ir_annot_get(&f->annots, "bce.mine");

    check("and leaves an annotation in a namespace it does not understand",
          kept && kept->s && !strcmp(kept->s, "keep me") && kept2);
    ir_ctx_free(ctx);
}

static void test_passes(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
    ir_func_t   *f   = nullptr;
    ir_module_t *m   = built(ctx, &f);

    ir_module_establish(m, IR_PROP_PARSED);

    ir_pass_t losing = {.name        = "loses-locations",
                        .requires    = IR_PROP_PARSED,
                        .run         = drop_spans};
    ir_pass_t honest = {.name        = "keeps-locations",
                        .requires    = IR_PROP_PARSED,
                        .run         = keep_spans};

    check("a pass that keeps its locations runs",
          ir_pass_run(&honest, m, nullptr, nullptr, d) == IR_PASS_OK);

    check("a pass that discards a known location fails",
          ir_pass_run(&losing, m, nullptr, nullptr, d)
              == IR_PASS_FAILED);

    ir_pass_t mute = {.name     = "gives-up",
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
          told(d, "gives-up failed and said nothing"));

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        produced[ir_diags_at(d, i)->code] = true;
    }

    ir_render_t rd = {.module = m};
    char       *rp = ir_diags_render_text(d, &rd);

    check("and says what it lost",
          told(d, "had a known location before loses-locations ran"));
    free(rp);
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

    ir_pass_t sneaky = {.name     = "adds-a-local",
                        .requires = IR_PROP_PARSED,
                        .run      = add_a_local};

    ir_pass_t honest = {.name     = "does-nothing",
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
    char       *rp = ir_diags_render_text(d, &rd);

    check("a pass that edited without declaring invalidation is caught",
          told(d, "claims ssa, which the verifier finds does not hold"));
    free(rp);
    ir_ctx_free(ctx);
}

static void test_spanless_diagnostic_is_a_bug(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
    ir_func_t   *f   = nullptr;
    ir_module_t *m   = built(ctx, &f);

    ir_diag_begin(d, IR_DIAG_INTERNAL, IR_SPAN_UNKNOWN, nullptr);
    ir_verify(m, d);

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        produced[ir_diags_at(d, i)->code] = true;
    }

    ir_render_t rd = {.module = m};
    char       *rp = ir_diags_render_text(d, &rd);

    check("a diagnostic reported with no location is itself reported as a bug",
          told(d, "no primary span"));
    free(rp);
    ir_ctx_free(ctx);
}

static void test_locals_have_no_address(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "t");
    ir_func_t   *f   = ir_func_add(m, "f", ir_type_parse(m, "() -> void"),
                                   IR_LINK_EXTERNAL);
    ir_block_t  *b   = ir_block_add(f, "entry");
    ir_local_t  *l   = ir_local_add(f, "t", ir_type_parse(m, "i32"));
    ir_instr_t  *ld  = ir_instr_add(b, IR_OP_LOAD);

    ir_instr_operand_local(ld, l);

    ir_diags_t *d = ir_diags_new(ctx, nullptr);
    ir_rules_t *rs = ir_module_rules(m);

    check("a local read has the local's type, not a pointer to it",
          ir_rule_apply(m, rs, ld, d).verdict == IR_RULE_REJECT);

    bool takes_address = false;

    for (uint16_t i = 0; i < IR_OP_COUNT; i++) {
        const char *sp = ir_op_spelling((ir_op_t)i);

        takes_address = takes_address
                     || (sp && (strstr(sp, "addrof") || strstr(sp, "addr_of")
                                || strstr(sp, "lea")));
    }

    check("and no opcode takes the address of anything", !takes_address);
    ir_ctx_free(ctx);
}


static void test_a_phase_can_be_asked_for_on_its_own(void)
{
    const char *src = MOD("  func @f(%a: i32) -> i32 {\n"
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
          !(ssa & IR_PROP_SSA) && told(sd, "ssa.locals"));
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
    test_locals_have_no_address();
    test_a_phase_can_be_asked_for_on_its_own();

    dump_produced_codes("produced-codes-verify.txt", produced);

    return report("ir verify");
}
