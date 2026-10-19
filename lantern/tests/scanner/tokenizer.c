#include <stdio.h>
#include <string.h>

#include "lib/alloc.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "scanner/scan_builtins.h"
#include "scanner/lex.h"
#include "scanner/tokenizer.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

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

#define count_tokens(g, src) count_tokens_in((g), S(src))

static size_t
count_tokens_in(parse_grammar_t *g, core_string_t src)
{
    core_buffer_t       *buf = nullptr;
    parse_scanner_t      *sc  = nullptr;
    parse_token_stream_t *ts  = scan_tokenize(g, src, &buf, &sc);
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
test_the_grammar_is_the_operator_table(void)
{
    parse_grammar_t *bare = parse_grammar_new();


    check("an operator the grammar never declares is not one token",
          count_tokens(bare, "a <<= b") == 5);
    parse_grammar_free(bare);

    parse_grammar_t *g = parse_grammar_new();


    static const core_string_t OPS[] = {S("<<="), S("->"), S("=="), S("<"),
                                        S("="),   S("-"),  S(">")};

    for (size_t i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) {
        parse_register_terminal(g, OPS[i]);
    }

    check("declaring it makes it one token, longest match first",
          count_tokens(g, "a <<= b") == 3);
    check("and the same holds for two-character operators",
          count_tokens(g, "a->b == c") == 5);
    check("a keyword is not split out of a longer identifier",
          count_tokens(g, "ax->b") == 3);

    parse_grammar_free(g);
}

static void
test_stream_token_to_trivia(void)
{
    parse_grammar_t *g = parse_grammar_new();
    parse_register_terminal(g, S("+"));

    core_buffer_t *buf = nullptr;
    parse_scanner_t *scanner = nullptr;
    parse_token_stream_t *stream = scan_tokenize(g, S("a + b"),
                                                  &buf, &scanner);
    parse_token_info_t *first = scan_stream_get(stream, 0);
    parse_token_info_t *middle = scan_stream_get(stream, 1);
    parse_token_info_t *last = scan_stream_get(stream, 2);
    bool neighbors_before_edit =
        scan_stream_previous_token(stream, middle) == first
        && scan_stream_previous_token(stream, last) == middle;
    bool converted = scan_stream_token_to_trivia(stream, 1);
    parse_token_info_t *next = scan_stream_get(stream, 1);
    bool neighbor_after_edit =
        scan_stream_previous_token(stream, next) == first;
    parse_trivia_t *trivia = next ? parse_token_leading_trivia(next) : nullptr;

    check("a stream token can be converted to trivia",
          converted && scan_stream_token_count(stream) == 2
              && last && next == last);
    check("token-neighbor lookup survives stable-index gaps",
          neighbors_before_edit && neighbor_after_edit
              && next
              && parse_token_index(next) > parse_token_index(first) + 1);
    check("token conversion preserves the token spelling as trivia",
          trivia && core_string_eq(trivia->text, S("+")));

    scan_token_stream_free(stream);
    scan_scanner_free(scanner);
    core_buffer_free(buf);
    parse_grammar_free(g);
}

int
main(void)
{
    check("the default tokenizer is available",
          scan_tokenizer_for(S(SCAN_TOKENIZER_DEFAULT)) != nullptr);
    check("an unregistered name resolves to nothing",
          scan_tokenizer_for(S("nope")) == nullptr);

    check("a tokenizer can be registered by name",
          scan_tokenizer_register(S("shouty"), scan_shouty));
    check("and resolves from the registry",
          scan_tokenizer_for(S("shouty")) == scan_shouty);

    check("registering the same name again replaces it",
          scan_tokenizer_register(S("shouty"), scan_lex_scan)
              && scan_tokenizer_for(S("shouty")) == scan_lex_scan
              && scan_tokenizer_register(S("shouty"), scan_shouty));

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

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    check("a grammar naming a tokenizer loads",
          parse_bnf_load(S("@tokenizer(\"shouty\")\n"
                           "<top> ::= %IDENTIFIER\n"),
                         S("top"), g));

    check("@tokenizer sets the grammar's tokenizer",
          core_string_eq(scan_grammar_tokenizer(g), S("shouty")));
    check("and it resolves through the registry",
          scan_grammar_tokenizer_fn(g) == scan_shouty);

    shouty_calls = 0;

    check("tokenizing through the grammar uses the named tokenizer",
          count_tokens(g, "AAA BBB") == 2 && shouty_calls > 0);

    parse_grammar_free(g);

    parse_grammar_t *m = parse_grammar_new();

    parse_grammar_set_error_recovery(m, false);
    parse_bnf_load(S("@tokenizer(\"absent\")\n"
                     "<top> ::= %IDENTIFIER\n"),
                   S("top"), m);

    check("a grammar naming an unregistered tokenizer resolves to nothing",
          scan_grammar_tokenizer_fn(m) == nullptr);
    check("and tokenizing with it yields no tokens rather than crashing",
          count_tokens(m, "x") == 0);

    parse_grammar_free(m);

    test_the_grammar_is_the_operator_table();
    test_stream_token_to_trivia();

    return report("fe tokenizer");
}
