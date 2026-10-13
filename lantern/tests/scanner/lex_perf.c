#include <stdio.h>
#include <string.h>
#include <time.h>

#include "parse/grammar.h"
#include "scanner/lex.h"
#include "scanner/scan_builtins.h"
#include "scanner/tokenizer.h"
#include "scanner/token_stream.h"
#include "lib/buffer.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static const scan_lex_comment_t COMMENTS[] = {
    {.kind = SCAN_LEX_LINE, .opener = S("//")},
    {},
};

static const scan_lex_string_t STRINGS[] = {
    {.opener = S("\""), .closer = S("\""), .escape = S("\\"),
     .tok = S("STRING")},
    {},
};

static const scan_lex_number_form_t NUMBERS[] = {
    {.digits = S("0123456789"), .tok_int = S("INTEGER"),
     .tok_float = S("FLOAT"), .flags = SCAN_LEX_NUM_POINT},
    {},
};

static const scan_lex_spec_t COMPILED = {
    .name = S("perf-compiled"),
    .comments = COMMENTS,
    .strings = STRINGS,
    .number_forms = NUMBERS,
    .tok_ident = S("IDENTIFIER"),
    .brackets = S("()[]{}"),
};

static bool
hand_scan(parse_scanner_t *s)
{
restart:
    scan_skip_whitespace(s);
    if (scan_at_eof(s)) {
        return false;
    }

    if (scan_peek_byte(s, 0) == '/' && scan_peek_byte(s, 1) == '/') {
        scan_mark(s);
        scan_skip_line_comment(s);
        goto restart;
    }

    scan_mark(s);
    core_codepoint_t c = scan_peek(s, 0);

    if (c == '"') {
        scan_emit_owned(s, scan_terminal_id(s, S("STRING")),
                        scan_string_double(s));
        return true;
    }

    if (unicode_cp_is_ascii_digit(c)) {
        if (scan_number(s, scan_terminal_id(s, S("INTEGER")),
                       scan_terminal_id(s, S("FLOAT")))) {
            scan_prev_token(s)->owns_value = true;
            return true;
        }
    }

    if (unicode_is_id_start(c)) {
        scan_emit_owned(s, scan_terminal_id(s, S("IDENTIFIER")),
                        scan_identifier(s));
        return true;
    }

    scan_advance(s);
    scan_emit_marked(s, (int32_t)c);
    return true;
}

static parse_grammar_t *
grammar(bool compiled)
{
    parse_grammar_t *g = parse_grammar_new();
    static const char *const names[] = {
        "IDENTIFIER", "INTEGER", "FLOAT", "STRING", nullptr,
    };

    for (size_t i = 0; names[i]; i++) {
        parse_register_terminal(g, core_string_from_cstr(names[i]));
    }

    if (compiled) {
        scan_grammar_set_lex(g, &COMPILED);
    }
    else {
        scan_tokenizer_register(S("perf-handwritten"), hand_scan);
        scan_grammar_set_tokenizer(g, S("perf-handwritten"));
    }

    return g;
}

static core_buffer_t *
source_of_size(size_t bytes)
{
    static const char LINE[] =
        "const alpha123 = 12345 + beta_2; // ordinary comment\n"
        "\"hello\\nworld\"; gamma = 3.14159;\n";
    core_buffer_t *source = core_buffer_empty();

    while (source->byte_len < bytes) {
        core_buffer_append(source, LINE, sizeof(LINE) - 1);
    }
    return source;
}

typedef struct {
    double seconds;
    size_t tokens;
    uint64_t checksum;
} run_t;

static double
elapsed(struct timespec a, struct timespec b)
{
    return (double)(b.tv_sec - a.tv_sec)
         + (double)(b.tv_nsec - a.tv_nsec) / 1000000000.0;
}

static run_t
run(parse_grammar_t *g, core_buffer_t *source, size_t repetitions)
{
    struct timespec begin;
    struct timespec end;
    run_t result = {0};

    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (size_t i = 0; i < repetitions; i++) {
        parse_token_stream_t *ts = scan_tokenize(
            g, source->data, source->byte_len, nullptr, nullptr);
        parse_token_info_t *tok = nullptr;

        while ((tok = scan_stream_next(ts))) {
            result.tokens++;
            result.checksum = result.checksum * UINT64_C(1099511628211)
                            ^ (uint64_t)(uint32_t)tok->tid;
        }
        scan_token_stream_free(ts);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    result.seconds = elapsed(begin, end);
    return result;
}

int
main(void)
{
    parse_grammar_t *compiled = grammar(true);
    parse_grammar_t *handwritten = grammar(false);
    core_buffer_t *small = source_of_size(1 << 20);
    core_buffer_t *large = source_of_size(2 << 20);

    run_t warm_compiled = run(compiled, small, 1);
    run_t warm_hand = run(handwritten, small, 1);
    run_t compiled_small = run(compiled, small, 3);
    run_t compiled_large = run(compiled, large, 3);
    run_t hand_large = run(handwritten, large, 3);

    check("compiled and handwritten lexers produce the same token count",
          warm_compiled.tokens == warm_hand.tokens);
    check("compiled and handwritten lexers produce the same token kinds",
          warm_compiled.checksum == warm_hand.checksum);
    check("compiled lexer throughput scales linearly with source size",
          compiled_large.seconds < compiled_small.seconds * 2.8);
    check("compiled dispatch stays within 4x of a direct handwritten scanner",
          compiled_large.seconds < hand_large.seconds * 4.0);
    printf("      compiled %.1f MiB/s, handwritten %.1f MiB/s, "
           "ratio %.2fx, 2x scaling %.2fx\n",
           (3.0 * large->byte_len / (1024.0 * 1024.0))
               / compiled_large.seconds,
           (3.0 * large->byte_len / (1024.0 * 1024.0)) / hand_large.seconds,
           compiled_large.seconds / hand_large.seconds,
           compiled_large.seconds / compiled_small.seconds);

    core_buffer_free(small);
    core_buffer_free(large);
    parse_grammar_free(compiled);
    parse_grammar_free(handwritten);
    return report("lexer performance");
}
