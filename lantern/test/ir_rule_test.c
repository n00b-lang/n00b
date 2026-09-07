#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/rule.h"
#include "ir/text.h"
#include "ir_test.h"

typedef struct {
    ir_ctx_t    *ctx;
    ir_module_t *m;
    ir_func_t   *f;
    ir_block_t  *b;
    ir_diags_t  *d;
} fix;

static fix setup(const ir_profile_t *p, const char *scalar)
{
    fix x = {0};

    x.ctx = ir_ctx_new();
    x.m   = ir_module_new(x.ctx, "m", p, "test");
    x.d   = ir_diags_new(x.ctx, nullptr);

    char sig[64];

    snprintf(sig, sizeof(sig), "(%s) -> %s", scalar, scalar);
    x.f = ir_func_add(x.m, "f", ir_type_parse(x.m, sig), IR_LINK_EXTERNAL);
    x.b = ir_block_add(x.f, "entry");

    return x;
}

static ir_instr_t *instr(fix *x, ir_op_t op, const char *type_text, int n)
{
    ir_instr_t *in = ir_instr_add(x->b, op);
    th_type    *t  = ir_type_parse(x->m, type_text);
    ir_value_t *v  = ir_param_add(x->f, "a", t);

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

    return (ir_rule_result_t){.verdict = IR_RULE_ACCEPT,
                              .result  = ir_type_parse(m, (const char *)state)};
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
                         "rejected");
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

static void test_ladder(void)
{
    fix x = setup(&ir_profile_bce, "i32");

    ir_rules_t *rs = ir_rules_new(x.ctx);

    ir_rules_add(rs, (ir_rule_t){.name = "by-class", .op = IR_OP_ADD,
                                 .match = IR_MATCH_CLASS,
                                 .class = IR_CLASS_SINT,
                                 .type_rule = accept_named, .state = "i32"});
    ir_rules_add(rs, (ir_rule_t){.name = "by-kind", .op = IR_OP_ADD,
                                 .match = IR_MATCH_KIND, .kind = TH_NAMED,
                                 .type_rule = accept_named, .state = "i32"});
    ir_rules_add(rs, (ir_rule_t){.name = "by-default", .op = IR_OP_ADD,
                                 .match = IR_MATCH_DEFAULT,
                                 .type_rule = accept_named, .state = "i32"});
    ir_rules_add(rs, (ir_rule_t){.name = "by-ctor", .op = IR_OP_ADD,
                                 .match = IR_MATCH_CONSTRUCTOR,
                                 .type_text = "i32",
                                 .type_rule = accept_named, .state = "i32"});
    ir_rules_add(rs, (ir_rule_t){.name = "by-exact", .op = IR_OP_ADD,
                                 .match = IR_MATCH_EXACT, .type_text = "i32",
                                 .type_rule = accept_named, .state = "i32"});

    check("all five rungs registered", ir_rules_count(rs) == 5);

    ir_instr_t *in = instr(&x, IR_OP_ADD, "i32", 2);
    ir_match_t  how;

    const ir_rule_t *r = ir_rule_resolve(x.m, rs, in, &how);

    check("the exact rung wins over every other",
          r && !strcmp(r->name, "by-exact") && how == IR_MATCH_EXACT);

    ir_rules_t *rs2 = ir_rules_new(x.ctx);

    ir_rules_add(rs2, (ir_rule_t){.name = "by-class", .op = IR_OP_ADD,
                                  .match = IR_MATCH_CLASS,
                                  .class = IR_CLASS_SINT,
                                  .type_rule = accept_named, .state = "i32"});
    ir_rules_add(rs2, (ir_rule_t){.name = "by-kind", .op = IR_OP_ADD,
                                  .match = IR_MATCH_KIND, .kind = TH_NAMED,
                                  .type_rule = accept_named, .state = "i32"});
    ir_rules_add(rs2, (ir_rule_t){.name = "by-ctor", .op = IR_OP_ADD,
                                  .match = IR_MATCH_CONSTRUCTOR,
                                  .type_text = "i32",
                                  .type_rule = accept_named, .state = "i32"});

    r = ir_rule_resolve(x.m, rs2, in, &how);
    check("with no exact rule the constructor rung wins",
          r && !strcmp(r->name, "by-ctor") && how == IR_MATCH_CONSTRUCTOR);

    ir_rules_t *rs3 = ir_rules_new(x.ctx);

    ir_rules_add(rs3, (ir_rule_t){.name = "by-class", .op = IR_OP_ADD,
                                  .match = IR_MATCH_CLASS,
                                  .class = IR_CLASS_SINT,
                                  .type_rule = accept_named, .state = "i32"});
    ir_rules_add(rs3, (ir_rule_t){.name = "by-kind", .op = IR_OP_ADD,
                                  .match = IR_MATCH_KIND, .kind = TH_NAMED,
                                  .type_rule = accept_named, .state = "i32"});

    r = ir_rule_resolve(x.m, rs3, in, &how);
    check("then the kind rung", r && !strcmp(r->name, "by-kind")
                                   && how == IR_MATCH_KIND);

    ir_rules_t *rs4 = ir_rules_new(x.ctx);

    ir_rules_add(rs4, (ir_rule_t){.name = "by-class", .op = IR_OP_ADD,
                                  .match = IR_MATCH_CLASS,
                                  .class = IR_CLASS_SINT,
                                  .type_rule = accept_named, .state = "i32"});
    ir_rules_add(rs4, (ir_rule_t){.name = "by-default", .op = IR_OP_ADD,
                                  .match = IR_MATCH_DEFAULT,
                                  .type_rule = accept_named, .state = "i32"});

    r = ir_rule_resolve(x.m, rs4, in, &how);
    check("then the machine-class rung",
          r && !strcmp(r->name, "by-class") && how == IR_MATCH_CLASS);

    ir_rules_t *rs5 = ir_rules_new(x.ctx);

    ir_rules_add(rs5, (ir_rule_t){.name = "by-default", .op = IR_OP_ADD,
                                  .match = IR_MATCH_DEFAULT,
                                  .type_rule = accept_named, .state = "i32"});

    r = ir_rule_resolve(x.m, rs5, in, &how);
    check("and finally the opcode default",
          r && !strcmp(r->name, "by-default") && how == IR_MATCH_DEFAULT);

    ir_rules_t *empty = ir_rules_new(x.ctx);

    check("with no rule at all nothing resolves",
          ir_rule_resolve(x.m, empty, in, &how) == nullptr);

    ir_instr_t *other = instr(&x, IR_OP_SUB, "i32", 2);

    check("a rule for one opcode does not fire for another",
          ir_rule_resolve(x.m, rs5, other, &how) == nullptr);

    ir_ctx_free(x.ctx);
}

