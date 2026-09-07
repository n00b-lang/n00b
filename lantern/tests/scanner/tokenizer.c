#include <stdio.h>
#include <string.h>

#include "lib/alloc.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "scanner/scan_builtins.h"
#include "scanner/tokenizer.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include "harness.h"

static core_string_t
S(const char *s)
{
    return core_string_from_cstr(s);
}

static int shouty_calls;

static bool
scan_shouty(parse_scanner_t *s)
{
    shouty_calls++;

    scan_skip_whitespace(s);

    if (scan_at_eof(s)) {
        return false;
    }

    scan_mark(s);

    core_codepoint_t c = scan_peek(s, 0);

    if (c >= 'A' && c <= 'Z') {
        while (!scan_at_eof(s)) {
            core_codepoint_t k = scan_peek(s, 0);

            if (k < 'A' || k > 'Z') {
                break;
            }

            scan_advance(s);
        }

        scan_emit_marked(s, PARSE_TOK_IDENTIFIER);

        return true;
    }

    scan_advance(s);
    scan_emit_marked(s, (int32_t)c);

    return true;
}

static size_t
count_tokens(parse_grammar_t *g, const char *src)
{
    core_buffer_t       *buf = nullptr;
    parse_scanner_t      *sc  = nullptr;
    parse_token_stream_t *ts  = scan_tokenize(g, src, strlen(src), &buf, &sc);
    size_t              n   = 0;

    if (!ts) {
        return 0;
    }

    scan_stream_mark_t mark = scan_stream_save(ts);

    (void)scan_stream_next(ts);
    scan_stream_restore(ts, mark);

    scan_stream_foreach(ts, tok) {
        (void)tok;
        n++;
    }

    scan_token_stream_free(ts);
    scan_scanner_free(sc);
    core_buffer_free(buf);

    return n;
}

static void
test_the_builtin_tokenizers(void)
{
    static const char *const NAMES[] = {"text", "json", "lisp", "shell", "c"};

    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (!scan_tokenizer_for(S(NAMES[i]))) {
            printf("FAIL  the %s tokenizer is not registered\n", NAMES[i]);
            fails++;
        }
    }

    parse_grammar_t *g = parse_grammar_new();

    scan_grammar_set_tokenizer(g, S("json"));

    check("json: a whole object is 9 tokens",
          count_tokens(g, "{\"a\": [1, 2.5e3]}") == 9);
    check("json: true is one token, not four",
          count_tokens(g, "true") == 1);
    check("json: a negative float is one token",
          count_tokens(g, "-0.5") == 1);

    scan_grammar_set_tokenizer(g, S("lisp"));

    check("lisp: a form is its parens plus its atoms",
          count_tokens(g, "(cons 'a \"s\")") == 6);
    check("lisp: a semicolon comment is skipped",
          count_tokens(g, "(a) ; note\n(b)") == 6);
    check("lisp: a hyphenated symbol stays one token",
          count_tokens(g, "call-with-x") == 1);

    scan_grammar_set_tokenizer(g, S("shell"));

    check("shell: a redirect and a pipe are separate",
          count_tokens(g, "a > b | c") == 5);
    check("shell: a hash comment is skipped",
          count_tokens(g, "a # note\n") == 2);

    scan_grammar_set_tokenizer(g, S("c"));

    check("c: both comment forms are skipped",
          count_tokens(g, "a // x\n/* y */ b") == 2);
    check("c: a char literal is one token",
          count_tokens(g, "'x'") == 1);
    check("c: a hex literal is one token",
          count_tokens(g, "0x1f") == 1);
    check("c: an exponent stays in the float",
          count_tokens(g, "2.5e3") == 1);

    parse_grammar_free(g);
}

static void
test_the_grammar_is_the_operator_table(void)
{
    parse_grammar_t *bare = parse_grammar_new();

    scan_grammar_set_tokenizer(bare, S("c"));

    check("an operator the grammar never declares is not one token",
          count_tokens(bare, "a <<= b") == 5);
    parse_grammar_free(bare);

    parse_grammar_t *g = parse_grammar_new();

    scan_grammar_set_tokenizer(g, S("c"));

    static const char *const OPS[] = {"<<=", "->", "==", "<", "=", "-", ">"};

    for (size_t i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) {
        parse_register_terminal(g, S(OPS[i]));
    }

    check("declaring it makes it one token, longest match first",
          count_tokens(g, "a <<= b") == 3);
    check("and the same holds for two-character operators",
          count_tokens(g, "a->b == c") == 5);
    check("a keyword is not split out of a longer identifier",
          count_tokens(g, "ax->b") == 3);

    parse_grammar_free(g);
}

int
main(void)
{
    check("the default tokenizer is always registered",
          scan_tokenizer_for(S(SCAN_TOKENIZER_DEFAULT)) != nullptr);
    check("an unregistered name resolves to nothing",
          scan_tokenizer_for(S("nope")) == nullptr);

    size_t before = scan_tokenizer_count();

    check("a tokenizer can be registered by name",
          scan_tokenizer_register(S("shouty"), scan_shouty));
    check("and shows up in the registry",
          scan_tokenizer_count() == before + 1
              && scan_tokenizer_for(S("shouty")) == scan_shouty);

    check("registering the same name again replaces it, not duplicates it",
          scan_tokenizer_register(S("shouty"), scan_shouty)
              && scan_tokenizer_count() == before + 1);

    parse_grammar_t *plain = parse_grammar_new();

    check("a grammar with no @tokenizer uses the default",
          core_string_eq(scan_grammar_tokenizer(plain),
                        S(SCAN_TOKENIZER_DEFAULT)));
    check("and resolves to the default's function",
          scan_grammar_tokenizer_fn(plain)
              == scan_tokenizer_for(S(SCAN_TOKENIZER_DEFAULT)));

    check("the default tokenizer splits identifiers and punctuation",
          count_tokens(plain, "a + bb;") == 4);

    parse_grammar_free(plain);

    static const char *const G = "@tokenizer(\"shouty\")\n"
                                 "<top> ::= %IDENTIFIER\n";

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    check("a grammar naming a tokenizer loads",
          parse_bnf_load(S(G), S("top"), g));

    check("@tokenizer sets the grammar's tokenizer",
          core_string_eq(scan_grammar_tokenizer(g), S("shouty")));
    check("and it resolves through the registry",
          scan_grammar_tokenizer_fn(g) == scan_shouty);

    shouty_calls = 0;

    check("tokenizing through the grammar uses the named tokenizer",
          count_tokens(g, "AAA BBB") == 2 && shouty_calls > 0);

    parse_grammar_free(g);

    static const char *const MISSING = "@tokenizer(\"absent\")\n"
                                       "<top> ::= %IDENTIFIER\n";

    parse_grammar_t *m = parse_grammar_new();

    parse_grammar_set_error_recovery(m, false);
    parse_bnf_load(S(MISSING), S("top"), m);

    check("a grammar naming an unregistered tokenizer resolves to nothing",
          scan_grammar_tokenizer_fn(m) == nullptr);
    check("and tokenizing with it yields no tokens rather than crashing",
          count_tokens(m, "x") == 0);

    parse_grammar_free(m);

    test_the_builtin_tokenizers();
    test_the_grammar_is_the_operator_table();

    return report("fe tokenizer");
}
