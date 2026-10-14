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

int
main(void)
{
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
