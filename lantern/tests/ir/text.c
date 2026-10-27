#include <stdio.h>
#include <stdlib.h>

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

#define S(x)    CORE_STRING_STATIC(x)
#define SARG(s) (int)(s).u8_bytes, (s).data

static bool contains(core_string_t haystack, core_string_t needle)
{
    return haystack.data && core_string_find(haystack, needle) >= 0;
}

static ir_module_t *parse(ir_ctx_t *ctx, core_string_t text, ir_diags_t **out)
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


static bool same_as_golden(core_string_t dir, core_string_t name,
                           core_string_t got)
{
    core_buffer_t *joined = core_buffer_empty();

    core_buffer_append_string(joined, dir);
    core_buffer_putc(joined, '/');
    core_buffer_append_string(joined, name);

    core_string_t path  = core_buffer_take_string(joined);
    char         *cpath = core_string_to_cstr(path);

    FILE *f = fopen(cpath, "r");

    if (!f) {
        if (!getenv("IR_BLESS_GOLDEN")) {
            printf("FAIL  golden %.*s is missing; re-run with "
                   "IR_BLESS_GOLDEN=1 to create it\n",
                   SARG(path));
            core_free(cpath);
            core_free(path.data);

            return false;
        }

        f = fopen(cpath, "w");

        if (f) {
            fwrite(got.data, 1, got.u8_bytes, f);
            fclose(f);
            printf("NOTE  blessed new golden %.*s\n", SARG(path));
        }
        core_free(cpath);
        core_free(path.data);

        return f != nullptr;
    }
    core_free(cpath);

    fseek(f, 0, SEEK_END);

    long   len  = ftell(f);
    char  *want = malloc((size_t)len + 1);

    rewind(f);

    if (!want || fread(want, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(want);
        core_free(path.data);

        return false;
    }
    fclose(f);

    core_string_t wanted = core_string_borrow(want, (size_t)len);
    bool          ok     = core_string_eq(wanted, got);

    if (!ok) {
        printf("---- got ----\n%.*s---- want ----\n%.*s----\n", SARG(got),
               SARG(wanted));
    }
    free(want);
    core_free(path.data);

    return ok;
}

static bool derives(core_string_t text)
{
    parse_grammar_t      *g  = ir_grammar();
    parse_token_stream_t *ts = ir_lex(text);

    if (!g || !ts) {
        return false;
    }

    parse_pwz_parser_t *p
        = parse_pwz_new_for_start(g, ir_grammar_nt(S("module")));
    bool              ok = parse_pwz_parse(p, ts);

    parse_pwz_free(p);
    scan_token_stream_free(ts);

    return ok;
}

static bool round_trips(ir_ctx_t *ctx, core_string_t text,
                        core_string_t *printed)
{
    ir_diags_t  *d = nullptr;
    ir_module_t *m = parse(ctx, text, &d);

    if (m && !derives(text)) {
        printf("---- the grammar does not derive what the parser read ----\n"
               "%.*s",
               SARG(text));
        fails++;
    }

    if (!m) {
        core_string_t r = ir_diags_render_text(d, &(ir_render_t){0});

        printf("---- first parse failed ----\n%.*s---- source ----\n%.*s",
               SARG(r), SARG(text));
        free(r.data);

        if (printed) {
            *printed = (core_string_t){};
        }

        return false;
    }

    core_string_t once = ir_module_print(m);

    if (printed) {
        *printed = once;
    }

    ir_diags_t  *d2 = nullptr;
    ir_module_t *m2 = parse(ctx, once, &d2);

    if (!m2) {
        core_string_t r = ir_diags_render_text(d2, &(ir_render_t){0});

        printf("---- reparse failed ----\n%.*s---- printed ----\n%.*s",
               SARG(r), SARG(once));
        free(r.data);

        return false;
    }

    core_string_t twice = ir_module_print(m2);
    bool          ok    = core_string_eq(once, twice);

    if (!ok) {
        printf("---- first ----\n%.*s---- second ----\n%.*s----\n",
               SARG(once), SARG(twice));
    }
    free(twice.data);

    return ok;
}

static void test_lexis(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    core_string_t src = S(
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
        "}\n");

    ir_diags_t  *d = nullptr;
    ir_module_t *m = parse(ctx, src, &d);

    check("a module with every lexical element parses", m != nullptr);

    if (!m) {
        core_string_t r = ir_diags_render_text(d, &(ir_render_t){0});

        printf("%.*s", SARG(r));
        free(r.data);
        ir_ctx_free(ctx);

        return;
    }

    check("the module name is read",
          core_string_eq(ir_module_name(m), S("m")));
    check("the profile is resolved",
          ir_module_profile(m) == &ir_profile_bce);
    check("the target is read",
          core_string_eq(ir_module_target(m), S("host")));
    check("a global is read", ir_global_find(m, S("g")) != nullptr);

    ir_func_t *f = ir_func_find(m, S("f"));

    check("a function is read", f != nullptr);
    check("a parameter is read", f && f->n_params == 1);
    check("a local is read", f && f->n_locals == 1);
    check("blocks are read", f && f->n_blocks == 3);

    ir_block_t *e = f ? ir_block_entry(f) : nullptr;

    check("the entry block comes first",
          e && core_string_eq(e->name, S("entry")));
    check("instructions are read", e && e->n_instrs == 8);

    if (e && e->n_instrs >= 1) {
        ir_instr_t *add = e->instrs[0];

        check("an opcode resolves", add->op == IR_OP_ADD);
        check("a modifier is read", ir_instr_has_mod(add, S("wrap")));
        check("a value destination is read",
              add->dest_kind == IR_DEST_VALUE
                  && core_string_eq(add->dest->name, S("t")));
        check("operands are read and resolved",
              add->n_operands == 2 && add->operands[0].value
                  && core_string_eq(add->operands[0].value->name, S("n")));
        check("an instruction comment is kept",
              ir_annot_get(&add->annots, S("ir.comment")) != nullptr);
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
          ir_annot_get(ir_module_annots(m), S("ir.comment")) != nullptr);

    ir_block_t *b = ir_block_find(f, S("b"));

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

    core_string_t tight = S(
        "module m{\nprofile bce\nfunc @f()->void{\nblock e:\nreturn\n}\n}\n");
    core_string_t loose = S(
        "module m {\n\n  profile bce\n\n\n  func @f() -> void {\n"
        "    block e:\n\n      return\n  }\n}\n");

    core_string_t a = {};
    core_string_t b = {};

    check("a tightly spaced module parses", round_trips(ctx, tight, &a));
    check("a loosely spaced module parses", round_trips(ctx, loose, &b));
    check("whitespace does not survive: both print identically",
          a.data && b.data && core_string_eq(a, b));
    free(a.data);
    free(b.data);

    core_string_t commented = S(
        "module m {\n  profile bce\n\n  func @f() -> void {\n"
        "    block e:\n      # keep me\n      return\n  }\n}\n");
    core_string_t c = {};

    check("a commented module round-trips", round_trips(ctx, commented, &c));
    check("the comment survives", contains(c, S("# keep me")));
    free(c.data);

    ir_ctx_free(ctx);
}

static void test_types_are_typelang(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    core_string_t src = S(
        "module m {\n  profile bce\n\n"
        "  global @a: (y: i64, x: int)\n"
        "  global @b: __ty.tuple[int, i64]\n"
        "  global @c: (int)\n"
        "  global @d: (int | char & i64)\n"
        "  global @e: ref[int; owning, exclusive]\n"
        "  global @f: forall[`K, `V] core.D[`K, `V]\n"
        "}\n");

    core_string_t out = {};

    check("typelang types parse in the IR", round_trips(ctx, src, &out));

    if (out.data) {
        check("record fields print canonically",
              contains(out, S("@a: (x: int, y: i64)")));
        check("a tuple prints canonically",
              contains(out, S("@b: t(int, i64)")));
        check("grouping parens are dropped", contains(out, S("@c: int")));
        check("sums print canonically",
              contains(out, S("@d: (int | (i64 & char))")));
        check("indirection attributes print canonically",
              contains(out, S("@e: ref[int; owning, exclusive]")));
        check("binders rename canonically",
              contains(out, S("@f: forall[`a, `b] core.D[`a, `b]")));
    }
    free(out.data);

    ir_ctx_free(ctx);
}

static void test_trait_method_round_trips(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    core_string_t source = S(
        "module trait_dispatch {\n"
        "  profile bce\n"
        "  target test\n"
        "  extern func @add_requirement(%left: i32, %right: i32) -> i32 {\n"
        "    block entry:\n"
        "      unreachable\n"
        "  }\n"
        "  func @use(%left: i32, %right: i32) -> i32 {\n"
        "    block entry:\n"
        "      %method: (i32, i32) -> i32 = trait.method \"Add\", \"__add\", %left, @add_requirement\n"
        "      %value: i32 = call %method, %left, %right\n"
        "      return %value\n"
        "  }\n"
        "}\n");
    core_string_t printed = {};
    check("semantic trait method calls round-trip without a dispatch ABI",
          round_trips(ctx, source, &printed));
    check("trait and method identity survive IR text",
          contains(printed,
              S("trait.method \"Add\", \"__add\", %left, @add_requirement")));
    free(printed.data);
    ir_ctx_free(ctx);
}

static void test_function_failure_channel_round_trips(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    core_string_t src = S(
        "module m {\n"
        "  profile bce\n"
        "  target t\n\n"
        "  func @fails() -> i32 fail: str {\n"
        "    block entry:\n"
        "      return 0\n"
        "  }\n"
        "}\n");
    ir_diags_t *diagnostics = ir_diags_new(ctx, nullptr);
    ir_module_t *module = ir_module_parse(ctx, src, 0, PROFILES,
        sizeof(PROFILES) / sizeof(PROFILES[0]), diagnostics);
    ir_func_t *function = module ? ir_func_find(module, S("fails")) : nullptr;
    check("text IR retains a function failure channel",
          function && th_func_error(function->type)
              && ir_type_eq(module, th_func_error(function->type),
                            ir_type_parse(module, S("str"))));

    core_string_t printed = module ? ir_module_print(module)
                                   : (core_string_t){};
    check("text IR prints the function failure channel",
          contains(printed, S("-> i32 fail: str")));
    ir_diags_t *roundtrip_diagnostics = ir_diags_new(ctx, nullptr);
    ir_module_t *roundtrip = printed.data
        ? ir_module_parse(ctx, printed, 0, PROFILES,
            sizeof(PROFILES) / sizeof(PROFILES[0]), roundtrip_diagnostics)
        : nullptr;
    ir_func_t *roundtrip_function = roundtrip
        ? ir_func_find(roundtrip, S("fails")) : nullptr;
    check("the printed failure channel reparses in the same type universe",
          roundtrip_function && th_func_error(roundtrip_function->type)
              && ir_type_eq(roundtrip,
                  th_func_error(roundtrip_function->type),
                  th_func_error(function->type)));

    free(printed.data);
    ir_ctx_free(ctx);
}

static void test_consume_parameters_round_trip(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    core_string_t src = S(
        "module consume_params {\n"
        "  profile bce\n"
        "  func @take(consume %first: i32, consume *%rest: i32) -> void {\n"
        "    block entry:\n"
        "      return\n"
        "  }\n"
        "}\n");
    core_string_t printed = {};
    bool ok = round_trips(ctx, src, &printed);
    check("consume positional and rest modes round-trip in IR text",
          ok && printed.data
              && contains(printed, S("consume %first: i32"))
              && contains(printed, S("consume *%rest: i32")));
    free(printed.data);
    ir_ctx_free(ctx);
}

static void test_phase_result_text_round_trips(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    core_string_t src = S(
        "module m {\n"
        "  profile bce\n"
        "  target t\n\n"
        "  func @consumer() -> i32 {\n"
        "    block entry:\n"
        "      %result: i32 = phase.result !ir.phase_result(\"build.value.1\")\n"
        "      return %result\n"
        "  }\n"
        "}\n");
    ir_diags_t *diagnostics = ir_diags_new(ctx, nullptr);
    ir_module_t *module = ir_module_parse(ctx, src, 0, PROFILES,
        sizeof(PROFILES) / sizeof(PROFILES[0]), diagnostics);
    ir_func_t *consumer = module ? ir_func_find(module, S("consumer"))
                                 : nullptr;
    ir_instr_t *phase_result = consumer && consumer->n_blocks
        && consumer->blocks[0]->n_instrs
        ? consumer->blocks[0]->instrs[0] : nullptr;
    check("text IR parses typed cross-phase result references",
          phase_result && phase_result->op == IR_OP_PHASE_RESULT
              && ir_instr_phase_result(phase_result).data
              && core_string_eq(ir_instr_phase_result(phase_result),
                                S("build.value.1")));
    check("text IR round-trips phase result identity",
          module && round_trips(ctx, src, nullptr));
    ir_ctx_free(ctx);
}

static void test_variant_type_definition_round_trips(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    core_string_t src = S(
        "module m {\n"
        "  profile bce\n"
        "  target t\n\n"
        "  type variant call_outcome = (ok: i32, error: str)\n"
        "}\n");
    core_string_t printed = {};
    check("a named variant type definition round-trips",
          round_trips(ctx, src, &printed));
    check("the variant kind is retained in canonical text",
          contains(printed,
              S("type variant call_outcome = (ok: i32, error: str)")));
    free(printed.data);
    ir_ctx_free(ctx);
}

static void test_semantic_cast_round_trip(void)
{
    core_string_t source = S(
        "module m {\n"
        "  profile bce\n"
        "  target host\n"
        "  type Amount = i32\n"
        "  func @f(%value: i32) -> Erased {\n"
        "    block entry:\n"
        "      %amount: Amount = nominal.cast %value\n"
        "      %raw: i32 = nominal.cast %amount\n"
        "      %boxed: Erased = box %raw\n"
        "      return %boxed\n"
        "  }\n"
        "}\n");
    ir_ctx_t *context = ir_ctx_new();
    ir_diags_t *diags = nullptr;
    ir_module_t *module = parse(context, source, &diags);
    bool verified = false;
    if (module) {
        ir_module_establish(module, IR_PROP_PARSED | IR_PROP_TYPED);
        verified = (ir_verify(module, diags) & IR_PROP_TYPED)
            && !ir_diags_failed(diags);
    }
    check("semantic cast text carries its representation and verifies independently",
          verified && round_trips(context, source, nullptr));
    ir_ctx_free(context);
}

static void test_mutability_round_trips(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    core_string_t src = S(
        "module m {\n  profile bce\n  target t\n\n"
        "  const global @k: i32 = 7\n\n"
        "  immutable global @j: i32 = 1\n\n"
        "  global @g: i32 = 2\n\n"
        "  func @f() -> i32 {\n"
        "    const local $a: i32 = 3\n"
        "    immutable local $b: i32\n"
        "    local $c: i32\n\n"
        "    block entry:\n      return @g\n  }\n}\n");

    ir_diags_t  *d = nullptr;
    ir_module_t *m = parse(ctx, src, &d);

    check("mutability parses", m != nullptr);

    if (!m) {
        ir_ctx_free(ctx);

        return;
    }

    ir_global_t *k = ir_global_find(m, S("k"));
    ir_global_t *j = ir_global_find(m, S("j"));
    ir_global_t *g = ir_global_find(m, S("g"));

    check("a const global is const", k && k->mutability == IR_MUT_CONST);
    check("an immutable global is immutable",
          j && j->mutability == IR_MUT_IMMUTABLE);
    check("an unmarked global is mutable",
          g && g->mutability == IR_MUT_MUTABLE);

    ir_func_t *f = ir_func_find(m, S("f"));

    check("the function is there", f != nullptr);

    if (f) {
        check("a const local is const",
              ir_local_find(f, S("a"))
                  && ir_local_find(f, S("a"))->mutability == IR_MUT_CONST);
        check("an immutable local is immutable",
              ir_local_find(f, S("b"))
                  && ir_local_find(f, S("b"))->mutability
                         == IR_MUT_IMMUTABLE);
        check("an unmarked local is mutable",
              ir_local_find(f, S("c"))
                  && ir_local_find(f, S("c"))->mutability == IR_MUT_MUTABLE);
    }

    core_string_t once = ir_module_print(m);

    check("printing keeps every marker",
          once.data && contains(once, S("const global @k"))
              && contains(once, S("immutable global @j"))
              && contains(once, S("\n  global @g"))
              && contains(once, S("const local $a"))
              && contains(once, S("immutable local $b"))
              && contains(once, S("    local $c")));

    ir_ctx_t     *c2    = ir_ctx_new();
    ir_diags_t   *d2    = nullptr;
    ir_module_t  *m2    = parse(c2, once, &d2);
    core_string_t twice = m2 ? ir_module_print(m2) : (core_string_t){};

    check("and reparsing gives the same text",
          twice.data && core_string_eq(once, twice));

    free(once.data);
    free(twice.data);
    ir_ctx_free(c2);
    ir_ctx_free(ctx);
}

static void test_global_string_initializer_round_trips(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    core_string_t source = S(
        "module strings {\n  profile bce\n  target t\n\n"
        "  const global @label: string = \"archive\\nvalue\"\n"
        "  const global @empty: string = \"\"\n"
        "  global @binary: string\n}\n");
    ir_diags_t *diagnostics = nullptr;
    ir_module_t *module = parse(ctx, source, &diagnostics);
    ir_global_t *label = module ? ir_global_find(module, S("label")) : nullptr;
    ir_global_t *empty = module ? ir_global_find(module, S("empty")) : nullptr;
    ir_global_t *binary = module ? ir_global_find(module, S("binary"))
                                 : nullptr;
    const char bytes[] = {'a', '\0', 'b'};
    core_string_t binary_text = core_string_borrow(bytes, sizeof(bytes));
    bool binary_set = binary && ir_global_set_string_initializer(
        module, binary, binary_text);
    check("global strings use typed static initializers, including empty strings",
          label && empty && label->has_static_initializer
              && label->static_initializer->kind == IR_GLOBAL_INIT_STRING
              && empty->has_static_initializer
              && empty->static_initializer->kind == IR_GLOBAL_INIT_STRING
              && binary_set && binary->has_static_initializer
              && binary->static_initializer->string.u8_bytes == sizeof(bytes)
              && core_string_eq(binary->static_initializer->string,
                                binary_text));
    core_string_t printed = module ? ir_module_print(module)
                                   : (core_string_t){};
    check("global string initializer spelling round-trips",
          contains(printed,
              S("const global @label: string = \"archive\\nvalue\""))
              && contains(printed, S("const global @empty: string = \"\""))
              && contains(printed,
                          S("global @binary: string = \"a\\u0000b\"")));
    ir_ctx_t *round_ctx = ir_ctx_new();
    ir_diags_t *round_diags = nullptr;
    ir_module_t *round_trip = printed.data
        ? parse(round_ctx, printed, &round_diags) : nullptr;
    check("printed global string initializers reparse", round_trip != nullptr);
    ir_global_t *round_binary = round_trip
        ? ir_global_find(round_trip, S("binary")) : nullptr;
    check("embedded NUL survives typed global initializer text round-trip",
          round_binary && round_binary->has_static_initializer
              && round_binary->static_initializer->kind
                  == IR_GLOBAL_INIT_STRING
              && round_binary->static_initializer->string.u8_bytes
                  == sizeof(bytes)
              && core_string_eq(round_binary->static_initializer->string,
                                binary_text));
    free(printed.data);
    ir_ctx_free(round_ctx);
    ir_ctx_free(ctx);
}

static void test_recursive_global_initializers(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    core_string_t source = S(
        "module init_tree {\n  profile bce\n  target t\n\n"
        "  type variant Choice = (number: i64, small: i8)\n\n"
        "  global @enabled: bool = true\n"
        "  global @answer: i64 = 42\n"
        "  global @ratio: f64 = 1.5\n"
        "  global @deferred: i64 = "
        "$\"default:init_tree:deferred\"\n"
        "  global @numbers: list[i64] = [1, 2, 3]\n"
        "  global @fixed_array: array[i64; 3] = [1, 2, 3]\n"
        "  global @fixed_matrix: array[array[i64; 2]; 2] = "
        "[[1, 2], [3, 4]]\n"
        "  global @pair: __ty.tuple[i64, string] = (7, \"seven\")\n"
        "  global @state: (values: list[i64], ready: bool) = "
        "{values = [4, 5], ready = true}\n"
        "  global @flags: set[i64] = set{1, 2}\n"
        "  global @empty_flags: set[i64] = set{}\n"
        "  global @lookup: dict[string, i64] = "
        "dict{\"one\" => 1, \"two\" => 2}\n"
        "  global @empty_lookup: dict[string, i64] = dict{}\n"
        "  global @word: string = \"nil\"\n"
        "  global @truth_word: string = \"true\"\n"
        "  global @chosen_type: typespec = type(i64)\n"
        "  global @selected: variant Choice = "
        "variant{number = 7}\n"
        "  global @small: variant Choice = "
        "variant{small = 3}\n"
        "}\n");
    ir_module_t *module = parse(ctx, source, nullptr);
    ir_global_t *numbers = module ? ir_global_find(module, S("numbers"))
                                  : nullptr;
    ir_global_t *fixed_array = module
        ? ir_global_find(module, S("fixed_array")) : nullptr;
    ir_global_t *fixed_matrix = module
        ? ir_global_find(module, S("fixed_matrix")) : nullptr;
    ir_global_t *enabled = module ? ir_global_find(module, S("enabled"))
                                  : nullptr;
    ir_global_t *answer = module ? ir_global_find(module, S("answer"))
                                 : nullptr;
    ir_global_t *ratio = module ? ir_global_find(module, S("ratio")) : nullptr;
    ir_global_t *deferred = module
        ? ir_global_find(module, S("deferred")) : nullptr;
    ir_global_t *pair = module ? ir_global_find(module, S("pair")) : nullptr;
    ir_global_t *state = module ? ir_global_find(module, S("state")) : nullptr;
    ir_global_t *flags = module ? ir_global_find(module, S("flags")) : nullptr;
    ir_global_t *empty_flags = module
        ? ir_global_find(module, S("empty_flags")) : nullptr;
    ir_global_t *lookup = module ? ir_global_find(module, S("lookup"))
                                 : nullptr;
    ir_global_t *empty_lookup = module
        ? ir_global_find(module, S("empty_lookup")) : nullptr;
    ir_global_t *word = module ? ir_global_find(module, S("word")) : nullptr;
    ir_global_t *truth_word = module
        ? ir_global_find(module, S("truth_word")) : nullptr;
    ir_global_t *chosen_type = module
        ? ir_global_find(module, S("chosen_type")) : nullptr;
    ir_global_t *selected = module
        ? ir_global_find(module, S("selected")) : nullptr;
    ir_global_t *small = module
        ? ir_global_find(module, S("small")) : nullptr;
    check("recursive aggregate global initializers parse", module != nullptr);
    check("scalar globals use the same typed initializer representation",
          enabled && answer && ratio
              && enabled->has_static_initializer
              && enabled->static_initializer->kind == IR_GLOBAL_INIT_BOOL
              && enabled->static_initializer->boolean
              && answer->has_static_initializer
              && answer->static_initializer->kind == IR_GLOBAL_INIT_INT
              && ir_lit_signed(answer->static_initializer->integer) == 42
              && ratio->has_static_initializer
              && ratio->static_initializer->kind == IR_GLOBAL_INIT_FLOAT
              && ratio->static_initializer->floating == 1.5
              && deferred && deferred->has_static_initializer
              && deferred->static_initializer->kind
                  == IR_GLOBAL_INIT_PHASE_RESULT
              && core_string_eq(
                  deferred->static_initializer->phase_result.identifier,
                  S("default:init_tree:deferred")));
    bool result_initializer = answer
        && ir_global_set_phase_result_initializer(module, answer,
                                                   S("default:init_tree:answer"));
    check("a phase result replaces literal data and remains type-valid",
          result_initializer
              && ir_global_static_initializer_valid(module, answer)
              && answer->static_initializer->kind
                  == IR_GLOBAL_INIT_PHASE_RESULT
              && ir_global_set_phase_result_initializer(module, answer,
                   S("default:init_tree:answer"))
              && !ir_global_set_phase_result_initializer(module, answer,
                   S("other:answer")));
    ir_global_init_value_t folded_answer = {
        .kind = IR_GLOBAL_INIT_INT,
        .integer = {.mag = 43},
    };
    check("a matching phase result becomes a typed static initializer",
          answer && !ir_global_materialize_phase_result(module, answer,
              S("other:answer"), &folded_answer)
              && ir_global_materialize_phase_result(module, answer,
                  S("default:init_tree:answer"), &folded_answer)
              && answer->static_initializer->kind == IR_GLOBAL_INIT_INT
              && ir_lit_signed(answer->static_initializer->integer) == 43
              && ir_global_static_initializer_valid(module, answer));
    check("sequence initializer is retained and type-valid",
          numbers && numbers->has_static_initializer
              && ir_global_static_initializer_valid(module, numbers)
              && numbers->static_initializer->kind
                  == IR_GLOBAL_INIT_SEQUENCE
              && numbers->static_initializer->aggregate.count == 3);
    uint64_t array_extent = 0, matrix_extent = 0, row_extent = 0;
    th_type *matrix_row = fixed_matrix
        ? th_seq_elem(th_resolve(fixed_matrix->type)) : nullptr;
    check("fixed array initializers retain rank and every declared extent",
          fixed_array && fixed_matrix
              && ir_global_static_initializer_valid(module, fixed_array)
              && ir_global_static_initializer_valid(module, fixed_matrix)
              && th_seq_extent(th_resolve(fixed_array->type), &array_extent)
              && array_extent == 3
              && th_seq_extent(th_resolve(fixed_matrix->type),
                               &matrix_extent)
              && matrix_extent == 2 && matrix_row
              && th_seq_extent(th_resolve(matrix_row), &row_extent)
              && row_extent == 2
              && fixed_matrix->static_initializer->aggregate.items[1]
                     .aggregate.count == 2);
    check("tuple initializer retains its nested string value",
          pair && pair->has_static_initializer
              && pair->static_initializer->kind == IR_GLOBAL_INIT_TUPLE
              && pair->static_initializer->aggregate.count == 2
              && pair->static_initializer->aggregate.items[1].kind
                  == IR_GLOBAL_INIT_STRING);
    check("record initializer recursively retains sequence and boolean",
          state && state->has_static_initializer
              && state->static_initializer->kind == IR_GLOBAL_INIT_RECORD
              && state->static_initializer->aggregate.count == 2
              && state->static_initializer->aggregate.items[0]
                     .aggregate.count == 2
              && state->static_initializer->aggregate.items[1].kind
                  == IR_GLOBAL_INIT_BOOL
              && state->static_initializer->aggregate.items[1].boolean);
    check("set and dictionary initializers retain their kinds",
          flags && lookup && flags->has_static_initializer
              && flags->static_initializer->kind == IR_GLOBAL_INIT_SET
              && lookup->has_static_initializer
              && lookup->static_initializer->kind
                  == IR_GLOBAL_INIT_DICTIONARY
              && lookup->static_initializer->aggregate.count == 4);
    check("empty set and dictionary initializers are represented",
          empty_flags && empty_lookup
              && ir_global_static_initializer_valid(module, empty_flags)
              && ir_global_static_initializer_valid(module, empty_lookup)
              && empty_flags->static_initializer->kind == IR_GLOBAL_INIT_SET
              && empty_flags->static_initializer->aggregate.count == 0
              && empty_lookup->static_initializer->kind
                  == IR_GLOBAL_INIT_DICTIONARY
              && empty_lookup->static_initializer->aggregate.count == 0);
    check("variant initializer text retains selected arms",
          selected && small && selected->has_static_initializer
              && small->has_static_initializer
              && selected->static_initializer->kind
                  == IR_GLOBAL_INIT_VARIANT
              && selected->static_initializer->aggregate.items[0].kind
                  == IR_GLOBAL_INIT_INT
              && small->static_initializer->aggregate.items[0].kind
                  == IR_GLOBAL_INIT_INT);
    check("quoted nil remains a string initializer",
          word && word->has_static_initializer
              && word->static_initializer->kind == IR_GLOBAL_INIT_STRING
              && word->static_initializer->string.u8_bytes == 3
              && core_string_eq(word->static_initializer->string, S("nil")));
    check("quoted boolean spelling remains a string initializer",
          truth_word && truth_word->has_static_initializer
              && truth_word->static_initializer->kind
                  == IR_GLOBAL_INIT_STRING
              && truth_word->static_initializer->string.u8_bytes == 4
              && core_string_eq(truth_word->static_initializer->string,
                                S("true")));
    check("type descriptors retain their structured referent",
          chosen_type && chosen_type->has_static_initializer
              && chosen_type->static_initializer->kind == IR_GLOBAL_INIT_TYPE
              && ir_type_eq(module,
                  chosen_type->static_initializer->descriptor_type,
                  th_name(ir_ctx_types(ctx), S("i64"))));
    check("recursive initializer text round-trips",
          round_trips(ctx, source, nullptr));
    ir_ctx_t *invalid_ctx = ir_ctx_new();
    check("bare identifiers are not static string initializers",
          !parse(invalid_ctx,
              S("module invalid {\n  profile bce\n  target t\n\n"
                "  global @value: string = unknown\n}\n"), nullptr));
    ir_ctx_free(invalid_ctx);
    ir_ctx_free(ctx);
}

static void test_phase_mutability_round_trips(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    core_string_t src = S(
        "module phase {\n  profile bce\n  target host\n\n"
        "  const global @config: i32 = 1 !ir.comptime_mutability(0)\n\n"
        "  func @init(%source: i32, %target: ptr[i32]) -> i32 "
        "!ir.execution_phase(1) {\n"
        "    local $captured: i32\n"
        "    block entry:\n"
        "      $captured = copy %source\n"
        "      return %source\n  }\n}\n");
    ir_diags_t *diagnostics = nullptr;
    ir_module_t *module = parse(ctx, src, &diagnostics);
    ir_global_t *global = module ? ir_global_find(module, S("config"))
                                 : nullptr;
    ir_func_t *function = module ? ir_func_find(module, S("init")) : nullptr;
    ir_value_t *source = function ? ir_value_find(function, S("source"))
                                  : nullptr;
    ir_value_t *target = function ? ir_value_find(function, S("target"))
                                  : nullptr;
    ir_local_t *captured = function
        ? ir_local_find(function, S("captured")) : nullptr;
    ir_instr_t *copy_instruction = nullptr;
    ir_block_t *entry = function ? ir_block_entry(function) : nullptr;
    for (uint32_t i = 0; entry && i < entry->n_instrs; i++)
        if (entry->instrs[i]->op == IR_OP_COPY)
            copy_instruction = entry->instrs[i];

    check("phase-specific mutability metadata parses",
          module && global && function);
    check("runtime and compile-time mutability remain distinct",
          global && global->mutability == IR_MUT_CONST
              && ir_global_comptime_mutability(global) == IR_MUT_MUTABLE);
    check("function execution phase parses",
          function
              && ir_func_execution_phase(function) == IR_PHASE_COMPTIME);
    check("phase-specific metadata survives text round-trip",
          round_trips(ctx, src, nullptr));
    check("global storage rejects an unhonored required class",
          global && !ir_global_set_storage_class(
              ctx, global, S("pool"), true, S("gc")));
    bool binding_facts = source && target && captured && copy_instruction
        && ir_value_set_phase_access(ctx, source, IR_PHASE_ACCESS_BOTH,
            IR_PHASE_ACCESS_NONE, IR_PHASE_ACCESS_NONE)
        && ir_value_set_indirect_access(ctx, target,
            IR_INDIRECT_ACCESS_READ_WRITE)
        && ir_value_set_phase_access(ctx, ir_local_value(function, captured),
            IR_PHASE_ACCESS_BOTH, IR_PHASE_ACCESS_NONE,
            IR_PHASE_ACCESS_COMPTIME)
        && ir_instr_set_binding_initialization(copy_instruction, true);
    bool phase_facts = global && function && binding_facts
        && ir_global_set_phase_access(ctx, global,
            IR_PHASE_ACCESS_BOTH, IR_PHASE_ACCESS_COMPTIME,
            IR_PHASE_ACCESS_COMPTIME)
        && ir_global_set_storage_class(ctx, global, S("pool"), true,
                                       S("pool"))
        && ir_global_set_storage_duration(
            ctx, global, IR_STORAGE_DURATION_MODULE)
        && ir_func_set_initializer(ctx, function, true);
    core_string_t serialized = phase_facts ? ir_module_print(module)
                                           : (core_string_t){};
    ir_module_t *copy = serialized.data ? parse(ctx, serialized, nullptr)
                                        : nullptr;
    ir_global_t *copy_global = copy
        ? ir_global_find(copy, S("config")) : nullptr;
    ir_func_t *copy_function = copy
        ? ir_func_find(copy, S("init")) : nullptr;
    ir_value_t *copy_source = copy_function
        ? ir_value_find(copy_function, S("source")) : nullptr;
    ir_value_t *copy_target = copy_function
        ? ir_value_find(copy_function, S("target")) : nullptr;
    ir_local_t *copy_captured = copy_function
        ? ir_local_find(copy_function, S("captured")) : nullptr;
    ir_instr_t *copy_binding_init = nullptr;
    ir_block_t *copy_entry = copy_function
        ? ir_block_entry(copy_function) : nullptr;
    for (uint32_t i = 0; copy_entry && i < copy_entry->n_instrs; i++)
        if (copy_entry->instrs[i]->op == IR_OP_COPY
            && ir_instr_is_binding_initialization(copy_entry->instrs[i]))
            copy_binding_init = copy_entry->instrs[i];
    bool storage_required = false;
    core_string_t storage_requested = {};
    core_string_t storage_selected = ir_global_storage_class(
        copy_global, &storage_required, &storage_requested);
    check("global phase masks and initializer identity survive IR round-trip",
          copy_global && copy_function
              && ir_global_can_read(copy_global, IR_PHASE_COMPTIME)
              && ir_global_can_read(copy_global, IR_PHASE_RUNTIME)
              && ir_global_can_write(copy_global, IR_PHASE_COMPTIME)
              && !ir_global_can_write(copy_global, IR_PHASE_RUNTIME)
              && ir_global_can_initialize(copy_global, IR_PHASE_COMPTIME)
              && ir_func_is_initializer(copy_function));
    check("global storage duration and required class survive IR round-trip",
          copy_global
              && ir_global_storage_duration(copy_global)
                     == IR_STORAGE_DURATION_MODULE
              && storage_required && storage_requested.data
              && storage_selected.data
              && core_string_eq(storage_requested, S("pool"))
              && core_string_eq(storage_selected, S("pool")));
    check("value phase facts and declaration initialization survive IR round-trip",
          copy_source && copy_captured && copy_binding_init
              && ir_value_can_read(copy_source, IR_PHASE_COMPTIME)
              && !ir_value_can_write(copy_source, IR_PHASE_COMPTIME)
              && ir_value_can_initialize(
                  ir_local_value(copy_function, copy_captured),
                  IR_PHASE_COMPTIME));
    check("indirect parameter effects survive IR text round-trip",
          copy_target
              && ir_value_indirect_access(copy_target)
                     == IR_INDIRECT_ACCESS_READ_WRITE);
    free(serialized.data);
    ir_ctx_free(ctx);
}

static void test_meta_round_trip(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    core_string_t src = S(
        "module m {\n  profile bce\n"
        "  meta !ir.note(\"module level\")\n\n"
        "  func @f(%n: i32) -> i32 !loc(0, 1, 2, 3, 4, 5, 6) {\n"
        "    local $x: i32 !loc(0, 7, 8, 9, 10, 11, 12)\n"
        "\n    block e: !loc(0, 13, 14, 15, 16, 17, 18)\n"
        "      %t: i32 = copy %n !loc(0, 19, 20, 21, 22, 23, 24)"
        " !origin(generated, \"promote\", 0, 2, 3, 4, 5, 6, 7)"
        " !origin(source, \"\", 0, 1, 2, 3, 4, 5, 6)"
        " !ir.rule(\"sint\" from typed) !bce.pin(true) !ir.n(42)\n"
        "      return %t\n  }\n}\n");

    ir_diags_t  *d = nullptr;
    ir_module_t *m = parse(ctx, src, &d);

    check("metadata parses", m != nullptr);

    if (!m) {
        core_string_t r = ir_diags_render_text(d, &(ir_render_t){0});

        printf("%.*s", SARG(r));
        free(r.data);
        ir_ctx_free(ctx);

        return;
    }

    ir_func_t  *f  = ir_func_find(m, S("f"));
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
          in->origin && in->origin->agent.data
              && core_string_eq(in->origin->agent, S("promote")));
    check("the origin kind survives",
          in->origin && in->origin->kind == IR_ORIGIN_GENERATED_BY);
    check("an outer origin survives",
          in->origin && in->origin->parent
              && in->origin->parent->kind == IR_ORIGIN_SOURCE);

    ir_annot_t *r = ir_annot_get(&in->annots, S("ir.rule"));

    check("a string annotation survives",
          r && r->kind == IR_ANNOT_STR && core_string_eq(r->s, S("sint")));
    check("annotation provenance survives", r && r->derived_from
                                                 == IR_PROP_TYPED);

    ir_annot_t *pin = ir_annot_get(&in->annots, S("bce.pin"));

    check("a bool annotation survives",
          pin && pin->kind == IR_ANNOT_BOOL && pin->b);

    ir_annot_t *num = ir_annot_get(&in->annots, S("ir.n"));

    check("an int annotation survives",
          num && num->kind == IR_ANNOT_INT && ir_lit_signed(num->i) == 42);
    check("a module annotation survives",
          ir_annot_get(ir_module_annots(m), S("ir.note")) != nullptr);

    check("metadata round-trips", round_trips(ctx, src, nullptr));

    ir_ctx_free(ctx);
}

