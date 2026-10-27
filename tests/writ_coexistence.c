#include <stdio.h>
#include <string.h>

#include "parse/bnf.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
#include "scanner/lex.h"
#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"
#include "writ/model.h"

static int failures;

static void
expect(bool ok, core_string_t what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %.*s\n", (int)what.u8_bytes, what.data);
        failures++;
    }
}

static bool
parses_n00b(core_string_t text)
{
    parse_grammar_t *grammar = parse_grammar_new();
    parse_grammar_set_error_recovery(grammar, false);
    bool ok = parse_bnf_load_file(CORE_STRING_STATIC(N00BB_SOURCE_ROOT "/grammar/n00b.bnf"), CORE_STRING_STATIC("module"), grammar);
    if (ok) {
        parse_grammar_finalize(grammar);
        ok = scan_grammar_lex_compiled(grammar) != nullptr;
    }
    parse_token_stream_t *tokens = ok ? scan_tokenize_owned(grammar, text) : nullptr;
    parse_pwz_parser_t *parser = nullptr;
    if (tokens) {
        parse_nt_id_t start = parse_nonterm_id(parse_find_nonterm(grammar, CORE_STRING_STATIC("module")));
        parser = parse_pwz_new_for_start(grammar, start);
        ok = parse_pwz_parse(parser, tokens);
    }
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(grammar);
    return ok && tokens;
}

static bool
resolves_ffi_literal(void)
{
    writ_session_t *session = writ_session_new(nullptr);
    writ_unit_t *unit = writ_unit_new(session);
    static const char literal[] = "library demo\nfn demo_add(a: c_int, b: c_int) -> c_int\n";
    writ_origin_t origin = {.host_path = {"program.n", 10}, .first_line = 3, .first_column = 13};
    bool ok = writ_unit_add_embedded(unit, (writ_str_t){"n00b", 4}, origin, (writ_str_t){literal, sizeof(literal) - 1},
                                     nullptr) == WRIT_OK;
    writ_platform_t *platform = nullptr;
    const writ_model_t *model = nullptr;
    ok = ok && writ_platform_from_triple(session, writ_platform_host_triple(), &platform) == WRIT_OK;
    ok = ok && writ_resolve(unit, platform, &model) == WRIT_OK && writ_model_fn_count(model) == 1;
    writ_session_free(session);
    return ok;
}

int
main(void)
{
    expect(resolves_ffi_literal(), CORE_STRING_STATIC("libwrit resolves an 'ffi literal"));
    expect(parses_n00b(CORE_STRING_STATIC("func add(a: int, b: int) -> int { return a + b }\n")),
           CORE_STRING_STATIC("lantern parses n00b source"));
    expect(resolves_ffi_literal(), CORE_STRING_STATIC("libwrit still resolves after lantern parsed n00b"));
    if (!failures) {
        printf("coexistence ok\n");
    }
    return failures != 0;
}
