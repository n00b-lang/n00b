#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/grammar.h"
#include "ir/ir.h"
#include "ir/text.h"

#include "parse/grammar.h"
#include "parse/pwz.h"
#include "scanner/token_stream.h"
#include "harness.h"

static bool grammar_accepts(const char *src, const char *start)
{
    parse_grammar_t      *g  = ir_grammar();
    parse_token_stream_t *ts = ir_lex(src, strlen(src));

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

static bool parser_accepts(const char *src)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
    const ir_profile_t *const ps[] = {&ir_profile_bce, &ir_profile_synth};
    ir_module_t *m   = ir_module_parse(ctx, src, 0, ps, 2, d);
    bool         ok  = m != nullptr && !ir_diags_failed(d);

    ir_ctx_free(ctx);

    return ok;
}

static void agree(const char *what, const char *src)
{
    bool byparser  = parser_accepts(src);
    bool bygrammar = grammar_accepts(src, "module");

    if (!byparser) {
        printf("FAIL  %s: the parser does not accept it, so the case is "
               "vacuous\n",
               what);
        fails++;

        return;
    }

    if (!bygrammar) {
        printf("FAIL  %s: the parser accepts it, the grammar does not\n",
               what);
        fails++;
    }
}

static char *try_read(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f) {
        return nullptr;
    }

    fseek(f, 0, SEEK_END);

    long n = ftell(f);

    rewind(f);

    char  *p = malloc((size_t)n + 1);
    size_t r = fread(p, 1, (size_t)n, f);

    p[r] = 0;
    fclose(f);

    return p;
}

static void test_the_canonical_module(const char *path)
{
    char *src = try_read(path);

    if (!src) {
        printf("FAIL  cannot open %s\n", path);
        fails++;

        return;
    }

    if (!parser_accepts(src)) {
        printf("FAIL  the parser no longer reads %s\n", path);
        fails++;
    }
    else if (!grammar_accepts(src, "module")) {
        printf("FAIL  the grammar does not derive %s\n", path);
        fails++;
    }

    free(src);
}

