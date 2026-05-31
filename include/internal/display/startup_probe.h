#pragma once

#include "n00b.h"
#include "display/render/backend_registry.h"
#include "display/render/canvas.h"

typedef struct n00b_display_startup_probe_t {
    bool        startup_ok;
    const char *selected_backend;
    bool        fallback_used;
} n00b_display_startup_probe_t;

extern void n00b_display_set_backend_override(const char *value);

extern n00b_display_startup_probe_t
n00b_display_probe_startup(const char *requested_backend) _kargs
{
    bool allow_fallback = true;
    bool allow_env_override = false;
    n00b_conduit_topic_t(n00b_buffer_t *) *output = nullptr;
};
