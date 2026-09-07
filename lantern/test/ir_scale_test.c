#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ir/layout.h"
#include "ir/analyze.h"
#include "ir/pass.h"
#include "ir/pdg.h"
#include "ir/text.h"
#include "ir/rule.h"
#include "ir/verify.h"
#include "ir_test.h"

static ir_module_t *chain(ir_ctx_t *c, uint32_t n, uint32_t locals)
{
    ir_module_t *m  = ir_module_new(c, "m", &ir_profile_bce, "t");
    ir_func_t   *f  = ir_func_add(m, "f", ir_type_parse(m, "(bool) -> void"),
                                  IR_LINK_EXTERNAL);
    ir_value_t  *cv = ir_param_add(f, "c", ir_type_parse(m, "bool"));
    ir_block_t **hd = malloc(sizeof(ir_block_t *) * (n + 1));
    char         nm[32];

    for (uint32_t i = 0; i < locals; i++) {
        snprintf(nm, sizeof nm, "v%u", i);
        ir_local_add(f, nm, ir_type_parse(m, "bool"));
    }

    for (uint32_t i = 0; i <= n; i++) {
        snprintf(nm, sizeof nm, "h%u", i);
        hd[i] = ir_block_add(f, nm);
    }

    for (uint32_t i = 0; i < n; i++) {
        snprintf(nm, sizeof nm, "t%u", i);

        ir_block_t *t = ir_block_add(f, nm);

        snprintf(nm, sizeof nm, "e%u", i);

        ir_block_t *e  = ir_block_add(f, nm);
        ir_instr_t *br = ir_instr_add(hd[i], IR_OP_BRANCH);

        ir_instr_operand_value(br, cv);
        ir_instr_operand_block(br, t);
        ir_instr_operand_block(br, e);

        if (locals) {
            ir_instr_t *w = ir_instr_add(t, IR_OP_COPY);

            ir_instr_set_dest_local(w, f->locals[i % locals]);
            ir_instr_operand_value(w, cv);
        }

        ir_instr_t *j1 = ir_instr_add(t, IR_OP_JUMP);
        ir_instr_t *j2 = ir_instr_add(e, IR_OP_JUMP);

        ir_instr_operand_block(j1, hd[i + 1]);
        ir_instr_operand_block(j2, hd[i + 1]);
    }
    ir_instr_add(hd[n], IR_OP_UNREACHABLE);
    free(hd);
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED);

    return m;
}

static double ms(clock_t a, clock_t b)
{
    return 1000.0 * (double)(b - a) / CLOCKS_PER_SEC;
}

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) \
    || __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif
#endif

#ifndef SANITIZED
#define SANITIZED 0
#endif

#define BUDGET_MS (SANITIZED ? 8000.0 : 1000.0)

static void test_the_pipeline_stays_linear(void)
{
    ir_ctx_t    *c = ir_ctx_new();
    ir_diags_t  *d = ir_diags_new(c, nullptr);
    ir_module_t *m = chain(c, 1500, 0);
    clock_t      a = clock();

    ir_structure(m, d);

    clock_t b = clock();

    ir_module_establish(m, IR_PROP_STRUCTURED);
    ir_verify(m, d);

    clock_t e = clock();

    check("structure walks 4500 blocks well inside the budget",
          ms(a, b) < BUDGET_MS);
    check("and so does the verifier", ms(b, e) < BUDGET_MS);

    if (fails || getenv("IR_SCALE_REPORT")) {
        printf("      structure %.1f ms, verify %.1f ms for 4501 blocks\n",
               ms(a, b), ms(b, e));
    }
    ir_ctx_free(c);
}

