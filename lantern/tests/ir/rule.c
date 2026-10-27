#include <stdio.h>
#include <stdlib.h>

#include "ir/rule.h"
#include "ir/text.h"
#include "lib/buffer.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static const core_string_t I32 = S("i32");
static const core_string_t U64 = S("u64");

typedef struct {
    ir_ctx_t    *ctx;
    ir_module_t *m;
    ir_func_t   *f;
    ir_block_t  *b;
    ir_diags_t  *d;
} fix;

static bool has(core_string_t hay, core_string_t needle)
{
    return hay.data && core_string_find(hay, needle) >= 0;
}

static fix setup(const ir_profile_t *p, core_string_t scalar)
{
    fix x = {0};

    x.ctx = ir_ctx_new();
    x.m   = ir_module_new(x.ctx, S("m"), p, S("test"));
    x.d   = ir_diags_new(x.ctx, nullptr);

    core_buffer_t sig = {0};

    core_buffer_printf(&sig, "(%.*s) -> %.*s", (int)scalar.u8_bytes,
                       scalar.data, (int)scalar.u8_bytes, scalar.data);
    x.f = ir_func_add(x.m, S("f"), ir_type_parse(x.m, core_buffer_string(&sig)),
                      IR_LINK_EXTERNAL);
    x.b = ir_block_add(x.f, S("entry"));
    core_buffer_release(&sig);

    return x;
}

static ir_instr_t *instr(fix *x, ir_op_t op, core_string_t type_text, int n)
{
    ir_instr_t *in = ir_instr_add(x->b, op);
    th_type    *t  = ir_type_parse(x->m, type_text);
    ir_value_t *v  = ir_param_add(x->f, S("a"), t);

    for (int i = 0; i < n; i++) {
        ir_instr_operand_value(in, v);
    }

    return in;
}

static ir_rule_result_t accept_named(ir_module_t *m, ir_instr_t *in,
                                     ir_diags_t *d, void *state)
{
    (void)in;
    (void)d;

    return (ir_rule_result_t){
        .verdict = IR_RULE_ACCEPT,
        .result  = ir_type_parse(m, *(const core_string_t *)state)};
}

static ir_rule_result_t always_decline(ir_module_t *m, ir_instr_t *in,
                                       ir_diags_t *d, void *state)
{
    (void)m;
    (void)in;
    (void)d;
    (void)state;

    return (ir_rule_result_t){.verdict = IR_RULE_DECLINE};
}

static ir_rule_result_t reject(ir_module_t *m, ir_instr_t *in, ir_diags_t *d,
                               void *state)
{
    (void)m;
    (void)state;

    if (d) {
        ir_diag_begin_at(d, IR_DIAG_INTERNAL, in, nullptr, nullptr,
                         IR_DETAIL(S("rejected")));
    }

    return (ir_rule_result_t){.verdict = IR_RULE_REJECT};
}

static int lowered;

static bool note_lowering(ir_module_t *m, ir_instr_t *in, ir_diags_t *d,
                          void *state)
{
    (void)m;
    (void)in;
    (void)d;
    (void)state;
    lowered++;

    return true;
}

static bool rule_named(const ir_annot_t *a, core_string_t name)
{
    return a && core_string_eq(a->s, name);
}

