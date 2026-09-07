#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/diag.h"
#include "ir/verify.h"
#include "ir_test.h"

static bool produced[IR_DIAG_COUNT];

static ir_diag_t *beg(ir_diags_t *d, ir_diagcode_t code, ir_span_t span,
                      const char *label)
{
    ir_diag_t *dg = ir_diag_begin(d, code, span, label);

    if (dg) {
        produced[code] = true;
    }

    return dg;
}

static ir_diag_t *beg_at(ir_diags_t *d, ir_diagcode_t code,
                         const ir_instr_t *in, const ir_block_t *b,
                         const ir_func_t *f, const char *label)
{
    ir_diag_t *dg = ir_diag_begin_at(d, code, in, b, f, label);

    if (dg) {
        produced[code] = true;
    }

    return dg;
}

static bool note(ir_diags_t *d, ir_severity_t sev, ir_diagcode_t code,
                 ir_span_t span, const char *label)
{
    bool ok = ir_diag_note(d, sev, code, span, label);

    if (ok) {
        produced[code] = true;
    }

    return ok;
}

static const char *SRC =
    "func sum(n) {\n"
    "  acc = 0\n"
    "  caf\xc3\xa9 = acc + n\n"
    "}\n";

static const char *source_of(void *state, int32_t file, size_t *len)
{
    (void)state;

    if (file != 0) {
        return nullptr;
    }
    *len = strlen(SRC);

    return SRC;
}

static ir_module_t *mod(ir_ctx_t *ctx)
{
    ir_module_t *m = ir_module_new(ctx, "m", &ir_profile_bce, "test");

    ir_file_add(m, "sum.bce");

    return m;
}

static ir_span_t at(uint32_t sb, uint32_t eb, uint32_t line, uint32_t col,
                    uint32_t ecol)
{
    return ir_span_make(0, sb, eb, line, col, line, ecol);
}

static void test_registry(void)
{
    check("the code registry is populated", IR_DIAG_COUNT >= 8);

    for (uint16_t i = 0; i < IR_DIAG_COUNT; i++) {
        ir_diagcode_t back;

        if (!ir_diag_code((ir_diagcode_t)i)
            || !ir_diag_code_lookup(ir_diag_code((ir_diagcode_t)i), &back)
            || back != (ir_diagcode_t)i) {
            check("every code round-trips through its spelling", false);
            break;
        }
    }

    for (uint16_t i = 0; i < IR_DIAG_COUNT; i++) {
        for (uint16_t j = (uint16_t)(i + 1); j < IR_DIAG_COUNT; j++) {
            if (!strcmp(ir_diag_code((ir_diagcode_t)i),
                        ir_diag_code((ir_diagcode_t)j))) {
                check("codes are unique", false);

                return;
            }
        }
    }

    for (uint16_t i = 0; i < IR_DIAG_COUNT; i++) {
        const char *c = ir_diag_code((ir_diagcode_t)i);

        if (strncmp(c, "IR-", 3)) {
            printf("FAIL  code %s is not namespaced\n", c);
            fails++;
        }
    }

    check("severities are named",
          !strcmp(ir_severity_name(IR_SEV_ERROR), "error")
              && !strcmp(ir_severity_name(IR_SEV_WARNING), "warning")
              && !strcmp(ir_severity_name(IR_SEV_NOTE), "note")
              && !strcmp(ir_severity_name(IR_SEV_HELP), "help"));

    check("a code carries a default severity",
          ir_diag_default_severity(IR_DIAG_INTERNAL) == IR_SEV_ERROR
              && ir_diag_default_severity(IR_DIAG_AUTHORED_ANNOT)
                     == IR_SEV_WARNING);
}

