#include <stdio.h>
#include <stdlib.h>

#include "parse/bnf.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
#include "scanner/lex.h"
#include "scanner/tokenizer.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static const struct {
    core_string_t file;
    core_string_t start;
    core_string_t sample;
} CORPUS[] = {
    {S("cel.bnf"),        S("expr"),                  S("s.cel")  },
    {S("csharp.bnf"),     S("compilation-unit"),       S("s.cs")   },
    {S("go.bnf"),         S("source-file"),            S("s.go")   },
    {S("java.bnf"),       S("compilation-unit"),       S("s.java") },
    {S("javascript.bnf"), S("script"),                 S("s.js")   },
    {S("json.bnf"),       S("json"),                   S("s.json")},
    {S("kotlin.bnf"),     S("kotlin-file"),            S("s.kt")   },
    {S("lua.bnf"),        S("chunk"),                  S("s.lua")  },
    {S("php.bnf"),        S("top-statement-list"),     S("s.php")  },
    {S("python.bnf"),     S("file"),                   S("s.py")   },
    {S("ruby.bnf"),       S("program"),                S("s.rb")   },
    {S("rust.bnf"),       S("crate"),                  S("s.rs")   },
    {S("swift.bnf"),      S("top-level-declaration"),  S("s.swift")},
    {S("typescript.bnf"), S("source-file"),            S("s.ts")   },
    {S("c_ncc.bnf"),      S("translation_unit"),       S("s.c")    },
    {S("bce.bnf"),        S("translation_unit"),       S("s.bce")  },
};

static const struct {
    core_string_t grammar;
    core_string_t lexer;
} EXTERNAL_LEXERS[] = {
    {S("bce.bnf"),        S("bce.lex")       },
    {S("c_ncc.bnf"),      S("c_ncc.lex")     },
    {S("cel.bnf"),        S("cel.lex")       },
    {S("csharp.bnf"),     S("csharp.lex")    },
    {S("go.bnf"),         S("go.lex")        },
    {S("java.bnf"),       S("java.lex")      },
    {S("kotlin.bnf"),     S("kotlin.lex")    },
    {S("python.bnf"),     S("python.lex")    },
    {S("lua.bnf"),        S("lua.lex")       },
    {S("javascript.bnf"), S("javascript.lex")},
    {S("php.bnf"),        S("php.lex")       },
    {S("ruby.bnf"),       S("ruby.lex")      },
    {S("rust.bnf"),       S("rust.lex")      },
    {S("swift.bnf"),      S("swift.lex")     },
    {S("typescript.bnf"), S("typescript.lex")},
    {S("json.bnf"),       S("json.lex")      },
};

#define SF(s) (int)(s).u8_bytes, (s).data

static core_string_t
join_path(char *out, size_t cap, core_string_t dir, core_string_t leaf)
{
    int n = snprintf(out, cap, "%.*s/%.*s", SF(dir), SF(leaf));

    if (n < 0) {
        return core_string_borrow(out, 0);
    }

    return core_string_borrow(out, (size_t)n < cap ? (size_t)n : cap - 1);
}

static bool
contains(core_string_t s, core_string_t needle)
{
    return core_string_find(s, needle) >= 0;
}

static void test_external_lexer_files_stay_separate(core_string_t dir)
{
    char what[256];

    for (size_t i = 0; i < sizeof(EXTERNAL_LEXERS) / sizeof(EXTERNAL_LEXERS[0]);
         i++) {
        char grammar_path[1024];
        char lexer_path[1024];

        join_path(grammar_path, sizeof(grammar_path), dir,
                  EXTERNAL_LEXERS[i].grammar);
        join_path(lexer_path, sizeof(lexer_path), dir,
                  EXTERNAL_LEXERS[i].lexer);
        core_string_t grammar = slurp(grammar_path);
        core_string_t lexer = slurp(lexer_path);

        snprintf(what, sizeof(what), "%.*s imports an external lexer",
                 SF(EXTERNAL_LEXERS[i].grammar));
        check(what, grammar.data && contains(grammar, S("@lexer("))
                        && !contains(grammar, S("@lex-")));

        snprintf(what, sizeof(what), "%.*s contains lexical declarations only",
                 SF(EXTERNAL_LEXERS[i].lexer));
        check(what, lexer.data && contains(lexer, S("@lex-"))
                        && !contains(lexer, S("::=")));

        core_free(grammar.data);
        core_free(lexer.data);
    }
}

