#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/grammar.h"
#include "ir/layout.h"
#include "ir/pass.h"
#include "ir/text.h"
#include "ir/verify.h"

#include "parse/grammar.h"
#include "parse/pwz.h"
#include "scanner/token_stream.h"
#include "harness.h"

#define FULL_TRIALS 400

static uint32_t trials = FULL_TRIALS;

static int  gen_ops[IR_OP_COUNT];
static bool produced[IR_DIAG_COUNT];

static const ir_profile_t *const PROFILES[] = {&ir_profile_bce,
                                               &ir_profile_synth};

static ir_module_t *parse(ir_ctx_t *ctx, const char *text, ir_diags_t **out)
{
    ir_diags_t *d = ir_diags_new(ctx, nullptr);

    if (out) {
        *out = d;
    }

    ir_module_t *m = ir_module_parse(ctx, text, 0, PROFILES, 2, d);

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        produced[ir_diags_at(d, i)->code] = true;
    }

    return m;
}


static bool same_as_golden(const char *dir, const char *name, const char *got)
{
    size_t n    = strlen(dir) + strlen(name) + 2;
    char  *path = malloc(n);

    if (!path) {
        return false;
    }
    snprintf(path, n, "%s/%s", dir, name);

    FILE *f = fopen(path, "r");

    if (!f) {
        if (!getenv("IR_BLESS_GOLDEN")) {
            printf("FAIL  golden %s is missing; re-run with "
                   "IR_BLESS_GOLDEN=1 to create it\n",
                   path);
            free(path);

            return false;
        }

        f = fopen(path, "w");

        if (f) {
            fputs(got, f);
            fclose(f);
            printf("NOTE  blessed new golden %s\n", path);
        }
        free(path);

        return f != nullptr;
    }

    fseek(f, 0, SEEK_END);

    long   len  = ftell(f);
    char  *want = malloc((size_t)len + 1);

    rewind(f);

    if (!want || fread(want, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(want);
        free(path);

        return false;
    }
    want[len] = 0;
    fclose(f);

    bool ok = !strcmp(want, got);

    if (!ok) {
        printf("---- got ----\n%s---- want ----\n%s----\n", got, want);
    }
    free(want);
    free(path);

    return ok;
}

static bool derives(const char *text)
{
    parse_grammar_t      *g  = ir_grammar();
    parse_token_stream_t *ts = ir_lex(text, strlen(text));

    if (!g || !ts) {
        return false;
    }

    parse_pwz_parser_t *p
        = parse_pwz_new_for_start(g, ir_grammar_nt("module"));
    bool              ok = parse_pwz_parse(p, ts);

    parse_pwz_free(p);
    scan_token_stream_free(ts);

    return ok;
}

static bool round_trips(ir_ctx_t *ctx, const char *text, char **printed)
{
    ir_diags_t  *d = nullptr;
    ir_module_t *m = parse(ctx, text, &d);

    if (m && !derives(text)) {
        printf("---- the grammar does not derive what the parser read ----\n"
               "%s",
               text);
        fails++;
    }

    if (!m) {
        char *r = ir_diags_render_text(d, &(ir_render_t){0});

        printf("---- first parse failed ----\n%s---- source ----\n%s", r, text);
        free(r);

        if (printed) {
            *printed = nullptr;
        }

        return false;
    }

    char *once = ir_module_print(m);

    if (printed) {
        *printed = once;
    }

    ir_diags_t  *d2 = nullptr;
    ir_module_t *m2 = parse(ctx, once, &d2);

    if (!m2) {
        char *r = ir_diags_render_text(d2, &(ir_render_t){0});

        printf("---- reparse failed ----\n%s---- printed ----\n%s", r, once);
        free(r);

        return false;
    }

    char *twice = ir_module_print(m2);
    bool  ok    = !strcmp(once, twice);

    if (!ok) {
        printf("---- first ----\n%s---- second ----\n%s----\n", once, twice);
    }
    free(twice);

    return ok;
}

static void test_lexis(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    const char *src =
        "# a leading comment\n"
        "module m {\n"
        "  profile bce\n"
        "  target host\n"
        "\n"
        "  global @g: i32\n"
        "\n"
        "  func @f(%n: i32) -> i32 {\n"
        "    local $x: i32\n"
        "\n"
        "    block entry:\n"
        "      # an instruction comment\n"
        "      %t: i32 = add wrap %n, %n\n"
        "      $x = copy %t\n"
        "      %p: ptr[i32] = alloca i32\n"
        "      store %p, %t\n"
        "      %l: i32 = load volatile %p\n"
        "      %s: u64 = sizeof (i32, i32) -> i32\n"
        "      %c: bool = lt %t, %n\n"
        "      branch %c, a, b\n"
        "\n"
        "    block a:\n"
        "      jump b\n"
        "\n"
        "    block b:\n"
        "      %ph: i32 = phi [entry: %t], [a: %t]\n"
        "      switch %t, default: a, [1: a], [2: b]\n"
        "  }\n"
        "}\n";

    ir_diags_t  *d = nullptr;
    ir_module_t *m = parse(ctx, src, &d);

    check("a module with every lexical element parses", m != nullptr);

    if (!m) {
        char *r = ir_diags_render_text(d, &(ir_render_t){0});

        printf("%s", r);
        free(r);
        ir_ctx_free(ctx);

        return;
    }

    check("the module name is read", !strcmp(ir_module_name(m), "m"));
    check("the profile is resolved",
          ir_module_profile(m) == &ir_profile_bce);
    check("the target is read", !strcmp(ir_module_target(m), "host"));
    check("a global is read", ir_global_find(m, "g") != nullptr);

    ir_func_t *f = ir_func_find(m, "f");

    check("a function is read", f != nullptr);
    check("a parameter is read", f && f->n_params == 1);
    check("a local is read", f && f->n_locals == 1);
    check("blocks are read", f && f->n_blocks == 3);

    ir_block_t *e = f ? ir_block_entry(f) : nullptr;

    check("the entry block comes first", e && !strcmp(e->name, "entry"));
    check("instructions are read", e && e->n_instrs == 8);

    if (e && e->n_instrs >= 1) {
        ir_instr_t *add = e->instrs[0];

        check("an opcode resolves", add->op == IR_OP_ADD);
        check("a modifier is read", ir_instr_has_mod(add, "wrap"));
        check("a value destination is read",
              add->dest_kind == IR_DEST_VALUE
                  && !strcmp(add->dest->name, "t"));
        check("operands are read and resolved",
              add->n_operands == 2 && add->operands[0].value
                  && !strcmp(add->operands[0].value->name, "n"));
        check("an instruction comment is kept",
              ir_annot_get(&add->annots, "ir.comment") != nullptr);
    }

    if (e && e->n_instrs >= 2) {
        check("a local destination is read",
              e->instrs[1]->dest_kind == IR_DEST_LOCAL);
    }

    if (e && e->n_instrs >= 6) {
        check("a type operand with commas is read",
              e->instrs[5]->op == IR_OP_SIZEOF
                  && e->instrs[5]->n_operands == 1
                  && e->instrs[5]->operands[0].type != nullptr);
    }

    check("a module comment is kept",
          ir_annot_get(ir_module_annots(m), "ir.comment") != nullptr);

    ir_block_t *b = ir_block_find(f, "b");

    check("a phi is read",
          b && b->n_instrs >= 1 && b->instrs[0]->op == IR_OP_PHI
              && b->instrs[0]->n_operands == 2
              && b->instrs[0]->operands[0].phi.value != nullptr);
    check("a switch is read",
          b && b->n_instrs >= 2 && b->instrs[1]->op == IR_OP_SWITCH
              && b->instrs[1]->n_operands == 4);

    check("the whole thing round-trips", round_trips(ctx, src, nullptr));

    ir_ctx_free(ctx);
}

static void test_whitespace_and_comments(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    const char *tight =
        "module m{\nprofile bce\nfunc @f()->void{\nblock e:\nreturn\n}\n}\n";
    const char *loose =
        "module m {\n\n  profile bce\n\n\n  func @f() -> void {\n"
        "    block e:\n\n      return\n  }\n}\n";

    char *a = nullptr;
    char *b = nullptr;

    check("a tightly spaced module parses", round_trips(ctx, tight, &a));
    check("a loosely spaced module parses", round_trips(ctx, loose, &b));
    check("whitespace does not survive: both print identically",
          a && b && !strcmp(a, b));
    free(a);
    free(b);

    const char *commented =
        "module m {\n  profile bce\n\n  func @f() -> void {\n"
        "    block e:\n      # keep me\n      return\n  }\n}\n";
    char *c = nullptr;

    check("a commented module round-trips", round_trips(ctx, commented, &c));
    check("the comment survives", c && strstr(c, "# keep me"));
    free(c);

    ir_ctx_free(ctx);
}

static void test_types_are_typelang(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    const char *src =
        "module m {\n  profile bce\n\n"
        "  global @a: (y: i64, x: int)\n"
        "  global @b: __ty.tuple[int, i64]\n"
        "  global @c: (int)\n"
        "  global @d: (int | char & i64)\n"
        "  global @e: ref[int; owning, exclusive]\n"
        "  global @f: forall[`K, `V] core.D[`K, `V]\n"
        "}\n";

    char *out = nullptr;

    check("typelang types parse in the IR", round_trips(ctx, src, &out));

    if (out) {
        check("record fields print canonically",
              strstr(out, "@a: (x: int, y: i64)"));
        check("a tuple prints canonically", strstr(out, "@b: t(int, i64)"));
        check("grouping parens are dropped", strstr(out, "@c: int"));
        check("sums print canonically",
              strstr(out, "@d: (int | (i64 & char))"));
        check("indirection attributes print canonically",
              strstr(out, "@e: ref[int; owning, exclusive]"));
        check("binders rename canonically",
              strstr(out, "@f: forall[`a, `b] core.D[`a, `b]"));
    }
    free(out);

    ir_ctx_free(ctx);
}

static void test_function_failure_channel_round_trips(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    const char *src =
        "module m {\n"
        "  profile bce\n"
        "  target t\n\n"
        "  func @fails() -> i32 fail: str {\n"
        "    block entry:\n"
        "      return 0\n"
        "  }\n"
        "}\n";
    ir_diags_t *diagnostics = ir_diags_new(ctx, nullptr);
    ir_module_t *module = ir_module_parse(ctx, src, 0, PROFILES,
        sizeof(PROFILES) / sizeof(PROFILES[0]), diagnostics);
    ir_func_t *function = module ? ir_func_find(module, "fails") : nullptr;
    check("text IR retains a function failure channel",
          function && th_func_error(function->type)
              && ir_type_eq(module, th_func_error(function->type),
                            ir_type_parse(module, "str")));

    char *printed = module ? ir_module_print(module) : nullptr;
    check("text IR prints the function failure channel",
          printed && strstr(printed, "-> i32 fail: str"));
    ir_diags_t *roundtrip_diagnostics = ir_diags_new(ctx, nullptr);
    ir_module_t *roundtrip = printed
        ? ir_module_parse(ctx, printed, 0, PROFILES,
            sizeof(PROFILES) / sizeof(PROFILES[0]), roundtrip_diagnostics)
        : nullptr;
    ir_func_t *roundtrip_function = roundtrip
        ? ir_func_find(roundtrip, "fails") : nullptr;
    check("the printed failure channel reparses in the same type universe",
          roundtrip_function && th_func_error(roundtrip_function->type)
              && ir_type_eq(roundtrip,
                  th_func_error(roundtrip_function->type),
                  th_func_error(function->type)));

    free(printed);
    ir_ctx_free(ctx);
}

static void test_variant_type_definition_round_trips(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    const char *src =
        "module m {\n"
        "  profile bce\n"
        "  target t\n\n"
        "  type variant call_outcome = (ok: i32, error: str)\n"
        "}\n";
    char *printed = nullptr;
    check("a named variant type definition round-trips",
          round_trips(ctx, src, &printed));
    check("the variant kind is retained in canonical text",
          printed && strstr(printed,
              "type variant call_outcome = (ok: i32, error: str)"));
    free(printed);
    ir_ctx_free(ctx);
}

static void test_mutability_round_trips(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    const char *src =
        "module m {\n  profile bce\n  target t\n\n"
        "  const global @k: i32 = 7\n\n"
        "  immutable global @j: i32 = 1\n\n"
        "  global @g: i32 = 2\n\n"
        "  func @f() -> i32 {\n"
        "    const local $a: i32 = 3\n"
        "    immutable local $b: i32\n"
        "    local $c: i32\n\n"
        "    block entry:\n      return @g\n  }\n}\n";

    ir_diags_t  *d = nullptr;
    ir_module_t *m = parse(ctx, src, &d);

    check("mutability parses", m != nullptr);

    if (!m) {
        ir_ctx_free(ctx);

        return;
    }

    ir_global_t *k = ir_global_find(m, "k");
    ir_global_t *j = ir_global_find(m, "j");
    ir_global_t *g = ir_global_find(m, "g");

    check("a const global is const", k && k->mutability == IR_MUT_CONST);
    check("an immutable global is immutable",
          j && j->mutability == IR_MUT_IMMUTABLE);
    check("an unmarked global is mutable",
          g && g->mutability == IR_MUT_MUTABLE);

    ir_func_t *f = ir_func_find(m, "f");

    check("the function is there", f != nullptr);

    if (f) {
        check("a const local is const",
              ir_local_find(f, "a")
                  && ir_local_find(f, "a")->mutability == IR_MUT_CONST);
        check("an immutable local is immutable",
              ir_local_find(f, "b")
                  && ir_local_find(f, "b")->mutability == IR_MUT_IMMUTABLE);
        check("an unmarked local is mutable",
              ir_local_find(f, "c")
                  && ir_local_find(f, "c")->mutability == IR_MUT_MUTABLE);
    }

    char *once = ir_module_print(m);

    check("printing keeps every marker",
          once && strstr(once, "const global @k")
              && strstr(once, "immutable global @j")
              && strstr(once, "\n  global @g")
              && strstr(once, "const local $a")
              && strstr(once, "immutable local $b")
              && strstr(once, "    local $c"));

    ir_ctx_t    *c2 = ir_ctx_new();
    ir_diags_t  *d2 = nullptr;
    ir_module_t *m2 = parse(c2, once, &d2);
    char        *twice = m2 ? ir_module_print(m2) : nullptr;

    check("and reparsing gives the same text",
          twice && !strcmp(once, twice));

    free(once);
    free(twice);
    ir_ctx_free(c2);
    ir_ctx_free(ctx);
}

static void test_meta_round_trip(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    const char *src =
        "module m {\n  profile bce\n"
        "  meta !ir.note(\"module level\")\n\n"
        "  func @f(%n: i32) -> i32 !loc(0, 1, 2, 3, 4, 5, 6) {\n"
        "    local $x: i32 !loc(0, 7, 8, 9, 10, 11, 12)\n"
        "\n    block e: !loc(0, 13, 14, 15, 16, 17, 18)\n"
        "      %t: i32 = copy %n !loc(0, 19, 20, 21, 22, 23, 24)"
        " !origin(generated, \"promote\", 0, 2, 3, 4, 5, 6, 7)"
        " !origin(source, \"\", 0, 1, 2, 3, 4, 5, 6)"
        " !ir.rule(\"sint\" from typed) !bce.pin(true) !ir.n(42)\n"
        "      return %t\n  }\n}\n";

    ir_diags_t  *d = nullptr;
    ir_module_t *m = parse(ctx, src, &d);

    check("metadata parses", m != nullptr);

    if (!m) {
        char *r = ir_diags_render_text(d, &(ir_render_t){0});

        printf("%s", r);
        free(r);
        ir_ctx_free(ctx);

        return;
    }

    ir_func_t  *f  = ir_func_find(m, "f");
    ir_block_t *e  = ir_block_entry(f);
    ir_instr_t *in = e->instrs[0];

    check("an instruction span survives",
          in->span.file == 0 && in->span.start_byte == 19
              && in->span.end_col == 24);
    check("a function span survives", f->span.start_byte == 1);
    check("a local span survives", f->locals[0]->span.start_byte == 7);
    check("a block span survives", e->span.start_byte == 13);
    check("an origin chain survives", ir_origin_depth(in->origin) == 2);
    check("the origin agent survives",
          in->origin && in->origin->agent
              && !strcmp(in->origin->agent, "promote"));
    check("the origin kind survives",
          in->origin && in->origin->kind == IR_ORIGIN_GENERATED_BY);
    check("an outer origin survives",
          in->origin && in->origin->parent
              && in->origin->parent->kind == IR_ORIGIN_SOURCE);

    ir_annot_t *r = ir_annot_get(&in->annots, "ir.rule");

    check("a string annotation survives",
          r && r->kind == IR_ANNOT_STR && !strcmp(r->s, "sint"));
    check("annotation provenance survives", r && r->derived_from
                                                 == IR_PROP_TYPED);

    ir_annot_t *pin = ir_annot_get(&in->annots, "bce.pin");

    check("a bool annotation survives",
          pin && pin->kind == IR_ANNOT_BOOL && pin->b);

    ir_annot_t *num = ir_annot_get(&in->annots, "ir.n");

    check("an int annotation survives",
          num && num->kind == IR_ANNOT_INT && ir_lit_signed(num->i) == 42);
    check("a module annotation survives",
          ir_annot_get(ir_module_annots(m), "ir.note") != nullptr);

    check("metadata round-trips", round_trips(ctx, src, nullptr));

    ir_ctx_free(ctx);
}

static void test_both_forms(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    const char *nonssa =
        "module sum {\n  profile bce\n  target x86_64-linux\n\n"
        "  func @sum(%n: i32) -> i32 {\n"
        "    local $acc: i32\n    local $i: i32\n\n"
        "    block entry:\n      jump loop\n\n"
        "    block loop:\n      %more: bool = lt $i, %n\n"
        "      branch %more, body, done\n\n"
        "    block body:\n      $acc = add wrap $acc, $i\n"
        "      $i = add wrap $i, %n\n      jump loop\n\n"
        "    block done:\n      return $acc\n  }\n}\n";

    const char *ssa =
        "module sum {\n  profile bce\n  target x86_64-linux\n\n"
        "  func @sum(%n: i32) -> i32 {\n"
        "    block entry:\n      jump loop\n\n"
        "    block loop:\n"
        "      %acc_1: i32 = phi [entry: %n], [body: %acc_2]\n"
        "      %i_1: i32 = phi [entry: %n], [body: %i_2]\n"
        "      %more: bool = lt %i_1, %n\n"
        "      branch %more, body, done\n\n"
        "    block body:\n"
        "      %acc_2: i32 = add wrap %acc_1, %i_1\n"
        "      %i_2: i32 = add wrap %i_1, %n\n      jump loop\n\n"
        "    block done:\n      return %acc_1\n  }\n}\n";

    check("the non-SSA form round-trips", round_trips(ctx, nonssa, nullptr));
    check("the SSA form round-trips", round_trips(ctx, ssa, nullptr));

    ir_ctx_free(ctx);
}

static void test_cross_profile(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    const char *synth =
        "module m {\n  profile synth\n\n"
        "  func @f(%n: int) -> int {\n"
        "    block e:\n      %t: int = add wrap %n, %n\n"
        "      return %t\n  }\n}\n";

    check("a module under the second profile round-trips",
          round_trips(ctx, synth, nullptr));

    ir_diags_t  *d = nullptr;
    ir_module_t *m = parse(ctx, "module m {\n profile nope\n}\n", &d);

    check("an unknown profile is refused", m == nullptr);
    check("and it is reported", ir_diags_count(d) == 1);

    ir_ctx_free(ctx);
}

static void test_errors(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    const char *bad[] = {
        "",
        "module",
        "module m",
        "module m {}",
        "module m {\n profile bce\n func @f() -> void {\n block e:\n"
        " frobnicate\n }\n}",
        "module m {\n profile bce\n func @f() -> void {\n block e:\n"
        " add wrap %a, %b\n }\n}",
        "module m {\n profile bce\n func @f() -> void {\n block e:\n"
        " %x: i32 = add sat wrap %a\n }\n}",
        "module m {\n profile bce\n func @f() -> nosuchtype[\n",
        "module m {\n profile bce\n func @f() -> void {\n block e:\n"
        " jump nowhere\n }\n}",
        "module m {\n profile bce\n func @f() -> void {\n block e:\n"
        " %x: i32 = and ordered %a, %b\n }\n}",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ir_diags_t  *d = nullptr;
        ir_module_t *m = parse(ctx, bad[i], &d);

        if (m && i != 8) {
            printf("FAIL  malformed input %zu was accepted\n", i);
            fails++;
        }

        if (!m && ir_diags_count(d) == 0) {
            printf("FAIL  malformed input %zu failed without a diagnostic\n",
                   i);
            fails++;
        }
    }

    ir_ctx_free(ctx);
}

static uint32_t next_op;
static uint32_t next_term;

static uint64_t rng = 0xDEADBEEFCAFEF00Du;

static uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;

    return n ? (uint32_t)(rng % n) : 0;
}