static void test_ladder(void)
{
    fix x = setup(&ir_profile_bce, S("i32"));

    ir_rules_t *rs = ir_rules_new(x.ctx);

    ir_rules_add(rs, (ir_rule_t){.name = S("by-class"), .op = IR_OP_ADD,
                                 .match = IR_MATCH_CLASS,
                                 .class = IR_CLASS_SINT,
                                 .type_rule = accept_named,
                                 .state = (void *)&I32});
    ir_rules_add(rs, (ir_rule_t){.name = S("by-kind"), .op = IR_OP_ADD,
                                 .match = IR_MATCH_KIND, .kind = TH_NAMED,
                                 .type_rule = accept_named,
                                 .state = (void *)&I32});
    ir_rules_add(rs, (ir_rule_t){.name = S("by-default"), .op = IR_OP_ADD,
                                 .match = IR_MATCH_DEFAULT,
                                 .type_rule = accept_named,
                                 .state = (void *)&I32});
    ir_rules_add(rs, (ir_rule_t){.name = S("by-ctor"), .op = IR_OP_ADD,
                                 .match = IR_MATCH_CONSTRUCTOR,
                                 .type_text = S("i32"),
                                 .type_rule = accept_named,
                                 .state = (void *)&I32});
    ir_rules_add(rs, (ir_rule_t){.name = S("by-exact"), .op = IR_OP_ADD,
                                 .match = IR_MATCH_EXACT,
                                 .type_text = S("i32"),
                                 .type_rule = accept_named,
                                 .state = (void *)&I32});

    check("all five rungs registered", ir_rules_count(rs) == 5);

    ir_instr_t *in = instr(&x, IR_OP_ADD, S("i32"), 2);
    ir_match_t  how;

    const ir_rule_t *r = ir_rule_resolve(x.m, rs, in, &how);

    check("the exact rung wins over every other",
          r && core_string_eq(r->name, S("by-exact"))
              && how == IR_MATCH_EXACT);

    ir_rules_t *rs2 = ir_rules_new(x.ctx);

    ir_rules_add(rs2, (ir_rule_t){.name = S("by-class"), .op = IR_OP_ADD,
                                  .match = IR_MATCH_CLASS,
                                  .class = IR_CLASS_SINT,
                                  .type_rule = accept_named,
                                  .state = (void *)&I32});
    ir_rules_add(rs2, (ir_rule_t){.name = S("by-kind"), .op = IR_OP_ADD,
                                  .match = IR_MATCH_KIND, .kind = TH_NAMED,
                                  .type_rule = accept_named,
                                  .state = (void *)&I32});
    ir_rules_add(rs2, (ir_rule_t){.name = S("by-ctor"), .op = IR_OP_ADD,
                                  .match = IR_MATCH_CONSTRUCTOR,
                                  .type_text = S("i32"),
                                  .type_rule = accept_named,
                                  .state = (void *)&I32});

    r = ir_rule_resolve(x.m, rs2, in, &how);
    check("with no exact rule the constructor rung wins",
          r && core_string_eq(r->name, S("by-ctor"))
              && how == IR_MATCH_CONSTRUCTOR);

    ir_rules_t *rs3 = ir_rules_new(x.ctx);

    ir_rules_add(rs3, (ir_rule_t){.name = S("by-class"), .op = IR_OP_ADD,
                                  .match = IR_MATCH_CLASS,
                                  .class = IR_CLASS_SINT,
                                  .type_rule = accept_named,
                                  .state = (void *)&I32});
    ir_rules_add(rs3, (ir_rule_t){.name = S("by-kind"), .op = IR_OP_ADD,
                                  .match = IR_MATCH_KIND, .kind = TH_NAMED,
                                  .type_rule = accept_named,
                                  .state = (void *)&I32});

    r = ir_rule_resolve(x.m, rs3, in, &how);
    check("then the kind rung", r && core_string_eq(r->name, S("by-kind"))
                                   && how == IR_MATCH_KIND);

    ir_rules_t *rs4 = ir_rules_new(x.ctx);

    ir_rules_add(rs4, (ir_rule_t){.name = S("by-class"), .op = IR_OP_ADD,
                                  .match = IR_MATCH_CLASS,
                                  .class = IR_CLASS_SINT,
                                  .type_rule = accept_named,
                                  .state = (void *)&I32});
    ir_rules_add(rs4, (ir_rule_t){.name = S("by-default"), .op = IR_OP_ADD,
                                  .match = IR_MATCH_DEFAULT,
                                  .type_rule = accept_named,
                                  .state = (void *)&I32});

    r = ir_rule_resolve(x.m, rs4, in, &how);
    check("then the machine-class rung",
          r && core_string_eq(r->name, S("by-class"))
              && how == IR_MATCH_CLASS);

    ir_rules_t *rs5 = ir_rules_new(x.ctx);

    ir_rules_add(rs5, (ir_rule_t){.name = S("by-default"), .op = IR_OP_ADD,
                                  .match = IR_MATCH_DEFAULT,
                                  .type_rule = accept_named,
                                  .state = (void *)&I32});

    r = ir_rule_resolve(x.m, rs5, in, &how);
    check("and finally the opcode default",
          r && core_string_eq(r->name, S("by-default"))
              && how == IR_MATCH_DEFAULT);

    ir_rules_t *empty = ir_rules_new(x.ctx);

    check("with no rule at all nothing resolves",
          ir_rule_resolve(x.m, empty, in, &how) == nullptr);

    ir_instr_t *other = instr(&x, IR_OP_SUB, S("i32"), 2);

    check("a rule for one opcode does not fire for another",
          ir_rule_resolve(x.m, rs5, other, &how) == nullptr);

    ir_ctx_free(x.ctx);
}

