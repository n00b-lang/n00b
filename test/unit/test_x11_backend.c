#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

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
    assert(vt->clipboard_copy != nullptr);
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
    assert(n00b_renderer_x11.clipboard_copy(ctx, "Athens", 6));

    n00b_renderer_x11.destroy(ctx);
    printf("  [PASS] x11 init/destroy\n");
}

static void
test_clipboard_copy_round_trip(void)
{
    void *ctx = n00b_renderer_x11.init(nullptr);
    if (!ctx) {
        printf("  [SKIP] x11 clipboard copy round trip (no DISPLAY)\n");
        return;
    }

    Display       *probe = XOpenDisplay(NULL);
    Window         requestor;
    Atom           clipboard;
    Atom           utf8_string;
    Atom           property;
    bool           got_notify = false;
    bool           got_data = false;
    n00b_event_t   ev = {};

    if (!probe) {
        n00b_renderer_x11.destroy(ctx);
        printf("  [SKIP] x11 clipboard copy round trip (probe display unavailable)\n");
        return;
    }

    requestor = XCreateSimpleWindow(probe,
                                    RootWindow(probe, DefaultScreen(probe)),
                                    0,
                                    0,
                                    1,
                                    1,
                                    0,
                                    0,
                                    0);
    clipboard   = XInternAtom(probe, "CLIPBOARD", False);
    utf8_string = XInternAtom(probe, "UTF8_STRING", False);
    property    = XInternAtom(probe, "N00B_TEST_CLIPBOARD", False);

    assert(n00b_renderer_x11.clipboard_copy(ctx, "Athens", 6));

    XConvertSelection(probe,
                      clipboard,
                      utf8_string,
                      property,
                      requestor,
                      CurrentTime);
    XFlush(probe);

    for (int attempt = 0; attempt < 100 && !got_notify; attempt++) {
        (void)n00b_renderer_x11.poll_event(ctx, 0, &ev);

        while (XPending(probe) > 0) {
            XEvent xev;
            XNextEvent(probe, &xev);

            if (xev.type != SelectionNotify) {
                continue;
            }

            got_notify = true;
            assert(xev.xselection.property != None);

            Atom           actual_type = None;
            int            actual_format = 0;
            unsigned long  nitems = 0;
            unsigned long  bytes_after = 0;
            unsigned char *buf = NULL;

            assert(XGetWindowProperty(probe,
                                      requestor,
                                      property,
                                      0,
                                      1024,
                                      False,
                                      AnyPropertyType,
                                      &actual_type,
                                      &actual_format,
                                      &nitems,
                                      &bytes_after,
                                      &buf) == Success);
            assert(actual_type == utf8_string);
            assert(actual_format == 8);
            assert(nitems == 6);
            assert(buf != NULL);
            assert(strncmp((const char *)buf, "Athens", 6) == 0);
            XFree(buf);
            got_data = true;
            break;
        }

        if (!got_notify) {
            usleep(10 * 1000);
        }
    }

    assert(got_notify);
    assert(got_data);

    XDestroyWindow(probe, requestor);
    XCloseDisplay(probe);
    n00b_renderer_x11.destroy(ctx);
    printf("  [PASS] x11 clipboard copy round trip\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running x11 backend tests...\n");
    test_registration();
    test_init_destroy();
    test_clipboard_copy_round_trip();

    printf("X11 backend tests done.\n");
    n00b_shutdown();
    return 0;
}