static void test_binding_facts_round_trip(void)
{
    core_string_t src = S(
        "module binding {\n  profile bce\n\n"
        "  global @owner: i32\n"
        "  global @view: i32 !ir.binding_phases(3) !ir.alias0(@owner)\n"
        "\n  func @f(%owner: i32, %view: i32 !ir.binding_phases(3)"
        " !ir.alias0(%owner)) -> i32 {\n"
        "    local $owner2: i32\n"
        "    local $view2: i32 !ir.alias0($owner2)\n"
        "\n    block entry:\n  }\n}\n");
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *diagnostics = nullptr;
    ir_module_t *module = parse(ctx, src, &diagnostics);
    check("binding facts parse on globals, parameters, and locals",
          module != nullptr);
    if (!module) {
        core_string_t rendered = ir_diags_render_text(diagnostics,
                                                      &(ir_render_t){0});
        printf("%.*s", SARG(rendered));
        free(rendered.data);
        ir_ctx_free(ctx);
        return;
    }

    ir_global_t *global_view = ir_global_find(module, S("view"));
    ir_annot_t *global_alias = global_view
        ? ir_annot_get(&global_view->annots, S("ir.alias0")) : nullptr;
    check("a global alias resolves to the referenced global",
          global_alias && global_alias->kind == IR_ANNOT_VALUE
              && global_alias->v && global_alias->v->global
              && core_string_eq(global_alias->v->global->name, S("owner")));

    ir_func_t *function = ir_func_find(module, S("f"));
    ir_value_t *parameter = function
        ? ir_value_find(function, S("view")) : nullptr;
    ir_annot_t *parameter_alias = parameter
        ? ir_annot_get(ir_value_annots(parameter), S("ir.alias0")) : nullptr;
    check("a parameter alias resolves to its peer parameter",
          parameter_alias && parameter_alias->kind == IR_ANNOT_VALUE
              && parameter_alias->v
              && core_string_eq(parameter_alias->v->name, S("owner")));

    ir_local_t *local_view = function
        ? ir_local_find(function, S("view2")) : nullptr;
    ir_value_t *local_value = function && local_view
        ? ir_local_value(function, local_view) : nullptr;
    ir_annot_t *local_alias = local_value
        ? ir_annot_get(ir_value_annots(local_value), S("ir.alias0"))
        : nullptr;
    check("a local alias resolves to its local storage identity",
          local_alias && local_alias->kind == IR_ANNOT_VALUE
              && local_alias->v && local_alias->v->local
              && core_string_eq(local_alias->v->local->name, S("owner2")));

    core_string_t printed = ir_module_print(module);
    ir_ctx_t *ctx2 = ir_ctx_new();
    ir_diags_t *diagnostics2 = nullptr;
    ir_module_t *round_trip = printed.data
        ? parse(ctx2, printed, &diagnostics2) : nullptr;
    check("binding and alias facts survive IR text round-trip",
          round_trip != nullptr);
    if (!round_trip && diagnostics2) {
        core_string_t rendered = ir_diags_render_text(diagnostics2,
                                                      &(ir_render_t){0});
        printf("%.*s", SARG(rendered));
        free(rendered.data);
    }
    core_string_t printed_again = round_trip ? ir_module_print(round_trip)
                                             : (core_string_t){};
    check("binding fact round-trip is text-stable",
          printed.data && printed_again.data
              && core_string_eq(printed, printed_again));
    free(printed.data);
    free(printed_again.data);
    ir_ctx_free(ctx2);
    ir_ctx_free(ctx);
}