static void test_decline(void)
{
    fix         x  = setup(&ir_profile_bce, S("i32"));
    ir_rules_t *rs = ir_rules_new(x.ctx);

    ir_rules_add(rs, (ir_rule_t){.name = S("specific-but-declines"),
                                 .op = IR_OP_ADD, .match = IR_MATCH_EXACT,
                                 .type_text = S("i32"),
                                 .type_rule = always_decline});
    ir_rules_add(rs, (ir_rule_t){.name = S("fallback"), .op = IR_OP_ADD,
                                 .match = IR_MATCH_DEFAULT,
                                 .type_rule = accept_named,
                                 .state = (void *)&U64});

    ir_instr_t      *in = instr(&x, IR_OP_ADD, S("i32"), 2);
    ir_rule_result_t r  = ir_rule_apply(x.m, rs, in, x.d);

    check("a declining rule falls through to the next rung",
          r.verdict == IR_RULE_ACCEPT);
    check("the fallback's result type is used",
          r.result
              && ir_type_eq(x.m, r.result, ir_type_parse(x.m, S("u64"))));
    check("the rule that fired is recorded, not the one that declined",
          rule_named(ir_annot_get(&in->annots, S("ir.rule")),
                     S("fallback")));

    ir_rules_t *only = ir_rules_new(x.ctx);

    ir_rules_add(only, (ir_rule_t){.name = S("declines"), .op = IR_OP_ADD,
                                   .match = IR_MATCH_DEFAULT,
                                   .type_rule = always_decline});

    ir_instr_t *in2 = instr(&x, IR_OP_ADD, S("i32"), 2);

    check("when every rule declines the result is a decline",
          ir_rule_apply(x.m, only, in2, x.d).verdict == IR_RULE_DECLINE);
    check("a declined instruction is not annotated",
          ir_annot_get(&in2->annots, S("ir.rule")) == nullptr);

    ir_rules_t *bad = ir_rules_new(x.ctx);

    ir_rules_add(bad, (ir_rule_t){.name = S("rejects"), .op = IR_OP_ADD,
                                  .match = IR_MATCH_DEFAULT,
                                  .type_rule = reject});
    ir_rules_add(bad, (ir_rule_t){.name = S("never-reached"), .op = IR_OP_ADD,
                                  .match = IR_MATCH_DEFAULT,
                                  .type_rule = accept_named,
                                  .state = (void *)&I32});

    ir_instr_t *in3 = instr(&x, IR_OP_ADD, S("i32"), 2);

    in3->span = ir_span_make(ir_file_add(x.m, S("t.ir")), 0, 1, 1, 1, 1, 2);

    check("a rejecting rule stops the ladder",
          ir_rule_apply(x.m, bad, in3, x.d).verdict == IR_RULE_REJECT);
    check("a rejection may report", ir_diags_count(x.d) > 0);
    check("a rejected instruction is not annotated",
          ir_annot_get(&in3->annots, S("ir.rule")) == nullptr);

    ir_ctx_free(x.ctx);
}