static void test_promote_stays_linear(void)
{
    ir_ctx_t    *c = ir_ctx_new();
    ir_diags_t  *d = ir_diags_new(c, nullptr);
    ir_module_t *m = chain(c, 1500, 16);
    clock_t      a = clock();

    ir_promote(m, nullptr);

    clock_t b = clock();

    check("promote handles a deep function with many locals",
          ms(a, b) < BUDGET_MS);
    check("and the result verifies as SSA",
          (ir_verify(m, d) & IR_PROP_SSA) != 0);

    if (fails || getenv("IR_SCALE_REPORT")) {
        printf("      promote %.1f ms for 4501 blocks and 16 locals\n",
               ms(a, b));
    }
    ir_ctx_free(c);
}

static ir_module_t *staircase(ir_ctx_t *c, uint32_t n)
{
    ir_module_t *m  = ir_module_new(c, "m", &ir_profile_bce, "t");
    ir_func_t   *f  = ir_func_add(m, "f", ir_type_parse(m, "(bool) -> void"),
                                  IR_LINK_EXTERNAL);
    ir_value_t  *cv = ir_param_add(f, "c", ir_type_parse(m, "bool"));
    ir_local_t  *x  = ir_local_add(f, "x", ir_type_parse(m, "bool"));
    ir_block_t  *at = ir_block_add(f, "entry");
    ir_instr_t  *w0 = ir_instr_add(at, IR_OP_COPY);
    char         nm[32];

    ir_instr_set_dest_local(w0, x);
    ir_instr_operand_value(w0, cv);

    for (uint32_t i = 0; i < n; i++) {
        snprintf(nm, sizeof nm, "b%u", i);

        ir_block_t *next = ir_block_add(f, nm);
        ir_instr_t *j    = ir_instr_add(at, IR_OP_JUMP);

        ir_instr_operand_block(j, next);
        at = next;

        ir_instr_t *rd = ir_instr_add(at, IR_OP_COPY);

        snprintf(nm, sizeof nm, "r%u", i);
        ir_instr_set_dest(rd, nm, ir_type_parse(m, "bool"));
        ir_instr_operand_local(rd, x);

        ir_instr_t *wr = ir_instr_add(at, IR_OP_COPY);

        ir_instr_set_dest_local(wr, x);
        ir_instr_operand_value(wr, cv);
    }
    ir_instr_add(at, IR_OP_UNREACHABLE);
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED);

    return m;
}

static void test_the_dependence_graph_stays_linear(void)
{
    double before = 0;

    for (uint32_t n = 1000; n <= 2000; n *= 2) {
        ir_ctx_t    *c = ir_ctx_new();
        ir_diags_t  *d = ir_diags_new(c, nullptr);
        ir_module_t *m = staircase(c, n);
        clock_t      a = clock();
        ir_pdg_t    *g = ir_pdg_of(ir_func_at(m, 0));

        clock_t b = clock();

        ir_analyze(m, d);

        clock_t e = clock();

        check("the graph is built inside the budget", ms(a, b) < BUDGET_MS);
        check("and the analyses run inside it too", ms(b, e) < BUDGET_MS);
        check("every use has the one definition that reaches it, not n",
              ir_pdg_edge_count(g) < 4 * n);

        if (fails || getenv("IR_SCALE_REPORT")) {
            printf("      %u definitions and uses of one local: pdg %.1f ms, "
                   "analyze %.1f ms, %u edges\n",
                   n, ms(a, b), ms(b, e), ir_pdg_edge_count(g));
        }

        double now = ms(a, e);

        check("doubling the function does not square the cost",
              !before || now < before * 8);
        before = now > 1.0 ? now : 1.0;
        ir_ctx_free(c);
    }
}