static void test_decline(void)
{
    fix         x  = setup(&ir_profile_bce, "i32");
    ir_rules_t *rs = ir_rules_new(x.ctx);

    ir_rules_add(rs, (ir_rule_t){.name = "specific-but-declines",
                                 .op = IR_OP_ADD, .match = IR_MATCH_EXACT,
                                 .type_text = "i32",
                                 .type_rule = always_decline});
    ir_rules_add(rs, (ir_rule_t){.name = "fallback", .op = IR_OP_ADD,
                                 .match = IR_MATCH_DEFAULT,
                                 .type_rule = accept_named, .state = "u64"});

    ir_instr_t      *in = instr(&x, IR_OP_ADD, "i32", 2);
    ir_rule_result_t r  = ir_rule_apply(x.m, rs, in, x.d);

    check("a declining rule falls through to the next rung",
          r.verdict == IR_RULE_ACCEPT);
    check("the fallback's result type is used",
          r.result && ir_type_eq(x.m, r.result, ir_type_parse(x.m, "u64")));
    check("the rule that fired is recorded, not the one that declined",
          ir_annot_get(&in->annots, "ir.rule")
              && !strcmp(ir_annot_get(&in->annots, "ir.rule")->s,
                         "fallback"));

    ir_rules_t *only = ir_rules_new(x.ctx);

    ir_rules_add(only, (ir_rule_t){.name = "declines", .op = IR_OP_ADD,
                                   .match = IR_MATCH_DEFAULT,
                                   .type_rule = always_decline});

    ir_instr_t *in2 = instr(&x, IR_OP_ADD, "i32", 2);

    check("when every rule declines the result is a decline",
          ir_rule_apply(x.m, only, in2, x.d).verdict == IR_RULE_DECLINE);
    check("a declined instruction is not annotated",
          ir_annot_get(&in2->annots, "ir.rule") == nullptr);

    ir_rules_t *bad = ir_rules_new(x.ctx);

    ir_rules_add(bad, (ir_rule_t){.name = "rejects", .op = IR_OP_ADD,
                                  .match = IR_MATCH_DEFAULT,
                                  .type_rule = reject});
    ir_rules_add(bad, (ir_rule_t){.name = "never-reached", .op = IR_OP_ADD,
                                  .match = IR_MATCH_DEFAULT,
                                  .type_rule = accept_named,
                                  .state = "i32"});

    ir_instr_t *in3 = instr(&x, IR_OP_ADD, "i32", 2);

    in3->span = ir_span_make(ir_file_add(x.m, "t.ir"), 0, 1, 1, 1, 1, 2);

    check("a rejecting rule stops the ladder",
          ir_rule_apply(x.m, bad, in3, x.d).verdict == IR_RULE_REJECT);
    check("a rejection may report", ir_diags_count(x.d) > 0);
    check("a rejected instruction is not annotated",
          ir_annot_get(&in3->annots, "ir.rule") == nullptr);

    ir_ctx_free(x.ctx);
}

