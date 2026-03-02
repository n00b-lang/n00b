/**
 * @file list_widget.h
 * @brief Scrollable single-selection list widget.
 *
 * Named `list_widget` to avoid collision with the generic
 * `n00b_list_t` container ADT.
 *
 * ### Usage
 *
 * ```c
 * n00b_string_t *items[] = { str1, str2, str3 };
 * n00b_plane_t *lst = n00b_list_widget_new(items, 3,
 *                                           .on_select = my_handler);
 * int sel = n00b_list_widget_get_selected(lst);
 * ```
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/event.h"

// ====================================================================
// Callback type
// ====================================================================

typedef void (*n00b_list_widget_cb_t)(n00b_plane_t *plane, int index, void *data);

// ====================================================================
// List data
// ====================================================================

typedef struct n00b_list_widget_t {
    n00b_string_t       **items;
    n00b_isize_t          item_count;
    n00b_isize_t          item_capacity;
    int                   selected;       /**< -1 = none. */
    int                   scroll_offset;  /**< First visible item index. */
    n00b_list_widget_cb_t on_select;
    void                 *on_select_data;
} n00b_list_widget_t;

// ====================================================================
// Vtable
// ====================================================================

extern const n00b_widget_vtable_t n00b_widget_list;

// ====================================================================
// Public API
// ====================================================================

/**
 * @brief Create a new list widget.
 *
 * @param items Array of string pointers (copied by reference).
 * @param count Number of items.
 *
 * @kw selected       Initial selected index (-1 = none).
 * @kw on_select      Selection callback (fires on Enter/Space).
 * @kw on_select_data User data for callback.
 * @kw width          Width (0 = auto from longest item).
 * @kw height         Visible rows (default = item_count, scaled by line height).
 * @kw style          Text style.
 * @kw canvas         Canvas to attach for font metrics (nullptr = none).
 * @kw allocator      Allocator.
 */
extern n00b_plane_t *
n00b_list_widget_new(n00b_string_t **items,
                     n00b_isize_t    count) _kargs {
    int                   selected       = -1;
    n00b_list_widget_cb_t on_select      = nullptr;
    void                 *on_select_data = nullptr;
    int32_t               width          = 0;
    int32_t               height         = 0;
    n00b_text_style_t    *style          = nullptr;
    n00b_canvas_t        *canvas         = nullptr;
    n00b_allocator_t     *allocator      = nullptr;
};

/**
 * @brief Get the currently selected index (-1 = none).
 */
extern int n00b_list_widget_get_selected(n00b_plane_t *plane);

/**
 * @brief Set the selected index programmatically.
 */
extern void n00b_list_widget_set_selected(n00b_plane_t *plane, int index);

/**
 * @brief Add an item to the end of the list.
 */
extern void n00b_list_widget_add_item(n00b_plane_t *plane, n00b_string_t *item);

/**
 * @brief Remove all items.
 */
extern void n00b_list_widget_clear(n00b_plane_t *plane);

/**
 * @brief Get the item count.
 */
extern n00b_isize_t n00b_list_widget_count(n00b_plane_t *plane);