static ir_module_t *webbed(ir_ctx_t *c, uint32_t n, uint32_t locals)
{
    ir_module_t *m  = ir_module_new(c, "m", &ir_profile_bce, "t");
    ir_func_t   *f  = ir_func_add(m, "f", ir_type_parse(m, "(bool) -> void"),
                                  IR_LINK_EXTERNAL);
    ir_value_t  *cv = ir_param_add(f, "c", ir_type_parse(m, "bool"));
    ir_block_t **hd = malloc(sizeof(ir_block_t *) * (n + 1));
    th_type     *bt = ir_type_parse(m, "bool");
    char         nm[32];

    for (uint32_t i = 0; i < locals; i++) {
        snprintf(nm, sizeof nm, "v%u", i);
        ir_local_add(f, nm, bt);
    }

    for (uint32_t i = 0; i <= n; i++) {
        snprintf(nm, sizeof nm, "h%u", i);
        hd[i] = ir_block_add(f, nm);
    }

    for (uint32_t i = 0; i < n; i++) {
        snprintf(nm, sizeof nm, "t%u", i);

        ir_block_t *t = ir_block_add(f, nm);

        snprintf(nm, sizeof nm, "e%u", i);

        ir_block_t *e  = ir_block_add(f, nm);
        ir_instr_t *br = ir_instr_add(hd[i], IR_OP_BRANCH);

        ir_instr_operand_value(br, cv);
        ir_instr_operand_block(br, t);
        ir_instr_operand_block(br, e);

        ir_instr_t *w = ir_instr_add(t, IR_OP_COPY);

        ir_instr_set_dest_local(w, f->locals[i % locals]);
        ir_instr_operand_value(w, cv);

        ir_instr_t *rd = ir_instr_add(e, IR_OP_COPY);

        snprintf(nm, sizeof nm, "r%u", i);
        ir_instr_set_dest(rd, nm, bt);
        ir_instr_operand_local(rd, f->locals[i % locals]);

        ir_instr_t *j1 = ir_instr_add(t, IR_OP_JUMP);
        ir_instr_t *j2 = ir_instr_add(e, IR_OP_JUMP);

        ir_instr_operand_block(j1, hd[i + 1]);
        ir_instr_operand_block(j2, hd[i + 1]);
    }
    ir_instr_add(hd[n], IR_OP_UNREACHABLE);
    free(hd);
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED);

    return m;
}

static void test_promote_is_linear_in_the_locals_too(void)
{
    double before_ms    = 0;
    size_t before_bytes = 0;

    for (uint32_t s = 1; s <= 2; s *= 2) {
        ir_ctx_t    *c    = ir_ctx_new();
        ir_module_t *m    = webbed(c, 1500 * s, 32 * s);
        size_t       base = ir_ctx_bytes(c);
        clock_t      a    = clock();

        check("promote succeeds", ir_promote(m, nullptr));

        double now_ms    = ms(a, clock());
        size_t now_bytes = ir_ctx_bytes(c) - base;

        if (fails || getenv("IR_SCALE_REPORT")) {
            printf("      %u blocks, %u locals: promote %.1f ms, %.1f MB\n",
                   3 * 1500 * s + 1, 32 * s, now_ms,
                   (double)now_bytes / 1048576.0);
        }

        if (before_bytes) {
            check("doubling blocks and locals together does not square "
                  "the memory promote needs",
                  now_bytes < before_bytes * 3);
            check("nor the time it takes",
                  now_ms < before_ms * 3 || now_ms < 20.0);
        }
        before_ms    = now_ms;
        before_bytes = now_bytes;
        ir_ctx_free(c);
    }
}

static void test_the_verifier_is_linear_in_the_phis(void)
{
    double before_ms    = 0;
    size_t before_bytes = 0;

    for (uint32_t s = 1; s <= 2; s *= 2) {
        ir_ctx_t    *c = ir_ctx_new();
        ir_diags_t  *d = ir_diags_new(c, nullptr);
        ir_module_t *m = webbed(c, 1500 * s, 32 * s);

        ir_promote(m, nullptr);

        size_t  base = ir_ctx_bytes(c);
        clock_t a    = clock();

        check("the promoted module verifies as SSA",
              (ir_verify(m, d) & IR_PROP_SSA) != 0);

        double now_ms    = ms(a, clock());
        size_t now_bytes = ir_ctx_bytes(c) - base;

        if (fails || getenv("IR_SCALE_REPORT")) {
            printf("      %u blocks of phis: verify %.1f ms, %.1f MB\n",
                   3 * 1500 * s + 1, now_ms, (double)now_bytes / 1048576.0);
        }

        if (before_bytes) {
            check("doubling the phis does not square the memory the verifier "
                  "needs",
                  now_bytes < before_bytes * 3);
            check("nor the time it takes",
                  now_ms < before_ms * 3 || now_ms < 20.0);
        }
        before_ms    = now_ms;
        before_bytes = now_bytes;
        ir_ctx_free(c);
    }
}

