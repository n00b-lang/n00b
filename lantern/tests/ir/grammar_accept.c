#include <stdio.h>
#include <stdlib.h>

#include "ir/grammar.h"
#include "ir/ir.h"
#include "ir/text.h"

#include "lib/buffer.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
#include "scanner/token_stream.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static bool grammar_accepts(core_string_t src, core_string_t start)
{
    parse_grammar_t      *g  = ir_grammar();
    parse_token_stream_t *ts = ir_lex(src);

    if (!g || !ts) {
        return false;
    }

    parse_pwz_parser_t *p
        = parse_pwz_new_for_start(g, ir_grammar_nt(start));
    bool              ok = parse_pwz_parse(p, ts);

    parse_pwz_free(p);
    scan_token_stream_free(ts);

    return ok;
}

static bool parser_accepts(core_string_t src)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
    const ir_profile_t *const ps[] = {&ir_profile_bce, &ir_profile_synth};
    ir_module_t *m   = ir_module_parse(ctx, src, 0, ps, 2, d);
    bool         ok  = m != nullptr && !ir_diags_failed(d);

    ir_ctx_free(ctx);

    return ok;
}

static void agree(core_string_t what, core_string_t src)
{
    bool byparser  = parser_accepts(src);
    bool bygrammar = grammar_accepts(src, S("module"));

    if (!byparser) {
        printf("FAIL  %.*s: the parser does not accept it, so the case is "
               "vacuous\n",
               (int)what.u8_bytes, what.data);
        fails++;

        return;
    }

    if (!bygrammar) {
        printf("FAIL  %.*s: the parser accepts it, the grammar does not\n",
               (int)what.u8_bytes, what.data);
        fails++;
    }
}

static bool try_read(core_string_t path, core_string_t *out)
{
    char *cpath = core_string_to_cstr(path);
    FILE *f     = fopen(cpath, "rb");

    core_free(cpath);

    if (!f) {
        return false;
    }

    fseek(f, 0, SEEK_END);

    long n = ftell(f);

    rewind(f);

    char  *p = malloc((size_t)n + 1);
    size_t r = fread(p, 1, (size_t)n, f);

    fclose(f);
    *out = r ? core_string_from_raw(p, (int64_t)r) : core_string_empty();
    free(p);

    return true;
}

static void test_the_canonical_module(core_string_t path)
{
    core_string_t src;

    if (!try_read(path, &src)) {
        printf("FAIL  cannot open %.*s\n", (int)path.u8_bytes, path.data);
        fails++;

        return;
    }

    if (!parser_accepts(src)) {
        printf("FAIL  the parser no longer reads %.*s\n", (int)path.u8_bytes,
               path.data);
        fails++;
    }
    else if (!grammar_accepts(src, S("module"))) {
        printf("FAIL  the grammar does not derive %.*s\n", (int)path.u8_bytes,
               path.data);
        fails++;
    }

    core_free(src.data);
}

