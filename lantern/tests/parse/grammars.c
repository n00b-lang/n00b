#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parse/bnf.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
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
    {"json.bnf",       "json",                   "s.json"},
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

static const struct {
    const char *grammar;
    const char *lexer;
} EXTERNAL_LEXERS[] = {
    {"bce.bnf",        "bce.lex"       },
    {"c_ncc.bnf",      "c_ncc.lex"     },
    {"cel.bnf",        "cel.lex"       },
    {"csharp.bnf",     "csharp.lex"    },
    {"go.bnf",         "go.lex"        },
    {"java.bnf",       "java.lex"      },
    {"kotlin.bnf",     "kotlin.lex"    },
    {"python.bnf",     "python.lex"    },
    {"lua.bnf",        "lua.lex"       },
    {"javascript.bnf", "javascript.lex"},
    {"php.bnf",        "php.lex"       },
    {"ruby.bnf",       "ruby.lex"      },
    {"rust.bnf",       "rust.lex"      },
    {"swift.bnf",      "swift.lex"     },
    {"typescript.bnf", "typescript.lex"},
    {"json.bnf",       "json.lex"      },
};

static void test_external_lexer_files_stay_separate(const char *dir)
{
    char what[256];

    for (size_t i = 0; i < sizeof(EXTERNAL_LEXERS) / sizeof(EXTERNAL_LEXERS[0]);
         i++) {
        char grammar_path[1024];
        char lexer_path[1024];

        snprintf(grammar_path, sizeof(grammar_path), "%s/%s", dir,
                 EXTERNAL_LEXERS[i].grammar);
        snprintf(lexer_path, sizeof(lexer_path), "%s/%s", dir,
                 EXTERNAL_LEXERS[i].lexer);
        core_string_t grammar = slurp(grammar_path);
        core_string_t lexer = slurp(lexer_path);

        snprintf(what, sizeof(what), "%s imports an external lexer",
                 EXTERNAL_LEXERS[i].grammar);
        check(what, grammar.data && strstr(grammar.data, "@lexer(")
                        && !strstr(grammar.data, "@lex-"));

        snprintf(what, sizeof(what), "%s contains lexical declarations only",
                 EXTERNAL_LEXERS[i].lexer);
        check(what, lexer.data && strstr(lexer.data, "@lex-")
                        && !strstr(lexer.data, "::="));

        core_free(grammar.data);
        core_free(lexer.data);
    }
}

typedef struct {
    const char *grammar;
    const char *start;
    const char *source;
    const char *tokens[6];
} lex_feature_case_t;

