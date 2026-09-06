#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/grammar.h"
#include "ir/ir.h"
#include "ir/text.h"

#include "parse/grammar.h"
#include "parse/pwz.h"
#include "scanner/token_stream.h"

static int fails;

static bool grammar_accepts(const char *src, const char *start)
{
    ncc_grammar_t      *g  = ir_grammar();
    ncc_token_stream_t *ts = ir_lex(src, strlen(src));

    if (!g || !ts) {
        return false;
    }

    ncc_grammar_set_start_id(g, ir_grammar_nt(start));

    ncc_pwz_parser_t *p  = ncc_pwz_new(g);
    bool              ok = ncc_pwz_parse(p, ts);

    ncc_pwz_free(p);
    ncc_token_stream_free(ts);

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

static char *slurp(const char *path)
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
    char *src = slurp(path);

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
    static const struct {
        const char *what;
        const char *src;
    } cases[] = {
        {"an empty module", "module m { profile bce }"},
        {"a profile", "module m { profile bce }"},
        {"a target with dashes", "module m { profile bce target x86_64-linux }"},
        {"a global", "module m { profile bce global @g: i32 }"},
        {"a global with linkage", "module m { profile bce static global @g: i32 }"},
        {"a record-typed global", "module m { profile bce global @g: (x: int, y: i64) }"},
        {"a type declaration", "module m { profile bce type T = ptr[i32] }"},
        {"an empty function",
         "module m { profile bce func @f() -> void { block e: unreachable } }"},
        {"parameters",
         "module m { profile bce func @f(%a: i32, %b: i32) -> i32 { block e: return %a } }"},
        {"a local", "module m { profile bce func @f() -> void {\n"
                    "  local $t: ptr[i32]\n  block e: unreachable } }"},
        {"a local with an initialiser",
         "module m { profile bce func @f() -> void {\n  local $n: i32 = 7\n"
         "  block e: unreachable } }"},
        {"a negative initialiser",
         "module m { profile bce func @f() -> void {\n  local $n: i32 = -7\n"
         "  block e: unreachable } }"},
        {"an instruction with a modifier",
         "module m { profile bce func @f(%a: i32) -> i32 { block e:\n"
         "  %s: i32 = add wrap %a, %a\n  return %s } }"},
        {"a phi",
         "module m { profile bce func @f(%a: i32) -> i32 { block e:\n  jump l\n"
         " block l:\n  %p: i32 = phi [e: %a]\n  return %p } }"},
        {"a switch",
         "module m { profile bce func @f(%a: i32) -> i32 { block e:\n"
         "  switch %a, default: d, [0: d], [1: d]\n block d: return %a } }"},
        {"a location",
         "module m { profile bce func @f() -> void { block e:\n"
         "  unreachable !loc(0, 1, 2, 1, 3, 1, 4) } }"},
        {"an annotation with provenance",
         "module m { profile bce func @f(%a: i32) -> i32 { block e:\n"
         "  %s: i32 = add %a, %a !ir.rule(\"sint\" from typed)\n"
         "  return %s } }"},
        {"a module annotation", "module m { profile bce meta !ir.note(\"hand written\") }"},
        {"a sizeof operand",
         "module m { profile bce func @f() -> u64 { block e:\n"
         "  %z: u64 = sizeof (i32, i32) -> i32\n  return %z } }"},
        {"a comment", "module m {\n  # a comment\n  profile bce \n}"},
        {"a name that is also a keyword",
         "module m { profile bce func @f() -> void {\n  local $size: i32\n"
         "  block e: unreachable } }"},
        {"a rest parameter",
         "module m { profile bce func @f(%a: i32, *i32) -> void {\n"
         "  block e: unreachable } }"},
        {"a keyword parameter",
         "module m { profile bce func @f(%a: i32, k: i32) -> void {\n"
         "  block e: unreachable } }"},
        {"extern linkage",
         "module m { profile bce extern func @f() -> void {\n"
         "  block e: unreachable } }"},
        {"an extern global", "module m { profile bce extern global @g: i32 }"},
        {"a layout declaration",
         "module m { profile bce layout i32 {\n  size = 4\n  align = 4\n"
         "  stride = 4\n} }"},
        {"a layout member pinned to bits",
         "module m { profile bce layout (x: i32, y: i32) {\n"
         "  x @ bits 0 .. 32\n} }"},
        {"a symbolic layout value",
         "module m { profile bce layout ptr[i32] {\n"
         "  size = sizeof(i32)\n  align = alignof(i32)\n} }"},
        {"an offsetof layout value",
         "module m { profile bce layout (x: i32) {\n"
         "  size = offsetof((x: i32), \"x\")\n} }"},
        {"an origin",
         "module m { profile bce func @f() -> void { block e:\n"
         "  unreachable !origin(source, \"\", 0, 1, 2, 3, 4, 5, 6) } }"},
        {"a float literal",
         "module m { profile bce func @f() -> void {\n  local $x: f64 = 1.5\n"
         "  block e: unreachable } }"},
        {"a hexadecimal literal",
         "module m { profile bce func @f() -> void {\n  local $x: i32 = 0x1f\n"
         "  block e: unreachable } }"},
        {"a named argument",
         "module m { profile bce func @g(%a: i32) -> i32 { block e:\n"
         "  return %a }\n"
         " func @f(%a: i32) -> i32 { block e:\n"
         "  %r: i32 = call @g, k = %a\n  return %r } }"},
        {"a module-level type used by a global",
         "module m { profile bce type T = i32 global @g: T }"},
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
         "module m { profile bce func @f(out %a: i32) -> void {\n"
         "  block e: unreachable } }"},
        {"a member pinned to a bit range", "layout-item", "x @ bits 0 .. 32"},
        {"every origin kind", "origin",
         "!origin(lowered, \"p\", 0, 1, 2, 3, 4, 5, 6)"},
        {"an inout mode", "func-param", "inout %a: i32"},
        {"a keyword parameter", "func-param", "k: i32"},
        {"a rest parameter", "func-param", "*i32"},
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

static void test_a_composed_keyword_is_still_a_name(void)
{
    static const char *const WORDS[] = {
        "array",  "bitint",     "ubitint",   "exclusive", "forall",
        "gcref",  "indexable",  "literal",   "mutref",    "nullable",
        "own",    "owning",     "ptr",       "ref",       "reseatable",
        "t",      "temporary",  "tmpref",    "variant",   "in",
        "out",    "inout",      "module",    "profile",   "target",
        "block",  "local",      "func",      "global",    "type",
        "layout", "size",       "align",     "stride",    "bits",
        "loc",    "origin",     "meta",      "from",      "default",
        "static", "extern",     "true",      "false",     "source",
        "sizeof", "alignof",    "offsetof",
    };

    for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) {
        char src[512];

        snprintf(src, sizeof src,
                 "module %s { profile bce target %s\n"
                 "  func @f() -> void {\n    local $%s: i32\n"
                 "    block %s: unreachable\n  }\n}\n",
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
        ncc_token_stream_t *ts = ir_lex(argv[2], strlen(argv[2]));
        ncc_token_info_t   *tk;

        for (int i = 0; (tk = ncc_stream_get(ts, i)); i++) {
            printf("%2d tid=%-11d [%zu,%zu) %s\n", i, tk->tid,
                   tk->start_byte, tk->end_byte,
                   ncc_option_is_set(tk->value)
                     ? ncc_option_get(tk->value).data : "");
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
    test_the_grammar_is_not_merely_permissive();
    ir_grammar_shutdown();

    if (!fails) {
        printf("ir grammar: the grammar and the parser agree\n");
    }

    return fails ? 1 : 0;
}