static const char *TYPES[] = {"i32",      "u64",  "f64",
                              "bool",     "i8",   "ptr[i32]",
                              "(x: i32)", "array[i32; 4]", "t(i32, u64)"};

static const char *MODS_FOR[] = {"wrap",     "trap",   "sat",     "undef",
                                 "mask",     "ordered","unordered",
                                 "volatile", "atomic", "to_zero", "nearest",
                                 "floor",    "ceil"};

typedef struct {
    ir_value_t *v[128];
    uint32_t    n;
} pool;

static ir_block_t *gen_target(ir_func_t *f, uint32_t nblocks)
{
    return f->blocks[nblocks > 1 ? 1 + rnd(nblocks - 1) : 0];
}

static void gen_operands(ir_module_t *m, ir_func_t *f, ir_instr_t *in,
                         pool *vals, uint32_t nblocks)
{
    const ir_opinfo_t *info = ir_op_info(in->op);
    uint32_t           want = info->min_operands;

    if (info->max_operands == IR_VAR_ARITY) {
        want = info->min_operands + rnd(3);

        if (!want) {
            want = 1;
        }
    }

    for (uint32_t i = 0; i < want; i++) {
        switch (ir_op_operand_kind(in->op, i)) {
        case IR_OPD_VALUE:
            if (!vals->n) {
                return;
            }
            ir_instr_operand_value(in, vals->v[rnd(vals->n)]);
            break;

        case IR_OPD_SPREAD:
            break;

        case IR_OPD_TYPE:
            ir_instr_operand_type(in, ir_type_parse(m, TYPES[rnd(9)]));
            break;

        case IR_OPD_NAME:
            ir_instr_operand_name(in, rnd(2) ? "x" : "some.field");
            break;

        case IR_OPD_BLOCK:
            ir_instr_operand_block(in, gen_target(f, nblocks));
            break;

        case IR_OPD_DEFAULT_BLOCK:
            ir_instr_operand_default(in, gen_target(f, nblocks));
            break;

        case IR_OPD_CONST:
            if (rnd(4) == 0) {
                ir_instr_operand_float(in, (double)(int32_t)rnd(1000) / 8.0);
            }
            else {
                ir_instr_operand_int(in, (int64_t)rnd(2000) - 1000);
            }
            break;

        case IR_OPD_PHI_ARM:
            if (!vals->n) {
                return;
            }
            ir_instr_operand_phi_arm(in, gen_target(f, nblocks),
                                  vals->v[rnd(vals->n)]);
            break;

        case IR_OPD_SWITCH_ARM:
            ir_instr_operand_switch_arm(in, (int64_t)rnd(10),
                                     gen_target(f, nblocks));
            break;

        case IR_OPD_ARG:
        case IR_OPD_NAMED_ARG:
            if (!vals->n) {
                return;
            }

            if (rnd(2)) {
                ir_instr_operand_named(in, rnd(2) ? "timeout" : "retries",
                                    vals->v[rnd(vals->n)]);
            }
            else {
                ir_instr_operand_value(in, vals->v[rnd(vals->n)]);
            }
            break;
        }
    }
}