static void test_both_forms(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    core_string_t nonssa = S(
        "module sum {\n  profile bce\n  target x86_64-linux\n\n"
        "  func @sum(%n: i32) -> i32 {\n"
        "    local $acc: i32\n    local $i: i32\n\n"
        "    block entry:\n      jump loop\n\n"
        "    block loop:\n      %more: bool = lt $i, %n\n"
        "      branch %more, body, done\n\n"
        "    block body:\n      $acc = add wrap $acc, $i\n"
        "      $i = add wrap $i, %n\n      jump loop\n\n"
        "    block done:\n      return $acc\n  }\n}\n");

    core_string_t ssa = S(
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
        "    block done:\n      return %acc_1\n  }\n}\n");

    check("the non-SSA form round-trips", round_trips(ctx, nonssa, nullptr));
    check("the SSA form round-trips", round_trips(ctx, ssa, nullptr));

    ir_ctx_free(ctx);
}

static void test_cross_profile(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    core_string_t synth = S(
        "module m {\n  profile synth\n\n"
        "  func @f(%n: int) -> int {\n"
        "    block e:\n      %t: int = add wrap %n, %n\n"
        "      return %t\n  }\n}\n");

    check("a module under the second profile round-trips",
          round_trips(ctx, synth, nullptr));

    ir_diags_t  *d = nullptr;
    ir_module_t *m = parse(ctx, S("module m {\n profile nope\n}\n"), &d);

    check("an unknown profile is refused", m == nullptr);
    check("and it is reported", ir_diags_count(d) == 1);

    ir_ctx_free(ctx);
}