static void test_emit(void)
{
    ir_ctx_t   *ctx = ir_ctx_new();
    ir_diags_t *d   = ir_diags_new(ctx, nullptr);

    check("a new accumulator is empty", ir_diags_count(d) == 0);
    check("an empty accumulator has not failed", !ir_diags_failed(d));

    ir_diag_t *dg = beg(d, IR_DIAG_INTERNAL, at(0, 4, 1, 1, 5),
                                  "here");

    check("a diagnostic is emitted", dg != nullptr);
    check("it is counted", ir_diags_count(d) == 1);
    check("it takes the code's default severity",
          dg->severity == IR_SEV_ERROR);
    check("it takes the code's default message",
          !strcmp(dg->message, ir_diag_default_text(IR_DIAG_INTERNAL)));
    check("an error means failure", ir_diags_failed(d));

    ir_diag_message(dg, d, "custom wording");
    check("the message is overridable", !strcmp(dg->message, "custom wording"));

    ir_diag_t *spanless = beg(d, IR_DIAG_INTERNAL, IR_SPAN_UNKNOWN, "x");

    check("a diagnostic with no span is still recorded, not dropped",
          spanless != nullptr);
    check("and its span is unknown, for the verifier to flag",
          spanless && !ir_span_known(spanless->primary.span));

    beg(d, IR_DIAG_AUTHORED_ANNOT, at(0, 1, 1, 1, 2), nullptr);
    check("errors and warnings accumulate", ir_diags_count(d) == 3);
    check("warnings are counted separately",
          ir_diags_count_of(d, IR_SEV_WARNING) == 1
              && ir_diags_count_of(d, IR_SEV_ERROR) == 2);

    beg(d, IR_DIAG_UNKNOWN_OPCODE, at(0, 1, 1, 1, 2), nullptr);
    check("diagnostics keep accumulating", ir_diags_count(d) == 4);
    check("and the new error is counted with the errors",
          ir_diags_count_of(d, IR_SEV_ERROR) == 3);

    ir_ctx_free(ctx);
}

static void test_notes(void)
{
    ir_ctx_t   *ctx = ir_ctx_new();
    ir_diags_t *d   = ir_diags_new(ctx, nullptr);

    check("a note with nothing to attach to is refused",
          !note(d, IR_SEV_NOTE, IR_DIAG_INTERNAL, at(0, 1, 1, 1, 2),
                        "orphan"));
    check("a help with nothing to attach to is refused",
          !note(d, IR_SEV_HELP, IR_DIAG_INTERNAL, at(0, 1, 1, 1, 2),
                        "orphan"));
    check("neither is recorded as a note", ir_diags_count(d) == 2);

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        const ir_diag_t *dg = ir_diags_at(d, i);

        produced[dg->code] = true;
        check("the orphan is reported rather than dropped in silence",
              dg->code == IR_DIAG_ORPHAN_NOTE
                  && dg->severity == IR_SEV_ERROR);
    }

    beg(d, IR_DIAG_INTERNAL, at(0, 1, 1, 1, 2), "anchor");

    check("a note attaches to a preceding diagnostic",
          note(d, IR_SEV_NOTE, IR_DIAG_INTERNAL, at(0, 1, 1, 1, 2),
                       "attached"));
    check("a note may have an unknown span",
          note(d, IR_SEV_NOTE, IR_DIAG_INTERNAL, IR_SPAN_UNKNOWN,
                       "spanless"));
    check("an error severity is refused by the note API",
          !note(d, IR_SEV_ERROR, IR_DIAG_INTERNAL,
                        at(0, 1, 1, 1, 2), "x"));

    ir_ctx_free(ctx);
}

static void test_policy(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_policy_t *p   = ir_policy_new(ctx);

    check("an empty policy defaults",
          ir_policy_for(p, "IR-W0005") == IR_POLICY_DEFAULT);

    ir_policy_set(p, "IR-W0005", IR_POLICY_DENY);
    check("policy applies by exact code",
          ir_policy_for(p, "IR-W0005") == IR_POLICY_DENY);

    ir_policy_set(p, "IR-", IR_POLICY_ALLOW);
    check("a longer match wins over a shorter prefix",
          ir_policy_for(p, "IR-W0005") == IR_POLICY_DENY);
    check("the prefix applies to everything else",
          ir_policy_for(p, "IR-E0006") == IR_POLICY_ALLOW);

    ir_policy_set(p, "IR-E", IR_POLICY_WARN);
    check("the longer prefix wins",
          ir_policy_for(p, "IR-E0006") == IR_POLICY_WARN);
    check("the shorter prefix still applies elsewhere",
          ir_policy_for(p, "IR-W0027") == IR_POLICY_ALLOW);

    ir_policy_set(p, "IR-E0006", IR_POLICY_FORBID);
    check("an exact code beats every prefix",
          ir_policy_for(p, "IR-E0006") == IR_POLICY_FORBID);

    ir_ctx_free(ctx);
}