static void test_rules_live_in_the_profile(void)
{
    fix         x  = setup(&ir_profile_synth, "int");
    ir_rules_t *rs = ir_rules_from_profile(x.ctx, &ir_profile_synth);

    check("a profile supplies its rules",
          ir_rules_count(rs) == ir_profile_synth.n_rules);

    ir_instr_t      *cat = instr(&x, IR_OP_ADD, "str", 2);
    ir_rule_result_t r   = ir_rule_apply(x.m, rs, cat, x.d);

    check("the profile's own type gets the profile's own meaning",
          r.verdict == IR_RULE_ACCEPT && r.result
              && ir_type_eq(x.m, r.result, ir_type_parse(x.m, "str")));
    check("the profile rule is named in the annotation",
          ir_annot_get(&cat->annots, "ir.rule")
              && !strcmp(ir_annot_get(&cat->annots, "ir.rule")->s,
                         "synth.concat"));

    ir_instr_t *ints = instr(&x, IR_OP_ADD, "int", 2);
    ir_match_t  how;

    check("the same opcode on another type finds no profile rule",
          ir_rule_resolve(x.m, rs, ints, &how) == nullptr);

    ir_instr_t *idx = instr(&x, IR_OP_INDEX, "list[int]", 2);

    r = ir_rule_apply(x.m, rs, idx, x.d);
    check("a constructor rule matches any argument",
          r.verdict == IR_RULE_ACCEPT && r.result
              && ir_type_eq(x.m, r.result, ir_type_parse(x.m, "int")));

    ir_instr_t *idx2 = instr(&x, IR_OP_INDEX, "list[str]", 2);

    r = ir_rule_apply(x.m, rs, idx2, x.d);
    check("and takes the argument it was given",
          r.verdict == IR_RULE_ACCEPT && r.result
              && ir_type_eq(x.m, r.result, ir_type_parse(x.m, "str")));

    ir_instr_t *seq = instr(&x, IR_OP_CLZ, "array[int; 4]", 1);

    r = ir_rule_apply(x.m, rs, seq, x.d);
    check("a kind rule matches by typehash node kind",
          r.verdict == IR_RULE_ACCEPT && r.result
              && ir_type_eq(x.m, r.result, ir_type_parse(x.m, "word")));

    fix         y   = setup(&ir_profile_bce, "i32");
    ir_rules_t *brs = ir_rules_from_profile(y.ctx, &ir_profile_bce);

    check("a profile with no rules supplies none", ir_rules_count(brs) == 0);

    ir_instr_t *bcat = instr(&y, IR_OP_ADD, "str", 2);

    check("the other profile's rule does not apply here",
          ir_rule_apply(y.m, brs, bcat, y.d).verdict == IR_RULE_DECLINE);

    ir_ctx_free(y.ctx);
    ir_ctx_free(x.ctx);
}

static void test_hooks_cannot_change_shape(void)
{
    fix         x  = setup(&ir_profile_synth, "int");
    ir_rules_t *rs = ir_rules_from_profile(x.ctx, &ir_profile_synth);

    ir_instr_t *in = ir_instr_add(x.b, IR_OP_ADD);
    th_type    *st = ir_type_parse(x.m, "str");
    ir_value_t *v  = ir_param_add(x.f, "s", st);

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
          !strcmp(ir_op_spelling(in->op), "add"));

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
          !ir_rules_add(rs, (ir_rule_t){.name = "x", .op = IR_OP_ADD,
                                        .match = IR_MATCH_DEFAULT}));
    check("an exact rule with no type is refused",
          !ir_rules_add(rs, (ir_rule_t){.name = "x", .op = IR_OP_ADD,
                                        .match = IR_MATCH_EXACT,
                                        .type_rule = always_decline}));
    check("a constructor rule with no name is refused",
          !ir_rules_add(rs, (ir_rule_t){.name = "x", .op = IR_OP_INDEX,
                                        .match = IR_MATCH_CONSTRUCTOR,
                                        .type_rule = always_decline}));
    check("a rule for an extension opcode is accepted, since a profile "
          "supplies both the opcode and its meaning",
          ir_rules_add(rs, (ir_rule_t){.name = "x", .op = IR_OP_COUNT,
                                       .match = IR_MATCH_DEFAULT,
                                       .type_rule = always_decline}));
    check("only that one was registered", ir_rules_count(rs) == 1);

    ir_ctx_free(ctx);
}