static void test_the_two_agree_construct_by_construct(void)
{
#define HEAD "module m {\n  profile bce\n"

    static const struct {
        core_string_t what;
        core_string_t src;
    } cases[] = {
        {S("an empty module"), S("module m {\n  profile bce\n}\n")},
        {S("a profile"), S("module m {\n  profile bce\n}\n")},
        {S("a target with dashes"),
         S("module m {\n  profile bce\n  target x86_64-linux\n}\n")},
        {S("a global"), S("module m {\n  profile bce\n  global @g: i32\n}\n")},
        {S("a global with linkage"),
         S("module m {\n  profile bce\n  static global @g: i32\n}\n")},
        {S("a record-typed global"),
         S("module m {\n  profile bce\n  global @g: (x: int, y: i64)\n}\n")},
        {S("a type declaration"),
         S("module m {\n  profile bce\n  type T = ptr[i32]\n}\n")},
        {S("an empty function"),
         S("module m {\n  profile bce\n  func @f() -> void {\n    block e:\n"
           "      unreachable\n  }\n}\n")},
        {S("parameters"),
         S("module m {\n  profile bce\n"
           "  func @f(%a: i32, %b: i32) -> i32 {\n    block e:\n"
           "      return %a\n  }\n}\n")},
        {S("a local"), S(HEAD "  func @f() -> void {\n    local $t: ptr[i32]\n"
                              "    block e:\n      unreachable\n  }\n}\n")},
        {S("a local with an initialiser"),
         S(HEAD "  func @f() -> void {\n    local $n: i32 = 7\n"
                "    block e:\n      unreachable\n  }\n}\n")},
        {S("a negative initialiser"),
         S(HEAD "  func @f() -> void {\n    local $n: i32 = -7\n"
                "    block e:\n      unreachable\n  }\n}\n")},
        {S("an instruction with a modifier"),
         S(HEAD "  func @f(%a: i32) -> i32 {\n    block e:\n"
                "      %s: i32 = add wrap %a, %a\n      return %s\n  }\n}\n")},
        {S("a phi"),
         S(HEAD "  func @f(%a: i32) -> i32 {\n    block e:\n      jump l\n"
                "    block l:\n      %p: i32 = phi [e: %a]\n      return %p\n"
                "  }\n}\n")},
        {S("a switch"),
         S(HEAD "  func @f(%a: i32) -> i32 {\n    block e:\n"
                "      switch %a, default: d, [0: d], [1: d]\n"
                "    block d:\n      return %a\n  }\n}\n")},
        {S("a location"),
         S(HEAD "  func @f() -> void {\n    block e:\n"
                "      unreachable !loc(0, 1, 2, 1, 3, 1, 4)\n  }\n}\n")},
        {S("an annotation with provenance"),
         S(HEAD "  func @f(%a: i32) -> i32 {\n    block e:\n"
                "      %s: i32 = add %a, %a !ir.rule(\"sint\" from typed)\n"
                "      return %s\n  }\n}\n")},
        {S("a module annotation"),
         S(HEAD "  meta !ir.note(\"hand written\")\n}\n")},
        {S("a sizeof operand"),
         S(HEAD "  func @f() -> u64 {\n    block e:\n"
                "      %z: u64 = sizeof (i32, i32) -> i32\n      return %z\n"
                "  }\n}\n")},
        {S("a comment"), S("module m {\n  # a comment\n  profile bce\n}\n")},
        {S("a name that is also a keyword"),
         S(HEAD "  func @f() -> void {\n    local $size: i32\n"
                "    block e:\n      unreachable\n  }\n}\n")},
        {S("a rest parameter"),
         S(HEAD "  func @f(%a: i32, *i32) -> void {\n"
                "    block e:\n      unreachable\n  }\n}\n")},
        {S("a keyword parameter"),
         S(HEAD "  func @f(%a: i32, k: i32) -> void {\n"
                "    block e:\n      unreachable\n  }\n}\n")},
        {S("extern linkage"),
         S(HEAD "  extern func @f() -> void {\n"
                "    block e:\n      unreachable\n  }\n}\n")},
        {S("an extern global"), S(HEAD "  extern global @g: i32\n}\n")},
        {S("a layout declaration"),
         S(HEAD "  layout i32 {\n    size = 4\n    align = 4\n"
                "    stride = 4\n  }\n}\n")},
        {S("a layout member pinned to bits"),
         S(HEAD "  layout (x: i32, y: i32) {\n    x @ bits 0 .. 32\n  }\n}\n")},
        {S("a symbolic layout value"),
         S(HEAD "  layout ptr[i32] {\n"
                "    size = sizeof(i32)\n    align = alignof(i32)\n  }\n}\n")},
        {S("an offsetof layout value"),
         S(HEAD "  layout (x: i32) {\n"
                "    size = offsetof((x: i32), \"x\")\n  }\n}\n")},
        {S("an origin"),
         S(HEAD "  func @f() -> void {\n    block e:\n"
                "      unreachable !origin(source, \"\", 0, 1, 2, 3, 4, 5, 6)\n"
                "  }\n}\n")},
        {S("a float literal"),
         S(HEAD "  func @f() -> void {\n    local $x: f64 = 1.5\n"
                "    block e:\n      unreachable\n  }\n}\n")},
        {S("a hexadecimal literal"),
         S(HEAD "  func @f() -> void {\n    local $x: i32 = 0x1f\n"
                "    block e:\n      unreachable\n  }\n}\n")},
        {S("a named argument"),
         S(HEAD "  func @g(%a: i32) -> i32 {\n    block e:\n      return %a\n"
                "  }\n"
                "  func @f(%a: i32) -> i32 {\n    block e:\n"
                "      %r: i32 = call @g, k = %a\n      return %r\n  }\n}\n")},
        {S("a module-level type used by a global"),
         S("module m {\n  profile bce\n  type T = i32\n  global @g: T\n}\n")},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        agree(cases[i].what, cases[i].src);
    }
}