typedef struct {
    core_string_t grammar;
    core_string_t start;
    core_string_t source;
    core_string_t tokens[6];
} lex_feature_case_t;

static void test_external_lexers_cover_distinct_recipe_families(
    core_string_t dir)
{
    static const lex_feature_case_t CASES[] = {
        {S("bce.bnf"), S("translation_unit"), S("r\"raw\" 0b101'01 0x1.fp2 "),
         {S("STRING"), S("INTEGER"), S("FLOAT")}},
        {S("c_ncc.bnf"), S("translation_unit"), S("0x1fULL 0x1.fp2 'x' "),
         {S("INTEGER"), S("FLOAT"), S("CHAR")}},
        {S("cel.bnf"), S("expr"), S("b\"bytes\" R'raw' 1u "),
         {S("BYTES"), S("STRING"), S("UINT")}},
        {S("csharp.bnf"), S("compilation-unit"),
         S("@\"verbatim\" \"\"\"raw\"\"\" 0b10UL 1.0m "),
         {S("VERBATIM_STRING"), S("RAW_STRING"), S("INTEGER"), S("FLOAT")}},
        {S("go.bnf"), S("source-file"), S("`raw\ntext` 'x' 0o7 1i 0x1.fp2 "),
         {S("STRING"), S("RUNE"), S("INTEGER"), S("IMAGINARY"), S("FLOAT")}},
        {S("java.bnf"), S("compilation-unit"),
         S("\"\"\"text\"\"\" 'x' 0b10L 0x1.fp2 "),
         {S("TEXT_BLOCK"), S("CHAR"), S("INTEGER"), S("FLOAT")}},
        {S("python.bnf"), S("file"),
         S("if x:\n    y = RF'''{x}'''\n    z = BR\"bytes\"\n    n = 0x_1\n"),
         {S("INDENT"), S("DEDENT"), S("FSTRING_START"), S("FSTRING_END"),
          S("INTEGER")}},
        {S("kotlin.bnf"), S("kotlin-file"),
         S("/* outer /* nested */ done */ \"\"\"text\"\"\" 1u 1f "),
         {S("MULTILINE_STRING"), S("INTEGER"), S("FLOAT")}},
        {S("lua.bnf"), S("chunk"),
         S("--[[comment]]\nlocal s = [[body]] 0x1.fp2 "),
         {S("STRING"), S("FLOAT")}},
        {S("javascript.bnf"), S("script"), S("/x/g; #slot; 1n; `x${y}` "),
         {S("REGEX_LIT"), S("PRIVATE_ID"), S("BIGINT"), S("TEMPLATE_HEAD"),
          S("TEMPLATE_TAIL")}},
        {S("php.bnf"), S("top-statement-list"),
         S("<?php $value 0o7 <<<END\nbody\nEND\n?>"),
         {S("VARIABLE"), S("INTEGER"), S("HEREDOC")}},
        {S("ruby.bnf"), S("program"),
         S("@@x @y $z Foo %q{a{b}c} %w[one two]\n"),
         {S("CVAR"), S("IVAR"), S("GVAR"), S("CONSTANT"), S("NEWLINE"),
          S("Q")}},
        {S("rust.bnf"), S("crate"),
         S("r###\"raw\"### br##\"bytes\"## r#type 0xffusize 1f32 "),
         {S("RAW_STRING"), S("RAW_BYTE_STRING"), S("IDENTIFIER"),
          S("INTEGER"), S("FLOAT")}},
        {S("swift.bnf"), S("top-level-declaration"),
         S("/x/ $12 ###\"raw\"### 0x1.fp2 "),
         {S("IMPLICIT_PARAM"), S("STRING"), S("REGEX_LIT"), S("FLOAT")}},
        {S("typescript.bnf"), S("source-file"), S("/x/g; `x${y}` "),
         {S("REGEX_LIT"), S("TEMPLATE_HEAD"), S("TEMPLATE_TAIL")}},
        {S("json.bnf"), S("json"), S("{\"x\": 1}"), {}},
    };
    char what[256];

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        char          buf[1024];
        core_string_t path = join_path(buf, sizeof(buf), dir,
                                       CASES[i].grammar);
        core_string_t grammar_text = slurp(buf);
        parse_grammar_t *g = parse_grammar_new();

        parse_grammar_set_error_recovery(g, false);
        parse_bnf_declare_terminals(g, grammar_text);
        bool loaded = parse_bnf_load_file(path, CASES[i].start, g);
        parse_token_stream_t *ts = loaded
            ? scan_tokenize(g, CASES[i].source, nullptr, nullptr)
            : nullptr;
        size_t counts[6] = {0};

        if (ts) {
            scan_stream_foreach(ts, tok) {
                for (size_t k = 0; k < 6 && CASES[i].tokens[k].data; k++) {
                    int64_t tid = parse_terminal_id(g, CASES[i].tokens[k]);
                    counts[k] += tid && parse_token_tid(tok) == tid;
                }
            }
        }

        snprintf(what, sizeof(what), "%.*s exercises its external lexer",
                 SF(CASES[i].grammar));
        bool ok = ts && scan_status(ts->scanner) == SCAN_STATUS_COMPLETE;
        for (size_t k = 0; ok && k < 6 && CASES[i].tokens[k].data; k++) {
            ok = counts[k] > 0;
        }
        check(what, ok);

        scan_token_stream_free(ts);
        parse_grammar_free(g);
        core_free(grammar_text.data);
    }
}