static void test_the_two_agree_construct_by_construct(void)
{
#define HEAD "module m {\n  profile bce\n"

    static const struct {
        const char *what;
        const char *src;
    } cases[] = {
        {"an empty module", "module m {\n  profile bce\n}\n"},
        {"a profile", "module m {\n  profile bce\n}\n"},
        {"a target with dashes", "module m {\n  profile bce\n  target x86_64-linux\n}\n"},
        {"a global", "module m {\n  profile bce\n  global @g: i32\n}\n"},
        {"a global with linkage", "module m {\n  profile bce\n  static global @g: i32\n}\n"},
        {"a record-typed global", "module m {\n  profile bce\n  global @g: (x: int, y: i64)\n}\n"},
        {"a type declaration", "module m {\n  profile bce\n  type T = ptr[i32]\n}\n"},
        {"an empty function",
         "module m {\n  profile bce\n  func @f() -> void {\n    block e:\n"
         "      unreachable\n  }\n}\n"},
        {"parameters",
         "module m {\n  profile bce\n"
         "  func @f(%a: i32, %b: i32) -> i32 {\n    block e:\n"
         "      return %a\n  }\n}\n"},
        {"a local", HEAD "  func @f() -> void {\n    local $t: ptr[i32]\n"
                    "    block e:\n      unreachable\n  }\n}\n"},
        {"a local with an initialiser",
         HEAD "  func @f() -> void {\n    local $n: i32 = 7\n"
         "    block e:\n      unreachable\n  }\n}\n"},
        {"a negative initialiser",
         HEAD "  func @f() -> void {\n    local $n: i32 = -7\n"
         "    block e:\n      unreachable\n  }\n}\n"},
        {"an instruction with a modifier",
         HEAD "  func @f(%a: i32) -> i32 {\n    block e:\n"
         "      %s: i32 = add wrap %a, %a\n      return %s\n  }\n}\n"},
        {"a phi",
         HEAD "  func @f(%a: i32) -> i32 {\n    block e:\n      jump l\n"
         "    block l:\n      %p: i32 = phi [e: %a]\n      return %p\n"
         "  }\n}\n"},
        {"a switch",
         HEAD "  func @f(%a: i32) -> i32 {\n    block e:\n"
         "      switch %a, default: d, [0: d], [1: d]\n"
         "    block d:\n      return %a\n  }\n}\n"},
        {"a location",
         HEAD "  func @f() -> void {\n    block e:\n"
         "      unreachable !loc(0, 1, 2, 1, 3, 1, 4)\n  }\n}\n"},
        {"an annotation with provenance",
         HEAD "  func @f(%a: i32) -> i32 {\n    block e:\n"
         "      %s: i32 = add %a, %a !ir.rule(\"sint\" from typed)\n"
         "      return %s\n  }\n}\n"},
        {"a module annotation",
         HEAD "  meta !ir.note(\"hand written\")\n}\n"},
        {"a sizeof operand",
         HEAD "  func @f() -> u64 {\n    block e:\n"
         "      %z: u64 = sizeof (i32, i32) -> i32\n      return %z\n"
         "  }\n}\n"},
        {"a comment", "module m {\n  # a comment\n  profile bce\n}\n"},
        {"a name that is also a keyword",
         HEAD "  func @f() -> void {\n    local $size: i32\n"
         "    block e:\n      unreachable\n  }\n}\n"},
        {"a rest parameter",
         HEAD "  func @f(%a: i32, *i32) -> void {\n"
         "    block e:\n      unreachable\n  }\n}\n"},
        {"a keyword parameter",
         HEAD "  func @f(%a: i32, k: i32) -> void {\n"
         "    block e:\n      unreachable\n  }\n}\n"},
        {"extern linkage",
         HEAD "  extern func @f() -> void {\n"
         "    block e:\n      unreachable\n  }\n}\n"},
        {"an extern global", HEAD "  extern global @g: i32\n}\n"},
        {"a layout declaration",
         HEAD "  layout i32 {\n    size = 4\n    align = 4\n"
         "    stride = 4\n  }\n}\n"},
        {"a layout member pinned to bits",
         HEAD "  layout (x: i32, y: i32) {\n    x @ bits 0 .. 32\n  }\n}\n"},
        {"a symbolic layout value",
         HEAD "  layout ptr[i32] {\n"
         "    size = sizeof(i32)\n    align = alignof(i32)\n  }\n}\n"},
        {"an offsetof layout value",
         HEAD "  layout (x: i32) {\n"
         "    size = offsetof((x: i32), \"x\")\n  }\n}\n"},
        {"an origin",
         HEAD "  func @f() -> void {\n    block e:\n"
         "      unreachable !origin(source, \"\", 0, 1, 2, 3, 4, 5, 6)\n"
         "  }\n}\n"},
        {"a float literal",
         HEAD "  func @f() -> void {\n    local $x: f64 = 1.5\n"
         "    block e:\n      unreachable\n  }\n}\n"},
        {"a hexadecimal literal",
         HEAD "  func @f() -> void {\n    local $x: i32 = 0x1f\n"
         "    block e:\n      unreachable\n  }\n}\n"},
        {"a named argument",
         HEAD "  func @g(%a: i32) -> i32 {\n    block e:\n      return %a\n"
         "  }\n"
         "  func @f(%a: i32) -> i32 {\n    block e:\n"
         "      %r: i32 = call @g, k = %a\n      return %r\n  }\n}\n"},
        {"a module-level type used by a global",
         "module m {\n  profile bce\n  type T = i32\n  global @g: T\n}\n"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        agree(cases[i].what, cases[i].src);
    }
}

static void test_the_grammar_defines_the_whole_surface(void)
{
    static const struct {
        const char *what;
        const char *start;
        const char *src;
    } cases[] = {
        {"a parameter mode", "module",
         "module m {\n  profile bce\n  func @f(out %a: i32) -> void {\n"
         "    block e:\n      unreachable\n  }\n}\n"},
        {"a member pinned to a bit range", "layout-item",
         "x @ bits 0 .. 32\n"},
        {"every origin kind", "origin",
         "!origin(lowered, \"p\", 0, 1, 2, 3, 4, 5, 6)"},
        {"an inout mode", "func-param", "inout %a: i32"},
        {"a consume mode", "func-param", "consume %a: i32"},
        {"an ownership transfer", "instr", "%a: i32 = consume %value, $source\n"},
        {"a keyword parameter", "func-param", "k: i32"},
        {"a rest parameter", "func-param", "*i32"},
        {"a consume rest parameter", "func-param", "consume *%a: i32"},
        {"a switch arm with a negative case", "switch-arm", "[-1: d]"},
        {"a phi arm", "phi-arm", "[e: %a]"},
        {"a default block", "default-block", "default: d"},
        {"an annotation with provenance", "annot",
         "!ir.rule(\"sint\" from typed)"},
        {"a boolean annotation", "annot", "!ir.flag(true)"},
        {"a type-valued annotation", "annot", "!ir.ty(i32)"},
        {"a value-valued annotation", "annot", "!ir.of(%x)"},
        {"a dotted target", "dotted-name", "x86_64-unknown-linux-gnu"},
        {"a target with a number", "dotted-name", "armv8.2-a"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!grammar_accepts(cases[i].src, cases[i].start)) {
            printf("FAIL  the grammar does not derive %s: %s\n",
                   cases[i].what, cases[i].src);
            fails++;
        }
    }
}

static void test_a_composed_grammar_keeps_its_own_leaves(void)
{
    static const struct {
        const char *what;
        const char *src;
        const char *want;
    } cases[] = {
        {"a qualified type name keeps every part",
         HEAD "  global @g: my.mod.Widget\n}\n", "my.mod.Widget"},
        {"an array extent survives",
         HEAD "  global @g: array[i32; 4]\n}\n", "array[i32; 4]"},
        {"a bit width survives",
         HEAD "  global @g: bitint[7]\n}\n", "bitint[7]"},
        {"a dotted opcode keeps every part",
         "module m {\n  profile synth\n  func @f(%a: str) -> str {\n"
         "    block e:\n      %c: str = synth.concat %a, %a\n"
         "      return %c\n  }\n}\n",
         "synth.concat"},
    };

    const ir_profile_t *const ps[] = {&ir_profile_bce, &ir_profile_synth};

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ir_ctx_t    *ctx = ir_ctx_new();
        ir_diags_t  *d   = ir_diags_new(ctx, nullptr);
        ir_module_t *m   = ir_module_parse(ctx, cases[i].src, 0, ps, 2, d);
        char        *out = m ? ir_module_print(m) : nullptr;

        if (!out || !strstr(out, cases[i].want)) {
            printf("FAIL  %s: %s is missing from\n%s", cases[i].what,
                   cases[i].want, out ? out : "(refused)\n");
            fails++;
        }
        free(out);
        ir_ctx_free(ctx);
    }
}