static void test_the_grammar_defines_the_whole_surface(void)
{
    static const struct {
        core_string_t what;
        core_string_t start;
        core_string_t src;
    } cases[] = {
        {S("a parameter mode"), S("module"),
         S("module m {\n  profile bce\n  func @f(out %a: i32) -> void {\n"
           "    block e:\n      unreachable\n  }\n}\n")},
        {S("a member pinned to a bit range"), S("layout-item"),
         S("x @ bits 0 .. 32\n")},
        {S("every origin kind"), S("origin"),
         S("!origin(lowered, \"p\", 0, 1, 2, 3, 4, 5, 6)")},
        {S("an inout mode"), S("func-param"), S("inout %a: i32")},
        {S("a consume mode"), S("func-param"), S("consume %a: i32")},
        {S("an ownership transfer"), S("instr"),
         S("%a: i32 = consume %value, $source\n")},
        {S("a keyword parameter"), S("func-param"), S("k: i32")},
        {S("a rest parameter"), S("func-param"), S("*i32")},
        {S("a consume rest parameter"), S("func-param"), S("consume *%a: i32")},
        {S("a switch arm with a negative case"), S("switch-arm"), S("[-1: d]")},
        {S("a phi arm"), S("phi-arm"), S("[e: %a]")},
        {S("a default block"), S("default-block"), S("default: d")},
        {S("an annotation with provenance"), S("annot"),
         S("!ir.rule(\"sint\" from typed)")},
        {S("a boolean annotation"), S("annot"), S("!ir.flag(true)")},
        {S("a type-valued annotation"), S("annot"), S("!ir.ty(i32)")},
        {S("a value-valued annotation"), S("annot"), S("!ir.of(%x)")},
        {S("a dotted target"), S("dotted-name"), S("x86_64-unknown-linux-gnu")},
        {S("a target with a number"), S("dotted-name"), S("armv8.2-a")},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!grammar_accepts(cases[i].src, cases[i].start)) {
            printf("FAIL  the grammar does not derive %.*s: %.*s\n",
                   (int)cases[i].what.u8_bytes, cases[i].what.data,
                   (int)cases[i].src.u8_bytes, cases[i].src.data);
            fails++;
        }
    }
}

static void test_a_composed_grammar_keeps_its_own_leaves(void)
{
    static const struct {
        core_string_t what;
        core_string_t src;
        core_string_t want;
    } cases[] = {
        {S("a qualified type name keeps every part"),
         S(HEAD "  global @g: my.mod.Widget\n}\n"), S("my.mod.Widget")},
        {S("an array extent survives"),
         S(HEAD "  global @g: array[i32; 4]\n}\n"), S("array[i32; 4]")},
        {S("a bit width survives"),
         S(HEAD "  global @g: bitint[7]\n}\n"), S("bitint[7]")},
        {S("a dotted opcode keeps every part"),
         S("module m {\n  profile synth\n  func @f(%a: str) -> str {\n"
           "    block e:\n      %c: str = synth.concat %a, %a\n"
           "      return %c\n  }\n}\n"),
         S("synth.concat")},
    };

    const ir_profile_t *const ps[] = {&ir_profile_bce, &ir_profile_synth};

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ir_ctx_t     *ctx = ir_ctx_new();
        ir_diags_t   *d   = ir_diags_new(ctx, nullptr);
        ir_module_t  *m   = ir_module_parse(ctx, cases[i].src, 0, ps, 2, d);
        core_string_t out = m ? ir_module_print(m) : (core_string_t){};

        if (!out.data || core_string_find(out, cases[i].want) < 0) {
            core_string_t shown = out.data ? out : S("(refused)\n");

            printf("FAIL  %.*s: %.*s is missing from\n%.*s",
                   (int)cases[i].what.u8_bytes, cases[i].what.data,
                   (int)cases[i].want.u8_bytes, cases[i].want.data,
                   (int)shown.u8_bytes, shown.data);
            fails++;
        }
        core_free(out.data);
        ir_ctx_free(ctx);
    }
}