static void test_rules_live_in_the_profile(void)
{
    fix         x  = setup(&ir_profile_synth, S("int"));
    ir_rules_t *rs = ir_rules_from_profile(x.ctx, &ir_profile_synth);

    check("a profile supplies its rules",
          ir_rules_count(rs) == ir_profile_synth.n_rules);

    ir_instr_t      *cat = instr(&x, IR_OP_ADD, S("str"), 2);
    ir_rule_result_t r   = ir_rule_apply(x.m, rs, cat, x.d);

    check("the profile's own type gets the profile's own meaning",
          r.verdict == IR_RULE_ACCEPT && r.result
              && ir_type_eq(x.m, r.result, ir_type_parse(x.m, S("str"))));
    check("the profile rule is named in the annotation",
          rule_named(ir_annot_get(&cat->annots, S("ir.rule")),
                     S("synth.concat")));

    ir_instr_t *ints = instr(&x, IR_OP_ADD, S("int"), 2);
    ir_match_t  how;

    check("the same opcode on another type finds no profile rule",
          ir_rule_resolve(x.m, rs, ints, &how) == nullptr);

    ir_instr_t *idx = instr(&x, IR_OP_INDEX, S("list[int]"), 2);

    r = ir_rule_apply(x.m, rs, idx, x.d);
    check("a constructor rule matches any argument",
          r.verdict == IR_RULE_ACCEPT && r.result
              && ir_type_eq(x.m, r.result, ir_type_parse(x.m, S("int"))));

    ir_instr_t *idx2 = instr(&x, IR_OP_INDEX, S("list[str]"), 2);

    r = ir_rule_apply(x.m, rs, idx2, x.d);
    check("and takes the argument it was given",
          r.verdict == IR_RULE_ACCEPT && r.result
              && ir_type_eq(x.m, r.result, ir_type_parse(x.m, S("str"))));

    ir_instr_t *seq = instr(&x, IR_OP_CLZ, S("array[int; 4]"), 1);

    r = ir_rule_apply(x.m, rs, seq, x.d);
    check("a kind rule matches by typehash node kind",
          r.verdict == IR_RULE_ACCEPT && r.result
              && ir_type_eq(x.m, r.result, ir_type_parse(x.m, S("word"))));

    fix         y   = setup(&ir_profile_bce, S("i32"));
    ir_rules_t *brs = ir_rules_from_profile(y.ctx, &ir_profile_bce);

    check("a profile with no rules supplies none", ir_rules_count(brs) == 0);

    ir_instr_t *bcat = instr(&y, IR_OP_ADD, S("str"), 2);

    check("the other profile's rule does not apply here",
          ir_rule_apply(y.m, brs, bcat, y.d).verdict == IR_RULE_DECLINE);

    ir_ctx_free(y.ctx);
    ir_ctx_free(x.ctx);
}

static void test_hooks_cannot_change_shape(void)
{
    fix         x  = setup(&ir_profile_synth, S("int"));
    ir_rules_t *rs = ir_rules_from_profile(x.ctx, &ir_profile_synth);

    ir_instr_t *in = ir_instr_add(x.b, IR_OP_ADD);
    th_type    *st = ir_type_parse(x.m, S("str"));
    ir_value_t *v  = ir_param_add(x.f, S("s"), st);

    check("arity is enforced before any rule runs",
          ir_instr_operand_value(in, v) && ir_instr_operand_value(in, v)
              && !ir_instr_operand_value(in, v));
    check("operand kind is enforced before any rule runs",
          !ir_instr_operand_block(in, x.b));

    ir_rule_result_t r = ir_rule_apply(x.m, rs, in, x.d);

    check("the rule runs on the instruction as parsed",
          r.verdict == IR_RULE_ACCEPT);
    check("and did not change its arity", in->n_operands == 2);
    check("and did not change its opcode", in->op == IR_OP_ADD);
    check("and did not change its spelling",
          core_string_eq(ir_op_spelling(in->op), S("add")));

    ir_ctx_free(x.ctx);
}

