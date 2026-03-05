#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/render/backend_registry.h"

static void
test_plugin_fixture_load(void)
{
    const char *search_path = getenv("N00B_RENDERER_PATH");
    assert(search_path && search_path[0]);

    n00b_option_t(n00b_renderer_vtable_ptr_t) before =
        n00b_renderer_find(r"m5_test");
    assert(!n00b_option_is_set(before));

    n00b_result_t(n00b_renderer_vtable_ptr_t) resolved =
        n00b_renderer_resolve_exact(r"m5_test",
                                    .allow_dynamic_load = true);
    assert(n00b_result_is_ok(resolved));

    const n00b_renderer_vtable_t *vt = n00b_result_get(resolved);
    assert(vt);
    assert(vt->name);
    assert(strcmp(vt->name, "m5_test") == 0);
    assert(vt->version == N00B_RENDERER_ABI_VERSION);

    n00b_option_t(n00b_renderer_vtable_ptr_t) after =
        n00b_renderer_find(r"m5_test");
    assert(n00b_option_is_set(after));
    assert(n00b_option_get(after) == vt);

    printf("  [PASS] backend plugin fixture load via N00B_RENDERER_PATH\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display backend-plugin tests...\n");
    test_plugin_fixture_load();
    printf("Display backend-plugin tests passed.\n");

    n00b_shutdown();
    return 0;
}
