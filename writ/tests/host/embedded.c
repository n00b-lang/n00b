#include <stdio.h>
#include <string.h>

#include "writ/model.h"

static int failures;

static void
expect(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

static writ_str_t
text(const char *value)
{
    return (writ_str_t){value, strlen(value)};
}

static writ_status_t
resolve_embedded(writ_session_t *session, const char *declarations, const writ_model_t **model)
{
    writ_unit_t *unit = writ_unit_new(session);
    writ_origin_t file = {.host_path = text("contract.writ")};
    writ_unit_add_source(unit, WRIT_SOURCE_CONTRACT_FILE, file, text("library host\n"), nullptr);
    writ_origin_t block = {.host_path = text("program.n00b"), .first_line = 10, .first_column = 5, .first_byte = 200};
    writ_unit_add_source(unit, WRIT_SOURCE_CONTRACT_DECLARATIONS, block, text(declarations), nullptr);
    writ_platform_t *platform = nullptr;
    writ_platform_from_triple(session, writ_platform_host_triple(), &platform);
    return writ_resolve(unit, platform, model);
}

static bool
ends_with(writ_str_t value, const char *suffix)
{
    size_t length = strlen(suffix);
    return value.length >= length && !memcmp(value.data + value.length - length, suffix, length);
}

static void
check_literal(const char *source_root)
{
    char contracts[4096];
    char host[4096];
    snprintf(contracts, sizeof(contracts), "%s/contracts", source_root);
    snprintf(host, sizeof(host), "%s/tests/host/program.n00b", source_root);

    writ_session_t *session = writ_session_new(nullptr);
    writ_session_add_contract_dir(session, text(contracts));
    writ_unit_t *unit = writ_unit_new(session);
    writ_origin_t literal = {.host_path = text(host), .first_line = 20, .first_column = 9, .first_byte = 400};
    expect(writ_unit_add_embedded(unit, text("n00b"), literal, text("use <zlib.writ>\nbinding {\n}\n"), nullptr)
               == WRIT_OK,
           "an embedded literal parses");
    writ_platform_t *platform = nullptr;
    writ_platform_from_triple(session, text("aarch64-apple-darwin"), &platform);
    const writ_model_t *model = nullptr;
    expect(writ_resolve(unit, platform, &model) == WRIT_OK && model, "the literal resolves through the search path");
    expect(writ_unit_file_count(unit) == 2, "the unit lists the contract and its platform draft");
    expect(ends_with(writ_unit_file_at(unit, 0), "contracts/zlib.writ"), "the used contract is listed first");
    expect(ends_with(writ_unit_file_at(unit, 1), "contracts/zlib/aarch64-apple-darwin.writ"),
           "files used by a used file are listed");
    expect(!writ_unit_file_at(unit, 2).data, "the list ends");
    expect(writ_resolve(unit, platform, &model) == WRIT_OK, "resolving twice reuses the used files");
    expect(writ_unit_file_count(unit) == 2, "resolving twice reads nothing again");
    writ_session_free(session);

    session = writ_session_new(nullptr);
    unit = writ_unit_new(session);
    writ_unit_add_embedded(unit, text("n00b"), literal, text("use \"../../contracts/zlib.writ\"\nbinding }\n"),
                           nullptr);
    const writ_diag_t *syntax = writ_diag_at(session, 0);
    expect(syntax && syntax->primary.span.line == 21, "a syntax error maps to the host line");
    expect(syntax && !strncmp(syntax->path.data, host, syntax->path.length), "the error names the host file");
    writ_session_free(session);

    session = writ_session_new(nullptr);
    unit = writ_unit_new(session);
    writ_unit_add_embedded(unit, text("n00b"), literal, text("use \"../../contracts/zlib.writ\"\n"), nullptr);
    writ_platform_from_triple(session, text("aarch64-apple-darwin"), &platform);
    expect(writ_resolve(unit, platform, &model) == WRIT_OK, "a quoted use resolves against the host file");
    writ_session_free(session);
}

int
main(int argc, char **argv)
{
    if (argc > 1) {
        check_literal(argv[1]);
    }
    writ_session_t *session = writ_session_new(nullptr);
    const writ_model_t *model = nullptr;
    resolve_embedded(session, "fn f(x: c_nope)", &model);
    const writ_diag_t *first_line = writ_diag_at(session, 0);
    expect(first_line && first_line->primary.span.line == 10, "first-line error maps to the host line");
    expect(first_line && first_line->primary.span.column == 13, "first-line error column includes the block column");
    expect(first_line && first_line->primary.span.start_byte == 208, "byte offsets include the block offset");
    expect(first_line && !strncmp(first_line->path.data, "program.n00b", first_line->path.length),
           "diagnostics name the host file");
    writ_session_free(session);

    session = writ_session_new(nullptr);
    resolve_embedded(session, "fn f(x: c_int)\nfn g(y: c_bad)", &model);
    const writ_diag_t *second_line = writ_diag_at(session, 0);
    expect(second_line && second_line->primary.span.line == 11, "later lines map to host lines");
    expect(second_line && second_line->primary.span.column == 9, "later lines keep their own columns");
    writ_session_free(session);

    session = writ_session_new(nullptr);
    expect(resolve_embedded(session, "fn h(x: c_int) -> c_int", &model) == WRIT_OK && model,
           "embedded declarations join the file's library");
    expect(model && writ_model_fn_count(model) == 1, "one embedded function");
    writ_session_free(session);

    if (!failures) {
        printf("embedded ok\n");
    }
    return failures != 0;
}
