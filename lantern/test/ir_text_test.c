#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/text.h"

static int  fails = 0;
static int  gen_ops[IR_OP_COUNT];
static bool produced[IR_DIAG_COUNT];

static void check(const char *what, bool ok)
{
    if (!ok) {
        fails++;
        printf("FAIL  %s\n", what);
    }
}

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

static void write_produced(const char *dir)
{
    const char *leaf = "/produced-codes-text.txt";
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

static bool round_trips(ir_ctx_t *ctx, const char *text, char **printed)
{
    ir_diags_t  *d = nullptr;
    ir_module_t *m = parse(ctx, text, &d);

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
        "module m{profile bce func @f()->void{block e:\nreturn}}";
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
          num && num->kind == IR_ANNOT_INT && num->i == 42);
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
            ir_instr_push_value(in, vals->v[rnd(vals->n)]);
            break;

        case IR_OPD_TYPE:
            ir_instr_push_type(in, ir_type_parse(m, TYPES[rnd(9)]));
            break;

        case IR_OPD_NAME:
            ir_instr_push_name(in, rnd(2) ? "x" : "some.field");
            break;

        case IR_OPD_BLOCK:
            ir_instr_push_block(in, f->blocks[rnd(nblocks)]);
            break;

        case IR_OPD_DEFAULT_BLOCK:
            ir_instr_push_default(in, f->blocks[rnd(nblocks)]);
            break;

        case IR_OPD_CONST:
            if (rnd(4) == 0) {
                ir_instr_push_float(in, (double)(int32_t)rnd(1000) / 8.0);
            }
            else {
                ir_instr_push_int(in, (int64_t)rnd(2000) - 1000);
            }
            break;

        case IR_OPD_PHI_ARM:
            if (!vals->n) {
                return;
            }
            ir_instr_push_phi_arm(in, f->blocks[rnd(nblocks)],
                                  vals->v[rnd(vals->n)]);
            break;

        case IR_OPD_SWITCH_ARM:
            ir_instr_push_switch_arm(in, (int64_t)rnd(10),
                                     f->blocks[rnd(nblocks)]);
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

        uint32_t nblocks = 1 + rnd(3);

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
                        ir_instr_push_mod(in, MODS_FOR[mi]);
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
                ir_instr_push_block(t, f->blocks[rnd(nblocks)]);
                break;

            case IR_OP_BRANCH:
                ir_instr_push_value(t, vals.v[rnd(vals.n)]);
                ir_instr_push_block(t, f->blocks[rnd(nblocks)]);
                ir_instr_push_block(t, f->blocks[rnd(nblocks)]);
                break;

            case IR_OP_SWITCH:
                ir_instr_push_value(t, vals.v[rnd(vals.n)]);
                ir_instr_push_default(t, f->blocks[rnd(nblocks)]);

                for (uint32_t s = 0; s < 1 + rnd(3); s++) {
                    ir_instr_push_switch_arm(t, (int64_t)rnd(8),
                                             f->blocks[rnd(nblocks)]);
                }
                break;

            case IR_OP_RETURN:
                if (vals.n && rnd(2)) {
                    ir_instr_push_value(t, vals.v[rnd(vals.n)]);
                }
                break;

            default:
                break;
            }
        }
    }

    return m;
}

static void test_generated_round_trip(void)
{
    uint32_t trials = 400;

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
    check("the generator reaches every opcode", missing == 0);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";

    test_lexis();
    test_whitespace_and_comments();
    test_types_are_typelang();
    test_meta_round_trip();
    test_both_forms();
    test_cross_profile();
    test_errors();
    test_generated_round_trip();
    report_coverage();
    write_produced(dir);

    if (!fails) {
        printf("ir text: all checks passed\n");
    }

    return fails ? 1 : 0;
}