static ir_module_t *gen_module(ir_ctx_t *ctx, uint32_t seq)
{
    const ir_profile_t *pro = rnd(2) ? &ir_profile_bce : &ir_profile_synth;
    char                name[32];

    snprintf(name, sizeof(name), "gen%u", seq);

    ir_module_t *m = ir_module_new(ctx, name, pro, rnd(2) ? "host" : nullptr);

    ir_file_add(m, "gen.src");

    const char *scalar = pro == &ir_profile_bce ? "i32" : "int";
    th_type    *st     = ir_type_parse(m, scalar);

    if (rnd(2)) {
        ir_global_add(m, "g", st);
    }

    uint32_t nfuncs = 1 + rnd(2);

    for (uint32_t fi = 0; fi < nfuncs; fi++) {
        char fname[32];

        snprintf(fname, sizeof(fname), "f%u", fi);

        char sig[128];

        snprintf(sig, sizeof(sig), "(%s) -> %s", scalar, scalar);

        ir_func_t *f = ir_func_add(m, fname, ir_type_parse(m, sig),
                                   (ir_linkage_t)rnd(3));

        if (!f) {
            continue;
        }

        pool vals = {0};

        vals.v[vals.n++] = ir_param_add(f, "p0", st);

        uint32_t nlocals = rnd(3);

        for (uint32_t i = 0; i < nlocals; i++) {
            char ln[16];

            snprintf(ln, sizeof(ln), "l%u", i);
            ir_local_add(f, ln, st);
        }

        uint32_t nblocks = 2 + rnd(3);

        for (uint32_t i = 0; i < nblocks; i++) {
            char bn[16];

            snprintf(bn, sizeof(bn), "b%u", i);
            ir_block_add(f, bn);
        }

        for (uint32_t bi = 0; bi < nblocks; bi++) {
            ir_block_t *b = f->blocks[bi];
            uint32_t    n = rnd(5);

            for (uint32_t k = 0; k < n; k++) {
                ir_op_t op = (ir_op_t)(next_op++ % IR_OP_COUNT);

                if (ir_op_is_terminator(op)) {
                    op = IR_OP_COPY;
                }

                ir_instr_t *in = ir_instr_add(b, op);

                if (!in) {
                    continue;
                }
                gen_ops[op]++;

                for (size_t mi = 0; mi < 13; mi++) {
                    if (rnd(3) == 0
                        && ir_op_accepts_mod(op, MODS_FOR[mi])) {
                        ir_instr_add_mod(in, MODS_FOR[mi]);
                    }
                }
                gen_operands(m, f, in, &vals, nblocks);

                if (ir_op_has_result(op) && vals.n < 120) {
                    char vn[24];

                    snprintf(vn, sizeof(vn), "v%u_%u", bi, k);

                    if (ir_instr_set_dest(in, vn, ir_type_parse(m, TYPES[rnd(9)]))
                        && in->dest) {
                        vals.v[vals.n++] = in->dest;
                    }
                }

                if (rnd(3) == 0) {
                    in->span = ir_span_make(0, k, k + 2, k + 1, 1, k + 1, 3);
                }

                if (rnd(4) == 0) {
                    in->origin = ir_origin_new(ctx, IR_ORIGIN_GENERATED_BY,
                                               in->span, "gen", nullptr);
                }

                if (rnd(4) == 0) {
                    ir_annot_set(ctx, &in->annots,
                                 (ir_annot_t){.key = "ir.rule",
                                              .kind = IR_ANNOT_STR,
                                              .s = "made-up",
                                              .derived_from = IR_PROP_TYPED});
                }

                if (rnd(5) == 0) {
                    ir_annot_set(ctx, &in->annots,
                                 (ir_annot_t){.key = "ir.comment",
                                              .kind = IR_ANNOT_STR,
                                              .s = "generated\nline two"});
                }
            }

            static const ir_op_t terms[] = {IR_OP_JUMP, IR_OP_RETURN,
                                            IR_OP_BRANCH, IR_OP_SWITCH,
                                            IR_OP_UNREACHABLE};
            ir_op_t     term = terms[next_term++ % 5];
            ir_instr_t *t    = ir_instr_add(b, term);

            gen_ops[term]++;

            switch (term) {
            case IR_OP_JUMP:
                ir_instr_operand_block(t, gen_target(f, nblocks));
                break;

            case IR_OP_BRANCH:
                ir_instr_operand_value(t, vals.v[rnd(vals.n)]);
                ir_instr_operand_block(t, gen_target(f, nblocks));
                ir_instr_operand_block(t, gen_target(f, nblocks));
                break;

            case IR_OP_SWITCH:
                ir_instr_operand_value(t, vals.v[rnd(vals.n)]);
                ir_instr_operand_default(t, gen_target(f, nblocks));

                for (uint32_t s = 0; s < 1 + rnd(3); s++) {
                    ir_instr_operand_switch_arm(t, (int64_t)rnd(8),
                                             gen_target(f, nblocks));
                }
                break;

            case IR_OP_RETURN:
                if (vals.n && rnd(2)) {
                    ir_instr_operand_value(t, vals.v[rnd(vals.n)]);
                }
                break;

            default:
                break;
            }
        }
    }

    return m;
}

