#include <stdlib.h>

#include "n00b.h"
#include "internal/display/startup_probe.h"

void
n00b_display_set_backend_override(const char *value)
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

n00b_display_startup_probe_t
n00b_display_probe_startup(const char *requested_backend) _kargs
{
    bool allow_fallback = true;
    bool allow_env_override = false;
    n00b_conduit_topic_t(n00b_buffer_t *) *output = nullptr;
}
{
    n00b_display_startup_probe_t result = {
        .startup_ok = false,
        .selected_backend = "none",
        .fallback_used = false,
    };

    const char *requested_cstr = requested_backend && requested_backend[0]
                               ? requested_backend
                               : "auto";
    n00b_string_t *requested = n00b_string_from_cstr(requested_cstr);

    n00b_canvas_t *canvas = n00b_alloc(n00b_canvas_t);
    n00b_canvas_init(canvas,
                     .backend_name = requested,
                     .backend_allow_fallback = allow_fallback,
                     .backend_allow_dynamic_load = false,
                     .backend_allow_env_override = allow_env_override,
                     .output = output);

    result.startup_ok = n00b_canvas_backend_ready(canvas);
    if (result.startup_ok && canvas->vtable && canvas->vtable->name) {
        result.selected_backend = canvas->vtable->name;
        result.fallback_used =
            n00b_renderer_selection_uses_fallback(requested,
                                                  canvas->vtable,
                                                  .allow_fallback = allow_fallback,
                                                  .allow_dynamic_load = false,
                                                  .allow_env_override = allow_env_override);
    }

    n00b_canvas_destroy(canvas);
    return result;
}