static void test_a_composed_keyword_is_still_a_name(void)
{
    static const char *const WORDS[] = {
        "array",  "bitint",     "ubitint",   "exclusive", "forall",
        "gcref",  "indexable",  "literal",   "mutref",    "nullable",
        "own",    "owning",     "ptr",       "ref",       "reseatable",
        "t",      "temporary",  "tmpref",    "variant",   "in",
        "out",    "inout",      "consume",   "module",    "profile",   "target",
        "block",  "local",      "func",      "global",    "type",
        "layout", "size",       "align",     "stride",    "bits",
        "loc",    "origin",     "meta",      "from",      "default",
        "static", "extern",     "true",      "false",     "source",
        "sizeof", "alignof",    "offsetof",
    };

    for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) {
        char src[512];

        snprintf(src, sizeof src,
                 "module %s {\n  profile bce\n  target %s\n"
                 "  func @f() -> void {\n    local $%s: i32\n"
                 "    block %s:\n      unreachable\n  }\n}\n",
                 WORDS[i], WORDS[i], WORDS[i], WORDS[i]);

        if (!grammar_accepts(src, "module")) {
            printf("FAIL  the grammar refuses %s as a name, though the "
                   "IR text format has no such keyword\n", WORDS[i]);
            fails++;
        }
    }
}

static void test_the_grammar_is_not_merely_permissive(void)
{
    static const char *const bad[] = {
        "module m { profile bce",
        "module m profile bce }",
        "module m { profile bce global @g }",
        "module m { profile bce global @g: }",
        "module m { profile bce func @f() -> void { } }",
        "module m { profile bce func @f( -> void { block e: unreachable } }",
        "module m { profile bce func @f() -> void { block : unreachable } }",
        "module m { profile bce func @f() -> void { block e: %x = } }",
        "module m { profile bce type = ptr[i32] }",
        "module m { profile bce local $t: i32 }",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (grammar_accepts(bad[i], "module")) {
            printf("FAIL  the grammar derives what it should not: %s\n",
                   bad[i]);
            fails++;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "-t")) {
        parse_token_stream_t *ts = ir_lex(argv[2], strlen(argv[2]));
        parse_token_info_t   *tk;

        for (int i = 0; (tk = scan_stream_get(ts, i)); i++) {
            core_string_t text = core_option_get_or_else(
                parse_token_value(tk), (core_string_t){0});
            printf("%2d tid=%-11d [%zu,%zu) %.*s\n", i, parse_token_tid(tk),
                   parse_token_start_byte(tk), parse_token_end_byte(tk),
                   (int)text.u8_bytes, text.data ? text.data : "");
        }

        printf("grammar: %s\n",
               grammar_accepts(argv[2], argc > 3 ? argv[3] : "module")
                 ? "accepts" : "rejects");

        return 0;
    }

    test_the_canonical_module(argc > 1 ? argv[1] : "golden/canonical.ir");
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