static void test_errors(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    core_string_t bad[] = {
        S(""),
        S("module"),
        S("module m"),
        S("module m {}"),
        S("module m {\n profile bce\n func @f() -> void {\n block e:\n"
          " frobnicate\n }\n}"),
        S("module m {\n profile bce\n func @f() -> void {\n block e:\n"
          " add wrap %a, %b\n }\n}"),
        S("module m {\n profile bce\n func @f() -> void {\n block e:\n"
          " %x: i32 = add sat wrap %a\n }\n}"),
        S("module m {\n profile bce\n func @f() -> nosuchtype[\n"),
        S("module m {\n profile bce\n func @f() -> void {\n block e:\n"
          " jump nowhere\n }\n}"),
        S("module m {\n profile bce\n func @f() -> void {\n block e:\n"
          " %x: i32 = and ordered %a, %b\n }\n}"),
        S("module m {\n profile bce\n target t\n"
          " global @bad: i32 = \"wrong\"\n}"),
        S("module m {\n profile bce\n target t\n"
          " global @bad: list[i32] = [1, \"wrong\"]\n}"),
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

static const core_string_t TYPES[] = {
    S("i32"),      S("u64"),           S("f64"),
    S("bool"),     S("i8"),            S("ptr[i32]"),
    S("(x: i32)"), S("array[i32; 4]"), S("t(i32, u64)")};

static const core_string_t MODS_FOR[] = {
    S("wrap"),     S("trap"),    S("sat"),       S("undef"),
    S("mask"),     S("ordered"), S("unordered"), S("volatile"),
    S("atomic"),   S("to_zero"), S("nearest"),   S("floor"),
    S("ceil")};

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

        case IR_OPD_MISSING:
            ir_instr_operand_missing(in);
            break;

        case IR_OPD_TYPE:
            ir_instr_operand_type(in, ir_type_parse(m, TYPES[rnd(9)]));
            break;

        case IR_OPD_NAME:
            ir_instr_operand_name(in, rnd(2) ? S("x") : S("some.field"));
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
                ir_instr_operand_named(in,
                                       rnd(2) ? S("timeout") : S("retries"),
                                    vals->v[rnd(vals->n)]);
            }
            else {
                ir_instr_operand_value(in, vals->v[rnd(vals->n)]);
            }
            break;

        case IR_OPD_OUT:
            ir_instr_operand_out(in);
            break;

        case IR_OPD_FRESH:
            return;
        }
    }
}

