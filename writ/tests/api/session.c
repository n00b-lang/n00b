#include <stdio.h>
#include <string.h>

#include "writ/gen.h"

static int failures;

static void
expect(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

typedef struct {
    int         calls;
    writ_span_t span;
    char        code[8];
} sink_state_t;

static void
sink(void *context, const writ_diag_t *diag)
{
    sink_state_t *state = context;
    state->calls++;
    state->span = diag->primary.span;
    snprintf(state->code, sizeof(state->code), "%.*s", (int)diag->code.length, diag->code.data);
}

static writ_str_t
text(const char *value)
{
    return (writ_str_t){value, strlen(value)};
}

int
main(void)
{
    expect(writ_api_version() == WRIT_API_VERSION, "api version");

    writ_session_t *session = writ_session_new(nullptr);
    sink_state_t state = {0};
    writ_session_set_diag_sink(session, sink, &state);
    writ_unit_t *unit = writ_unit_new(session);
    writ_origin_t origin = {.host_path = text("api.writ")};
    expect(writ_unit_add_source(unit, WRIT_SOURCE_CONTRACT_FILE, origin,
                                text("library t\n\nfn f(x: c_bogus) -> c_int\n"), nullptr) == WRIT_OK,
           "parse succeeds");
    const writ_model_t *model = nullptr;
    writ_platform_t *platform = nullptr;
    expect(writ_platform_from_triple(session, writ_platform_host_triple(), &platform) == WRIT_OK, "host platform");
    expect(writ_resolve(unit, platform, &model) == WRIT_HAS_ERRORS && !model, "resolve reports errors");
    expect(state.calls == 1 && !strcmp(state.code, "E0102"), "sink receives the unknown-type diagnostic");
    expect(state.span.line == 3 && state.span.column == 9, "diagnostic location");
    expect(writ_diag_count(session) == 1, "diagnostic count");
    writ_str_t rendered;
    expect(writ_diag_render(session, writ_diag_at(session, 0), &rendered) == WRIT_OK
               && strstr(rendered.data, "api.writ:3:9"),
           "rendered diagnostic names the location");
    writ_session_free(session);

    session = writ_session_new(nullptr);
    unit = writ_unit_new(session);
    expect(writ_unit_add_file(unit, text("tests/skeleton/zlib.writ"), nullptr) == WRIT_OK, "skeleton parses");
    writ_platform_from_triple(session, writ_platform_host_triple(), &platform);
    expect(writ_resolve(unit, platform, &model) == WRIT_OK && model, "skeleton resolves");
    expect(writ_model_fn_count(model) == 3, "three functions");
    const writ_fn_view_t *compress2 = writ_model_fn_named(model, text("compress2"));
    expect(compress2 && compress2->slots_count == 5, "compress2 has five slots");
    const writ_plan_view_t *plan = writ_model_fn_plan(model, compress2);
    expect(plan && plan->items_count == 4 && plan->outcomes_count == 2, "compress2 plan shape");
    expect(plan && plan->struct_size == sizeof(writ_plan_view_t), "views carry their size");
    expect(writ_model_const_count(model) == 1 && writ_model_const_at(model, 0)->value == 0, "Z_OK is 0");
    writ_session_free(session);

    if (!failures) {
        printf("api ok\n");
    }
    return failures != 0;
}
