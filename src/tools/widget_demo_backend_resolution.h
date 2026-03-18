#pragma once

#include <stdbool.h>

#include "n00b.h"
#include "display/render/backend.h"

typedef struct {
    const n00b_renderer_vtable_t *vtable;
    bool                          uses_terminal_io;
    const char                   *canonical_name;
} n00b_widget_demo_backend_resolution_t;

extern bool n00b_widget_demo_resolve_backend(
    const char                             *requested_name,
    n00b_widget_demo_backend_resolution_t  *out);
