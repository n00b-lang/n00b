#include <stdio.h>
#include <string.h>

#include "parse/bnf.h"
#include "parse/grammar.h"
#include "scanner/lex.h"
#include "scanner/tokenizer.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

#define HEX "0123456789abcdefABCDEF"

static const scan_lex_comment_t PY_COMMENTS[] = {
    {.kind = SCAN_LEX_LINE, .opener = S("#")},
    {},
};

static const scan_lex_string_t PY_STRINGS[] = {
    {.opener = S("\"\"\""), .closer = S("\"\"\""), .escape = S("\\"),
     .flags = SCAN_LEX_STR_MULTILINE, .tok = S("STRING")},
    {.opener = S("f\""), .closer = S("\""), .escape = S("\\"),
     .interp_open = S("{"), .interp_close = S("}"),
     .tok = S("FSTRING_START"), .tok_mid = S("FSTRING_MIDDLE"),
     .tok_end = S("FSTRING_END")},
    {.opener = S("\""), .closer = S("\""), .escape = S("\\"),
     .tok = S("STRING")},
    {},
};

static const scan_lex_radix_t PY_RADIXES[] = {
    {.prefix = S("0x"), .digits = S(HEX)},
    {.prefix = S("0b"), .digits = S("01")},
    {},
};

static const scan_lex_suffix_t PY_SUFFIXES[] = {
    {.suffix = S("j"), .tok = S("IMAGINARY")},
    {},
};

static const scan_lex_spec_t PYTHON = {
    .name        = S("lex-test-python"),
    .comments    = PY_COMMENTS,
    .strings     = PY_STRINGS,
    .number      = {.radixes   = PY_RADIXES,
                    .suffixes  = PY_SUFFIXES,
                    .sep       = S("_"),
                    .exponent  = S("eE"),
                    .tok_int   = S("INTEGER"),
                    .tok_float = S("FLOAT")},
    .layout      = SCAN_LEX_LAYOUT_INDENT | SCAN_LEX_LAYOUT_NEWLINE,
    .tok_newline = S("NEWLINE"),
    .tok_indent  = S("INDENT"),
    .tok_dedent  = S("DEDENT"),
    .tok_eof     = S("ENDMARKER"),
    .tok_ident   = S("IDENTIFIER"),
    .line_continue = S("\\"),
    .brackets      = S("()[]{}"),
};

static const scan_lex_comment_t JS_COMMENTS[] = {
    {.kind = SCAN_LEX_LINE, .opener = S("//")},
    {.kind = SCAN_LEX_BLOCK, .opener = S("/*"), .closer = S("*/")},
    {},
};

static const scan_lex_string_t JS_STRINGS[] = {
    {.opener = S("`"), .closer = S("`"), .escape = S("\\"),
     .flags = SCAN_LEX_STR_MULTILINE,
     .interp_open = S("${"), .interp_close = S("}"),
     .tok = S("TEMPLATE_HEAD"), .tok_mid = S("TEMPLATE_MIDDLE"),
     .tok_end = S("TEMPLATE_TAIL")},
    {.opener = S("\""), .closer = S("\""), .escape = S("\\"),
     .tok = S("STRING")},
    {},
};

static const scan_lex_regex_t JS_REGEXES[] = {
    {.opener = S("/"), .closer = S("/"), .suffix_chars = S("gimsuyd"),
     .tok = S("REGEX_LIT")},
    {},
};

static const scan_lex_spec_t JS = {
    .name      = S("lex-test-js"),
    .comments  = JS_COMMENTS,
    .strings   = JS_STRINGS,
    .regexes   = JS_REGEXES,
    .number    = {.exponent = S("eE"), .tok_int = S("INTEGER")},
    .tok_ident = S("IDENTIFIER"),
    .brackets  = S("()[]{}"),
};

static const scan_lex_heredoc_t RB_HEREDOCS[] = {
    {.opener = S("<<~"), .tok = S("HEREDOC"), .indent_strip = true},
    {},
};

static const scan_lex_sigil_t RB_SIGILS[] = {
    {.prefix = S("@@"), .tok = S("CVAR")},
    {.prefix = S("@"), .tok = S("IVAR")},
    {.prefix = S("$"), .tok = S("GVAR")},
    {},
};