static void test_policy_effects(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_policy_t *p   = ir_policy_new(ctx);

    ir_policy_set(p, "IR-W0005", IR_POLICY_ALLOW);

    ir_diags_t *d = ir_diags_new(ctx, p);

    check("an allowed code is suppressed entirely",
          beg(d, IR_DIAG_AUTHORED_ANNOT, at(0, 1, 1, 1, 2), nullptr)
              == nullptr);
    check("nothing was recorded", ir_diags_count(d) == 0);

    ir_policy_set(p, "IR-W0005", IR_POLICY_DENY);

    ir_diag_t *dg = beg(d, IR_DIAG_AUTHORED_ANNOT, at(0, 1, 1, 1, 2),
                                  nullptr);

    check("deny promotes a warning to an error",
          dg && dg->severity == IR_SEV_ERROR);

    ir_policy_set(p, "IR-E0001", IR_POLICY_WARN);

    ir_diag_t *dw = beg(d, IR_DIAG_INTERNAL, at(0, 1, 1, 1, 2),
                                  nullptr);

    check("warn demotes an error to a warning",
          dw && dw->severity == IR_SEV_WARNING);

    ir_policy_set(p, "IR-W0005", IR_POLICY_FORBID);

    ir_diag_t *df = beg(d, IR_DIAG_AUTHORED_ANNOT, at(0, 1, 1, 1, 2),
                                  nullptr);

    check("forbid promotes like deny", df && df->severity == IR_SEV_ERROR);

    ir_ctx_free(ctx);
}

static void test_span_fallback(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = mod(ctx);
    th_type     *t   = ir_type_parse(m, "i32");
    th_type     *ft  = ir_type_parse(m, "(i32) -> i32");
    ir_func_t   *f   = ir_func_add(m, "f", ft, IR_LINK_EXTERNAL);
    ir_block_t  *b   = ir_block_add(f, "entry");
    ir_instr_t  *in  = ir_instr_add(b, IR_OP_ADD);

    ir_instr_set_dest(in, "t0", t);

    ir_diags_t *d = ir_diags_new(ctx, nullptr);

    f->span = at(0, 0, 1, 1, 2);

    ir_diag_t *g1 = beg_at(d, IR_DIAG_INTERNAL, in, b, f, nullptr);

    check("with only a function span, the function's is used",
          g1 && g1->primary.span.start_line == 1);

    b->span = at(14, 22, 2, 3, 10);

    ir_diag_t *g2 = beg_at(d, IR_DIAG_INTERNAL, in, b, f, nullptr);

    check("a block span beats the function's",
          g2 && g2->primary.span.start_line == 2);

    in->span = at(24, 40, 3, 3, 18);

    ir_diag_t *g3 = beg_at(d, IR_DIAG_INTERNAL, in, b, f, nullptr);

    check("an instruction span beats the block's",
          g3 && g3->primary.span.start_line == 3);

    ir_ctx_free(ctx);
}

static void test_ordering(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = mod(ctx);

    ir_file_add(m, "b.bce");

    ir_diags_t *d = ir_diags_new(ctx, nullptr);

    beg(d, IR_DIAG_INTERNAL, ir_span_make(1, 5, 6, 1, 1, 1, 2), "z");
    beg(d, IR_DIAG_INTERNAL, at(40, 41, 3, 1, 2), "c");
    beg(d, IR_DIAG_INTERNAL, at(4, 5, 1, 5, 6), "a");
    beg(d, IR_DIAG_INTERNAL, at(20, 21, 2, 3, 4), "b");
    ir_diags_sort(d);

    const char *want[] = {"a", "b", "c", "z"};

    for (uint32_t i = 0; i < 4; i++) {
        const ir_diag_t *g = ir_diags_at(d, i);

        if (!g || !g->primary.label || strcmp(g->primary.label, want[i])) {
            printf("FAIL  ordering: slot %u is %s, wanted %s\n", i,
                   g && g->primary.label ? g->primary.label : "?", want[i]);
            fails++;
        }
    }

    ir_diags_t *e = ir_diags_new(ctx, nullptr);
    ir_span_t   s = at(4, 5, 1, 5, 6);

    beg(e, IR_DIAG_MISSING_PROFILE, s, "second");
    beg(e, IR_DIAG_INTERNAL, s, "first");
    ir_diags_sort(e);
    check("at one position, ties break by code",
          !strcmp(ir_diags_at(e, 0)->primary.label, "first"));

    ir_diags_t *g = ir_diags_new(ctx, nullptr);

    beg(g, IR_DIAG_INTERNAL, s, "one");
    beg(g, IR_DIAG_INTERNAL, s, "two");
    beg(g, IR_DIAG_INTERNAL, s, "three");
    ir_diags_sort(g);
    check("identical codes at one position keep insertion order",
          !strcmp(ir_diags_at(g, 0)->primary.label, "one")
              && !strcmp(ir_diags_at(g, 1)->primary.label, "two")
              && !strcmp(ir_diags_at(g, 2)->primary.label, "three"));

    ir_ctx_free(ctx);
}