static void test_named_args(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    const char *src =
        "module m {\n  profile bce\n\n"
        "  func @open(%path: i32, timeout: i32, retries: i32) -> i32 {\n"
        "    block e:\n      return %path\n  }\n\n"
        "  func @f(%n: i32) -> i32 {\n"
        "    block e:\n"
        "      %r: i32 = call @open, %n, timeout = %n, retries = %n\n"
        "      return %r\n  }\n}\n";

    ir_diags_t  *d = nullptr;
    ir_module_t *m = parse(ctx, src, &d);

    check("keyword arguments parse", m != nullptr);

    if (!m) {
        char *r = ir_diags_render_text(d, &(ir_render_t){0});

        printf("%s", r);
        free(r);
        ir_ctx_free(ctx);

        return;
    }

    ir_func_t  *f  = ir_func_find(m, "f");
    ir_instr_t *in = ir_block_entry(f)->instrs[0];

    check("a call takes callee then arguments",
          in->op == IR_OP_CALL && in->n_operands == 4);
    check("a positional argument stays a value",
          in->operands[1].kind == IR_OPD_VALUE);
    check("a keyword argument keeps its name",
          in->operands[2].kind == IR_OPD_NAMED_ARG
              && !strcmp(in->operands[2].named.name, "timeout"));
    check("a keyword argument resolves its value",
          in->operands[2].named.value != nullptr
              && !strcmp(in->operands[2].named.value->name, "n"));
    check("keyword arguments round-trip", round_trips(ctx, src, nullptr));

    ir_ctx_free(ctx);
}

