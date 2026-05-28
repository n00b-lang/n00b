#include <stddef.h>

#include "n00b.h"
#include "display/event.h"
#include "display/render/backend.h"
#include "display/render/cell.h"
#include "text/strings/text_style.h"
#include "internal/display/cocoa_bridge_contracts.h"

n00b_cocoa_bridge_layout_t
n00b_cocoa_bridge_layout_canonical(void)
{
    n00b_cocoa_bridge_layout_t out = {
        .abi_version               = N00B_RENDERER_ABI_VERSION,
        .text_style_size           = (uint32_t)sizeof(n00b_text_style_t),
        .text_style_fg_rgb_off     = (uint32_t)offsetof(n00b_text_style_t, fg_rgb),
        .text_style_bg_rgb_off     = (uint32_t)offsetof(n00b_text_style_t, bg_rgb),
        .text_style_font_size_off  = (uint32_t)offsetof(n00b_text_style_t, font_size),
        .rcell_size                = (uint32_t)sizeof(n00b_rcell_t),
        .rcell_style_off           = (uint32_t)offsetof(n00b_rcell_t, style),
        .rcell_grapheme_len_off    = (uint32_t)offsetof(n00b_rcell_t, grapheme_len),
        .rcell_display_width_off   = (uint32_t)offsetof(n00b_rcell_t, display_width),
        .event_size                = (uint32_t)sizeof(n00b_event_t),
        .event_key_off             = (uint32_t)offsetof(n00b_event_t, key),
        .event_mouse_off           = (uint32_t)offsetof(n00b_event_t, mouse),
        .renderer_vtable_size      = (uint32_t)sizeof(n00b_renderer_vtable_t),
        .renderer_vtable_get_font_metrics_off =
            (uint32_t)offsetof(n00b_renderer_vtable_t, get_font_metrics),
        .renderer_vtable_poll_event_off =
            (uint32_t)offsetof(n00b_renderer_vtable_t, poll_event),
    };

    return out;
}

bool
n00b_cocoa_bridge_layout_match(const n00b_cocoa_bridge_layout_t *canonical,
                                const n00b_cocoa_bridge_layout_t *bridge,
                                const char                      **mismatch_field)
{
    if (!canonical || !bridge) {
        if (mismatch_field) {
            *mismatch_field = "null_layout";
        }
        return false;
    }

#define N00B_COCOA_LAYOUT_CHECK(field)                      \
    do {                                                    \
        if (canonical->field != bridge->field) {            \
            if (mismatch_field) {                           \
                *mismatch_field = #field;                   \
            }                                               \
            return false;                                   \
        }                                                   \
    } while (0)

    N00B_COCOA_LAYOUT_CHECK(abi_version);
    N00B_COCOA_LAYOUT_CHECK(text_style_size);
    N00B_COCOA_LAYOUT_CHECK(text_style_fg_rgb_off);
    N00B_COCOA_LAYOUT_CHECK(text_style_bg_rgb_off);
    N00B_COCOA_LAYOUT_CHECK(text_style_font_size_off);
    N00B_COCOA_LAYOUT_CHECK(rcell_size);
    N00B_COCOA_LAYOUT_CHECK(rcell_style_off);
    N00B_COCOA_LAYOUT_CHECK(rcell_grapheme_len_off);
    N00B_COCOA_LAYOUT_CHECK(rcell_display_width_off);
    N00B_COCOA_LAYOUT_CHECK(event_size);
    N00B_COCOA_LAYOUT_CHECK(event_key_off);
    N00B_COCOA_LAYOUT_CHECK(event_mouse_off);
    N00B_COCOA_LAYOUT_CHECK(renderer_vtable_size);
    N00B_COCOA_LAYOUT_CHECK(renderer_vtable_get_font_metrics_off);
    N00B_COCOA_LAYOUT_CHECK(renderer_vtable_poll_event_off);

#undef N00B_COCOA_LAYOUT_CHECK

    if (mismatch_field) {
        *mismatch_field = nullptr;
    }

    return true;
}