static const scan_lex_spec_t RUBY = {
    .name      = S("lex-test-ruby"),
    .heredocs  = RB_HEREDOCS,
    .sigils    = RB_SIGILS,
    .number    = {.tok_int = S("INTEGER")},
    .tok_ident = S("IDENTIFIER"),
};

static parse_grammar_t *
mk(const scan_lex_spec_t *spec, const char *const *terms)
{
    parse_grammar_t *g = parse_grammar_new();

    for (size_t i = 0; terms && terms[i]; i++) {
        parse_register_terminal(g, core_string_from_cstr(terms[i]));
    }

    scan_lex_register(spec);
    scan_grammar_set_tokenizer(g, spec->name);

    return g;
}

static char *
render(parse_grammar_t *g, const char *src)
{
    static char    out[2048];
    core_buffer_t  *buf = nullptr;
    parse_scanner_t *sc  = nullptr;

    out[0] = '\0';

    parse_token_stream_t *ts = scan_tokenize(g, src, strlen(src), &buf, &sc);

    if (!ts) {
        return out;
    }

    size_t at = 0;

    scan_stream_foreach(ts, tok) {
        const char *nm = nullptr;

        for (size_t i = 0; i < core_list_len(g->named_terms); i++) {
            if (g->named_terms.data[i].id == tok->tid) {
                nm = g->named_terms.data[i].value.data;

                break;
            }
        }

        if (nm) {
            at += (size_t)snprintf(out + at, sizeof(out) - at, "%s%s",
                                   at ? " " : "", nm);
        }
        else if (tok->tid > 0 && tok->tid < 128) {
            at += (size_t)snprintf(out + at, sizeof(out) - at, "%s%c",
                                   at ? " " : "", (char)tok->tid);
        }
        else {
            at += (size_t)snprintf(out + at, sizeof(out) - at, "%s?%d",
                                   at ? " " : "", tok->tid);
        }
    }

    scan_token_stream_free(ts);
    scan_scanner_free(sc);
    core_buffer_free(buf);

    return out;
}

static void
kinds(parse_grammar_t *g, const char *what, const char *src, const char *want)
{
    char *got = render(g, src);

    if (strcmp(got, want)) {
        fails++;
        printf("FAIL  %s\n   want: %s\n    got: %s\n", what, want, got);
    }
}

static const char *const PY_TERMS[] = {
    "NEWLINE", "INDENT", "DEDENT", "ENDMARKER", "IDENTIFIER", "INTEGER",
    "FLOAT", "STRING", "IMAGINARY", "FSTRING_START", "FSTRING_MIDDLE",
    "FSTRING_END", "def", "if", "return", nullptr,
};

static void
test_indentation_becomes_tokens(void)
{
    parse_grammar_t *g = mk(&PYTHON, PY_TERMS);

    kinds(g,
          "an indented block opens and closes",
          "def f():\n    return 1\n",
          "def IDENTIFIER ( ) : NEWLINE INDENT return INTEGER NEWLINE "
          "DEDENT ENDMARKER");

    kinds(g,
          "a dedent lands before the token that caused it",
          "if x:\n  a\n  b\nc\n",
          "if IDENTIFIER : NEWLINE INDENT IDENTIFIER NEWLINE IDENTIFIER "
          "NEWLINE DEDENT IDENTIFIER NEWLINE ENDMARKER");

    kinds(g,
          "a newline inside brackets is not significant",
          "a = (1,\n     2)\n",
          "IDENTIFIER = ( INTEGER , INTEGER ) NEWLINE ENDMARKER");

    kinds(g,
          "a blank line does not dedent",
          "if x:\n  a\n\n  b\n",
          "if IDENTIFIER : NEWLINE INDENT IDENTIFIER NEWLINE IDENTIFIER "
          "NEWLINE DEDENT ENDMARKER");

    kinds(g,
          "a comment line does not dedent",
          "if x:\n  a\n# note\n  b\n",
          "if IDENTIFIER : NEWLINE INDENT IDENTIFIER NEWLINE IDENTIFIER "
          "NEWLINE DEDENT ENDMARKER");

    kinds(g,
          "a continued line is one logical line",
          "a = 1 + \\\n    2\n",
          "IDENTIFIER = INTEGER + INTEGER NEWLINE ENDMARKER");

    kinds(g,
          "nested blocks close in one step",
          "if a:\n  if b:\n    c\nd\n",
          "if IDENTIFIER : NEWLINE INDENT if IDENTIFIER : NEWLINE INDENT "
          "IDENTIFIER NEWLINE DEDENT DEDENT IDENTIFIER NEWLINE ENDMARKER");

    parse_grammar_free(g);
}

