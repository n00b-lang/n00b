#include <stdio.h>
#include <stdlib.h>

#include "ir/diag.h"
#include "ir/typelang.h"
#include "ir/verify.h"
#include "lib/buffer.h"
#include "lib/module.h"
#include "th/typehash.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)
#define NO_TEXT ((core_string_t){})

static bool produced[IR_DIAG_COUNT];

static bool contains(core_string_t haystack, core_string_t needle)
{
    return haystack.data && core_string_find(haystack, needle) >= 0;
}

static ir_diag_t *beg(ir_diags_t *d, ir_diagcode_t code, ir_span_t span,
                      core_string_t label)
{
    ir_diag_t *dg = ir_diag_begin(d, code, span,
        label.u8_bytes ? IR_DETAIL(label)
                                     : IR_NO_LABEL);

    if (dg) {
        produced[code] = true;
    }

    return dg;
}

static ir_diag_t *beg_at(ir_diags_t *d, ir_diagcode_t code,
                         const ir_instr_t *in, const ir_block_t *b,
                         const ir_func_t *f, core_string_t label)
{
    ir_diag_t *dg = ir_diag_begin_at(d, code, in, b, f,
        label.u8_bytes ? IR_DETAIL(label)
                                     : IR_NO_LABEL);

    if (dg) {
        produced[code] = true;
    }

    return dg;
}

static const core_string_t SRC = CORE_STRING_STATIC(
    "func sum(n) {\n"
    "  acc = 0\n"
    "  caf\xc3\xa9 = acc + n\n"
    "}\n");

static core_string_t source_of(void *state, int32_t file)
{
    (void)state;

    if (file != 0) {
        return NO_TEXT;
    }

    return SRC;
}

static const core_diagnostic_t *item(const ir_diags_t *d, const ir_diag_t *dg)
{
    return ir_diag_item(d, dg);
}

static uint32_t count_severity(const ir_diags_t *d,
                               core_diag_severity_t severity)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        n += item(d, ir_diags_at(d, i))->severity == severity;
    }

    return n;
}

static ir_module_t *mod(ir_ctx_t *ctx)
{
    ir_module_t *m = ir_module_new(ctx, S("m"), &ir_profile_bce, S("test"));

    ir_file_add(m, S("sum.bce"));

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

        if (!ir_diag_code((ir_diagcode_t)i).data
            || !ir_diag_code_lookup(ir_diag_code((ir_diagcode_t)i), &back)
            || back != (ir_diagcode_t)i) {
            check("every code round-trips through its spelling", false);
            break;
        }
    }

    for (uint16_t i = 0; i < IR_DIAG_COUNT; i++) {
        for (uint16_t j = (uint16_t)(i + 1); j < IR_DIAG_COUNT; j++) {
            if (core_string_eq(ir_diag_code((ir_diagcode_t)i),
                               ir_diag_code((ir_diagcode_t)j))) {
                check("codes are unique", false);

                return;
            }
        }
    }

    for (uint16_t i = 0; i < IR_DIAG_COUNT; i++) {
        core_string_t c = ir_diag_code((ir_diagcode_t)i);

        if (!core_string_starts_with(c, S("IR-"))) {
            printf("FAIL  code %.*s is not namespaced\n", (int)c.u8_bytes,
                   c.data);
            fails++;
        }
    }

    check("a code carries a default severity",
          ir_diag_default_severity(IR_DIAG_INTERNAL) == CORE_DIAG_ERROR
              && ir_diag_default_severity(IR_DIAG_AUTHORED_ANNOT)
                     == CORE_DIAG_WARNING);
}

