/**
 * @file text.h
 * @brief Wrapped rich-text widget with selection and clipboard copy.
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/types.h"
#include "display/widget.h"

typedef struct n00b_text_selection_t {
    int32_t start_line;
    int32_t start_col;
    int32_t end_line;
    int32_t end_col;
    bool    active;
} n00b_text_selection_t;

typedef struct n00b_text_t {
    n00b_string_t        *text;
    n00b_alignment_t      alignment;
    bool                  wrap;
    int32_t               hang_indent_cols;
    bool                  selectable;
    bool                  copy_on_release;
    n00b_text_selection_t selection;
    int32_t               wrapped_line_count;
    int32_t               cached_wrap_cols;
} n00b_text_t;

extern const n00b_widget_vtable_t n00b_widget_text;

extern n00b_plane_t *
n00b_text_new(n00b_string_t *text) _kargs {
    n00b_alignment_t  alignment        = N00B_ALIGN_LEFT;
    bool              wrap             = true;
    int32_t           hang_indent_cols = 0;
    bool              selectable       = false;
    bool              copy_on_release  = true;
    n00b_canvas_t    *canvas           = nullptr;
    n00b_allocator_t *allocator        = nullptr;
};

extern void           n00b_text_set_text(n00b_plane_t *text_plane, n00b_string_t *text);
extern n00b_string_t *n00b_text_get_text(n00b_plane_t *text_plane);
extern void           n00b_text_set_alignment(n00b_plane_t *text_plane, n00b_alignment_t alignment);
extern void           n00b_text_set_hang_indent(n00b_plane_t *text_plane, int32_t hang_indent_cols);
extern void           n00b_text_set_selectable(n00b_plane_t *text_plane, bool selectable);
extern bool           n00b_text_has_selection(n00b_plane_t *text_plane);
extern void           n00b_text_clear_selection(n00b_plane_t *text_plane);
extern n00b_string_t *n00b_text_get_selection(n00b_plane_t *text_plane);
extern bool           n00b_text_copy_selection(n00b_plane_t *text_plane);
extern int32_t        n00b_text_get_wrapped_line_count(n00b_plane_t *text_plane);