static void test_registration_rejects_nonsense(void)
{
    ir_ctx_t   *ctx = ir_ctx_new();
    ir_rules_t *rs  = ir_rules_new(ctx);

    check("a rule with no name is refused",
          !ir_rules_add(rs, (ir_rule_t){.op = IR_OP_ADD,
                                        .match = IR_MATCH_DEFAULT,
                                        .type_rule = always_decline}));
    check("a rule with no body is refused",
          !ir_rules_add(rs, (ir_rule_t){.name = S("x"), .op = IR_OP_ADD,
                                        .match = IR_MATCH_DEFAULT}));
    check("an exact rule with no type is refused",
          !ir_rules_add(rs, (ir_rule_t){.name = S("x"), .op = IR_OP_ADD,
                                        .match = IR_MATCH_EXACT,
                                        .type_rule = always_decline}));
    check("a constructor rule with no name is refused",
          !ir_rules_add(rs, (ir_rule_t){.name = S("x"), .op = IR_OP_INDEX,
                                        .match = IR_MATCH_CONSTRUCTOR,
                                        .type_rule = always_decline}));
    check("a rule for an extension opcode is accepted, since a profile "
          "supplies both the opcode and its meaning",
          ir_rules_add(rs, (ir_rule_t){.name = S("x"), .op = IR_OP_COUNT,
                                       .match = IR_MATCH_DEFAULT,
                                       .type_rule = always_decline}));
    check("only that one was registered", ir_rules_count(rs) == 1);

    ir_ctx_free(ctx);
}

static void test_typing_and_lowering_are_separate(void)
{
    fix         x  = setup(&ir_profile_bce, S("i32"));
    ir_rules_t *rs = ir_rules_new(x.ctx);

    ir_rules_add(rs, (ir_rule_t){.name = S("types-only"), .op = IR_OP_ADD,
                                 .match = IR_MATCH_DEFAULT,
                                 .type_rule = accept_named,
                                 .state = (void *)&I32});
    ir_rules_add(rs, (ir_rule_t){.name = S("lowers-only"), .op = IR_OP_ADD,
                                 .match = IR_MATCH_DEFAULT,
                                 .lower_rule = note_lowering});

    ir_instr_t *in = instr(&x, IR_OP_ADD, S("i32"), 2);

    lowered = 0;

    ir_rule_result_t r = ir_rule_apply(x.m, rs, in, x.d);

    check("typing dispatches to the typing rule",
          r.verdict == IR_RULE_ACCEPT);
    check("typing does not run the lowering rule", lowered == 0);

    check("lowering dispatches to the lowering rule",
          ir_rule_lower(x.m, rs, in, x.d));
    check("the lowering rule ran once", lowered == 1);

    ir_rules_t *no_lower = ir_rules_new(x.ctx);

    ir_rules_add(no_lower, (ir_rule_t){.name = S("t"), .op = IR_OP_ADD,
                                       .match = IR_MATCH_DEFAULT,
                                       .type_rule = accept_named,
                                       .state = (void *)&I32});
    check("an instruction with no lowering rule does not lower",
          !ir_rule_lower(x.m, no_lower, in, x.d));

    ir_ctx_free(x.ctx);
}

static void test_explain(void)
{
    fix           x  = setup(&ir_profile_synth, S("int"));
    ir_rules_t   *rs = ir_rules_from_profile(x.ctx, &ir_profile_synth);
    ir_instr_t   *in = instr(&x, IR_OP_ADD, S("str"), 2);
    core_string_t e  = ir_rule_explain(x.m, rs, in);

    check("explain names the opcode", has(e, S("opcode add")));
    check("explain names the key type", has(e, S("key type str")));
    check("explain shows every rung",
          has(e, S("exact type")) && has(e, S("constructor"))
              && has(e, S("type kind")) && has(e, S("machine class"))
              && has(e, S("opcode default")));
    check("explain marks the rung that matched",
          has(e, S("MATCH synth.concat")));
    check("explain reports what it resolved to",
          has(e, S("resolves to synth.concat by exact type")));
    core_free(e.data);

    ir_instr_t *none = instr(&x, IR_OP_SUB, S("int"), 2);

    e = ir_rule_explain(x.m, rs, none);
    check("explain says so when nothing resolves",
          has(e, S("resolves to nothing")));
    core_free(e.data);

    ir_ctx_free(x.ctx);
}