static void test_diagnostics_attach_to_translation_units(void)
{
    core_module_spec_t module = {0};
    core_source_file_t source = {0};
    core_translation_unit_t unit = {0};
    bool initialized = core_module_spec_init(&module, S("pkg.lowering"))
        && core_source_file_init(&source, S("pkg/lowering.n00b"),
                                 S("value"))
        && core_translation_unit_init(&unit, &module, &source);
    check("a test translation unit initializes", initialized);
    if (!initialized) return;

    ir_ctx_t *ctx = ir_ctx_new();
    core_diagnostics_t *destination =
        core_translation_unit_diagnostics(&unit);
    ir_diags_t *diagnostics = ir_diags_new_attached(ctx, nullptr,
                                                     destination, &unit);
    ir_span_t span = ir_span_make(0, 0, 5, 2, 3, 2, 8);
    ir_diag_t *item = ir_diag_begin(diagnostics, IR_DIAG_UNKNOWN_OPCODE,
                                    span, IR_DETAIL(S("invalid key")));
    ir_diag_message(item, diagnostics,
        IR_DETAIL(S("lowering metadata is invalid")));
    const core_diagnostic_t *attached = core_diagnostics_at(destination, 0);
    check("IR diagnostics land in the owning TU with source identity",
          attached
              && core_string_eq(attached->module, S("pkg.lowering"))
              && core_string_eq(attached->path, S("pkg/lowering.n00b"))
              && attached->location.has_location
              && attached->location.line == 2
              && attached->location.column == 3
              && core_string_eq(attached->message,
                                S("lowering metadata is invalid")));
    check("attached IR errors make the shared TU diagnostics fail",
          ir_diags_failed(diagnostics)
              && core_diagnostics_failed(destination));

    ir_policy_t *policy = ir_policy_new(ctx);
    ir_policy_set(policy, S("IR-W0027"), IR_POLICY_DENY);
    ir_diags_t *promoted = ir_diags_new_attached(ctx, policy,
                                                 destination, &unit);
    ir_diag_t *unset = ir_diag_begin(promoted, IR_DIAG_UNSET_LOCAL,
                                     span, IR_DETAIL(S("unset local")));
    ir_diag_message(unset, promoted, IR_DETAIL(S("read before assignment")));
    const core_diagnostic_t *promoted_item =
        core_diagnostics_at(destination, 1);
    check("attached diagnostics preserve promoted severity",
          promoted_item && promoted_item->severity == CORE_DIAG_ERROR
              && ir_diags_failed(promoted));

    ir_ctx_free(ctx);
    core_translation_unit_release(&unit);
    core_module_spec_release(&module);
    core_source_file_release(&source);
}

static void test_emit(void)
{
    ir_ctx_t   *ctx = ir_ctx_new();
    ir_diags_t *d   = ir_diags_new(ctx, nullptr);

    check("a new accumulator is empty", ir_diags_count(d) == 0);
    check("an empty accumulator has not failed", !ir_diags_failed(d));

    ir_diag_t *dg = beg(d, IR_DIAG_INTERNAL, at(0, 4, 1, 1, 5),
                                  S("here"));

    check("a diagnostic is emitted", dg != nullptr);
    check("it is counted", ir_diags_count(d) == 1);
    check("it takes the code's default severity",
          item(d, dg)->severity == CORE_DIAG_ERROR);
    check("it takes the code's default message",
          core_string_eq(item(d, dg)->message,
                         ir_diag_default_text(IR_DIAG_INTERNAL)));
    check("an error means failure", ir_diags_failed(d));

    ir_diag_message(dg, d, IR_DETAIL(S("custom wording")));
    check("the message is overridable",
          core_string_eq(item(d, dg)->message, S("custom wording")));

    ir_diag_t *spanless = beg(d, IR_DIAG_INTERNAL, IR_SPAN_UNKNOWN, S("x"));

    check("a diagnostic with no span is still recorded, not dropped",
          spanless != nullptr);
    check("and its span is unknown, for the verifier to flag",
          spanless && !ir_span_known(spanless->span));

    beg(d, IR_DIAG_AUTHORED_ANNOT, at(0, 1, 1, 1, 2), NO_TEXT);
    check("errors and warnings accumulate", ir_diags_count(d) == 3);
    check("warnings are counted separately",
          count_severity(d, CORE_DIAG_WARNING) == 1
              && count_severity(d, CORE_DIAG_ERROR) == 2);

    beg(d, IR_DIAG_UNKNOWN_OPCODE, at(0, 1, 1, 1, 2), NO_TEXT);
    check("diagnostics keep accumulating", ir_diags_count(d) == 4);
    check("and the new error is counted with the errors",
          count_severity(d, CORE_DIAG_ERROR) == 3);

    ir_ctx_free(ctx);
}