static uint64_t rng = 0x243F6A8885A308D3u;

static uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;

    return n ? (uint32_t)(rng % n) : 0;
}

static void test_ordering_is_deterministic(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = mod(ctx);

    ir_file_add(m, "b.bce");
    ir_file_add(m, "c.bce");

    enum { N = 12 };

    struct {
        int32_t       file;
        uint32_t      byte;
        ir_diagcode_t code;
    } set[N];

    const ir_diagcode_t pool[] = {IR_DIAG_INTERNAL, IR_DIAG_MISSING_PROFILE,
                                  IR_DIAG_AUTHORED_ANNOT};

    for (int k = 0; k < N; k++) {
        set[k].file = (int32_t)(k % 3);
        set[k].byte = (uint32_t)(k * 7);
        set[k].code = pool[k % 3];
    }

    char *first = nullptr;
    int   order[N];

    for (int k = 0; k < N; k++) {
        order[k] = k;
    }

    for (int trial = 0; trial < 200; trial++) {
        for (int k = N - 1; k > 0; k--) {
            int j = (int)rnd((uint32_t)k + 1);
            int t = order[k];

            order[k] = order[j];
            order[j] = t;
        }

        ir_diags_t *d = ir_diags_new(ctx, nullptr);

        for (int k = 0; k < N; k++) {
            uint32_t by = set[order[k]].byte;

            beg(d, set[order[k]].code,
                          ir_span_make(set[order[k]].file, by, by + 1,
                                       by / 8 + 1, by % 8 + 1, by / 8 + 1,
                                       by % 8 + 2),
                          nullptr);
        }

        ir_render_t r = {.module = m};
        char       *j = ir_diags_render_json(d, &r);

        if (!first) {
            first = j;
            continue;
        }

        if (strcmp(first, j)) {
            check("shuffled input renders identically", false);
            free(j);
            break;
        }
        free(j);
    }
    free(first);

    ir_ctx_free(ctx);
}

static int ran;

static bool count_pass(ir_module_t *m, ir_diags_t *d, void *state)
{
    (void)m;
    (void)d;
    (void)state;
    ran++;

    return true;
}

static bool error_pass(ir_module_t *m, ir_diags_t *d, void *state)
{
    (void)m;
    (void)state;
    ran++;
    beg(d, IR_DIAG_INTERNAL, at(0, 1, 1, 1, 2), nullptr);

    return true;
}

static void run_pipeline(const ir_pass_t *passes, size_t n, ir_module_t *m,
                         ir_diags_t *d)
{
    for (size_t i = 0; i < n; i++) {
        ir_pass_run(&passes[i], m, nullptr, nullptr, d);
    }
}

static void test_errors_do_not_stop_the_pipeline(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = mod(ctx);
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);

    ir_module_establish(m, IR_PROP_PARSED);

    const ir_pass_t errs[] = {
        {"a", IR_PROP_PARSED, 0, 0, error_pass},
        {"b", IR_PROP_PARSED, 0, 0, error_pass},
        {"c", IR_PROP_PARSED, 0, 0, count_pass},
    };

    ran = 0;
    run_pipeline(errs, 3, m, d);
    check("errors do not stop the pipeline; one run reports many",
          ran == 3 && ir_diags_count(d) == 2 && ir_diags_failed(d));

    ir_ctx_free(ctx);
}