static void test_nfc_identifiers(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    const char *src =
        "module caf\xc3\xa9 {\n  profile bce\n\n"
        "  global @na\xc3\xafve: i32\n\n"
        "  func @\xe6\xbc\xa2(%\xc3\xa9: i32) -> i32 {\n"
        "    local $\xcf\x80: i32\n\n"
        "    block \xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba:\n"
        "      $\xcf\x80 = copy %\xc3\xa9\n"
        "      return $\xcf\x80\n  }\n}\n";

    char *out = nullptr;

    check("non-ASCII identifiers parse and round-trip",
          round_trips(ctx, src, &out));
    check("a non-ASCII module name survives",
          out && strstr(out, "module caf\xc3\xa9"));
    check("a non-ASCII global survives",
          out && strstr(out, "@na\xc3\xafve"));
    check("a non-ASCII block name survives",
          out && strstr(out, "block \xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba"));
    free(out);

    ir_ctx_free(ctx);
}

#define MOD(...) "module m {\n  profile bce\n  target t\n\n" __VA_ARGS__ "}\n"

static void test_an_error_points_at_what_it_is_about(void)
{
    static const struct {
        const char *what;
        const char *token;
        const char *src;
    } cases[] = {
        {"unknown local",
         "$nope",
         MOD("  func @f(%a: i32) -> i32 {\n    block entry:\n"
             "      %r: i32 = copy $nope\n      return %r\n  }\n")},
        {"unknown local as destination",
         "$gone",
         MOD("  func @f(%a: i32) -> i32 {\n    block entry:\n"
             "      $gone = copy %a\n      return %a\n  }\n")},
        {"unknown value",
         "%missing",
         MOD("  func @f(%a: i32) -> i32 {\n    block entry:\n"
             "      return %missing\n  }\n")},
        {"unknown opcode",
         "frobnicate",
         MOD("  func @f(%a: i32) -> i32 {\n    block entry:\n"
             "      %r: i32 = frobnicate %a\n      return %r\n  }\n")},
        {"duplicate local",
         "$x",
         MOD("  func @f(%a: i32) -> i32 {\n    local $x: i32\n"
             "    local $x: i64\n    block entry:\n"
             "      return %a\n  }\n")},
        {"unknown profile",
         "nosuch",
         "module m {\n  profile nosuch\n  target t\n\n"
         "  func @f(%a: i32) -> i32 {\n    block entry:\n"
         "      return %a\n  }\n}\n"},
        {"this opcode has no result",
         "%r",
         MOD("  func @f(%a: i32) -> void {\n    block entry:\n"
             "      %r: i32 = return %a\n  }\n")},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ir_ctx_t   *ctx = ir_ctx_new();
        ir_diags_t *d   = ir_diags_new(ctx, nullptr);

        ir_module_parse(ctx, cases[i].src, 0, PROFILES, 2, d);

        if (!ir_diags_count(d)) {
            printf("FAIL  %s produced no diagnostic\n", cases[i].what);
            fails++;
            ir_ctx_free(ctx);
            continue;
        }

        const ir_diag_t *dg  = ir_diags_at(d, 0);
        ir_span_t        s   = dg->primary.span;
        size_t           n   = strlen(cases[i].token);
        size_t           len = strlen(cases[i].src);

        if (!ir_span_known(s) || s.end_byte > len || s.start_byte > s.end_byte) {
            printf("FAIL  %s has no usable span\n", cases[i].what);
            fails++;
            ir_ctx_free(ctx);
            continue;
        }

        size_t      got = s.end_byte - s.start_byte;
        const char *at  = cases[i].src + s.start_byte;

        if (got != n || memcmp(at, cases[i].token, n)) {
            printf("FAIL  %s points at \"%.*s\", not \"%s\"\n", cases[i].what,
                   (int)got, at, cases[i].token);
            fails++;
        }
        ir_ctx_free(ctx);
    }
    check("every parse error points at the token it is about", true);
}