static void test_policy(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_policy_t *p   = ir_policy_new(ctx);

    check("an empty policy defaults",
          ir_policy_for(p, S("IR-W0005")) == IR_POLICY_DEFAULT);

    ir_policy_set(p, S("IR-W0005"), IR_POLICY_DENY);
    check("policy applies by exact code",
          ir_policy_for(p, S("IR-W0005")) == IR_POLICY_DENY);

    ir_policy_set(p, S("IR-"), IR_POLICY_ALLOW);
    check("a longer match wins over a shorter prefix",
          ir_policy_for(p, S("IR-W0005")) == IR_POLICY_DENY);
    check("the prefix applies to everything else",
          ir_policy_for(p, S("IR-E0006")) == IR_POLICY_ALLOW);

    ir_policy_set(p, S("IR-E"), IR_POLICY_WARN);
    check("the longer prefix wins",
          ir_policy_for(p, S("IR-E0006")) == IR_POLICY_WARN);
    check("the shorter prefix still applies elsewhere",
          ir_policy_for(p, S("IR-W0027")) == IR_POLICY_ALLOW);

    ir_policy_set(p, S("IR-E0006"), IR_POLICY_FORBID);
    check("an exact code beats every prefix",
          ir_policy_for(p, S("IR-E0006")) == IR_POLICY_FORBID);

    ir_ctx_free(ctx);
}

static void test_policy_effects(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_policy_t *p   = ir_policy_new(ctx);

    ir_policy_set(p, S("IR-W0005"), IR_POLICY_ALLOW);

    ir_diags_t *d = ir_diags_new(ctx, p);

    check("an allowed code is suppressed entirely",
          beg(d, IR_DIAG_AUTHORED_ANNOT, at(0, 1, 1, 1, 2), NO_TEXT)
              == nullptr);
    check("nothing was recorded", ir_diags_count(d) == 0);

    ir_policy_set(p, S("IR-W0005"), IR_POLICY_DENY);

    ir_diag_t *dg = beg(d, IR_DIAG_AUTHORED_ANNOT, at(0, 1, 1, 1, 2),
                                  NO_TEXT);

    check("deny promotes a warning to an error",
          dg && item(d, dg)->severity == CORE_DIAG_ERROR);

    ir_policy_set(p, S("IR-E0001"), IR_POLICY_WARN);

    ir_diag_t *dw = beg(d, IR_DIAG_INTERNAL, at(0, 1, 1, 1, 2),
                                  NO_TEXT);

    check("warn demotes an error to a warning",
          dw && item(d, dw)->severity == CORE_DIAG_WARNING);

    ir_policy_set(p, S("IR-W0005"), IR_POLICY_FORBID);

    ir_diag_t *df = beg(d, IR_DIAG_AUTHORED_ANNOT, at(0, 1, 1, 1, 2),
                                  NO_TEXT);

    check("forbid promotes like deny", df && item(d, df)->severity == CORE_DIAG_ERROR);

    ir_ctx_free(ctx);
}