static void
test_interpolation_pushes_a_mode(void)
{
    parse_grammar_t *g = mk(&PYTHON, PY_TERMS);

    kinds(g,
          "an f-string with one hole is head and end",
          "f\"a{b}c\"",
          "FSTRING_START IDENTIFIER FSTRING_END NEWLINE ENDMARKER");

    kinds(g,
          "two holes put a middle between them",
          "f\"{x}{y}\"",
          "FSTRING_START IDENTIFIER FSTRING_MIDDLE IDENTIFIER FSTRING_END "
          "NEWLINE ENDMARKER");

    kinds(g,
          "an f-string with no hole is one token",
          "f\"plain\"",
          "FSTRING_START NEWLINE ENDMARKER");

    kinds(g,
          "a triple-quoted string spans lines",
          "\"\"\"a\nb\"\"\"\n",
          "STRING NEWLINE ENDMARKER");

    parse_grammar_free(g);
}

static void
test_templates_and_regex(void)
{
    static const char *const T[] = {
        "IDENTIFIER", "INTEGER", "STRING", "TEMPLATE_HEAD",
        "TEMPLATE_MIDDLE", "TEMPLATE_TAIL", "REGEX_LIT", nullptr,
    };

    parse_grammar_t *g = mk(&JS, T);

    kinds(g, "a template with one hole", "`a${b}c`",
          "TEMPLATE_HEAD IDENTIFIER TEMPLATE_TAIL");

    kinds(g, "back-to-back holes", "`${a}${b}`",
          "TEMPLATE_HEAD IDENTIFIER TEMPLATE_MIDDLE IDENTIFIER "
          "TEMPLATE_TAIL");

    kinds(g,
          "a brace inside a hole does not close it",
          "`x${ {k:1} }y`",
          "TEMPLATE_HEAD { IDENTIFIER : INTEGER } TEMPLATE_TAIL");

    kinds(g, "a regex where an operand is expected", "x = /ab+/gi",
          "IDENTIFIER = REGEX_LIT");

    kinds(g, "a slash after a name is division", "a / b",
          "IDENTIFIER / IDENTIFIER");

    kinds(g, "a slash after a paren is division", "(1) / 2",
          "( INTEGER ) / INTEGER");

    kinds(g, "a slash that never closes stays division", "a / b\nc",
          "IDENTIFIER / IDENTIFIER IDENTIFIER");

    kinds(g, "both comment forms are trivia", "a // x\n/* y */ b",
          "IDENTIFIER IDENTIFIER");

    parse_grammar_free(g);
}

static void
test_sigils_and_heredocs(void)
{
    static const char *const T[] = {
        "IDENTIFIER", "INTEGER", "HEREDOC", "CVAR", "IVAR", "GVAR", nullptr,
    };

    parse_grammar_t *g = mk(&RUBY, T);

    kinds(g, "the longest sigil wins", "@@a @b $c", "CVAR IVAR GVAR");
    kinds(g, "a bare sigil is not a sigil token", "@ 1", "@ INTEGER");

    kinds(g,
          "a heredoc body is one token and the line continues",
          "x = <<~EOS\n  body\nEOS\ny\n",
          "IDENTIFIER = HEREDOC IDENTIFIER");

    parse_grammar_free(g);
}

static void
test_a_spec_is_reusable_across_runs(void)
{
    parse_grammar_t *g = mk(&PYTHON, PY_TERMS);

    kinds(g, "the first run", "if x:\n  a\n",
          "if IDENTIFIER : NEWLINE INDENT IDENTIFIER NEWLINE DEDENT "
          "ENDMARKER");
    kinds(g, "and the state does not leak into the second", "if x:\n  a\n",
          "if IDENTIFIER : NEWLINE INDENT IDENTIFIER NEWLINE DEDENT "
          "ENDMARKER");

    parse_grammar_free(g);
}