static ir_module_t *gen_module(ir_ctx_t *ctx, uint32_t seq)
{
    const ir_profile_t *pro = rnd(2) ? &ir_profile_bce : &ir_profile_synth;
    char                name[32];
    int                 name_len = snprintf(name, sizeof(name), "gen%u", seq);

    ir_module_t *m = ir_module_new(ctx,
                                   core_string_borrow(name, (size_t)name_len),
                                   pro,
                                   rnd(2) ? S("host") : (core_string_t){});

    ir_file_add(m, S("gen.src"));

    core_string_t scalar = pro == &ir_profile_bce ? S("i32") : S("int");
    th_type      *st     = ir_type_parse(m, scalar);

    if (rnd(2)) {
        ir_global_add(m, S("g"), st);
    }

    uint32_t nfuncs = 1 + rnd(2);

    for (uint32_t fi = 0; fi < nfuncs; fi++) {
        char fname[32];
        int  fname_len = snprintf(fname, sizeof(fname), "f%u", fi);

        char sig[128];
        int  sig_len = snprintf(sig, sizeof(sig), "(%.*s) -> %.*s",
                                SARG(scalar), SARG(scalar));

        ir_func_t *f = ir_func_add(
            m, core_string_borrow(fname, (size_t)fname_len),
            ir_type_parse(m, core_string_borrow(sig, (size_t)sig_len)),
            (ir_linkage_t)rnd(3));

        if (!f) {
            continue;
        }

        pool vals = {0};

        vals.v[vals.n++] = ir_param_add(f, S("p0"), st);

        uint32_t nlocals = rnd(3);

        for (uint32_t i = 0; i < nlocals; i++) {
            char ln[16];
            int  ln_len = snprintf(ln, sizeof(ln), "l%u", i);

            ir_local_add(f, core_string_borrow(ln, (size_t)ln_len), st);
        }

        uint32_t nblocks = 2 + rnd(3);

        for (uint32_t i = 0; i < nblocks; i++) {
            char bn[16];
            int  bn_len = snprintf(bn, sizeof(bn), "b%u", i);

            ir_block_add(f, core_string_borrow(bn, (size_t)bn_len));
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
                    int  vn_len = snprintf(vn, sizeof(vn), "v%u_%u", bi, k);

                    if (ir_instr_set_dest(
                            in, core_string_borrow(vn, (size_t)vn_len),
                            ir_type_parse(m, TYPES[rnd(9)]))
                        && in->dest) {
                        vals.v[vals.n++] = in->dest;
                    }
                }

                if (rnd(3) == 0) {
                    in->span = ir_span_make(0, k, k + 2, k + 1, 1, k + 1, 3);
                }

                if (rnd(4) == 0) {
                    in->origin = ir_origin_new(ctx, IR_ORIGIN_GENERATED_BY,
                                               in->span, S("gen"), nullptr);
                }

                if (rnd(4) == 0) {
                    ir_annot_set(ctx, &in->annots,
                                 (ir_annot_t){.key = S("ir.rule"),
                                              .kind = IR_ANNOT_STR,
                                              .s = S("made-up"),
                                              .derived_from = IR_PROP_TYPED});
                }

                if (rnd(5) == 0) {
                    ir_annot_set(ctx, &in->annots,
                                 (ir_annot_t){.key = S("ir.comment"),
                                              .kind = IR_ANNOT_STR,
                                              .s = S("generated\nline two")});
                }
            }

            static const ir_op_t terms[] = {IR_OP_JUMP, IR_OP_RETURN,
                                            IR_OP_BRANCH, IR_OP_SWITCH,
                                            IR_OP_UNREACHABLE, IR_OP_FAIL,
                                            IR_OP_TRAP};
            ir_op_t     term = terms[next_term++ % (sizeof(terms) / sizeof(terms[0]))];
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
            case IR_OP_TRAP:
                if (vals.n && rnd(2)) {
                    ir_instr_operand_value(t, vals.v[rnd(vals.n)]);
                }
                break;

            case IR_OP_FAIL:
                ir_instr_operand_value(t, vals.v[rnd(vals.n)]);
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

    core_string_t src = S(
        "module m {\n  profile bce\n\n"
        "  func @open(%path: i32, timeout: i32, retries: i32) -> i32 {\n"
        "    block e:\n      return %path\n  }\n\n"
        "  func @f(%n: i32) -> i32 {\n"
        "    block e:\n"
        "      %r: i32 = call @open, %n, timeout = %n, retries = %n\n"
        "      return %r\n  }\n}\n");

    ir_diags_t  *d = nullptr;
    ir_module_t *m = parse(ctx, src, &d);

    check("keyword arguments parse", m != nullptr);

    if (!m) {
        core_string_t r = ir_diags_render_text(d, &(ir_render_t){0});

        printf("%.*s", SARG(r));
        free(r.data);
        ir_ctx_free(ctx);

        return;
    }

    ir_func_t  *f  = ir_func_find(m, S("f"));
    ir_instr_t *in = ir_block_entry(f)->instrs[0];

    check("a call takes callee then arguments",
          in->op == IR_OP_CALL && in->n_operands == 4);
    check("a positional argument stays a value",
          in->operands[1].kind == IR_OPD_VALUE);
    check("a keyword argument keeps its name",
          in->operands[2].kind == IR_OPD_NAMED_ARG
              && core_string_eq(in->operands[2].named.name, S("timeout")));
    check("a keyword argument resolves its value",
          in->operands[2].named.value != nullptr
              && core_string_eq(in->operands[2].named.value->name, S("n")));
    check("keyword arguments round-trip", round_trips(ctx, src, nullptr));

    core_string_t fresh_src = S(
        "module fresh_target {\n  profile bce\n\n"
        "  func @init(inout %self: i32) -> void {\n"
        "    block e:\n      return\n  }\n\n"
        "  func @make() -> ptr[i32] {\n"
        "    block e:\n"
        "      %storage: ptr[i32] = alloc i32\n"
        "      %done: void = call @init, fresh %storage\n"
        "      return %storage\n  }\n}\n");
    check("fresh construction targets round-trip in call text",
          round_trips(ctx, fresh_src, nullptr));

    ir_ctx_free(ctx);
}

