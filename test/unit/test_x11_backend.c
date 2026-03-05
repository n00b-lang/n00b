#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/render/backend.h"

static void
test_registration(void)
{
    const n00b_renderer_vtable_t *vt = &n00b_renderer_x11;
    assert(vt != nullptr);
    assert(vt->name != nullptr);
    assert(strcmp(vt->name, "x11") == 0);
    assert(vt->version == N00B_RENDERER_ABI_VERSION);
    assert(vt->init != nullptr);
    assert(vt->destroy != nullptr);
    assert(vt->capabilities != nullptr);
    assert(vt->get_size != nullptr);
    assert(vt->render_frame != nullptr);
    assert(vt->flush != nullptr);
    assert(vt->render_planes != nullptr);
    printf("  [PASS] x11 registration\n");
}

static void
test_init_destroy(void)
{
    void *ctx = n00b_renderer_x11.init(nullptr);
    if (!ctx) {
        printf("  [SKIP] x11 init/destroy (no DISPLAY)\n");
        return;
    }

    n00b_render_cap_t caps = n00b_renderer_x11.capabilities(ctx);
    assert(caps & N00B_RCAP_MANAGES_TTY);
    assert(caps & N00B_RCAP_PIXEL_COORDS);

    n00b_render_size_t sz = n00b_renderer_x11.get_size(ctx);
    assert(sz.rows > 0);
    assert(sz.cols > 0);
    assert(sz.cell_pixel_w > 0);
    assert(sz.cell_pixel_h > 0);

    n00b_renderer_x11.destroy(ctx);
    printf("  [PASS] x11 init/destroy\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running x11 backend tests...\n");
    test_registration();
    test_init_destroy();

    printf("X11 backend tests done.\n");
    n00b_shutdown();
    return 0;
}