static void test_a_record_is_checked_once_not_once_per_mention(void)
{
    uint32_t before = 0;

    for (uint32_t mentions = 50; mentions <= 400; mentions *= 8) {
        ir_ctx_t    *c   = ir_ctx_new();
        ir_diags_t  *d   = ir_diags_new(c, nullptr);
        ir_module_t *mod = ir_module_new(c, "m", &ir_profile_bce, "t");
        th_type     *rec = ir_type_parse(mod, "(x: i32, y: i32)");
        ir_func_t   *f   = ir_func_add(mod, "f",
                                       ir_type_parse(mod, "() -> void"),
                                       IR_LINK_EXTERNAL);
        ir_block_t  *b   = ir_block_add(f, "entry");

        ir_layout_pin_member(mod, rec, "x", 0, 32);
        ir_layout_pin_member(mod, rec, "y", 16, 32);

        for (uint32_t i = 0; i < mentions; i++) {
            char        nm[32];
            ir_instr_t *in = ir_instr_add(b, IR_OP_UNDEF);

            snprintf(nm, sizeof nm, "v%u", i);
            ir_instr_set_dest(in, nm, rec);
        }
        ir_instr_add(b, IR_OP_RETURN);
        ir_module_establish(mod, IR_PROP_PARSED | IR_PROP_TYPED);
        ir_layout_run(mod, d);
        ir_module_establish(mod, IR_PROP_LAID_OUT);

        ir_verify(mod, d);

        uint32_t said = 0;

        for (uint32_t i = 0; i < ir_diags_count(d); i++) {
            said += ir_diags_at(d, i)->code == IR_DIAG_VERIFY_LAID_OUT;
        }

        if (fails || getenv("IR_SCALE_REPORT")) {
            printf("      a bad record named by %u instructions: %u "
                   "diagnostics\n", mentions, said);
        }
        check("the overlap is reported, once", said == 1);
        check("and naming the type more often does not say it more often",
              !before || said == before);
        before = said;
        ir_ctx_free(c);
    }
}

static ir_module_t *looped(ir_ctx_t *c, uint32_t n)
{
    ir_module_t *m  = ir_module_new(c, "m", &ir_profile_bce, "t");
    ir_func_t   *f  = ir_func_add(m, "f", ir_type_parse(m, "(bool) -> void"),
                                  IR_LINK_EXTERNAL);
    ir_value_t  *cv = ir_param_add(f, "c", ir_type_parse(m, "bool"));
    ir_block_t  *at = ir_block_add(f, "entry");
    char         nm[32];

    for (uint32_t i = 0; i < n; i++) {
        snprintf(nm, sizeof nm, "head%u", i);

        ir_block_t *head = ir_block_add(f, nm);

        snprintf(nm, sizeof nm, "body%u", i);

        ir_block_t *body = ir_block_add(f, nm);

        snprintf(nm, sizeof nm, "out%u", i);

        ir_block_t *out = ir_block_add(f, nm);
        ir_instr_t *j0  = ir_instr_add(at, IR_OP_JUMP);
        ir_instr_t *br  = ir_instr_add(head, IR_OP_BRANCH);
        ir_instr_t *j1  = ir_instr_add(body, IR_OP_JUMP);

        ir_instr_operand_block(j0, head);
        ir_instr_operand_value(br, cv);
        ir_instr_operand_block(br, body);
        ir_instr_operand_block(br, out);
        ir_instr_operand_block(j1, head);
        at = out;
    }
    ir_instr_add(at, IR_OP_UNREACHABLE);
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED);

    return m;
}