static void test_nfc_identifiers(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    core_string_t src = S(
        "module caf\xc3\xa9 {\n  profile bce\n\n"
        "  global @na\xc3\xafve: i32\n\n"
        "  func @\xe6\xbc\xa2(%\xc3\xa9: i32) -> i32 {\n"
        "    local $\xcf\x80: i32\n\n"
        "    block \xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba:\n"
        "      $\xcf\x80 = copy %\xc3\xa9\n"
        "      return $\xcf\x80\n  }\n}\n");

    core_string_t out = {};

    check("non-ASCII identifiers parse and round-trip",
          round_trips(ctx, src, &out));
    check("a non-ASCII module name survives",
          contains(out, S("module caf\xc3\xa9")));
    check("a non-ASCII global survives",
          contains(out, S("@na\xc3\xafve")));
    check("a non-ASCII block name survives",
          contains(out, S("block \xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba")));
    free(out.data);

    ir_ctx_free(ctx);
}

#define MOD(...) S("module m {\n  profile bce\n  target t\n\n" __VA_ARGS__ "}\n")

static void test_an_error_points_at_what_it_is_about(void)
{
    static const struct {
        core_string_t what;
        core_string_t token;
        core_string_t src;
    } cases[] = {
        {S("unknown local"),
         S("$nope"),
         MOD("  func @f(%a: i32) -> i32 {\n    block entry:\n"
             "      %r: i32 = copy $nope\n      return %r\n  }\n")},
        {S("unknown local as destination"),
         S("$gone"),
         MOD("  func @f(%a: i32) -> i32 {\n    block entry:\n"
             "      $gone = copy %a\n      return %a\n  }\n")},
        {S("unknown value"),
         S("%missing"),
         MOD("  func @f(%a: i32) -> i32 {\n    block entry:\n"
             "      return %missing\n  }\n")},
        {S("unknown opcode"),
         S("frobnicate"),
         MOD("  func @f(%a: i32) -> i32 {\n    block entry:\n"
             "      %r: i32 = frobnicate %a\n      return %r\n  }\n")},
        {S("duplicate local"),
         S("$x"),
         MOD("  func @f(%a: i32) -> i32 {\n    local $x: i32\n"
             "    local $x: i64\n    block entry:\n"
             "      return %a\n  }\n")},
        {S("unknown profile"),
         S("nosuch"),
         S("module m {\n  profile nosuch\n  target t\n\n"
           "  func @f(%a: i32) -> i32 {\n    block entry:\n"
           "      return %a\n  }\n}\n")},
        {S("this opcode has no result"),
         S("%r"),
         MOD("  func @f(%a: i32) -> void {\n    block entry:\n"
             "      %r: i32 = return %a\n  }\n")},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ir_ctx_t   *ctx = ir_ctx_new();
        ir_diags_t *d   = ir_diags_new(ctx, nullptr);

        ir_module_parse(ctx, cases[i].src, 0, PROFILES, 2, d);

        if (!ir_diags_count(d)) {
            printf("FAIL  %.*s produced no diagnostic\n",
                   SARG(cases[i].what));
            fails++;
            ir_ctx_free(ctx);
            continue;
        }

        const ir_diag_t *dg  = ir_diags_at(d, 0);
        ir_span_t        s   = dg->span;
        size_t           n   = cases[i].token.u8_bytes;
        size_t           len = cases[i].src.u8_bytes;

        if (!ir_span_known(s) || s.end_byte > len || s.start_byte > s.end_byte) {
            printf("FAIL  %.*s has no usable span\n", SARG(cases[i].what));
            fails++;
            ir_ctx_free(ctx);
            continue;
        }

        size_t        got = s.end_byte - s.start_byte;
        core_string_t at  = core_string_slice(cases[i].src, s.start_byte, got);

        if (got != n || !core_string_eq(at, cases[i].token)) {
            printf("FAIL  %.*s points at \"%.*s\", not \"%.*s\"\n",
                   SARG(cases[i].what), SARG(at), SARG(cases[i].token));
            fails++;
        }
        ir_ctx_free(ctx);
    }
    check("every parse error points at the token it is about", true);
}

