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

static const struct {
    const char *grammar;
    const char *start;
    const char *sample;
    int32_t     indent;
} CASES[] = {
    {"lua.bnf",  "chunk",            "lua.lua",   2},
    {"go.bnf",   "source-file",      "go.go",     4},
    {"java.bnf", "compilation-unit", "java.java", 4},
    {"csharp.bnf", "compilation-unit", "csharp.cs", 4},
    {"python.bnf", "file",              "python.py", 4},
    {"ruby.bnf",   "program",           "ruby.rb",   2},
    {"swift.bnf",  "top-level-declaration", "swift.swift", 4},
    {"rust.bnf",   "crate",             "rust.rs",   4},
    {"javascript.bnf", "module",        "javascript.js", 4},
    {"typescript.bnf", "source-file",   "typescript.ts", 4},
    {"kotlin.bnf", "kotlin-file",       "kotlin.kt", 4},
    {"php.bnf",    "top-statement-list", "php.php",  4},
};

static char *
format_one(const char *grammar_path, const char *start, core_string_t src,
           int32_t indent)
{
    core_string_t gtext = slurp(grammar_path);

    if (!gtext.data) {
        return nullptr;
    }

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);
    parse_bnf_declare_terminals(g, gtext);

    if (!parse_bnf_load(gtext, core_string_borrow(start, strlen(start)), g)) {
        check("the grammar loads", false);
        parse_grammar_free(g);
        core_free(gtext.data);

        return nullptr;
    }

    core_buffer_t       *buf = nullptr;
    parse_scanner_t      *sc  = nullptr;
    parse_token_stream_t *ts  = scan_tokenize(g, src.data, src.u8_bytes, &buf,
                                           &sc);
    char               *out = nullptr;

    if (ts) {
        parse_pwz_parser_t *p = parse_pwz_new(g);

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
    core_free(gtext.data);

    return out;
}

static void
test_formatting_reproduces_the_source(const char *gdir, const char *fdir)
{
    char what[256];

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        char gpath[1024];
        char spath[1024];

        snprintf(gpath, sizeof(gpath), "%s/%s", gdir, CASES[i].grammar);
        snprintf(spath, sizeof(spath), "%s/%s", fdir, CASES[i].sample);

        core_string_t src = slurp(spath);

        if (!src.data) {
            continue;
        }

        char *out = format_one(gpath, CASES[i].start, src, CASES[i].indent);

        snprintf(what, sizeof(what), "%s formats %s", CASES[i].grammar,
                 CASES[i].sample);

        if (!out) {
            check(what, false);
            core_free(src.data);

            continue;
        }

        size_t n = strlen(out);

        while (n && out[n - 1] == '\n') {
            n--;
        }

        size_t m = src.u8_bytes;

        while (m && src.data[m - 1] == '\n') {
            m--;
        }

        bool same = n == m && !memcmp(out, src.data, n);

        if (!same) {
            printf("FAIL  %s\n---- want ----\n%.*s\n---- got ----\n%.*s\n",
                   what, (int)m, src.data, (int)n, out);
            fails++;
        }

        core_free(out);
        core_free(src.data);
    }
}

static void
test_formatting_is_idempotent(const char *gdir, const char *fdir)
{
    char what[256];

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        char gpath[1024];
        char spath[1024];

        snprintf(gpath, sizeof(gpath), "%s/%s", gdir, CASES[i].grammar);
        snprintf(spath, sizeof(spath), "%s/%s", fdir, CASES[i].sample);

        core_string_t src = slurp(spath);

        if (!src.data) {
            continue;
        }

        char *once = format_one(gpath, CASES[i].start, src, CASES[i].indent);

        if (!once) {
            core_free(src.data);

            continue;
        }

        core_string_t again = core_string_borrow(once, strlen(once));
        char        *twice = format_one(gpath, CASES[i].start, again,
                                        CASES[i].indent);

        snprintf(what, sizeof(what), "%s formatting is idempotent",
                 CASES[i].grammar);

        if (!twice) {
            check(what, false);
        }
        else {
            check(what, !strcmp(once, twice));
            core_free(twice);
        }

        core_free(once);
        core_free(src.data);
    }
}

int
main(int argc, char **argv)
{
    const char *gdir = argc > 1 ? argv[1] : "../../grammars";
    const char *fdir = argc > 2 ? argv[2] : "../format";

    test_formatting_reproduces_the_source(gdir, fdir);
    test_formatting_is_idempotent(gdir, fdir);

    return report("fe format");
}