static void test_structure_is_linear_in_the_loops(void)
{
    double before_ms    = 0;
    size_t before_bytes = 0;

    for (uint32_t s = 1; s <= 2; s *= 2) {
        ir_ctx_t    *c    = ir_ctx_new();
        ir_diags_t  *d    = ir_diags_new(c, nullptr);
        ir_module_t *m    = looped(c, 600 * s);
        size_t       base = ir_ctx_bytes(c);
        clock_t      a    = clock();

        check("a function of many loops structures", ir_structure(m, d));

        double now_ms    = ms(a, clock());
        size_t now_bytes = ir_ctx_bytes(c) - base;

        if (fails || getenv("IR_SCALE_REPORT")) {
            printf("      %u loops: structure %.1f ms, %.1f MB\n", 600 * s,
                   now_ms, (double)now_bytes / 1048576.0);
        }

        if (before_bytes) {
            check("doubling the loops does not square the memory the "
                  "structurer needs",
                  now_bytes < before_bytes * 3);
            check("nor the time it takes",
                  now_ms < before_ms * 3 || now_ms < 20.0);
        }
        before_ms    = now_ms;
        before_bytes = now_bytes;
        ir_ctx_free(c);
    }
}

static void test_splitting_a_wide_join_is_linear(void)
{
    double before = 0;

    for (uint32_t s = 1; s <= 2; s *= 2) {
        uint32_t     n  = 800 * s;
        ir_ctx_t    *c  = ir_ctx_new();
        ir_diags_t  *d  = ir_diags_new(c, nullptr);
        ir_module_t *m  = ir_module_new(c, "m", &ir_profile_bce, "t");
        ir_func_t   *f  = ir_func_add(m, "f",
                                      ir_type_parse(m, "(bool, i32) -> void"),
                                      IR_LINK_EXTERNAL);
        ir_value_t  *cv = ir_param_add(f, "c", ir_type_parse(m, "bool"));
        ir_value_t  *av = ir_param_add(f, "a", ir_type_parse(m, "i32"));
        ir_block_t  *e  = ir_block_add(f, "entry");
        ir_block_t  *j  = ir_block_add(f, "join");
        ir_instr_t  *sw = ir_instr_add(e, IR_OP_SWITCH);
        char         nm[32];

        ir_instr_operand_value(sw, av);

        for (uint32_t i = 0; i < n; i++) {
            snprintf(nm, sizeof nm, "arm%u", i);

            ir_block_t *arm = ir_block_add(f, nm);

            snprintf(nm, sizeof nm, "tail%u", i);

            ir_block_t *tail = ir_block_add(f, nm);
            ir_instr_t *br   = ir_instr_add(arm, IR_OP_BRANCH);
            ir_instr_t *jt   = ir_instr_add(tail, IR_OP_JUMP);

            ir_instr_operand_value(br, cv);
            ir_instr_operand_block(br, j);
            ir_instr_operand_block(br, tail);
            ir_instr_operand_block(jt, j);
            ir_instr_operand_switch_arm(sw, (ir_int_t)i, arm);
        }
        ir_instr_operand_default(sw, j);

        for (uint32_t i = 0; i < 20; i++) {
            ir_instr_t *phi = ir_instr_add(j, IR_OP_PHI);

            snprintf(nm, sizeof nm, "p%u", i);
            ir_instr_set_dest(phi, nm, ir_type_parse(m, "i32"));

            for (uint32_t k = 0; k < f->n_blocks; k++) {
                if (f->blocks[k] != j) {
                    ir_instr_operand_phi_arm(phi, f->blocks[k], av);
                }
            }
        }
        ir_instr_add(j, IR_OP_RETURN);
        ir_module_establish(m, IR_PROP_PARSED);

        clock_t a = clock();

        check("a wide join splits", ir_split_edges(m, d));

        double now = ms(a, clock());

        check("and every phi still names each predecessor exactly once",
              (ir_verify(m, d) & IR_PROP_PARSED) != 0);

        if (fails || getenv("IR_SCALE_REPORT")) {
            printf("      a join of %u predecessors and 20 phis: "
                   "split-edges %.1f ms\n", n, now);
        }

        if (before) {
            check("doubling the predecessors of a join does not square the "
                  "cost of splitting it",
                  now < before * 3 || now < 5.0);
        }
        before = now;
        ir_ctx_free(c);
    }
}

