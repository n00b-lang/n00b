#include <stdio.h>
#include <string.h>

#include "lib/alloc.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "scanner/scan_builtins.h"
#include "scanner/tokenizer.h"
#include "scanner/token_stream.h"
#include "internal/parse/grammar_internal.h"
#include "ir_test.h"

static ncc_string_t
S(const char *s)
{
    return ncc_string_from_cstr(s);
}

static int shouty_calls;

static bool
scan_shouty(ncc_scanner_t *s)
{
    shouty_calls++;

    ncc_scan_skip_whitespace(s);

    if (ncc_scan_at_eof(s)) {
        return false;
    }

    ncc_scan_mark(s);

    ncc_codepoint_t c = ncc_scan_peek(s, 0);

    if (c >= 'A' && c <= 'Z') {
        while (!ncc_scan_at_eof(s)) {
            ncc_codepoint_t k = ncc_scan_peek(s, 0);

            if (k < 'A' || k > 'Z') {
                break;
            }

            ncc_scan_advance(s);
        }

        ncc_scan_emit_marked(s, NCC_TOK_IDENTIFIER);

        return true;
    }

    ncc_scan_advance(s);
    ncc_scan_emit_marked(s, (int32_t)c);

    return true;
}

static size_t
count_tokens(ncc_grammar_t *g, const char *src)
{
    ncc_buffer_t       *buf = nullptr;
    ncc_scanner_t      *sc  = nullptr;
    ncc_token_stream_t *ts  = ncc_tokenize(g, src, strlen(src), &buf, &sc);
    size_t              n   = 0;

    if (!ts) {
        return 0;
    }

    while (ncc_stream_next(ts)) {
        n++;
    }

    ncc_token_stream_free(ts);
    ncc_scanner_free(sc);
    ncc_buffer_free(buf);

    return n;
}

static void
test_the_builtin_tokenizers(void)
{
    static const char *const NAMES[] = {"text", "json", "lisp", "shell", "c"};

    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (!ncc_tokenizer_for(S(NAMES[i]))) {
            printf("FAIL  the %s tokenizer is not registered\n", NAMES[i]);
            fails++;
        }
    }

    ncc_grammar_t *g = ncc_grammar_new();

    ncc_grammar_set_tokenizer(g, S("json"));

    check("json: a whole object is 9 tokens",
          count_tokens(g, "{\"a\": [1, 2.5e3]}") == 9);
    check("json: true is one token, not four",
          count_tokens(g, "true") == 1);
    check("json: a negative float is one token",
          count_tokens(g, "-0.5") == 1);

    ncc_grammar_set_tokenizer(g, S("lisp"));

    check("lisp: a form is its parens plus its atoms",
          count_tokens(g, "(cons 'a \"s\")") == 6);
    check("lisp: a semicolon comment is skipped",
          count_tokens(g, "(a) ; note\n(b)") == 6);
    check("lisp: a hyphenated symbol stays one token",
          count_tokens(g, "call-with-x") == 1);

    ncc_grammar_set_tokenizer(g, S("shell"));

    check("shell: && is one token, not two",
          count_tokens(g, "a && b") == 3);
    check("shell: a redirect and a pipe are separate",
          count_tokens(g, "a > b | c") == 5);
    check("shell: a hash comment is skipped",
          count_tokens(g, "a # note\n") == 2);

    ncc_grammar_set_tokenizer(g, S("c"));

    check("c: three-character operators are one token",
          count_tokens(g, "a <<= b") == 3);
    check("c: two-character operators are one token",
          count_tokens(g, "a->b == c") == 5);
    check("c: both comment forms are skipped",
          count_tokens(g, "a // x\n/* y */ b") == 2);
    check("c: a char literal is one token",
          count_tokens(g, "'x'") == 1);

    ncc_grammar_free(g);
}

int
main(void)
{
    check("the default tokenizer is always registered",
          ncc_tokenizer_for(S(NCC_TOKENIZER_DEFAULT)) != nullptr);
    check("an unregistered name resolves to nothing",
          ncc_tokenizer_for(S("nope")) == nullptr);

    size_t before = ncc_tokenizer_count();

    check("a tokenizer can be registered by name",
          ncc_tokenizer_register(S("shouty"), scan_shouty));
    check("and shows up in the registry",
          ncc_tokenizer_count() == before + 1
              && ncc_tokenizer_for(S("shouty")) == scan_shouty);

    check("registering the same name again replaces it, not duplicates it",
          ncc_tokenizer_register(S("shouty"), scan_shouty)
              && ncc_tokenizer_count() == before + 1);

    ncc_grammar_t *plain = ncc_grammar_new();

    check("a grammar with no @tokenizer uses the default",
          ncc_string_eq(ncc_grammar_tokenizer(plain),
                        S(NCC_TOKENIZER_DEFAULT)));
    check("and resolves to the default's function",
          ncc_grammar_tokenizer_fn(plain)
              == ncc_tokenizer_for(S(NCC_TOKENIZER_DEFAULT)));

    check("the default tokenizer splits identifiers and punctuation",
          count_tokens(plain, "a + bb;") == 4);

    ncc_grammar_free(plain);

    static const char *const G = "@tokenizer(\"shouty\")\n"
                                 "<top> ::= %IDENTIFIER\n";

    ncc_grammar_t *g = ncc_grammar_new();

    ncc_grammar_set_error_recovery(g, false);

    check("a grammar naming a tokenizer loads",
          ncc_bnf_load(S(G), S("top"), g));

    check("@tokenizer sets the grammar's tokenizer",
          ncc_string_eq(ncc_grammar_tokenizer(g), S("shouty")));
    check("and it resolves through the registry",
          ncc_grammar_tokenizer_fn(g) == scan_shouty);

    shouty_calls = 0;

    check("tokenizing through the grammar uses the named tokenizer",
          count_tokens(g, "AAA BBB") == 2 && shouty_calls > 0);

    ncc_grammar_free(g);

    static const char *const MISSING = "@tokenizer(\"absent\")\n"
                                       "<top> ::= %IDENTIFIER\n";

    ncc_grammar_t *m = ncc_grammar_new();

    ncc_grammar_set_error_recovery(m, false);
    ncc_bnf_load(S(MISSING), S("top"), m);

    check("a grammar naming an unregistered tokenizer resolves to nothing",
          ncc_grammar_tokenizer_fn(m) == nullptr);
    check("and tokenizing with it yields no tokens rather than crashing",
          count_tokens(m, "x") == 0);

    ncc_grammar_free(m);

    test_the_builtin_tokenizers();

    return report("fe tokenizer");
}
