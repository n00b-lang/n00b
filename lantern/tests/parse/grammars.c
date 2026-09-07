#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parse/bnf.h"
#include "parse/grammar.h"
#include "scanner/lex.h"
#include "scanner/tokenizer.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include "harness.h"

static const struct {
    const char *file;
    const char *start;
    const char *sample;
} CORPUS[] = {
    {"cel.bnf",        "expr",                  "s.cel"  },
    {"csharp.bnf",     "compilation-unit",       "s.cs"   },
    {"go.bnf",         "source-file",            "s.go"   },
    {"java.bnf",       "compilation-unit",       "s.java" },
    {"javascript.bnf", "script",                 "s.js"   },
    {"json.bnf",       "value",                  nullptr  },
    {"kotlin.bnf",     "kotlin-file",            "s.kt"   },
    {"lua.bnf",        "chunk",                  "s.lua"  },
    {"php.bnf",        "top-statement-list",     "s.php"  },
    {"python.bnf",     "file",                   "s.py"   },
    {"ruby.bnf",       "program",                "s.rb"   },
    {"rust.bnf",       "crate",                  "s.rs"   },
    {"swift.bnf",      "top-level-declaration",  "s.swift"},
    {"typescript.bnf", "source-file",            "s.ts"   },
    {"c_ncc.bnf",      "translation_unit",       "s.c"    },
    {"bce.bnf",        "translation_unit",       "s.bce"  },
};

static void test_every_grammar_in_the_corpus_loads(const char *dir)
{
    char what[256];

    for (size_t i = 0; i < sizeof(CORPUS) / sizeof(CORPUS[0]); i++) {
        char path[1024];

        snprintf(path, sizeof(path), "%s/%s", dir, CORPUS[i].file);

        core_string_t text = slurp(path);

        if (!text.data) {
            continue;
        }

        parse_grammar_t *g = parse_grammar_new();

        parse_grammar_set_error_recovery(g, false);
        parse_bnf_declare_terminals(g, text);

        bool ok = parse_bnf_load(text, core_string_borrow(CORPUS[i].start,
                                                       strlen(CORPUS[i].start)),
                               g);

        snprintf(what, sizeof(what), "%s loads", CORPUS[i].file);
        check(what, ok);

        snprintf(what, sizeof(what), "%s defines every non-terminal it uses",
                 CORPUS[i].file);
        check(what, !ok || parse_grammar_report_undefined(g, stdout));

        snprintf(what, sizeof(what), "%s has rules", CORPUS[i].file);
        check(what, !ok || g->rules.len > 0);

        parse_grammar_free(g);
        core_free(text.data);
    }
}

static void test_every_grammar_tokenizes_its_own_language(const char *dir,
                                                          const char *samples)
{
    char what[256];

    for (size_t i = 0; i < sizeof(CORPUS) / sizeof(CORPUS[0]); i++) {
        if (!CORPUS[i].sample) {
            continue;
        }

        char path[1024];

        snprintf(path, sizeof(path), "%s/%s", dir, CORPUS[i].file);

        core_string_t text = slurp(path);

        if (!text.data) {
            continue;
        }

        parse_grammar_t *g = parse_grammar_new();

        parse_grammar_set_error_recovery(g, false);
        parse_bnf_declare_terminals(g, text);
        parse_bnf_load(text,
                     core_string_borrow(CORPUS[i].start,
                                       strlen(CORPUS[i].start)),
                     g);

        snprintf(what, sizeof(what), "%s declares a lexis", CORPUS[i].file);
        check(what, scan_grammar_lex(g) != nullptr);

        snprintf(path, sizeof(path), "%s/%s", samples, CORPUS[i].sample);

        core_string_t src = slurp(path);

        if (!src.data) {
            parse_grammar_free(g);
            core_free(text.data);

            continue;
        }

        core_buffer_t       *buf = nullptr;
        parse_scanner_t      *sc  = nullptr;
        parse_token_stream_t *ts  = scan_tokenize(g, src.data, src.u8_bytes,
                                               &buf, &sc);

        snprintf(what, sizeof(what), "%s: the lexis builds with no complaint",
                 CORPUS[i].file);
        check(what, scan_lex_build_error() == nullptr);

        snprintf(what, sizeof(what), "%s: %s tokenizes", CORPUS[i].file,
                 CORPUS[i].sample);
        check(what, ts != nullptr);

        if (!ts) {
            parse_grammar_free(g);
            core_free(text.data);
            core_free(src.data);

            continue;
        }

        size_t n         = 0;
        size_t unmapped  = 0;

        scan_stream_foreach(ts, tok) {
            n++;

            if (tok->tid > 0 && tok->tid < 128) {
                continue;
            }

            bool known = false;

            for (size_t k = 0; k < core_list_len(g->named_terms); k++) {
                if (g->named_terms.data[k].id == tok->tid) {
                    known = true;

                    break;
                }
            }

            if (!known) {
                unmapped++;
            }
        }

        snprintf(what, sizeof(what), "%s: %s produces tokens",
                 CORPUS[i].file, CORPUS[i].sample);
        check(what, n > 0);

        snprintf(what, sizeof(what),
                 "%s: every token of %s is a terminal the grammar declares",
                 CORPUS[i].file, CORPUS[i].sample);

        if (unmapped) {
            printf("      %zu of %zu tokens unmapped\n", unmapped, n);
        }

        check(what, unmapped == 0);

        scan_token_stream_free(ts);
        scan_scanner_free(sc);
        core_buffer_free(buf);
        parse_grammar_free(g);
        core_free(text.data);
        core_free(src.data);
    }
}