static void test_spans_come_from_the_text(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    const char *src = "module m {\n"
                      "  profile bce\n"
                      "  target t\n"
                      "\n"
                      "  func @f(%a: i32) -> i32 {\n"
                      "    local $t: i32\n"
                      "\n"
                      "    block entry:\n"
                      "      %s: i32 = add wrap %a, %a\n"
                      "      return %s\n"
                      "  }\n"
                      "}\n";

    ir_module_t *m = parse(ctx, src, nullptr);

    check("a module with no !loc parses", m != nullptr);

    if (!m) {
        ir_ctx_free(ctx);

        return;
    }

    ir_func_t  *f = ir_func_at(m, 0);
    ir_block_t *b = ir_block_entry(f);

    check("a function parsed from text knows where it is",
          ir_span_known(f->span) && f->span.start_line == 5);
    check("so does a local",
          f->n_locals == 1 && ir_span_known(f->locals[0]->span)
              && f->locals[0]->span.start_line == 6);
    check("so does a block",
          ir_span_known(b->span) && b->span.start_line == 8);
    check("so does an instruction",
          b->n_instrs == 2 && ir_span_known(b->instrs[0]->span)
              && b->instrs[0]->span.start_line == 9);
    check("and its span covers the instruction, not one token of it",
          b->instrs[0]->span.start_col == 7
              && b->instrs[0]->span.end_col > 20);
    check("the second instruction is on its own line",
          ir_span_known(b->instrs[1]->span)
              && b->instrs[1]->span.start_line == 10);

    check("a span the text implies is marked implied",
          b->instrs[0]->span.implied);

    char *out = ir_module_print(m);

    check("and is not printed back, since the text already says it",
          out && !strstr(out, "!loc"));
    free(out);

    const char *authored = "module m {\n"
                           "  profile bce\n"
                           "  target t\n"
                           "\n"
                           "  func @f() -> void {\n"
                           "    block entry:\n"
                           "      unreachable !loc(3, 90, 95, 40, 2, 40, 7)\n"
                           "  }\n"
                           "}\n";

    ir_module_t *am = parse(ctx, authored, nullptr);

    check("an authored !loc parses", am != nullptr);

    if (am) {
        ir_instr_t *in = ir_block_entry(ir_func_at(am, 0))->instrs[0];

        check("an authored location wins over the one the text implies",
              in->span.start_line == 40 && in->span.file == 3);
        check("and is not marked implied", !in->span.implied);

        char *back = ir_module_print(am);

        check("so it is printed back",
              back && strstr(back, "!loc(3, 90, 95, 40, 2, 40, 7)"));
        free(back);
    }

    ir_ctx_free(ctx);
}