static void test_character_literal_width_rules(core_string_t dir)
{
    static const struct {
        core_string_t grammar;
        core_string_t start;
        core_string_t source;
        bool          valid;
    } CASES[] = {
        {S("c_ncc.bnf"),  S("translation_unit"),      S("'ab' "), true },
        {S("bce.bnf"),    S("translation_unit"),      S("'ab' "), true },
        {S("csharp.bnf"), S("compilation-unit"),      S("'ab' "), false},
        {S("go.bnf"),     S("source-file"),           S("'ab' "), false},
        {S("java.bnf"),   S("compilation-unit"),      S("'ab' "), false},
        {S("kotlin.bnf"), S("kotlin-file"),           S("'ab' "), false},
        {S("rust.bnf"),   S("crate"),                 S("'ab' "), false},
    };
    char what[256];

    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        char          buf[1024];
        core_string_t path = join_path(buf, sizeof(buf), dir,
                                       CASES[i].grammar);

        parse_grammar_t *g = parse_grammar_new();
        bool loaded = parse_bnf_load_file(path, CASES[i].start, g);
        parse_token_stream_t *ts = loaded
            ? scan_tokenize_owned(g, CASES[i].source)
            : nullptr;

        if (ts) {
            while (scan_stream_next(ts)) {
            }
        }

        scan_status_t status = ts ? scan_status(ts->scanner)
                                  : SCAN_STATUS_INVALID;
        if (CASES[i].valid) {
            snprintf(what, sizeof(what), "%.*s accepts multicharacter literals",
                     SF(CASES[i].grammar));
        }
        else {
            snprintf(what, sizeof(what), "%.*s rejects multicharacter literals",
                     SF(CASES[i].grammar));
        }
        check(what, loaded && ts
                        && (CASES[i].valid
                                ? status == SCAN_STATUS_COMPLETE
                                : status == SCAN_STATUS_INVALID));

        scan_token_stream_free(ts);
        parse_grammar_free(g);
    }
}

static void test_every_grammar_in_the_corpus_loads(core_string_t dir)
{
    char what[256];

    for (size_t i = 0; i < sizeof(CORPUS) / sizeof(CORPUS[0]); i++) {
        char          buf[1024];
        core_string_t path = join_path(buf, sizeof(buf), dir, CORPUS[i].file);

        core_string_t text = slurp(buf);

        if (!text.data) {
            continue;
        }

        parse_grammar_t *g = parse_grammar_new();

        parse_grammar_set_error_recovery(g, false);
        parse_bnf_declare_terminals(g, text);

        bool ok = parse_bnf_load_file(path, CORPUS[i].start, g);

        snprintf(what, sizeof(what), "%.*s loads", SF(CORPUS[i].file));
        check(what, ok);

        snprintf(what, sizeof(what),
                 "%.*s defines every non-terminal it uses",
                 SF(CORPUS[i].file));
        check(what, !ok || parse_grammar_report_undefined(g, stdout));

        snprintf(what, sizeof(what), "%.*s has rules", SF(CORPUS[i].file));
        check(what, !ok || g->rules.len > 0);

        parse_grammar_free(g);
        core_free(text.data);
    }
}