static void test_parsing_ignores_rules(void)
{
    core_string_t src =
        S("module m {\n  profile synth\n\n"
          "  func @f(%a: str) -> str {\n"
          "    block e:\n      %c: str = add %a, %a\n      return %c\n  }\n}\n");

    const ir_profile_t *const with[]    = {&ir_profile_synth};
    ir_ctx_t                 *ctx       = ir_ctx_new();
    ir_diags_t               *d1        = ir_diags_new(ctx, nullptr);
    ir_module_t              *m1        = ir_module_parse(ctx, src, 0, with, 1,
                                                          d1);

    check("a module using a profile's own type parses", m1 != nullptr);

    if (!m1) {
        ir_ctx_free(ctx);

        return;
    }

    core_string_t printed = ir_module_print(m1);

    ir_ctx_t     *ctx2 = ir_ctx_new();
    ir_diags_t   *d2   = ir_diags_new(ctx2, nullptr);
    ir_module_t  *m2   = ir_module_parse(ctx2, src, 0, with, 1, d2);
    core_string_t p2   = ir_module_print(m2);

    check("parsing and printing do not depend on rules being applied",
          p2.data && core_string_eq(printed, p2));

    ir_rules_t *rs = ir_rules_from_profile(ctx2, &ir_profile_synth);
    ir_func_t  *f  = ir_func_find(m2, S("f"));

    ir_rule_apply(m2, rs, ir_block_entry(f)->instrs[0], d2);

    core_string_t p3 = ir_module_print(m2);

    check("what a rule decided is recorded in the text",
          has(p3, S("!ir.rule(\"synth.concat\" from typed)")));

    ir_diags_t  *d3 = ir_diags_new(ctx2, nullptr);
    ir_module_t *m3 = ir_module_parse(ctx2, p3, 0, with, 1, d3);

    check("and reads back", m3 != nullptr);

    if (m3) {
        ir_instr_t *back =
            ir_block_entry(ir_func_find(m3, S("f")))->instrs[0];
        ir_annot_t *a = ir_annot_get(&back->annots, S("ir.rule"));

        check("with its provenance intact",
              a && a->derived_from == IR_PROP_TYPED
                  && core_string_eq(a->s, S("synth.concat")));
    }

    core_free(printed.data);
    core_free(p2.data);
    core_free(p3.data);
    ir_ctx_free(ctx2);
    ir_ctx_free(ctx);
}

