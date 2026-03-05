#include <stddef.h>

#include "display/render/cocoa_bridge.h"
#include "internal/display/cocoa_bridge_contracts.h"

n00b_cocoa_bridge_layout_t
n00b_cocoa_bridge_layout_bridge(void)
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
    };

    return out;
}
