/**
 * @file tabs.h
 * @brief Tabs widget: focusable multi-page container with a header strip.
 *
 * A tabs widget hosts zero or more named content planes and shows one
 * page at a time. The inactive pages stay parented but hidden so their
 * internal widget state survives repeated tab switches. Content planes
 * may be widget-backed children or plain `n00b_plane_t` instances.
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

/**
 * @brief Create a new tabs widget.
 *
 * The returned plane is focusable and owns only its internal tabs
 * bookkeeping. Added content planes remain caller-owned.
 */
extern n00b_plane_t *
n00b_tabs_new() _kargs {
    n00b_tabs_position_t  position       = N00B_TABS_TOP;
    n00b_string_t        *separator      = nullptr;
    n00b_tabs_select_cb_t on_select      = nullptr;
    void                 *on_select_data = nullptr;
    n00b_canvas_t        *canvas         = nullptr;
    n00b_allocator_t     *allocator      = nullptr;
};

/**
 * @brief Append a named content page.
 *
 * Plain planes and widget-backed planes are both supported.
 *
 * @return The new zero-based tab index, or `-1` on invalid input.
 */
extern int               n00b_tabs_add(n00b_plane_t *tabs, n00b_string_t *name, n00b_plane_t *content);
/**
 * @brief Remove a tab without destroying its content plane.
 */
extern bool              n00b_tabs_remove(n00b_plane_t *tabs, int index);
/**
 * @brief Select a tab by zero-based index.
 *
 * Switching hides the previously selected page and, if that page owns
 * mouse capture, cancels capture before the page becomes hidden.
 */
extern bool              n00b_tabs_select_index(n00b_plane_t *tabs, int index);
/**
 * @brief Return the selected tab index, or `-1` when none exists.
 */
extern int               n00b_tabs_selected_index(n00b_plane_t *tabs);
/**
 * @brief Return the number of tab entries.
 */
extern int               n00b_tabs_count(n00b_plane_t *tabs);
/**
 * @brief Return a pointer to one internal tab entry.
 *
 * The returned pointer becomes invalid after the next add/remove
 * operation on the same tabs widget.
 */
extern n00b_tab_entry_t *n00b_tabs_get(n00b_plane_t *tabs, int index);
/**
 * @brief Select the next tab with wrap-around.
 */
extern bool              n00b_tabs_next(n00b_plane_t *tabs);
/**
 * @brief Select the previous tab with wrap-around.
 */
extern bool              n00b_tabs_prev(n00b_plane_t *tabs);