static void test_extension_opcodes(void)
{
    const ir_profile_t *const with[]    = {&ir_profile_synth};
    const ir_profile_t *const without[] = {&ir_profile_bce,
                                           &ir_profile_synth};

    core_string_t src =
        S("module m {\n  profile synth\n\n"
          "  func @f(%a: str) -> str {\n"
          "    block e:\n"
          "      %c: str = synth.concat %a, %a\n"
          "      synth.trace %a\n"
          "      return %c\n  }\n}\n");

    ir_ctx_t    *ctx = ir_ctx_new();
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
    ir_module_t *m   = ir_module_parse(ctx, src, 0, with, 1, d);

    check("an extension opcode parses when its profile is loaded",
          m != nullptr);

    if (m) {
        ir_instr_t *in = ir_block_entry(ir_func_find(m, S("f")))->instrs[0];

        check("it resolves to an extension id", ir_op_is_extension(in->op));
        check("its arity comes from the profile's table",
              ir_module_op_info(m, in->op)->min_operands == 2
                  && in->n_operands == 2);

        core_string_t out = ir_module_print(m);

        check("it prints by its registered spelling",
              has(out, S("synth.concat %a, %a"))
                  && has(out, S("synth.trace %a")));

        ir_diags_t   *d2 = ir_diags_new(ctx, nullptr);
        ir_module_t  *m2 = ir_module_parse(ctx, out, 0, with, 1, d2);
        core_string_t o2 = m2 ? ir_module_print(m2) : (core_string_t){};

        check("and round-trips", o2.data && core_string_eq(out, o2));
        core_free(out.data);
        core_free(o2.data);

        ir_block_t *b  = ir_block_entry(ir_func_find(m, S("f")));
        ir_rules_t *rs = ir_module_rules(m);
        ir_match_t  how;

        for (uint32_t k = 0; k < b->n_instrs; k++) {
            ir_instr_t *ex = b->instrs[k];

            if (!ir_op_is_extension(ex->op)) {
                continue;
            }

            const ir_rule_t *rule = ir_rule_resolve(m, rs, ex, &how);

            check("a rule the profile supplied resolves for its own opcode",
                  rule != nullptr);
            check("and it is the profile's rule, not a built-in",
                  rule && core_string_starts_with(rule->name, S("synth.")));

            ir_rule_result_t got = ir_rule_apply(m, rs, ex, d);

            check("and applying it accepts", got.verdict == IR_RULE_ACCEPT);
        }
    }

    check("every extension opcode is namespaced to its profile",
          ir_op_spelling_is_namespaced(S("synth.concat"), S("synth"))
              && ir_op_spelling_is_namespaced(S("synth.trace"), S("synth")));
    check("a bare mnemonic is not namespaced",
          !ir_op_spelling_is_namespaced(S("concat"), S("synth")));
    check("another profile's namespace does not count",
          !ir_op_spelling_is_namespaced(S("n00b.concat"), S("synth")));

    for (size_t i = 0; i < ir_profile_synth.n_ext_ops; i++) {
        core_string_t sp = ir_profile_synth.ext_ops[i].spelling;
        ir_op_t       clash;

        if (!ir_op_spelling_is_namespaced(sp, ir_profile_synth.name)) {
            printf("FAIL  extension opcode %.*s is not namespaced\n",
                   (int)sp.u8_bytes, sp.data);
            fails++;
        }

        if (ir_op_lookup(sp, &clash)) {
            printf("FAIL  extension opcode %.*s collides with a built-in\n",
                   (int)sp.u8_bytes, sp.data);
            fails++;
        }
    }

    core_string_t bare =
        S("module m {\n  profile bce\n\n"
          "  func @f(%a: i32) -> i32 {\n"
          "    block e:\n      %c: i32 = synth.concat %a, %a\n"
          "      return %c\n  }\n}\n");

    ir_ctx_t   *c2 = ir_ctx_new();
    ir_diags_t *d3 = ir_diags_new(c2, nullptr);

    check("an extension opcode without its profile is refused",
          ir_module_parse(c2, bare, 0, without, 2, d3) == nullptr);

    core_string_t r = ir_diags_render_text(d3, &(ir_render_t){0});

    check("and reports a missing profile, not an unknown opcode",
          has(r, S("needs a profile")) && !has(r, S("unknown opcode")));
    core_free(r.data);

    ir_diags_t   *d4 = ir_diags_new(c2, nullptr);
    core_string_t nonsense =
        S("module m {\n  profile bce\n\n"
          "  func @f(%a: i32) -> i32 {\n"
          "    block e:\n      %c: i32 = frobnicate %a\n"
          "      return %c\n  }\n}\n");

    check("a bare unknown mnemonic is still an unknown opcode",
          ir_module_parse(c2, nonsense, 0, without, 2, d4) == nullptr);

    core_string_t r2 = ir_diags_render_text(d4, &(ir_render_t){0});

    check("and says so", has(r2, S("unknown opcode")));
    core_free(r2.data);

    ir_ctx_free(c2);
    ir_ctx_free(ctx);
}

int main(void)
{
    test_ladder();
    test_decline();
    test_rules_live_in_the_profile();
    test_hooks_cannot_change_shape();
    test_registration_rejects_nonsense();
    test_typing_and_lowering_are_separate();
    test_explain();
    test_parsing_ignores_rules();
    test_extension_opcodes();

    return report("ir rules");
}