static void
test_a_grammar_declares_its_own_lexis(void)
{
    static const char *const G =
        "@lex-comment(\"#\")\n"
        "@lex-comment(\"/*\", \"*/\", nested)\n"
        "@lex-string('\"', '\"', escape('\\\\'), as(\"STRING\"))\n"
        "@lex-string('f\"', '\"', escape('\\\\'), interp(\"{\", \"}\"), "
        "hole_tokens, "
        "as(\"FSTRING_START\", \"FSTRING_MIDDLE\", \"FSTRING_END\"))\n"
        "@lex-number(radix(\"0x\", \"0123456789abcdef\"), sep(\"_\"), "
        "exp(\"eE\"), int(\"INTEGER\"), float(\"FLOAT\"), "
        "suffix(\"j\", \"IMAGINARY\"))\n"
        "@lex-layout(indent(\"INDENT\", \"DEDENT\"), newline(\"NEWLINE\"), "
        "eof(\"ENDMARKER\"), continue(\"\\\\\"), brackets(\"()[]{}\"))\n"
        "@lex-ident(as(\"IDENTIFIER\"))\n"
        "<file> ::= %IDENTIFIER %NEWLINE %INDENT %DEDENT %ENDMARKER "
        "%INTEGER %FLOAT %STRING %IMAGINARY %FSTRING_START "
        "%FSTRING_MIDDLE %FSTRING_END %\"def\" %\"return\"\n";

    parse_grammar_t *g    = parse_grammar_new();
    core_string_t   text = core_string_from_cstr(G);

    parse_grammar_set_error_recovery(g, false);
    parse_bnf_declare_terminals(g, text);

    if (!parse_bnf_load(text, CORE_STRING_STATIC("file"), g)) {
        check("a grammar with @lex- declarations loads", false);
        parse_grammar_free(g);

        return;
    }

    kinds(g,
          "the declared lexis drives the scanner",
          "def f():\n    x = 0x1f + 2_0\n    return f\"a{x}b\"\n",
          "def IDENTIFIER ( ) : NEWLINE INDENT IDENTIFIER = INTEGER + "
          "INTEGER NEWLINE return FSTRING_START { IDENTIFIER } "
          "FSTRING_END NEWLINE DEDENT ENDMARKER");

    kinds(g, "a nested block comment closes once", "a /* x /* y */ z */ b",
          "IDENTIFIER IDENTIFIER NEWLINE ENDMARKER");

    check("nothing was rejected", scan_lex_build_error() == nullptr);

    parse_grammar_free(g);
    core_free(text.data);
}

static void
test_an_unknown_option_is_named(void)
{
    static const char *const G = "@lex-string(\"'\", \"'\", bogus)\n"
                                 "<file> ::= %IDENTIFIER\n";

    parse_grammar_t *g    = parse_grammar_new();
    core_string_t   text = core_string_from_cstr(G);

    parse_grammar_set_error_recovery(g, false);
    parse_bnf_declare_terminals(g, text);
    parse_bnf_load(text, CORE_STRING_STATIC("file"), g);

    core_buffer_t  *buf = nullptr;
    parse_scanner_t *sc  = nullptr;

    parse_token_stream_t *ts = scan_tokenize(g, "a", 1, &buf, &sc);

    const char *err = scan_lex_build_error();

    check("an unknown @lex- option is reported", err != nullptr);
    check("and the message names it",
          err && strstr(err, "bogus") && strstr(err, "lex-string"));

    if (ts) {
        scan_token_stream_free(ts);
        scan_scanner_free(sc);
        core_buffer_free(buf);
    }

    parse_grammar_free(g);
    core_free(text.data);
}

int
main(void)
{
    test_indentation_becomes_tokens();
    test_interpolation_pushes_a_mode();
    test_templates_and_regex();
    test_sigils_and_heredocs();
    test_a_spec_is_reusable_across_runs();
    test_a_grammar_declares_its_own_lexis();
    test_an_unknown_option_is_named();

    return report("scanner lex");
}