static void test_parsing_is_linear_in_the_annotations(void)
{
    double before = 0;

    for (uint32_t s = 1; s <= 2; s *= 2) {
        uint32_t n    = 4000 * s;
        size_t   room = (size_t)n * 80 + 256;
        char    *src  = malloc(room);
        size_t   at   = 0;

        at += (size_t)snprintf(src, room,
                               "module m {\n  profile bce\n  target t\n\n"
                               "  func @f(%%a: i32) -> i32 {\n"
                               "    block entry:\n");

        for (uint32_t i = 0; i < n; i++) {
            at += (size_t)snprintf(src + at, room - at,
                                   "      %%v%u: i32 = copy %%a "
                                   "!ir.of(%%v%u)\n", i, i);
        }
        snprintf(src + at, room - at, "      return %%a\n  }\n}\n");

        const ir_profile_t *const profiles[] = {&ir_profile_bce};
        ir_ctx_t                 *c          = ir_ctx_new();
        ir_diags_t               *d          = ir_diags_new(c, nullptr);
        clock_t                   a          = clock();
        ir_module_t              *m          = ir_module_parse(c, src, 0,
                                                               profiles, 1, d);

        double now = ms(a, clock());

        check("a function of many value annotations parses", m != nullptr);

        if (fails || getenv("IR_SCALE_REPORT")) {
            printf("      %u annotations naming a value: parse %.1f ms\n", n,
                   now);
        }

        if (before) {
            check("doubling the annotations does not square the parse",
                  now < before * 3 || now < 20.0);
        }
        before = now;
        free(src);
        ir_ctx_free(c);
    }
}

static ir_rule_result_t decline(ir_module_t *m, ir_instr_t *in, ir_diags_t *d,
                                void *state)
{
    (void)m;
    (void)in;
    (void)d;
    (void)state;

    return (ir_rule_result_t){.verdict = IR_RULE_DECLINE};
}

static void test_typing_does_not_cost_the_rule_count(void)
{
    double before = 0;

    for (uint32_t rules = 256; rules <= 16384; rules *= 64) {
        ir_ctx_t    *c  = ir_ctx_new();
        ir_module_t *m  = ir_module_new(c, "m", &ir_profile_bce, "t");
        ir_rules_t  *rs = ir_rules_new(c);
        ir_func_t   *f  = ir_func_add(m, "f",
                                      ir_type_parse(m, "(i32) -> void"),
                                      IR_LINK_EXTERNAL);
        ir_value_t  *av = ir_param_add(f, "a", ir_type_parse(m, "i32"));
        ir_block_t  *b  = ir_block_add(f, "entry");

        for (uint32_t i = 0; i < rules; i++) {
            char nm[32];

            snprintf(nm, sizeof nm, "filler%u", i);
            ir_rules_add(rs, (ir_rule_t){.name      = nm,
                                         .op        = IR_OP_SELECT,
                                         .match     = IR_MATCH_DEFAULT,
                                         .type_rule = decline});
        }
        ir_rules_add(rs, (ir_rule_t){.name      = "the-one-that-matches",
                                     .op        = IR_OP_ADD,
                                     .match     = IR_MATCH_DEFAULT,
                                     .type_rule = decline});

        for (uint32_t i = 0; i < 20000; i++) {
            char        nm[32];
            ir_instr_t *in = ir_instr_add(b, IR_OP_ADD);

            snprintf(nm, sizeof nm, "v%u", i);
            ir_instr_operand_value(in, av);
            ir_instr_operand_value(in, av);
            ir_instr_set_dest(in, nm, ir_type_parse(m, "i32"));
        }
        ir_instr_add(b, IR_OP_RETURN);

        clock_t  a = clock();
        uint32_t n = 0;

        for (uint32_t i = 0; i < b->n_instrs; i++) {
            if (b->instrs[i]->op == IR_OP_ADD) {
                n += ir_rule_resolve(m, rs, b->instrs[i], nullptr) != nullptr;
            }
        }

        double now = ms(a, clock());

        check("the rule that matches is found for every instruction",
              n == 20000);

        if (fails || getenv("IR_SCALE_REPORT")) {
            printf("      20000 instructions against %u rules: resolve "
                   "%.1f ms\n", rules, now);
        }

        if (before) {
            check("sixty-four times the rules does not cost sixty-four times "
                  "as much to resolve",
                  now < before * 4 || now < 5.0);
        }
        before = now;
        ir_ctx_free(c);
    }
}