static void test_literal_spellings(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    const char *src = "module m {\n"
                      "  profile bce\n"
                      "  target t\n"
                      "\n"
                      "  func @f() -> void {\n"
                      "    block entry:\n"
                      "      %m: u32 = const 0xdeadbeef "
                      "!ir.mask(0xcafe)\n"
                      "      %d: u32 = const 255\n"
                      "      %w: ubitint[128] = const "
                      "0xffffffffffffffffffffffffffffffff\n"
                      "      %n: bitint[128] = const "
                      "-170141183460469231731687303715884105728\n"
                      "      %e: f64 = const 1.5e10\n"
                      "      %p: f64 = const 1.2345678901234567890123456\n"
                      "      switch %d, default: entry, [0xff: entry]\n"
                      "  }\n"
                      "}\n";

    char        *out = nullptr;
    ir_module_t *m   = parse(ctx, src, nullptr);

    check("a module of mixed literal spellings parses", m != nullptr);

    if (m) {
        out = ir_module_print(m);
    }

    check("a hexadecimal literal comes back as hexadecimal",
          out && strstr(out, "const 0xdeadbeef"));
    check("a decimal literal comes back as decimal",
          out && strstr(out, "const 255"));
    check("the whole of u128 survives the round trip",
          out && strstr(out, "const 0xffffffffffffffffffffffffffffffff"));
    check("so does the bottom of i128",
          out
              && strstr(out,
                        "const -170141183460469231731687303715884105728"));
    check("a float keeps the form it was written in",
          out && strstr(out, "const 1.5e10"));
    check("and keeps digits a double cannot hold",
          out && strstr(out, "const 1.2345678901234567890123456"));
    check("an annotation keeps the radix it was written in",
          out && strstr(out, "!ir.mask(0xcafe"));
    check("a case value keeps its radix too",
          out && strstr(out, "[0xff: entry]"));
    check("and the whole thing round-trips", round_trips(ctx, src, nullptr));
    free(out);

    ir_diags_t  *d  = nullptr;
    ir_module_t *bad = parse(ctx,
                             "module m {\n  profile bce\n  target t\n\n"
                             "  func @f() -> void {\n    block entry:\n"
                             "      %k: u32 = const 0x1"
                             "00000000000000000000000000000000\n"
                             "  }\n}\n",
                             &d);

    check("a hexadecimal literal wider than 128 bits is refused",
          bad == nullptr);

    ir_ctx_free(ctx);
}

static void test_canonical_golden(const char *dir)
{
    ir_ctx_t *ctx = ir_ctx_new();

    const char *src =
        "# a module comment\n"
        "module shapes{\nprofile bce\ntarget x86_64-linux\n"
        "meta !ir.note(\"hand written\")\n"
        "global @count: i32\n"
        "static global @seen: (y: i64, x: int)\n"
        "static func @helper()->void{\nblock e:\nunreachable\n}\n"
        "func @sum(%n: i32,%acc: i32)->i32{\n"
        "local $t: ptr[i32]\n"
        "block entry:\n"
        "# the loop counter starts here\n"
        "%z: i32 = const 0 !loc(0, 1, 2, 1, 3, 1, 4)\n"
        "jump loop\n"
        "block loop:\n"
        "%p: i32 = phi [entry: %z], [body: %s]\n"
        "%c: bool = lt %p, %n\n"
        "branch %c, body, done\n"
        "block body:\n"
        "%s: i32 = add wrap %p, %acc !ir.rule(\"sint\" from typed)\n"
        "%sz: u64 = sizeof (i32, i32) -> i32\n"
        "switch %s, default: done, [0: loop], [1: done]\n"
        "block done:\n"
        "return %acc\n}\n}\n";

    ir_module_t *m = parse(ctx, src, nullptr);

    check("the golden source parses", m != nullptr);

    if (m) {
        char *out = ir_module_print(m);

        check("canonical printing matches its golden",
              same_as_golden(dir, "canonical.ir", out));
        free(out);
    }

    ir_ctx_free(ctx);
}

static void test_generated_round_trip(void)
{
    for (uint32_t i = 0; i < trials; i++) {
        ir_ctx_t    *ctx = ir_ctx_new();
        ir_module_t *m   = gen_module(ctx, i);
        char        *one = ir_module_print(m);
        ir_diags_t  *d   = nullptr;
        ir_module_t *m2  = parse(ctx, one, &d);

        if (!m2) {
            char *r = ir_diags_render_text(d, &(ir_render_t){0});

            printf("FAIL  generated module %u did not parse\n%s---- text ----"
                   "\n%s",
                   i, r, one);
            free(r);
            fails++;
            free(one);
            ir_ctx_free(ctx);

            return;
        }

        char *two = ir_module_print(m2);

        if (strcmp(one, two)) {
            printf("FAIL  generated module %u does not round-trip\n"
                   "---- once ----\n%s---- twice ----\n%s",
                   i, one, two);
            fails++;
            free(one);
            free(two);
            ir_ctx_free(ctx);

            return;
        }

        ir_module_t *m3   = parse(ctx, two, nullptr);
        char        *three = m3 ? ir_module_print(m3) : nullptr;

        if (!three || strcmp(two, three)) {
            printf("FAIL  printing is not idempotent for module %u\n", i);
            fails++;
        }
        free(three);
        free(one);
        free(two);
        ir_ctx_free(ctx);
    }
}

