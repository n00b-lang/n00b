#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct n00b_cocoa_bridge_layout_t {
    uint32_t abi_version;
    uint32_t text_style_size;
    uint32_t text_style_fg_rgb_off;
    uint32_t text_style_bg_rgb_off;
    uint32_t text_style_font_size_off;
    uint32_t rcell_size;
    uint32_t rcell_style_off;
    uint32_t rcell_grapheme_len_off;
    uint32_t rcell_display_width_off;
    uint32_t event_size;
    uint32_t event_key_off;
    uint32_t event_mouse_off;
    uint32_t renderer_vtable_size;
    uint32_t renderer_vtable_get_font_metrics_off;
    uint32_t renderer_vtable_poll_event_off;
} n00b_cocoa_bridge_layout_t;

extern n00b_cocoa_bridge_layout_t n00b_cocoa_bridge_layout_canonical(void);
extern n00b_cocoa_bridge_layout_t n00b_cocoa_bridge_layout_bridge(void);
extern bool n00b_cocoa_bridge_layout_match(const n00b_cocoa_bridge_layout_t *canonical,
                                            const n00b_cocoa_bridge_layout_t *bridge,
                                            const char                      **mismatch_field);
