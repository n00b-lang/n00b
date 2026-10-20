#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parse/bnf.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
#include "parse/tree_print.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static const struct {
    core_string_t grammar;
    core_string_t start;
    core_string_t sample;
    int32_t       indent;
} CASES[] = {
    {S("lua.bnf"),  S("chunk"),            S("lua.lua"),   2},
    {S("go.bnf"),   S("source-file"),      S("go.go"),     4},
    {S("java.bnf"), S("compilation-unit"), S("java.java"), 4},
    {S("csharp.bnf"), S("compilation-unit"), S("csharp.cs"), 4},
    {S("python.bnf"), S("file"),              S("python.py"), 4},
    {S("ruby.bnf"),   S("program"),           S("ruby.rb"),   2},
    {S("swift.bnf"),  S("top-level-declaration"), S("swift.swift"), 4},
    {S("rust.bnf"),   S("crate"),             S("rust.rs"),   4},
    {S("javascript.bnf"), S("module"),        S("javascript.js"), 4},
    {S("typescript.bnf"), S("source-file"),   S("typescript.ts"), 4},
    {S("kotlin.bnf"), S("kotlin-file"),       S("kotlin.kt"), 4},
    {S("php.bnf"),    S("top-statement-list"), S("php.php"),  4},
};

static core_string_t
format_one(core_string_t grammar_path, core_string_t start, core_string_t src,
           int32_t indent)
{
    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    if (!parse_bnf_load_file(grammar_path, start, g)) {
        check("the grammar loads", false);
        parse_grammar_free(g);

        return (core_string_t){};
    }

    core_buffer_t        *buf = nullptr;
    parse_scanner_t      *sc  = nullptr;
    parse_token_stream_t *ts  = scan_tokenize(g, src, &buf, &sc);
    core_string_t         out = {};

    if (ts) {
        parse_pwz_parser_t *p = parse_pwz_new_for_start(
            g, parse_nonterm_id(parse_find_nonterm(g, start)));

        if (parse_pwz_parse(p, ts)) {
            parse_print_opts_t o = {
                .source = src,
                .width  = 80,
                .indent = indent,
            };

            out = parse_tree_print(g, parse_pwz_get_tree(p), &o);
        }

        parse_pwz_free(p);
        scan_token_stream_free(ts);
        scan_scanner_free(sc);
        core_buffer_free(buf);
    }

    parse_grammar_free(g);

    return out;
}

static core_string_t
join_path(char *out, size_t cap, core_string_t dir, core_string_t leaf)
{
    int n = snprintf(out, cap, "%.*s/%.*s", (int)dir.u8_bytes, dir.data,
                     (int)leaf.u8_bytes, leaf.data);

    return core_string_borrow(out, n > 0 ? (size_t)n : 0);
}

static void
test_formatting_reproduces_the_source(core_string_t gdir, core_string_t fdir)
{
    char what[256];

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        char gbuf[1024];
        char sbuf[1024];

        core_string_t gpath = join_path(gbuf, sizeof(gbuf), gdir,
                                        CASES[i].grammar);

        join_path(sbuf, sizeof(sbuf), fdir, CASES[i].sample);

        core_string_t src = slurp(sbuf);

        if (!src.data) {
            continue;
        }

        core_string_t out = format_one(gpath, CASES[i].start, src,
                                       CASES[i].indent);

        snprintf(what, sizeof(what), "%.*s formats %.*s",
                 (int)CASES[i].grammar.u8_bytes, CASES[i].grammar.data,
                 (int)CASES[i].sample.u8_bytes, CASES[i].sample.data);

        if (!out.data) {
            check(what, false);
            core_free(src.data);

            continue;
        }

        size_t n = out.u8_bytes;

        while (n && out.data[n - 1] == '\n') {
            n--;
        }

        size_t m = src.u8_bytes;

        while (m && src.data[m - 1] == '\n') {
            m--;
        }

        bool same = n == m && !memcmp(out.data, src.data, n);

        if (!same) {
            printf("FAIL  %.*s formats %.*s\n---- want ----\n%.*s\n"
                   "---- got ----\n%.*s\n",
                   (int)CASES[i].grammar.u8_bytes, CASES[i].grammar.data,
                   (int)CASES[i].sample.u8_bytes, CASES[i].sample.data,
                   (int)m, src.data, (int)n, out.data);
            fails++;
        }

        core_free(out.data);
        core_free(src.data);
    }
}

static void
test_formatting_is_idempotent(core_string_t gdir, core_string_t fdir)
{
    char what[256];

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        char gbuf[1024];
        char sbuf[1024];

        core_string_t gpath = join_path(gbuf, sizeof(gbuf), gdir,
                                        CASES[i].grammar);

        join_path(sbuf, sizeof(sbuf), fdir, CASES[i].sample);

        core_string_t src = slurp(sbuf);

        if (!src.data) {
            continue;
        }

        core_string_t once = format_one(gpath, CASES[i].start, src,
                                        CASES[i].indent);

        if (!once.data) {
            core_free(src.data);

            continue;
        }

        core_string_t twice = format_one(gpath, CASES[i].start, once,
                                         CASES[i].indent);

        snprintf(what, sizeof(what), "%.*s formatting is idempotent",
                 (int)CASES[i].grammar.u8_bytes, CASES[i].grammar.data);

        if (!twice.data) {
            check(what, false);
        }
        else {
            check(what, core_string_eq(once, twice));
            core_free(twice.data);
        }

        core_free(once.data);
        core_free(src.data);
    }
}

int
main(int argc, char **argv)
{
    core_string_t gdir = argc > 1 ? core_string_from_cstr(argv[1])
                                  : S("../../grammars");
    core_string_t fdir = argc > 2 ? core_string_from_cstr(argv[2])
                                  : S("../format");

    test_formatting_reproduces_the_source(gdir, fdir);
    test_formatting_is_idempotent(gdir, fdir);

    return report("fe format");
}
