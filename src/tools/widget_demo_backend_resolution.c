#include <string.h>

#include "n00b.h"
#include "display/render/backend_registry.h"
#include "widget_demo_backend_resolution.h"

static bool
widget_demo_lookup_backend(const char *name,
                           bool        uses_terminal_io,
                           n00b_widget_demo_backend_resolution_t *out)
{
    if (!name || !out) {
        return false;
    }

    n00b_option_t(n00b_renderer_vtable_ptr_t) found =
        n00b_renderer_find(n00b_string_from_cstr(name));

    if (!n00b_option_is_set(found)) {
        return false;
    }

    out->vtable = n00b_option_get(found);
    out->uses_terminal_io = uses_terminal_io;
    out->canonical_name = name;
    return true;
}

bool
n00b_widget_demo_resolve_backend(const char                            *requested_name,
                                 n00b_widget_demo_backend_resolution_t *out)
{
    if (!requested_name || !out) {
        return false;
    }

    n00b_renderer_registry_init();
    *out = (n00b_widget_demo_backend_resolution_t){0};

    if (strcmp(requested_name, "tui") == 0) {
        return widget_demo_lookup_backend("ansi", true, out);
    }

    if (strcmp(requested_name, "nc") == 0) {
        return widget_demo_lookup_backend("notcurses", true, out);
    }

    if (strcmp(requested_name, "notcurses") == 0) {
        return widget_demo_lookup_backend("notcurses", true, out);
    }

    return widget_demo_lookup_backend(requested_name, false, out);
}