static void test_pass_declarations(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = mod(ctx);

    const ir_pass_t good = {"good", IR_PROP_PARSED, IR_PROP_TYPED,
                            IR_PROP_SSA, count_pass};

    check("a pass declaring requires/establishes/invalidates is valid",
          ir_pass_declares(&good));

    const ir_pass_t nameless = {nullptr, 0, 0, 0, count_pass};
    const ir_pass_t bodiless = {"x", 0, 0, 0, nullptr};
    const ir_pass_t confused = {"x", 0, IR_PROP_TYPED, IR_PROP_TYPED,
                                count_pass};

    check("a pass with no name is invalid", !ir_pass_declares(&nameless));
    check("a pass with no body is invalid", !ir_pass_declares(&bodiless));
    check("a pass that both establishes and invalidates one property "
          "is invalid",
          !ir_pass_declares(&confused));

    ir_props_t unmet = 0;

    ran = 0;
    check("a pass whose requirements are unmet does not run",
          ir_pass_run(&good, m, nullptr, &unmet, nullptr) == IR_PASS_UNMET && !ran);
    check("the unmet requirement is reported", unmet == IR_PROP_PARSED);

    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_SSA);
    ran = 0;
    check("a pass whose requirements are met runs",
          ir_pass_run(&good, m, nullptr, &unmet, nullptr) == IR_PASS_OK && ran == 1);
    check("what it establishes is established",
          ir_module_props(m) & IR_PROP_TYPED);
    check("what it invalidates is invalidated",
          !(ir_module_props(m) & IR_PROP_SSA));
    check("what it does not name is untouched",
          ir_module_props(m) & IR_PROP_PARSED);

    check("an undeclared pass is refused",
          ir_pass_run(&confused, m, nullptr, nullptr, nullptr) == IR_PASS_INVALID);

    ir_ctx_free(ctx);
}

