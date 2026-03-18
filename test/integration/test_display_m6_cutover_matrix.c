#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "internal/display/startup_probe.h"

static void
restore_backend_override(char *saved)
{
    if (saved) {
        n00b_display_set_backend_override(saved);
    }
    else {
        n00b_display_set_backend_override(nullptr);
    }
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
    n00b_display_startup_probe_t probe =
        n00b_display_probe_startup(requested_backend,
                                   .allow_fallback = allow_fallback,
                                   .allow_env_override = allow_env_override,
                                   .output = output);
    return (startup_result_t){
        .startup_ok = probe.startup_ok,
        .selected_backend = probe.selected_backend,
        .fallback_used = probe.fallback_used,
    };
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
    const char *saved_raw = getenv("N00B_RENDERER_BACKEND");
    char *saved = nullptr;
    if (saved_raw) {
        saved = malloc(strlen(saved_raw) + 1);
        assert(saved != nullptr);
        strcpy(saved, saved_raw);
    }

    n00b_display_set_backend_override("stream");

    startup_result_t result = run_startup_case("auto", true, true, output);
    assert(result.startup_ok);
    assert(strcmp(result.selected_backend, "stream") == 0);
    assert(!result.fallback_used);

    restore_backend_override(saved);
    free(saved);
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