static void test_spans_come_from_the_text(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    core_string_t src = S("module m {\n"
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
                          "}\n");

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

    core_string_t out = ir_module_print(m);

    check("and is not printed back, since the text already says it",
          out.data && !contains(out, S("!loc")));
    free(out.data);

    core_string_t authored = S("module m {\n"
                               "  profile bce\n"
                               "  target t\n"
                               "\n"
                               "  func @f() -> void {\n"
                               "    block entry:\n"
                               "      unreachable !loc(3, 90, 95, 40, 2, 40, 7)\n"
                               "  }\n"
                               "}\n");

    ir_module_t *am = parse(ctx, authored, nullptr);

    check("an authored !loc parses", am != nullptr);

    if (am) {
        ir_instr_t *in = ir_block_entry(ir_func_at(am, 0))->instrs[0];

        check("an authored location wins over the one the text implies",
              in->span.start_line == 40 && in->span.file == 3);
        check("and is not marked implied", !in->span.implied);

        core_string_t back = ir_module_print(am);

        check("so it is printed back",
              contains(back, S("!loc(3, 90, 95, 40, 2, 40, 7)")));
        free(back.data);
    }

    ir_ctx_free(ctx);
}

static void test_literal_spellings(void)
{
    ir_ctx_t *ctx = ir_ctx_new();

    core_string_t src = S("module m {\n"
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
                          "}\n");

    core_string_t out = {};
    ir_module_t  *m   = parse(ctx, src, nullptr);

    check("a module of mixed literal spellings parses", m != nullptr);

    if (m) {
        out = ir_module_print(m);
    }

    check("a hexadecimal literal comes back as hexadecimal",
          contains(out, S("const 0xdeadbeef")));
    check("a decimal literal comes back as decimal",
          contains(out, S("const 255")));
    check("the whole of u128 survives the round trip",
          contains(out, S("const 0xffffffffffffffffffffffffffffffff")));
    check("so does the bottom of i128",
          contains(out,
                   S("const -170141183460469231731687303715884105728")));
    check("a float keeps the form it was written in",
          contains(out, S("const 1.5e10")));
    check("and keeps digits a double cannot hold",
          contains(out, S("const 1.2345678901234567890123456")));
    check("an annotation keeps the radix it was written in",
          contains(out, S("!ir.mask(0xcafe")));
    check("a case value keeps its radix too",
          contains(out, S("[0xff: entry]")));
    check("and the whole thing round-trips", round_trips(ctx, src, nullptr));
    free(out.data);

    ir_diags_t  *d  = nullptr;
    ir_module_t *bad = parse(ctx,
                             S("module m {\n  profile bce\n  target t\n\n"
                               "  func @f() -> void {\n    block entry:\n"
                               "      %k: u32 = const 0x1"
                               "00000000000000000000000000000000\n"
                               "  }\n}\n"),
                             &d);

    check("a hexadecimal literal wider than 128 bits is refused",
          bad == nullptr);

    ir_ctx_free(ctx);
}