static void test_a_composed_keyword_is_still_a_name(void)
{
    static const core_string_t WORDS[] = {
        S("array"),  S("bitint"),    S("ubitint"),  S("exclusive"),
        S("forall"), S("gcref"),     S("indexable"), S("literal"),
        S("mutref"), S("nullable"),  S("own"),      S("owning"),
        S("ptr"),    S("ref"),       S("reseatable"), S("t"),
        S("temporary"), S("tmpref"), S("variant"),  S("in"),
        S("out"),    S("inout"),     S("consume"),  S("module"),
        S("profile"), S("target"),   S("block"),    S("local"),
        S("func"),   S("global"),    S("type"),     S("layout"),
        S("size"),   S("align"),     S("stride"),   S("bits"),
        S("loc"),    S("origin"),    S("meta"),     S("from"),
        S("default"), S("static"),   S("extern"),   S("true"),
        S("false"),  S("source"),    S("sizeof"),   S("alignof"),
        S("offsetof"),
    };

    for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) {
        core_string_t  w = WORDS[i];
        int            n = (int)w.u8_bytes;
        core_buffer_t *b = core_buffer_empty();

        core_buffer_printf(b,
                           "module %.*s {\n  profile bce\n  target %.*s\n"
                           "  func @f() -> void {\n    local $%.*s: i32\n"
                           "    block %.*s:\n      unreachable\n  }\n}\n",
                           n, w.data, n, w.data, n, w.data, n, w.data);

        core_string_t src = core_buffer_take_string(b);

        if (!grammar_accepts(src, S("module"))) {
            printf("FAIL  the grammar refuses %.*s as a name, though the "
                   "IR text format has no such keyword\n", n, w.data);
            fails++;
        }
        core_free(src.data);
    }
}

static void test_the_grammar_is_not_merely_permissive(void)
{
    static const core_string_t bad[] = {
        S("module m { profile bce"),
        S("module m profile bce }"),
        S("module m { profile bce global @g }"),
        S("module m { profile bce global @g: }"),
        S("module m { profile bce func @f() -> void { } }"),
        S("module m { profile bce func @f( -> void { block e: unreachable } }"),
        S("module m { profile bce func @f() -> void { block : unreachable } }"),
        S("module m { profile bce func @f() -> void { block e: %x = } }"),
        S("module m { profile bce type = ptr[i32] }"),
        S("module m { profile bce local $t: i32 }"),
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (grammar_accepts(bad[i], S("module"))) {
            printf("FAIL  the grammar derives what it should not: %.*s\n",
                   (int)bad[i].u8_bytes, bad[i].data);
            fails++;
        }
    }
}

int main(int argc, char **argv)
{
    core_string_t flag = argc > 1 ? core_string_from_cstr(argv[1])
                                  : (core_string_t){};

    if (argc > 2 && core_string_eq(flag, S("-t"))) {
        core_string_t         src   = core_string_from_cstr(argv[2]);
        core_string_t         start = argc > 3 ? core_string_from_cstr(argv[3])
                                               : core_string_dup(S("module"));
        parse_token_stream_t *ts    = ir_lex(src);
        parse_token_info_t   *tk;

        for (int i = 0; (tk = scan_stream_get(ts, i)); i++) {
            core_string_t text = core_option_get_or_else(
                parse_token_value(tk), (core_string_t){0});
            printf("%2d tid=%-11d [%zu,%zu) %.*s\n", i, parse_token_tid(tk),
                   parse_token_start_byte(tk), parse_token_end_byte(tk),
                   (int)text.u8_bytes, text.data ? text.data : "");
        }

        printf("grammar: %s\n",
               grammar_accepts(src, start) ? "accepts" : "rejects");
        core_free(src.data);
        core_free(start.data);
        core_free(flag.data);

        return 0;
    }

    core_string_t path = argc > 1 ? flag : S("golden/canonical.ir");

    test_the_canonical_module(path);
    core_free(flag.data);
    test_the_two_agree_construct_by_construct();
    test_the_grammar_defines_the_whole_surface();
    test_a_composed_keyword_is_still_a_name();
    test_a_composed_grammar_keeps_its_own_leaves();
    test_the_grammar_is_not_merely_permissive();
    ir_grammar_shutdown();

    if (!fails) {
        printf("ir grammar: the grammar and the parser agree\n");
    }

    return fails ? 1 : 0;
}