static void test_a_long_function_is_not_a_long_call_chain(void)
{
    const uint32_t n = 60000;
    ir_ctx_t      *c = ir_ctx_new();
    ir_diags_t    *d = ir_diags_new(c, nullptr);
    ir_module_t   *m = ir_module_new(c, "m", &ir_profile_bce, "t");
    ir_func_t     *f = ir_func_add(m, "f", ir_type_parse(m, "(i32) -> void"),
                                   IR_LINK_EXTERNAL);
    ir_value_t    *a = ir_param_add(f, "a", ir_type_parse(m, "i32"));
    ir_block_t   **b = malloc(sizeof(ir_block_t *) * n);
    char           nm[24];

    ir_local_add(f, "x", ir_type_parse(m, "i32"));

    for (uint32_t i = 0; i < n; i++) {
        snprintf(nm, sizeof nm, "b%u", i);
        b[i] = ir_block_add(f, nm);
    }

    for (uint32_t i = 0; i + 1 < n; i++) {
        ir_instr_t *w = ir_instr_add(b[i], IR_OP_COPY);
        ir_instr_t *j = ir_instr_add(b[i], IR_OP_JUMP);

        ir_instr_set_dest_local(w, f->locals[0]);
        ir_instr_operand_value(w, a);
        ir_instr_operand_block(j, b[i + 1]);
    }
    ir_instr_add(b[n - 1], IR_OP_UNREACHABLE);
    free(b);
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED);

    clock_t t0 = clock();

    check("a chain of 60000 blocks structures without exhausting the stack",
          ir_structure(m, d));
    check("and promotes, which is the walk that carries a value per path",
          ir_promote(m, nullptr));
    check("both inside the budget", ms(t0, clock()) < BUDGET_MS);

    if (fails || getenv("IR_SCALE_REPORT")) {
        printf("      structure and promote over 60000 blocks: %.1f ms\n",
               ms(t0, clock()));
    }
    ir_ctx_free(c);
}

