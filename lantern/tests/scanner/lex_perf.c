#define _XOPEN_SOURCE 700

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

static parse_grammar_t *
grammar(void)
{
    parse_grammar_t *g = parse_grammar_new();
    static const core_string_t names[] = {
        S("IDENTIFIER"), S("INTEGER"), S("FLOAT"), S("STRING"), {0},
    };

    for (size_t i = 0; names[i].data; i++) {
        parse_register_terminal(g, names[i]);
    }
    scan_grammar_set_lex(g, &COMPILED);
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
            g, core_buffer_string(source), nullptr, nullptr);
        parse_token_info_t *tok = nullptr;

        while ((tok = scan_stream_next(ts))) {
            result.tokens++;
            result.checksum = result.checksum * UINT64_C(1099511628211)
                            ^ (uint64_t)(uint32_t)parse_token_tid(tok);
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
    parse_grammar_t *compiled = grammar();
    core_buffer_t *small = source_of_size(1 << 20);
    core_buffer_t *large = source_of_size(2 << 20);

    run(compiled, small, 1);
    run_t compiled_small = run(compiled, small, 3);
    run_t compiled_large = run(compiled, large, 3);

    check("compiled lexer throughput scales linearly with source size",
          compiled_large.seconds < compiled_small.seconds * 2.8);
    printf("      compiled %.1f MiB/s, 2x scaling %.2fx\n",
           (3.0 * large->byte_len / (1024.0 * 1024.0))
               / compiled_large.seconds,
           compiled_large.seconds / compiled_small.seconds);

    core_buffer_free(small);
    core_buffer_free(large);
    parse_grammar_free(compiled);
    return report("lexer performance");
}