static void test_typing_and_lowering_are_separate(void)
{
    fix         x  = setup(&ir_profile_bce, "i32");
    ir_rules_t *rs = ir_rules_new(x.ctx);

    ir_rules_add(rs, (ir_rule_t){.name = "types-only", .op = IR_OP_ADD,
                                 .match = IR_MATCH_DEFAULT,
                                 .type_rule = accept_named, .state = "i32"});
    ir_rules_add(rs, (ir_rule_t){.name = "lowers-only", .op = IR_OP_ADD,
                                 .match = IR_MATCH_DEFAULT,
                                 .lower_rule = note_lowering});

    ir_instr_t *in = instr(&x, IR_OP_ADD, "i32", 2);

    lowered = 0;

    ir_rule_result_t r = ir_rule_apply(x.m, rs, in, x.d);

    check("typing dispatches to the typing rule",
          r.verdict == IR_RULE_ACCEPT);
    check("typing does not run the lowering rule", lowered == 0);

    check("lowering dispatches to the lowering rule",
          ir_rule_lower(x.m, rs, in, x.d));
    check("the lowering rule ran once", lowered == 1);

    ir_rules_t *no_lower = ir_rules_new(x.ctx);

    ir_rules_add(no_lower, (ir_rule_t){.name = "t", .op = IR_OP_ADD,
                                       .match = IR_MATCH_DEFAULT,
                                       .type_rule = accept_named,
                                       .state = "i32"});
    check("an instruction with no lowering rule does not lower",
          !ir_rule_lower(x.m, no_lower, in, x.d));

    ir_ctx_free(x.ctx);
}

static void test_explain(void)
{
    fix         x  = setup(&ir_profile_synth, "int");
    ir_rules_t *rs = ir_rules_from_profile(x.ctx, &ir_profile_synth);
    ir_instr_t *in = instr(&x, IR_OP_ADD, "str", 2);
    char       *e  = ir_rule_explain(x.m, rs, in);

    check("explain names the opcode", e && strstr(e, "opcode add"));
    check("explain names the key type", e && strstr(e, "key type str"));
    check("explain shows every rung",
          e && strstr(e, "exact type") && strstr(e, "constructor")
              && strstr(e, "type kind") && strstr(e, "machine class")
              && strstr(e, "opcode default"));
    check("explain marks the rung that matched",
          e && strstr(e, "MATCH synth.concat"));
    check("explain reports what it resolved to",
          e && strstr(e, "resolves to synth.concat by exact type"));
    free(e);

    ir_instr_t *none = instr(&x, IR_OP_SUB, "int", 2);

    e = ir_rule_explain(x.m, rs, none);
    check("explain says so when nothing resolves",
          e && strstr(e, "resolves to nothing"));
    free(e);

    ir_ctx_free(x.ctx);
}

static void test_parsing_ignores_rules(void)
{
    const char *src =
        "module m {\n  profile synth\n\n"
        "  func @f(%a: str) -> str {\n"
        "    block e:\n      %c: str = add %a, %a\n      return %c\n  }\n}\n";

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

    char *printed = ir_module_print(m1);

    ir_ctx_t    *ctx2 = ir_ctx_new();
    ir_diags_t  *d2   = ir_diags_new(ctx2, nullptr);
    ir_module_t *m2   = ir_module_parse(ctx2, src, 0, with, 1, d2);
    char        *p2   = ir_module_print(m2);

    check("parsing and printing do not depend on rules being applied",
          p2 && !strcmp(printed, p2));

    ir_rules_t *rs = ir_rules_from_profile(ctx2, &ir_profile_synth);
    ir_func_t  *f  = ir_func_find(m2, "f");

    ir_rule_apply(m2, rs, ir_block_entry(f)->instrs[0], d2);

    char *p3 = ir_module_print(m2);

    check("what a rule decided is recorded in the text",
          p3 && strstr(p3, "!ir.rule(\"synth.concat\" from typed)"));

    ir_diags_t  *d3 = ir_diags_new(ctx2, nullptr);
    ir_module_t *m3 = ir_module_parse(ctx2, p3, 0, with, 1, d3);

    check("and reads back", m3 != nullptr);

    if (m3) {
        ir_instr_t *back =
            ir_block_entry(ir_func_find(m3, "f"))->instrs[0];
        ir_annot_t *a = ir_annot_get(&back->annots, "ir.rule");

        check("with its provenance intact",
              a && a->derived_from == IR_PROP_TYPED
                  && !strcmp(a->s, "synth.concat"));
    }

    free(printed);
    free(p2);
    free(p3);
    ir_ctx_free(ctx2);
    ir_ctx_free(ctx);
}