static void test_the_shapes_that_were_quadratic(void)
{
    ir_ctx_t   *c      = ir_ctx_new();
    ir_diags_t *d      = ir_diags_new(c, nullptr);
    double      before = 0;

    for (uint32_t s = 1; s <= 2; s *= 2) {
        uint32_t lines = 10000 * s;
        size_t   cap   = (size_t)lines * 48 + 512;
        char    *src   = malloc(cap);
        int      at    = snprintf(src, cap,
                                  "module m {\n  profile bce\n  target t\n"
                                  "\n  func @f(%%a: i32) -> void {\n"
                                  "    block entry:\n");

        for (uint32_t i = 0; i < lines; i++) {
            at += snprintf(src + at, cap - (size_t)at,
                           "      %%v%u: i32 = copy %%a\n", i);
        }
        snprintf(src + at, cap - (size_t)at, "      unreachable\n  }\n}\n");

        const ir_profile_t *const profiles[] = {&ir_profile_bce};
        clock_t                   t   = clock();
        ir_module_t              *m   = ir_module_parse(c, src, 0, profiles, 1,
                                                        d);
        double                    now = ms(t, clock());

        check("a long straight-line block parses", m != nullptr);

        if (fails || getenv("IR_SCALE_REPORT")) {
            printf("      %u instructions: parse %.1f ms\n", lines, now);
        }

        if (before) {
            check("without the parse being quadratic in the buffer",
                  now < before * 3 || now < 20.0);
        }
        before = now;
        free(src);
    }

    ir_module_t *sm = ir_module_new(c, "s", &ir_profile_bce, "t");
    ir_func_t   *sf = ir_func_add(sm, "f", ir_type_parse(sm, "(i32) -> void"),
                                  IR_LINK_EXTERNAL);
    ir_value_t  *sv = ir_param_add(sf, "s", ir_type_parse(sm, "i32"));
    ir_block_t  *se = ir_block_add(sf, "entry");
    ir_block_t  *so = ir_block_add(sf, "out");
    ir_instr_t  *sw = ir_instr_add(se, IR_OP_SWITCH);

    ir_instr_operand_value(sw, sv);
    ir_instr_operand_default(sw, so);

    for (uint32_t i = 0; i < 4000; i++) {
        ir_instr_operand_switch_arm(sw, i, so);
    }
    ir_instr_add(so, IR_OP_UNREACHABLE);
    ir_module_establish(sm, IR_PROP_PARSED | IR_PROP_TYPED);

    clock_t t1 = clock();

    ir_structure(sm, d);
    ir_verify(sm, d);
    check("a switch of 4000 arms structures and verifies in one pass each",
          ms(t1, clock()) < BUDGET_MS);

    ir_module_t *tm = ir_module_new(c, "t", &ir_profile_bce, "t");
    ir_func_t   *tf = ir_func_add(tm, "f", ir_type_parse(tm, "() -> void"),
                                  IR_LINK_EXTERNAL);
    ir_block_t  *tb = ir_block_add(tf, "entry");
    char         ty[48];

    for (uint32_t i = 0; i < 4000; i++) {
        snprintf(ty, sizeof ty, "array[i32; %u]", i + 1);

        ir_instr_t *in = ir_instr_add(tb, IR_OP_SIZEOF);

        ir_instr_operand_type(in, ir_type_parse(tm, ty));
    }
    ir_instr_add(tb, IR_OP_UNREACHABLE);
    ir_module_establish(tm, IR_PROP_PARSED | IR_PROP_TYPED);

    clock_t t2 = clock();

    ir_layout_run(tm, d);
    free(ir_module_print(tm));
    check("4000 distinct types lay out and print in one pass each",
          ms(t2, clock()) < BUDGET_MS);

    if (fails || getenv("IR_SCALE_REPORT")) {
        printf("      switch %.1f ms, types %.1f ms\n", ms(t1, t2),
               ms(t2, clock()));
    }
    ir_ctx_free(c);
}

int main(void)
{
    test_the_pipeline_stays_linear();
    test_promote_stays_linear();
    test_the_dependence_graph_stays_linear();
    test_promote_is_linear_in_the_locals_too();
    test_the_verifier_is_linear_in_the_phis();
    test_a_record_is_checked_once_not_once_per_mention();
    test_structure_is_linear_in_the_loops();
    test_splitting_a_wide_join_is_linear();
    test_parsing_is_linear_in_the_annotations();
    test_typing_does_not_cost_the_rule_count();
    test_a_long_function_is_not_a_long_call_chain();
    test_the_shapes_that_were_quadratic();

    return report("ir scale");
}
