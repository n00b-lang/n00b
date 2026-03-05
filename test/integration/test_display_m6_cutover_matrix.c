#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/render/backend_registry.h"
#include "display/render/canvas.h"
#include "text/strings/string_ops.h"

static void
set_backend_override(const char *value)
{
#ifdef _WIN32
    _putenv_s("N00B_RENDERER_BACKEND", value ? value : "");
#else
    if (value) {
        setenv("N00B_RENDERER_BACKEND", value, 1);
    }
    else {
        unsetenv("N00B_RENDERER_BACKEND");
    }
#endif
}

static bool
backend_request_used_fallback(const char *requested_backend,
                              const char *selected_backend,
                              bool        allow_fallback,
                              bool        allow_env_override)
{
    if (!selected_backend || !selected_backend[0]) {
        return false;
    }

    n00b_string_t *requested = n00b_string_from_cstr(
        (requested_backend && requested_backend[0]) ? requested_backend : "auto");
    n00b_list_t(n00b_string_t *) candidates =
        n00b_renderer_candidate_names(requested,
                                      .allow_fallback     = allow_fallback,
                                      .allow_env_override = allow_env_override);

    if (candidates.len == 0) {
        return false;
    }

    n00b_string_t *primary_candidate = n00b_list_get(candidates, 0);
    n00b_result_t(n00b_renderer_vtable_ptr_t) primary_resolved =
        n00b_renderer_resolve_exact(primary_candidate, .allow_dynamic_load = false);

    if (n00b_result_is_ok(primary_resolved)) {
        const n00b_renderer_vtable_t *vtable = n00b_result_get(primary_resolved);
        if (vtable && vtable->name) {
            return strcmp(vtable->name, selected_backend) != 0;
        }
    }

    return !n00b_unicode_str_eq(primary_candidate,
                                n00b_string_from_cstr(selected_backend),
                                .case_sensitive = false);
}

typedef struct {
    bool        startup_ok;
    const char *selected_backend;
    bool        fallback_used;
} startup_result_t;

static startup_result_t
run_startup_case(const char                         *requested_backend,
                 bool                                allow_fallback,
                 bool                                allow_env_override,
                 n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    startup_result_t result = {
        .startup_ok        = false,
        .selected_backend  = "none",
        .fallback_used     = false,
    };

    n00b_canvas_t *canvas = n00b_alloc(n00b_canvas_t);
    n00b_canvas_init(canvas,
                     .backend_name               = n00b_string_from_cstr(requested_backend),
                     .backend_allow_fallback     = allow_fallback,
                     .backend_allow_dynamic_load = false,
                     .backend_allow_env_override = allow_env_override,
                     .output                     = output);

    result.startup_ok = canvas->backend_ctx != nullptr;
    if (result.startup_ok && canvas->vtable && canvas->vtable->name) {
        result.selected_backend = canvas->vtable->name;
        result.fallback_used = backend_request_used_fallback(requested_backend,
                                                             result.selected_backend,
                                                             allow_fallback,
                                                             allow_env_override);
    }

    n00b_canvas_destroy(canvas);
    return result;
}

static void
test_explicit_backend_startup(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    startup_result_t result = run_startup_case("stream", false, false, output);
    assert(result.startup_ok);
    assert(strcmp(result.selected_backend, "stream") == 0);
    assert(!result.fallback_used);
    printf("  [PASS] m6 explicit backend startup\n");
}

static void
test_auto_selection_with_env_override(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    set_backend_override("stream");

    startup_result_t result = run_startup_case("auto", true, true, output);
    assert(result.startup_ok);
    assert(strcmp(result.selected_backend, "stream") == 0);
    assert(!result.fallback_used);

    set_backend_override(nullptr);
    printf("  [PASS] m6 auto backend selection with env override\n");
}

static void
test_gui_request_behavior(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    startup_result_t strict = run_startup_case("gui", false, false, output);
    startup_result_t with_fallback = run_startup_case("gui", true, false, output);

    assert(with_fallback.startup_ok);
    assert(with_fallback.selected_backend[0] != '\0');

    if (strict.startup_ok) {
        assert(!strict.fallback_used);
        assert(!with_fallback.fallback_used);
        printf("  [PASS] m6 gui request selected '%s' without fallback\n",
               strict.selected_backend);
        return;
    }

    assert(with_fallback.fallback_used);
    printf("  [PASS] m6 gui request unavailable in environment, fallback selected '%s'\n",
           with_fallback.selected_backend);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    n00b_runtime_t *rt = n00b_get_runtime();
    auto *stdout_topic =
        (n00b_conduit_topic_t(n00b_buffer_t *) *)rt->stdout_topic;

    printf("Running display m6 cutover matrix integration test...\n");
    test_explicit_backend_startup(stdout_topic);
    test_auto_selection_with_env_override(stdout_topic);
    test_gui_request_behavior(stdout_topic);
    printf("Display m6 cutover matrix integration test passed.\n");

    n00b_shutdown();
    return 0;
}