static void test_declaring_terminals_is_what_makes_them_loadable(const char *dir)
{
    char path[1024];

    snprintf(path, sizeof(path), "%s/%s", dir, "python.bnf");

    core_string_t text = slurp(path);

    if (!text.data) {
        return;
    }

    parse_grammar_t *bare = parse_grammar_new();

    parse_grammar_set_error_recovery(bare, false);

    check("a grammar naming its own tokens does not load undeclared",
          !parse_bnf_load(text, CORE_STRING_STATIC("python-file"), bare));
    parse_grammar_free(bare);

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    check("declaring its terminals reports how many it had to add",
          parse_bnf_declare_terminals(g, text) > 0);
    check("and then it loads",
          parse_bnf_load(text, CORE_STRING_STATIC("python-file"), g));
    check("declaring twice adds nothing the second time",
          parse_bnf_declare_terminals(g, text) == 0);

    parse_grammar_free(g);
    core_free(text.data);
}

static void test_a_borrowed_name_registers_like_any_other(void)
{
    static const char *const run = "FOOBAR_AND_MORE";

    parse_grammar_t *g = parse_grammar_new();
    int64_t        a = parse_register_terminal(g, core_string_borrow(run, 6));

    check("a name borrowed from a longer buffer registers", a != 0);
    check("and is found again by content",
          parse_lookup_terminal(g, CORE_STRING_STATIC("FOOBAR")) == a);
    check("without matching the longer buffer it came from",
          parse_lookup_terminal(g, core_string_borrow(run, 15)) != a);

    parse_grammar_free(g);
}

static void test_a_grammar_that_can_never_match_says_so(void)
{
    static const char TYPO[] = "<top> ::= <a> <nonexistent-thing>\n"
                               "<a> ::= %\"x\"\n";

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);
    parse_bnf_load(CORE_STRING_STATIC(TYPO), CORE_STRING_STATIC("top"), g);

    core_string_t names[4];

    check("a typo'd nonterminal reference is reported",
          parse_grammar_undefined(g, names, 4) == 1);
    check("and it is named",
          core_string_eq(names[0], CORE_STRING_STATIC("nonexistent-thing")));
    parse_grammar_free(g);

    parse_grammar_t *h = parse_grammar_new();

    parse_grammar_set_error_recovery(h, false);
    parse_bnf_load(CORE_STRING_STATIC("<a> ::= %\"x\"\n"),
                 CORE_STRING_STATIC("no-such-start"),
                 h);

    check("a start symbol the grammar never defines is reported",
          parse_grammar_undefined(h, names, 4) == 1);
    check("and it is named",
          core_string_eq(names[0], CORE_STRING_STATIC("no-such-start")));
    parse_grammar_free(h);

    parse_grammar_t *ok = parse_grammar_new();

    parse_grammar_set_error_recovery(ok, false);
    parse_bnf_load(CORE_STRING_STATIC("<a> ::= %\"x\" <b>\n<b> ::= %\"y\"\n"),
                 CORE_STRING_STATIC("a"),
                 ok);

    check("a complete grammar reports nothing undefined",
          parse_grammar_undefined(ok, nullptr, 0) == 0);
    parse_grammar_free(ok);
}

int main(int argc, char **argv)
{
    const char *dir     = argc > 1 ? argv[1] : "../../grammars";
    const char *samples = argc > 2 ? argv[2] : "../samples";

    test_every_grammar_in_the_corpus_loads(dir);
    test_every_grammar_tokenizes_its_own_language(dir, samples);
    test_declaring_terminals_is_what_makes_them_loadable(dir);
    test_a_borrowed_name_registers_like_any_other();
    test_a_grammar_that_can_never_match_says_so();

    return report("fe grammars");
}