static void test_the_grammar_lays_the_text_out(void)
{
    static const char *const WANT[] = {
        "module m {\n",
        "  profile bce\n",
        "  target x86-64\n",
        "  global @g: i32 = 7\n",
        "  func @f(%a: i32, %b: ptr[i32]) -> i32 {\n",
        "    local $t: array[i32; 4]\n",
        "\n",
        "    block entry:\n",
        "      %c: i32 = add.wrap %a, %a !ir.note(\"hi\")\n",
        "      ret %c\n",
        "  }\n",
        "}\n",
    };

    const char *src = "module m {\n"
                      "profile bce\n"
                      "target x86-64\n"
                      "global @g:i32=7\n"
                      "func @f(%a:i32,%b:ptr[i32])->i32 {\n"
                      "local $t:array[i32;4]\n"
                      "block entry:\n"
                      "%c:i32=add.wrap %a,%a !ir.note(\"hi\")\n"
                      "ret %c\n"
                      "}\n"
                      "}\n";

    char *got = ir_text_format(src, 80);

    check("unspaced source lays out", got != nullptr);

    if (!got) {
        return;
    }

    size_t at = 0;

    for (size_t i = 0; i < sizeof(WANT) / sizeof(WANT[0]); i++) {
        size_t n = strlen(WANT[i]);

        if (strncmp(got + at, WANT[i], n)) {
            printf("FAIL  line %zu is not what the annotations ask for\n"
                   "---- want ----\n%s---- got ----\n%s",
                   i, WANT[i], got + at);
            fails++;
            break;
        }
        at += n;
    }
    free(got);
}

static void test_the_layout_annotations_round_trip(void)
{
    for (uint32_t i = 0; i < trials; i++) {
        ir_ctx_t    *ctx = ir_ctx_new();
        ir_module_t *m   = gen_module(ctx, i);
        char        *one = ir_module_print(m);
        char        *fmt = ir_text_format(one, 80);

        if (!fmt) {
            printf("FAIL  generated module %u does not lay out\n---- text ----"
                   "\n%s",
                   i, one);
            fails++;
            free(one);
            ir_ctx_free(ctx);

            return;
        }

        ir_diags_t  *d  = nullptr;
        ir_module_t *m2 = parse(ctx, fmt, &d);
        char        *two = m2 ? ir_module_print(m2) : nullptr;

        if (!two || strcmp(one, two)) {
            char *r = ir_diags_render_text(d, &(ir_render_t){0});

            printf("FAIL  laid-out module %u is not the same module\n%s"
                   "---- printed ----\n%s---- laid out ----\n%s"
                   "---- reprinted ----\n%s",
                   i, r, one, fmt, two ? two : "(did not parse)\n");
            free(r);
            fails++;
            free(one);
            free(fmt);
            free(two);
            ir_ctx_free(ctx);

            return;
        }
        free(one);
        free(fmt);
        free(two);
        ir_ctx_free(ctx);
    }
}

static void test_the_passes_over_the_generated_corpus(void)
{
    static const ir_pass_t *const passes[] = {
        &ir_pass_promote,
        &ir_pass_split_edges,
        &ir_pass_structure,
        &ir_pass_layout,
    };
    uint32_t ran[4] = {0};

    for (uint32_t i = 0; i < trials; i++) {
        ir_ctx_t    *ctx = ir_ctx_new();
        ir_module_t *m   = gen_module(ctx, i);
        ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
        ir_props_t   have = ir_verify(m, d);

        ir_module_establish(m, have);

        for (size_t k = 0; k < sizeof(passes) / sizeof(passes[0]); k++) {
            if ((have & passes[k]->requires) != passes[k]->requires) {
                continue;
            }

            ir_locsnap_t *before = ir_loc_snapshot(m);
            ir_props_t    unmet  = 0;

            if (ir_pass_run(passes[k], m, nullptr, &unmet, d)
                != IR_PASS_OK) {
                continue;
            }
            ran[k]++;

            if (!ir_loc_monotone(before, m, passes[k]->name, d)) {
                printf("FAIL  %s lost a location on generated module %u\n",
                       passes[k]->name, i);
                fails++;
                break;
            }

            ir_diags_t *vd  = ir_diags_new(ctx, nullptr);
            ir_props_t  now = ir_verify(m, vd);

            if (!(now & IR_PROP_PARSED)) {
                char *why  = ir_diags_render_text(vd, &(ir_render_t){0});
                char *text = ir_module_print(m);

                printf("FAIL  %s left generated module %u malformed\n%s"
                       "---- module ----\n%s",
                       passes[k]->name, i, why ? why : "", text ? text : "");
                free(why);
                free(text);
                fails++;
                break;
            }
            have = now;
        }
        ir_ctx_free(ctx);
    }

    for (size_t k = 0; k < sizeof(passes) / sizeof(passes[0]); k++) {
        if (!ran[k]) {
            printf("NOTE  %s never ran over the corpus: the generator does "
                   "not produce modules that meet what it requires\n",
                   passes[k]->name);
        }
    }
    check("the passes that the corpus can feed ran over it, and no run lost "
          "a location or broke the module",
          ran[1] > 0 && ran[2] > 0);
}

static void report_coverage(void)
{
    uint32_t covered = 0;
    uint32_t missing = 0;

    for (uint16_t i = 0; i < IR_OP_COUNT; i++) {
        covered += gen_ops[i] > 0;
    }

    printf("generator covered %u of %u opcodes\n", covered, IR_OP_COUNT);

    for (uint16_t i = 0; i < IR_OP_COUNT; i++) {
        if (!gen_ops[i]) {
            if (!missing) {
                printf("  never generated:");
            }
            printf(" %s", ir_op_spelling((ir_op_t)i));
            missing++;
        }
    }

    if (missing) {
        printf("\n");
    }
    if (trials >= FULL_TRIALS) {
        check("the generator reaches every opcode", missing == 0);
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";

    if (argc > 2) {
        trials = (uint32_t)strtoul(argv[2], nullptr, 10);
    }

    test_lexis();
    test_whitespace_and_comments();
    test_types_are_typelang();
    test_function_failure_channel_round_trips();
    test_variant_type_definition_round_trips();
    test_mutability_round_trips();
    test_meta_round_trip();
    test_both_forms();
    test_cross_profile();
    test_named_args();
    test_nfc_identifiers();
    test_an_error_points_at_what_it_is_about();
    test_spans_come_from_the_text();
    test_literal_spellings();
    test_canonical_golden(dir);
    test_errors();
    test_generated_round_trip();
    test_the_grammar_lays_the_text_out();
    test_the_layout_annotations_round_trip();
    test_the_passes_over_the_generated_corpus();
    report_coverage();
    dump_produced_codes("produced-codes-text.txt", produced);

    return report("ir text");
}
