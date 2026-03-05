#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/render/backend.h"
#include "display/render/backend_registry.h"

static void
test_runtime_init_registers_backends(void)
{
    n00b_option_t(n00b_renderer_vtable_ptr_t) stream =
        n00b_renderer_find(r"stream");
    n00b_option_t(n00b_renderer_vtable_ptr_t) ansi =
        n00b_renderer_find(r"ansi");
    n00b_option_t(n00b_renderer_vtable_ptr_t) dumb =
        n00b_renderer_find(r"dumb");

    assert(n00b_option_is_set(stream));
    assert(n00b_option_is_set(ansi));
    assert(n00b_option_is_set(dumb));

    printf("  [PASS] runtime init registers builtins\n");
}

static void
test_builtin_registry_entries(void)
{
    n00b_renderer_registry_init();

    n00b_option_t(n00b_renderer_vtable_ptr_t) stream =
        n00b_renderer_find(r"stream");
    n00b_option_t(n00b_renderer_vtable_ptr_t) ansi =
        n00b_renderer_find(r"ansi");
    n00b_option_t(n00b_renderer_vtable_ptr_t) dumb =
        n00b_renderer_find(r"dumb");

    assert(n00b_option_is_set(stream));
    assert(n00b_option_is_set(ansi));
    assert(n00b_option_is_set(dumb));

    assert(n00b_option_get(stream) == &n00b_renderer_stream);
    assert(n00b_option_get(ansi) == &n00b_renderer_ansi);
    assert(n00b_option_get(dumb) == &n00b_renderer_dumb);

#if defined(N00B_HAVE_X11)
    n00b_option_t(n00b_renderer_vtable_ptr_t) x11 =
        n00b_renderer_find(r"x11");
    assert(n00b_option_is_set(x11));
    assert(n00b_option_get(x11) == &n00b_renderer_x11);
#endif

    printf("  [PASS] backend registry builtins\n");
}

static void
test_gui_alias(void)
{
    n00b_renderer_registry_init();

    n00b_option_t(n00b_renderer_vtable_ptr_t) gui =
        n00b_renderer_find(r"gui");

#if defined(__APPLE__)
    assert(n00b_option_is_set(gui));
    assert(n00b_option_get(gui) == &n00b_renderer_cocoa);
    printf("  [PASS] gui alias -> cocoa\n");
#elif defined(N00B_HAVE_X11)
    assert(n00b_option_is_set(gui));
    assert(n00b_option_get(gui) == &n00b_renderer_x11);
    printf("  [PASS] gui alias -> x11\n");
#else
    assert(!n00b_option_is_set(gui));
    printf("  [PASS] gui alias unavailable without GUI backend\n");
#endif
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display backend-registry tests...\n");
    test_runtime_init_registers_backends();
    test_builtin_registry_entries();
    test_gui_alias();

    printf("Display backend-registry tests passed.\n");
    n00b_shutdown();
    return 0;
}