static void test_merge(void)
{
    ir_ctx_t   *ctx = ir_ctx_new();
    ir_diags_t *a   = ir_diags_new(ctx, nullptr);
    ir_diags_t *b   = ir_diags_new(ctx, nullptr);

    beg(a, IR_DIAG_INTERNAL, at(0, 1, 1, 1, 2), "a1");
    beg(b, IR_DIAG_INTERNAL, at(4, 5, 1, 5, 6), "b1");
    beg(b, IR_DIAG_AUTHORED_ANNOT, at(6, 7, 1, 7, 8), "b2");

    check("merge succeeds", ir_diags_merge(a, b));
    check("merged diagnostics are counted", ir_diags_count(a) == 3);
    check("and their severities come across",
          ir_diags_count_of(a, IR_SEV_ERROR) == 2
              && ir_diags_count_of(a, IR_SEV_WARNING) == 1);

    ir_ctx_t   *other = ir_ctx_new();
    ir_diags_t *far   = ir_diags_new(other, nullptr);

    beg(far, IR_DIAG_INTERNAL, at(0, 1, 1, 1, 2), "elsewhere");
    check("merging across contexts is refused", !ir_diags_merge(a, far));
    check("the refused merge changed nothing", ir_diags_count(a) == 3);

    ir_ctx_free(other);
    ir_ctx_free(ctx);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static bool same_as_golden(const char *path, const char *got)
{
    FILE *f = fopen(path, "r");

    if (!f) {
        if (getenv("IR_BLESS_GOLDEN")) {
            write_file(path, got);
            printf("NOTE  blessed new golden %s\n", path);

            return true;
        }

        printf("FAIL  golden %s is missing; re-run with IR_BLESS_GOLDEN=1 to "
               "create it\n",
               path);

        return false;
    }

    char  *buf = nullptr;
    size_t cap = 0, len = 0;
    int    c;

    while ((c = fgetc(f)) != EOF) {
        if (len + 2 > cap) {
            cap = cap ? cap * 2 : 1024;
            buf = realloc(buf, cap);
        }
        buf[len++] = (char)c;
    }
    fclose(f);

    if (!buf) {
        buf = calloc(1, 1);
    }
    else {
        buf[len] = 0;
    }

    bool ok = !strcmp(buf, got);

    if (!ok) {
        printf("---- got ----\n%s---- want ----\n%s----\n", got, buf);
    }
    free(buf);

    return ok;
}

static void test_rendering(const char *golden_dir)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = mod(ctx);
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);

    ir_diag_t *dg = beg(d, IR_DIAG_BAD_ANNOT_KEY,
                                  at(16, 19, 2, 3, 6), "this key");

    ir_diag_message(dg, d, "annotation key is not namespaced");
    ir_diag_secondary(d, dg, at(27, 31, 3, 3, 8), "declared here");
    ir_diag_secondary(d, dg, at(38, 41, 3, 14, 17), "and used here");
    ir_diag_suggest(d, dg, at(16, 19, 2, 3, 6), "bce.acc",
                    "namespace it with the profile");

    ir_origin_t *src = ir_origin_new(ctx, IR_ORIGIN_SOURCE,
                                     at(16, 19, 2, 3, 6), nullptr, nullptr);
    ir_origin_t *exp = ir_origin_new(ctx, IR_ORIGIN_EXPANDED_FROM,
                                     at(0, 4, 1, 1, 5), "macro", src);
    ir_origin_t *inl = ir_origin_new(ctx, IR_ORIGIN_INLINED_FROM,
                                     at(27, 31, 3, 3, 8), "inline", exp);

    ir_diag_origin(dg, inl);
    note(d, IR_SEV_NOTE, IR_DIAG_BAD_ANNOT_KEY, at(0, 4, 1, 1, 5),
                 "the enclosing function");

    beg(d, IR_DIAG_INTERNAL, IR_SPAN_UNKNOWN, nullptr);

    ir_render_t r = {.source = source_of, .module = m};

    char *txt = ir_diags_render_text(d, &r);
    char  p1[512];

    snprintf(p1, sizeof(p1), "%s/diag_text.txt", golden_dir);
    check("human rendering matches its golden", same_as_golden(p1, txt));
    free(txt);

    char *js = ir_diags_render_json(d, &r);
    char  p2[512];

    snprintf(p2, sizeof(p2), "%s/diag.json", golden_dir);
    check("machine rendering matches its golden", same_as_golden(p2, js));
    free(js);

    char *chain = ir_diags_render_text(d, &r);
    char *inner = strstr(chain, "inlined from here");
    char *outer = strstr(chain, "expanded from here");

    check("origin notes render innermost first",
          inner && outer && inner < outer);
    check("a SOURCE origin is not rendered as a note",
          !strstr(chain, "from here by (null)"));

    char *arrow = strstr(chain, "--> sum.bce:2:3");

    check("the primary location precedes the origin notes",
          arrow && inner && arrow < inner);
    free(chain);

    ir_render_t bare = {.module = m};
    char       *nc   = ir_diags_render_text(d, &bare);

    check("without a source provider the rendering has no carets",
          nc && !strchr(nc, '^'));
    check("without a source provider it still names locations",
          nc && strstr(nc, "sum.bce:2:3"));
    free(nc);

    char *spanless = ir_diags_render_text(d, &r);
    char *entry    = strstr(spanless, "error[IR-E0001]: internal error");

    check("a span-less diagnostic still renders", entry != nullptr);
    check("and carries no invented location",
          entry && !strstr(entry, "-->"));
    check("and sorts after the located ones",
          entry && strstr(spanless, "sum.bce:2:3") < entry);
    free(spanless);

    ir_ctx_free(ctx);
}

static void report_coverage(const char *dir)
{
    const char *leaf = "/../produced-codes-diag.txt";
    size_t      n    = strlen(dir) + strlen(leaf) + 1;
    char       *path = malloc(n);

    if (!path) {
        check("the produced-code list could be written", false);

        return;
    }
    snprintf(path, n, "%s%s", dir, leaf);

    FILE *f = fopen(path, "w");

    if (!f) {
        check("the produced-code list could be written", false);
        free(path);

        return;
    }

    for (uint16_t i = 0; i < IR_DIAG_COUNT; i++) {
        if (produced[i]) {
            fprintf(f, "%s\n", ir_diag_code((ir_diagcode_t)i));
        }
    }
    fclose(f);
    free(path);
}

int main(int argc, char **argv)
{
    const char *golden = argc > 1 ? argv[1] : ".";

    test_registry();
    test_emit();
    test_notes();
    test_policy();
    test_policy_effects();
    test_span_fallback();
    test_ordering();
    test_ordering_is_deterministic();
    test_pass_declarations();
    test_errors_do_not_stop_the_pipeline();
    test_merge();
    test_rendering(golden);
    report_coverage(golden);

    return report("ir diagnostics");
}