static void test_external_lexers_cover_distinct_recipe_families(const char *dir)
{
    static const lex_feature_case_t CASES[] = {
        {"bce.bnf", "translation_unit", "r\"raw\" 0b101'01 0x1.fp2 ",
         {"STRING", "INTEGER", "FLOAT"}},
        {"c_ncc.bnf", "translation_unit", "0x1fULL 0x1.fp2 'x' ",
         {"INTEGER", "FLOAT", "CHAR"}},
        {"cel.bnf", "expr", "b\"bytes\" R'raw' 1u ",
         {"BYTES", "STRING", "UINT"}},
        {"csharp.bnf", "compilation-unit",
         "@\"verbatim\" \"\"\"raw\"\"\" 0b10UL 1.0m ",
         {"VERBATIM_STRING", "RAW_STRING", "INTEGER", "FLOAT"}},
        {"go.bnf", "source-file", "`raw\ntext` 'x' 0o7 1i 0x1.fp2 ",
         {"STRING", "RUNE", "INTEGER", "IMAGINARY", "FLOAT"}},
        {"java.bnf", "compilation-unit",
         "\"\"\"text\"\"\" 'x' 0b10L 0x1.fp2 ",
         {"TEXT_BLOCK", "CHAR", "INTEGER", "FLOAT"}},
        {"python.bnf", "file",
         "if x:\n    y = RF'''{x}'''\n    z = BR\"bytes\"\n    n = 0x_1\n",
         {"INDENT", "DEDENT", "FSTRING_START", "FSTRING_END", "INTEGER"}},
        {"kotlin.bnf", "kotlin-file",
         "/* outer /* nested */ done */ \"\"\"text\"\"\" 1u 1f ",
         {"MULTILINE_STRING", "INTEGER", "FLOAT"}},
        {"lua.bnf", "chunk", "--[[comment]]\nlocal s = [[body]] 0x1.fp2 ",
         {"STRING", "FLOAT"}},
        {"javascript.bnf", "script", "/x/g; #slot; 1n; `x${y}` ",
         {"REGEX_LIT", "PRIVATE_ID", "BIGINT", "TEMPLATE_HEAD",
          "TEMPLATE_TAIL"}},
        {"php.bnf", "top-statement-list",
         "<?php $value 0o7 <<<END\nbody\nEND\n?>",
         {"VARIABLE", "INTEGER", "HEREDOC"}},
        {"ruby.bnf", "program",
         "@@x @y $z Foo %q{a{b}c} %w[one two]\n",
         {"CVAR", "IVAR", "GVAR", "CONSTANT", "NEWLINE", "Q"}},
        {"rust.bnf", "crate",
         "r###\"raw\"### br##\"bytes\"## r#type 0xffusize 1f32 ",
         {"RAW_STRING", "RAW_BYTE_STRING", "IDENTIFIER", "INTEGER", "FLOAT"}},
        {"swift.bnf", "top-level-declaration",
         "/x/ $12 ###\"raw\"### 0x1.fp2 ",
         {"IMPLICIT_PARAM", "STRING", "REGEX_LIT", "FLOAT"}},
        {"typescript.bnf", "source-file", "/x/g; `x${y}` ",
         {"REGEX_LIT", "TEMPLATE_HEAD", "TEMPLATE_TAIL"}},
        {"json.bnf", "json", "{\"x\": 1}", {nullptr}},
    };
    char what[256];

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, CASES[i].grammar);
        core_string_t grammar_text = slurp(path);
        parse_grammar_t *g = parse_grammar_new();

        parse_grammar_set_error_recovery(g, false);
        parse_bnf_declare_terminals(g, grammar_text);
        bool loaded = parse_bnf_load_file(
            path, core_string_borrow(CASES[i].start, strlen(CASES[i].start)),
            g);
        parse_token_stream_t *ts = loaded
            ? scan_tokenize(g, CASES[i].source, strlen(CASES[i].source),
                            nullptr, nullptr)
            : nullptr;
        size_t counts[6] = {0};

        if (ts) {
            scan_stream_foreach(ts, tok) {
                for (size_t k = 0; k < 6 && CASES[i].tokens[k]; k++) {
                    int64_t tid = parse_terminal_id(
                        g, core_string_borrow(CASES[i].tokens[k],
                                              strlen(CASES[i].tokens[k])));
                    counts[k] += tid && parse_token_tid(tok) == tid;
                }
            }
        }

        snprintf(what, sizeof(what), "%s exercises its external lexer",
                 CASES[i].grammar);
        bool ok = ts && scan_status(ts->scanner) == SCAN_STATUS_COMPLETE;
        for (size_t k = 0; ok && k < 6 && CASES[i].tokens[k]; k++) {
            ok = counts[k] > 0;
        }
        check(what, ok);

        scan_token_stream_free(ts);
        parse_grammar_free(g);
        core_free(grammar_text.data);
    }
}