static void test_every_grammar_tokenizes_its_own_language(
    core_string_t dir, core_string_t samples)
{
    char what[256];

    for (size_t i = 0; i < sizeof(CORPUS) / sizeof(CORPUS[0]); i++) {
        if (!CORPUS[i].sample.data) {
            continue;
        }

        char          buf[1024];
        core_string_t path = join_path(buf, sizeof(buf), dir, CORPUS[i].file);

        core_string_t text = slurp(buf);

        if (!text.data) {
            continue;
        }

        parse_grammar_t *g = parse_grammar_new();

        parse_grammar_set_error_recovery(g, false);
        parse_bnf_declare_terminals(g, text);
        parse_bnf_load_file(path, CORPUS[i].start, g);

        snprintf(what, sizeof(what), "%.*s declares a lexis",
                 SF(CORPUS[i].file));
        const scan_lex_spec_t *lex = scan_grammar_lex(g);
        core_string_t          lex_err = scan_lex_build_error();

        if (!lex && lex_err.data) {
            printf("      %.*s\n", SF(lex_err));
        }

        check(what, lex != nullptr);

        join_path(buf, sizeof(buf), samples, CORPUS[i].sample);

        core_string_t src = slurp(buf);

        if (!src.data) {
            parse_grammar_free(g);
            core_free(text.data);

            continue;
        }

        core_buffer_t        *sbuf = nullptr;
        parse_scanner_t      *sc   = nullptr;
        parse_token_stream_t *ts   = scan_tokenize(g, src, &sbuf, &sc);

        snprintf(what, sizeof(what), "%.*s: the lexis builds with no complaint",
                 SF(CORPUS[i].file));
        check(what, scan_lex_build_error().data == nullptr);

        snprintf(what, sizeof(what), "%.*s: %.*s tokenizes",
                 SF(CORPUS[i].file), SF(CORPUS[i].sample));
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

        snprintf(what, sizeof(what), "%.*s: %.*s produces tokens",
                 SF(CORPUS[i].file), SF(CORPUS[i].sample));
        check(what, n > 0);

        snprintf(what, sizeof(what),
                 "%.*s: every token of %.*s is a terminal the grammar declares",
                 SF(CORPUS[i].file), SF(CORPUS[i].sample));

        if (unmapped) {
            printf("      %zu of %zu tokens unmapped\n", unmapped, n);
        }

        check(what, unmapped == 0);

        if (core_string_eq(CORPUS[i].file, S("json.bnf"))) {
            scan_stream_reset(ts);

            parse_pwz_parser_t *parser = parse_pwz_new_for_start(
                g, parse_nonterm_id(parse_find_nonterm(g, CORPUS[i].start)));
            bool                parsed = parse_pwz_parse(parser, ts);
            snprintf(what, sizeof(what), "%.*s: %.*s parses",
                     SF(CORPUS[i].file), SF(CORPUS[i].sample));
            check(what, parsed);
            parse_pwz_free(parser);
        }

        scan_token_stream_free(ts);
        scan_scanner_free(sc);
        core_buffer_free(sbuf);
        parse_grammar_free(g);
        core_free(text.data);
        core_free(src.data);
    }
}

static void test_declaring_terminals_is_what_makes_them_loadable(
    core_string_t dir)
{
    char          buf[1024];
    core_string_t path = join_path(buf, sizeof(buf), dir, S("python.bnf"));

    core_string_t text = slurp(buf);

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
    static const char run[] = "FOOBAR_AND_MORE";

    parse_grammar_t *g = parse_grammar_new();
    int64_t        a = parse_register_terminal(g, core_string_borrow(run, 6));

    check("a name borrowed from a longer buffer registers", a != 0);
    check("and is found again by content",
          parse_lookup_terminal(g, CORE_STRING_STATIC("FOOBAR")) == a);
    check("without matching the longer buffer it came from",
          parse_lookup_terminal(g, core_string_borrow(run, 15)) != a);

    parse_grammar_free(g);
}

#define TYPO                                \
    "<top> ::= <a> <nonexistent-thing>\n"   \
    "<a> ::= %\"x\"\n"

static void test_a_grammar_that_can_never_match_says_so(void)
{
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
    core_string_t dir     = argc > 1 ? core_string_from_cstr(argv[1])
                                     : S("../../grammars");
    core_string_t samples = argc > 2 ? core_string_from_cstr(argv[2])
                                     : S("../samples");

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