static void test_span_fallback(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = mod(ctx);
    th_type     *t   = ir_type_parse(m, S("i32"));
    th_type     *ft  = ir_type_parse(m, S("(i32) -> i32"));
    ir_func_t   *f   = ir_func_add(m, S("f"), ft, IR_LINK_EXTERNAL);
    ir_block_t  *b   = ir_block_add(f, S("entry"));
    ir_instr_t  *in  = ir_instr_add(b, IR_OP_ADD);

    ir_instr_set_dest(in, S("t0"), t);

    ir_diags_t *d = ir_diags_new(ctx, nullptr);

    f->span = at(0, 0, 1, 1, 2);

    ir_diag_t *g1 = beg_at(d, IR_DIAG_INTERNAL, in, b, f, NO_TEXT);

    check("with only a function span, the function's is used",
          g1 && g1->span.start_line == 1);

    b->span = at(14, 22, 2, 3, 10);

    ir_diag_t *g2 = beg_at(d, IR_DIAG_INTERNAL, in, b, f, NO_TEXT);

    check("a block span beats the function's",
          g2 && g2->span.start_line == 2);

    in->span = at(24, 40, 3, 3, 18);

    ir_diag_t *g3 = beg_at(d, IR_DIAG_INTERNAL, in, b, f, NO_TEXT);

    check("an instruction span beats the block's",
          g3 && g3->span.start_line == 3);

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

    ir_file_add(m, S("b.bce"));
    ir_file_add(m, S("c.bce"));

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

    core_string_t first = NO_TEXT;
    int           order[N];

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
                          NO_TEXT);
        }

        ir_render_t   r = {.module = m};
        core_string_t j = ir_diags_render_json(d, &r);

        if (!first.data) {
            first = j;
            continue;
        }

        if (!core_string_eq(first, j)) {
            check("shuffled input renders identically", false);
            core_free(j.data);
            break;
        }
        core_free(j.data);
    }
    core_free(first.data);

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
    beg(d, IR_DIAG_INTERNAL, at(0, 1, 1, 1, 2), NO_TEXT);

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
        {S("a"), IR_PROP_PARSED, 0, 0, error_pass},
        {S("b"), IR_PROP_PARSED, 0, 0, error_pass},
        {S("c"), IR_PROP_PARSED, 0, 0, count_pass},
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

    const ir_pass_t good = {S("good"), IR_PROP_PARSED, IR_PROP_TYPED,
                            IR_PROP_SSA, count_pass};

    check("a pass declaring requires/establishes/invalidates is valid",
          ir_pass_declares(&good));

    const ir_pass_t nameless = {NO_TEXT, 0, 0, 0, count_pass};
    const ir_pass_t bodiless = {S("x"), 0, 0, 0, nullptr};
    const ir_pass_t confused = {S("x"), 0, IR_PROP_TYPED, IR_PROP_TYPED,
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

static void test_type_errors_import_once(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = mod(ctx);
    th_ctx *types = ir_module_types(m);
    th_type *integer = ir_type_parse(m, S("int"));
    th_type *string = ir_type_parse(m, S("string"));
    ir_diags_t *diags = ir_diags_new(ctx, nullptr);

    check("mismatched types produce a type error",
          integer && string && !th_unify_in_place(types, integer, string)
              && th_error_count(types) == 1);
    check("type errors become IR diagnostics exactly once",
          ir_import_type_errors(m, diags) == 1
              && ir_import_type_errors(m, diags) == 0
              && th_error_count(types) == 0);
    const ir_diag_t *diagnostic = ir_diags_at(diags, 0);
    check("the diagnostic retains both type names",
          diagnostic && diagnostic->code == IR_DIAG_TYPE_MISMATCH
              && contains(item(diags, diagnostic)->message, S("int"))
              && contains(item(diags, diagnostic)->message, S("string")));
    produced[IR_DIAG_TYPE_MISMATCH] = diagnostic != nullptr;
    ir_ctx_free(ctx);
}

static FILE *open_path(core_string_t path, const char *mode)
{
    char *native = core_string_to_cstr(path);
    FILE *f      = fopen(native, mode);

    core_free(native);

    return f;
}

static core_string_t golden_path(core_string_t dir, core_string_t leaf)
{
    core_buffer_t *b = core_buffer_empty();

    core_buffer_append_string(b, dir);
    core_buffer_putc(b, '/');
    core_buffer_append_string(b, leaf);

    return core_buffer_take_string(b);
}

static void write_file(core_string_t path, core_string_t text)
{
    FILE *f = open_path(path, "w");

    if (f) {
        fwrite(text.data, 1, text.u8_bytes, f);
        fclose(f);
    }
}

static bool same_as_golden(core_string_t path, core_string_t got)
{
    FILE *f = open_path(path, "r");

    if (!f) {
        if (getenv("IR_BLESS_GOLDEN")) {
            write_file(path, got);
            printf("NOTE  blessed new golden %.*s\n", (int)path.u8_bytes,
                   path.data);

            return true;
        }

        printf("FAIL  golden %.*s is missing; re-run with IR_BLESS_GOLDEN=1 "
               "to create it\n",
               (int)path.u8_bytes, path.data);

        return false;
    }

    core_buffer_t buf = {0};
    int           c;

    while ((c = fgetc(f)) != EOF) {
        core_buffer_putc(&buf, (char)c);
    }
    fclose(f);

    core_string_t want = core_buffer_string(&buf);
    bool          ok   = core_string_eq(want, got);

    if (!ok) {
        printf("---- got ----\n%.*s---- want ----\n%.*s----\n",
               (int)got.u8_bytes, got.data, (int)want.u8_bytes, want.data);
    }
    core_buffer_release(&buf);

    return ok;
}

static void test_rendering(core_string_t golden_dir)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = mod(ctx);
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);

    ir_diag_t *dg = beg(d, IR_DIAG_BAD_ANNOT_KEY,
                                  at(16, 19, 2, 3, 6), S("this key"));

    ir_diag_message(dg, d, IR_DETAIL(S("annotation key is not namespaced")));
    ir_diag_secondary(d, dg, at(27, 31, 3, 3, 8),
        IR_DETAIL(S("declared here")));
    ir_diag_secondary(d, dg, at(38, 41, 3, 14, 17),
        IR_DETAIL(S("and used here")));
    ir_diag_help(d, dg,
        IR_DETAIL(S("namespace it with the profile: `bce.acc`")));

    beg(d, IR_DIAG_INTERNAL, IR_SPAN_UNKNOWN, NO_TEXT);

    ir_render_t r = {.source = source_of, .module = m};

    core_string_t txt = ir_diags_render_text(d, &r);
    core_string_t p1  = golden_path(golden_dir, S("diag_text.txt"));

    check("human rendering matches its golden", same_as_golden(p1, txt));
    core_free(p1.data);
    core_free(txt.data);

    core_string_t js = ir_diags_render_json(d, &r);
    core_string_t p2 = golden_path(golden_dir, S("diag.json"));

    check("machine rendering matches its golden", same_as_golden(p2, js));
    core_free(p2.data);
    core_free(js.data);

    ir_render_t   bare = {.module = m};
    core_string_t nc   = ir_diags_render_text(d, &bare);

    check("without a source provider the rendering has no carets",
          nc.data && core_string_find_byte(nc, '^') < 0);
    check("without a source provider it still names locations",
          contains(nc, S("sum.bce:2:3")));
    core_free(nc.data);

    core_string_t spanless = ir_diags_render_text(d, &r);
    int64_t       entry    = core_string_find(spanless,
        S("error: internal error"));

    check("a span-less diagnostic still renders", entry >= 0);
    check("and carries no invented location",
          entry >= 0
              && !contains(core_string_slice(spanless, (size_t)entry,
                                             spanless.u8_bytes
                                                 - (size_t)entry),
                           S("sum.bce")));
    check("and sorts after the located ones",
          entry >= 0 && core_string_find(spanless, S("sum.bce:2:3")) < entry);
    core_free(spanless.data);

    ir_ctx_free(ctx);
}


int main(int argc, char **argv)
{
    core_string_t golden = argc > 1 ? core_string_from_cstr(argv[1])
                                    : S(".");

    test_registry();
    test_diagnostics_attach_to_translation_units();
    test_emit();
    test_policy();
    test_policy_effects();
    test_span_fallback();
    test_ordering_is_deterministic();
    test_pass_declarations();
    test_type_errors_import_once();
    test_errors_do_not_stop_the_pipeline();
    test_rendering(golden);
    dump_produced_codes("produced-codes-diag.txt", produced);

    if (argc > 1) {
        core_free(golden.data);
    }

    return report("ir diagnostics");
}