static void test_canonical_golden(core_string_t dir)
{
    ir_ctx_t *ctx = ir_ctx_new();

    core_string_t src = S(
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
        "return %acc\n}\n}\n");

    ir_module_t *m = parse(ctx, src, nullptr);

    check("the golden source parses", m != nullptr);

    if (m) {
        core_string_t out = ir_module_print(m);

        check("canonical printing matches its golden",
              same_as_golden(dir, S("canonical.ir"), out));
        free(out.data);
    }

    ir_ctx_free(ctx);
}

static void test_generated_round_trip(void)
{
    for (uint32_t i = 0; i < trials; i++) {
        ir_ctx_t    *ctx = ir_ctx_new();
        ir_module_t  *m   = gen_module(ctx, i);
        core_string_t one = ir_module_print(m);
        ir_diags_t   *d   = nullptr;
        ir_module_t  *m2  = parse(ctx, one, &d);

        if (!m2) {
            core_string_t r = ir_diags_render_text(d, &(ir_render_t){0});

            printf("FAIL  generated module %u did not parse\n%.*s---- text ----"
                   "\n%.*s",
                   i, SARG(r), SARG(one));
            free(r.data);
            fails++;
            free(one.data);
            ir_ctx_free(ctx);

            return;
        }

        core_string_t two = ir_module_print(m2);

        if (!core_string_eq(one, two)) {
            printf("FAIL  generated module %u does not round-trip\n"
                   "---- once ----\n%.*s---- twice ----\n%.*s",
                   i, SARG(one), SARG(two));
            fails++;
            free(one.data);
            free(two.data);
            ir_ctx_free(ctx);

            return;
        }

        ir_module_t  *m3    = parse(ctx, two, nullptr);
        core_string_t three = m3 ? ir_module_print(m3) : (core_string_t){};

        if (!three.data || !core_string_eq(two, three)) {
            printf("FAIL  printing is not idempotent for module %u\n", i);
            fails++;
        }
        free(three.data);
        free(one.data);
        free(two.data);
        ir_ctx_free(ctx);
    }
}

static void test_the_grammar_lays_the_text_out(void)
{
    static const core_string_t WANT[] = {
        S("module m {\n"),
        S("  profile bce\n"),
        S("  target x86-64\n"),
        S("  global @g: i32 = 7\n"),
        S("  func @f(%a: i32, %b: ptr[i32]) -> i32 {\n"),
        S("    local $t: array[i32; 4]\n"),
        S("\n"),
        S("    block entry:\n"),
        S("      %c: i32 = add.wrap %a, %a !ir.note(\"hi\")\n"),
        S("      ret %c\n"),
        S("  }\n"),
        S("}\n"),
    };

    core_string_t src = S("module m {\n"
                          "profile bce\n"
                          "target x86-64\n"
                          "global @g:i32=7\n"
                          "func @f(%a:i32,%b:ptr[i32])->i32 {\n"
                          "local $t:array[i32;4]\n"
                          "block entry:\n"
                          "%c:i32=add.wrap %a,%a !ir.note(\"hi\")\n"
                          "ret %c\n"
                          "}\n"
                          "}\n");

    core_string_t got = ir_text_format(src, 80);

    check("unspaced source lays out", got.data != nullptr);

    if (!got.data) {
        return;
    }

    size_t at = 0;

    for (size_t i = 0; i < sizeof(WANT) / sizeof(WANT[0]); i++) {
        core_string_t rest = core_string_slice(got, at, got.u8_bytes - at);

        if (!core_string_starts_with(rest, WANT[i])) {
            printf("FAIL  line %zu is not what the annotations ask for\n"
                   "---- want ----\n%.*s---- got ----\n%.*s",
                   i, SARG(WANT[i]), SARG(rest));
            fails++;
            break;
        }
        at += WANT[i].u8_bytes;
    }
    free(got.data);
}

static void test_the_layout_annotations_round_trip(void)
{
    for (uint32_t i = 0; i < trials; i++) {
        ir_ctx_t    *ctx = ir_ctx_new();
        ir_module_t  *m   = gen_module(ctx, i);
        core_string_t one = ir_module_print(m);
        core_string_t fmt = ir_text_format(one, 80);

        if (!fmt.data) {
            printf("FAIL  generated module %u does not lay out\n---- text ----"
                   "\n%.*s",
                   i, SARG(one));
            fails++;
            free(one.data);
            ir_ctx_free(ctx);

            return;
        }

        ir_diags_t   *d   = nullptr;
        ir_module_t  *m2  = parse(ctx, fmt, &d);
        core_string_t two = m2 ? ir_module_print(m2) : (core_string_t){};

        if (!two.data || !core_string_eq(one, two)) {
            core_string_t r = ir_diags_render_text(d, &(ir_render_t){0});
            core_string_t reprinted
                = two.data ? two : S("(did not parse)\n");

            printf("FAIL  laid-out module %u is not the same module\n%.*s"
                   "---- printed ----\n%.*s---- laid out ----\n%.*s"
                   "---- reprinted ----\n%.*s",
                   i, SARG(r), SARG(one), SARG(fmt), SARG(reprinted));
            free(r.data);
            fails++;
            free(one.data);
            free(fmt.data);
            free(two.data);
            ir_ctx_free(ctx);

            return;
        }
        free(one.data);
        free(fmt.data);
        free(two.data);
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
                ir_loc_snapshot_free(before);
                continue;
            }
            ran[k]++;

            bool monotone = ir_loc_monotone(before, m, passes[k]->name, d);
            ir_loc_snapshot_free(before);
            if (!monotone) {
                printf("FAIL  %.*s lost a location on generated module %u\n",
                       SARG(passes[k]->name), i);
                fails++;
                break;
            }

            ir_diags_t *vd  = ir_diags_new(ctx, nullptr);
            ir_props_t  now = ir_verify(m, vd);

            if (!(now & IR_PROP_PARSED)) {
                core_string_t why  = ir_diags_render_text(vd,
                                                          &(ir_render_t){0});
                core_string_t text = ir_module_print(m);

                printf("FAIL  %.*s left generated module %u malformed\n%.*s"
                       "---- module ----\n%.*s",
                       SARG(passes[k]->name), i, SARG(why), SARG(text));
                free(why.data);
                free(text.data);
                fails++;
                break;
            }
            have = now;
        }
        ir_ctx_free(ctx);
    }

    for (size_t k = 0; k < sizeof(passes) / sizeof(passes[0]); k++) {
        if (!ran[k]) {
            printf("NOTE  %.*s never ran over the corpus: the generator does "
                   "not produce modules that meet what it requires\n",
                   SARG(passes[k]->name));
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
            core_string_t spelling = ir_op_spelling((ir_op_t)i);

            printf(" %.*s", SARG(spelling));
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
    core_string_t dir = argc > 1 ? core_string_from_cstr(argv[1])
                                 : core_string_dup(S("."));

    if (argc > 2) {
        core_string_t count = core_string_from_cstr(argv[2]);

        trials = (uint32_t)core_string_strtoull(count, 10);
        core_free(count.data);
    }

    test_lexis();
    test_whitespace_and_comments();
    test_types_are_typelang();
    test_trait_method_round_trips();
    test_function_failure_channel_round_trips();
    test_consume_parameters_round_trip();
    test_phase_result_text_round_trips();
    test_variant_type_definition_round_trips();
    test_mutability_round_trips();
    test_global_string_initializer_round_trips();
    test_recursive_global_initializers();
    test_phase_mutability_round_trips();
    test_meta_round_trip();
    test_binding_facts_round_trip();
    test_semantic_cast_round_trip();
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
    core_free(dir.data);

    return report("ir text");
}