static void test_extension_opcodes(void)
{
    const ir_profile_t *const with[]    = {&ir_profile_synth};
    const ir_profile_t *const without[] = {&ir_profile_bce,
                                           &ir_profile_synth};

    const char *src =
        "module m {\n  profile synth\n\n"
        "  func @f(%a: str) -> str {\n"
        "    block e:\n"
        "      %c: str = synth.concat %a, %a\n"
        "      synth.trace %a\n"
        "      return %c\n  }\n}\n";

    ir_ctx_t    *ctx = ir_ctx_new();
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
    ir_module_t *m   = ir_module_parse(ctx, src, 0, with, 1, d);

    check("an extension opcode parses when its profile is loaded",
          m != nullptr);

    if (m) {
        ir_instr_t *in = ir_block_entry(ir_func_find(m, "f"))->instrs[0];

        check("it resolves to an extension id", ir_op_is_extension(in->op));
        check("its arity comes from the profile's table",
              ir_module_op_info(m, in->op)->min_operands == 2
                  && in->n_operands == 2);

        char *out = ir_module_print(m);

        check("it prints by its registered spelling",
              out && strstr(out, "synth.concat %a, %a")
                  && strstr(out, "synth.trace %a"));

        ir_diags_t  *d2 = ir_diags_new(ctx, nullptr);
        ir_module_t *m2 = ir_module_parse(ctx, out, 0, with, 1, d2);
        char        *o2 = m2 ? ir_module_print(m2) : nullptr;

        check("and round-trips", o2 && !strcmp(out, o2));
        free(out);
        free(o2);

        ir_block_t *b  = ir_block_entry(ir_func_find(m, "f"));
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
                  rule && !strncmp(rule->name, "synth.", 6));

            ir_rule_result_t got = ir_rule_apply(m, rs, ex, d);

            check("and applying it accepts", got.verdict == IR_RULE_ACCEPT);
        }
    }

    check("every extension opcode is namespaced to its profile",
          ir_op_spelling_is_namespaced("synth.concat", "synth")
              && ir_op_spelling_is_namespaced("synth.trace", "synth"));
    check("a bare mnemonic is not namespaced",
          !ir_op_spelling_is_namespaced("concat", "synth"));
    check("another profile's namespace does not count",
          !ir_op_spelling_is_namespaced("n00b.concat", "synth"));

    for (size_t i = 0; i < ir_profile_synth.n_ext_ops; i++) {
        const char *sp = ir_profile_synth.ext_ops[i].spelling;
        ir_op_t     clash;

        if (!ir_op_spelling_is_namespaced(sp, ir_profile_synth.name)) {
            printf("FAIL  extension opcode %s is not namespaced\n", sp);
            fails++;
        }

        if (ir_op_lookup(sp, &clash)) {
            printf("FAIL  extension opcode %s collides with a built-in\n",
                   sp);
            fails++;
        }
    }

    const char *bare =
        "module m {\n  profile bce\n\n"
        "  func @f(%a: i32) -> i32 {\n"
        "    block e:\n      %c: i32 = synth.concat %a, %a\n"
        "      return %c\n  }\n}\n";

    ir_ctx_t   *c2 = ir_ctx_new();
    ir_diags_t *d3 = ir_diags_new(c2, nullptr);

    check("an extension opcode without its profile is refused",
          ir_module_parse(c2, bare, 0, without, 2, d3) == nullptr);

    char *r = ir_diags_render_text(d3, &(ir_render_t){0});

    check("and reports a missing profile, not an unknown opcode",
          r && strstr(r, "needs a profile") && !strstr(r, "unknown opcode"));
    free(r);

    ir_diags_t *d4 = ir_diags_new(c2, nullptr);
    const char *nonsense =
        "module m {\n  profile bce\n\n"
        "  func @f(%a: i32) -> i32 {\n"
        "    block e:\n      %c: i32 = frobnicate %a\n"
        "      return %c\n  }\n}\n";

    check("a bare unknown mnemonic is still an unknown opcode",
          ir_module_parse(c2, nonsense, 0, without, 2, d4) == nullptr);

    char *r2 = ir_diags_render_text(d4, &(ir_render_t){0});

    check("and says so", r2 && strstr(r2, "unknown opcode"));
    free(r2);

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
