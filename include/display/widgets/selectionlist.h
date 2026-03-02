/**
 * @file selectionlist.h
 * @brief Multi-select scrollable list with checkbox indicators per item.
 *
 * Combines list scrolling behavior with checkbox toggle glyphs.
 * Items are independently toggleable; Ctrl+A/D for select all/none.
 *
 * ### Usage
 *
 * ```c
 * n00b_string_t *labels[] = { str1, str2, str3 };
 * n00b_plane_t *sl = n00b_selectionlist_new(labels, 3,
 *                                            .on_change = my_handler);
 * n00b_selectionlist_toggle(sl, 0);
 * int count = n00b_selectionlist_selected_count(sl);
 * ```
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/backend.h"
#include "display/widget.h"
#include "display/widgets/checkbox.h"
#include "display/event.h"

// ====================================================================
// Callback type
// ====================================================================

typedef void (*n00b_sellist_cb_t)(n00b_plane_t *plane, void *data);

// ====================================================================
// Item struct
// ====================================================================

typedef struct n00b_sellist_item_t {
    n00b_string_t *label;
    bool           selected;
    void          *user_data;
} n00b_sellist_item_t;

// ====================================================================
// Selection list data
// ====================================================================

typedef struct n00b_selectionlist_t {
    n00b_sellist_item_t  *items;
    n00b_isize_t          count;
    n00b_isize_t          capacity;
    int                   cursor;        /**< Highlighted row. */
    int                   scroll_offset;
    n00b_sellist_cb_t     on_change;
    void                 *on_change_data;
    n00b_checkbox_glyphs_t glyphs;       /**< Reused from checkbox. */
} n00b_selectionlist_t;

// ====================================================================
// Vtable
// ====================================================================

extern const n00b_widget_vtable_t n00b_widget_selectionlist;

// ====================================================================
// Public API
// ====================================================================

/**
 * @brief Create a new selection list.
 *
 * @param labels Array of label strings.
 * @param count  Number of items.
 *
 * @kw on_change      Change callback.
 * @kw on_change_data User data for callback.
 * @kw indicator      Checkbox style for indicators.
 * @kw caps           Render capabilities for AUTO resolution.
 * @kw width          Width (0 = auto from longest item).
 * @kw height         Visible rows (default = item_count, scaled by line height).
 * @kw style          Text style.
 * @kw canvas         Canvas to attach for font metrics (nullptr = none).
 * @kw allocator      Allocator.
 */
extern n00b_plane_t *
n00b_selectionlist_new(n00b_string_t **labels,
                       n00b_isize_t    count) _kargs {
    n00b_sellist_cb_t          on_change      = nullptr;
    void                      *on_change_data = nullptr;
    n00b_checkbox_indicator_t  indicator      = N00B_CB_STYLE_AUTO;
    n00b_render_cap_t          caps           = N00B_RCAP_UNICODE;
    int32_t                    width          = 0;
    int32_t                    height         = 0;
    n00b_text_style_t         *style          = nullptr;
    n00b_canvas_t             *canvas         = nullptr;
    n00b_allocator_t          *allocator      = nullptr;
};

/**
 * @brief Toggle an item's selected state by index.
 */
extern void n00b_selectionlist_toggle(n00b_plane_t *plane, int index);

/**
 * @brief Select all items.
 */
extern void n00b_selectionlist_select_all(n00b_plane_t *plane);

/**
 * @brief Deselect all items.
 */
extern void n00b_selectionlist_select_none(n00b_plane_t *plane);

/**
 * @brief Check if a specific item is selected.
 */
extern bool n00b_selectionlist_is_selected(n00b_plane_t *plane, int index);

/**
 * @brief Get count of selected items.
 */
extern int n00b_selectionlist_selected_count(n00b_plane_t *plane);

/**
 * @brief Add an item.
 */
extern void n00b_selectionlist_add_item(n00b_plane_t  *plane,
                                         n00b_string_t *label,
                                         void          *user_data);

/**
 * @brief Remove all items.
 */
extern void n00b_selectionlist_clear(n00b_plane_t *plane);