static void test_character_literal_width_rules(const char *dir)
{
    static const struct {
        const char *grammar;
        const char *start;
        const char *source;
        bool        valid;
    } CASES[] = {
        {"c_ncc.bnf",  "translation_unit",      "'ab' ", true },
        {"bce.bnf",    "translation_unit",      "'ab' ", true },
        {"csharp.bnf", "compilation-unit",      "'ab' ", false},
        {"go.bnf",     "source-file",           "'ab' ", false},
        {"java.bnf",   "compilation-unit",      "'ab' ", false},
        {"kotlin.bnf", "kotlin-file",           "'ab' ", false},
        {"rust.bnf",   "crate",                 "'ab' ", false},
    };
    char what[256];

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, CASES[i].grammar);

        parse_grammar_t *g = parse_grammar_new();
        bool loaded = parse_bnf_load_file(
            path,
            core_string_borrow(CASES[i].start, strlen(CASES[i].start)),
            g);
        parse_token_stream_t *ts = loaded
            ? scan_tokenize_owned(g, CASES[i].source, strlen(CASES[i].source))
            : nullptr;

        if (ts) {
            while (scan_stream_next(ts)) {
            }
        }

        scan_status_t status = ts ? scan_status(ts->scanner)
                                  : SCAN_STATUS_INVALID;
        snprintf(what, sizeof(what), "%s %s multicharacter literals",
                 CASES[i].grammar, CASES[i].valid ? "accepts" : "rejects");
        check(what, loaded && ts
                        && (CASES[i].valid
                                ? status == SCAN_STATUS_COMPLETE
                                : status == SCAN_STATUS_INVALID));

        scan_token_stream_free(ts);
        parse_grammar_free(g);
    }
}

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

        bool ok = parse_bnf_load_file(
            path,
            core_string_borrow(CORPUS[i].start, strlen(CORPUS[i].start)),
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
        parse_bnf_load_file(
            path,
            core_string_borrow(CORPUS[i].start, strlen(CORPUS[i].start)),
            g);

        snprintf(what, sizeof(what), "%s declares a lexis", CORPUS[i].file);
        const scan_lex_spec_t *lex = scan_grammar_lex(g);

        if (!lex && scan_lex_build_error()) {
            printf("      %s\n", scan_lex_build_error());
        }

        check(what, lex != nullptr);

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

            if (parse_token_tid(tok) > 0 && parse_token_tid(tok) < 128) {
                continue;
            }

            bool known = false;

            for (size_t k = 0; k < core_list_len(g->named_terms); k++) {
                if (g->named_terms.data[k].id == parse_token_tid(tok)) {
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

        if (!strcmp(CORPUS[i].file, "json.bnf")) {
            scan_stream_reset(ts);

            parse_pwz_parser_t *parser = parse_pwz_new_for_start(
                g, parse_nonterm_id(parse_find_nonterm(
                       g, core_string_from_cstr(CORPUS[i].start))));
            bool                parsed = parse_pwz_parse(parser, ts);
            snprintf(what, sizeof(what), "%s: %s parses", CORPUS[i].file,
                     CORPUS[i].sample);
            check(what, parsed);
            parse_pwz_free(parser);
        }

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
          parse_bnf_load_file(path, CORE_STRING_STATIC("python-file"), g));
    check("declaring twice adds nothing the second time",
          parse_bnf_declare_terminals(g, text) == 0);

    parse_grammar_free(g);

    parse_grammar_t *with_origin = parse_grammar_new();
    parse_grammar_set_error_recovery(with_origin, false);
    check("in-memory grammars can resolve imports from an explicit origin",
          parse_bnf_load_with_origin(text, path,
                                     CORE_STRING_STATIC("python-file"),
                                     with_origin)
              && scan_grammar_lex(with_origin) != nullptr);
    parse_grammar_free(with_origin);
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
    test_external_lexer_files_stay_separate(dir);
    test_external_lexers_cover_distinct_recipe_families(dir);
    test_character_literal_width_rules(dir);
    test_declaring_terminals_is_what_makes_them_loadable(dir);
    test_a_borrowed_name_registers_like_any_other();
    test_a_grammar_that_can_never_match_says_so();

    return report("fe grammars");
}
