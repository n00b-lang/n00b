/**
 * @file tabs.h
 * @brief Tabs widget: focusable multi-page container with a header strip.
 *
 * A tabs widget hosts zero or more named content planes and shows one
 * page at a time. The inactive pages stay parented but hidden so their
 * internal widget state survives repeated tab switches.
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/widget.h"

typedef enum {
    N00B_TABS_TOP,
    N00B_TABS_BOTTOM,
} n00b_tabs_position_t;

typedef struct n00b_tab_entry_t {
    n00b_string_t *name;
    n00b_plane_t  *content;
} n00b_tab_entry_t;

typedef void (*n00b_tabs_select_cb_t)(n00b_plane_t *tabs,
                                      int           new_index,
                                      int           old_index,
                                      void         *data);

typedef struct n00b_tabs_t {
    n00b_tab_entry_t      *entries;
    n00b_rect_t           *header_rects;
    n00b_isize_t           count;
    n00b_isize_t           capacity;
    int32_t                selected_index;
    n00b_tabs_position_t   position;
    n00b_string_t         *separator;
    n00b_tabs_select_cb_t  on_select;
    void                  *on_select_data;
    int32_t                header_height_px;
} n00b_tabs_t;

extern const n00b_widget_vtable_t n00b_widget_tabs;

extern n00b_plane_t *
n00b_tabs_new() _kargs {
    n00b_tabs_position_t  position       = N00B_TABS_TOP;
    n00b_string_t        *separator      = nullptr;
    n00b_tabs_select_cb_t on_select      = nullptr;
    void                 *on_select_data = nullptr;
    n00b_canvas_t        *canvas         = nullptr;
    n00b_allocator_t     *allocator      = nullptr;
};

extern int               n00b_tabs_add(n00b_plane_t *tabs, n00b_string_t *name, n00b_plane_t *content);
extern bool              n00b_tabs_remove(n00b_plane_t *tabs, int index);
extern bool              n00b_tabs_select_index(n00b_plane_t *tabs, int index);
extern int               n00b_tabs_selected_index(n00b_plane_t *tabs);
extern int               n00b_tabs_count(n00b_plane_t *tabs);
extern n00b_tab_entry_t *n00b_tabs_get(n00b_plane_t *tabs, int index);
extern bool              n00b_tabs_next(n00b_plane_t *tabs);
extern bool              n00b_tabs_prev(n00b_plane_t *tabs);
