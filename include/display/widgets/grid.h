/**
 * @file grid.h
 * @brief Grid widget: flow-based 2D container with spans and tracks.
 *
 * A grid is a container widget that places visible child planes into
 * rows and columns. Wave 1 supports equal-width columns, content-sized
 * `AUTO` tracks, proportional `FR` tracks, auto-fit columns, and
 * per-child column/row spans.
 */
#pragma once

#include "n00b.h"
#include "display/render/plane.h"
#include "display/render/types.h"
#include "display/widget.h"

typedef enum {
    N00B_GRID_SIZE_AUTO,
    N00B_GRID_SIZE_FIXED,
    N00B_GRID_SIZE_FR,
} n00b_grid_size_t;

typedef struct n00b_grid_track_t {
    n00b_grid_size_t type;
    int32_t          value;
    int32_t          min_px;
    int32_t          max_px;
} n00b_grid_track_t;

typedef struct n00b_grid_span_t {
    n00b_plane_t *child;
    int32_t       col_span;
    int32_t       row_span;
} n00b_grid_span_t;

typedef struct n00b_grid_t {
    int32_t            columns;
    int32_t            min_col_width;
    int32_t            max_col_width;
    int32_t            row_gap;
    int32_t            col_gap;
    n00b_padding_t     padding;
    n00b_grid_track_t *tracks;
    n00b_isize_t       track_count;
    n00b_grid_span_t  *spans;
    n00b_isize_t       span_count;
    n00b_isize_t       span_capacity;
} n00b_grid_t;

extern const n00b_widget_vtable_t n00b_widget_grid;

extern n00b_plane_t *
n00b_grid_new() _kargs {
    int32_t           columns       = 1;
    int32_t           min_col_width = 0;
    int32_t           max_col_width = 0;
    int32_t           row_gap       = 0;
    int32_t           col_gap       = 0;
    int32_t           gap           = 0;
    int32_t           pad_top       = 0;
    int32_t           pad_right     = 0;
    int32_t           pad_bottom    = 0;
    int32_t           pad_left      = 0;
    n00b_canvas_t    *canvas        = nullptr;
    n00b_allocator_t *allocator     = nullptr;
};

extern void n00b_grid_set_columns(n00b_plane_t *grid, int32_t columns);
extern void n00b_grid_set_tracks(n00b_plane_t              *grid,
                                 const n00b_grid_track_t   *tracks,
                                 n00b_isize_t               count);
extern void n00b_grid_set_auto_fit(n00b_plane_t *grid,
                                   int32_t       min_col_width,
                                   int32_t       max_col_width);
extern void n00b_grid_set_gap(n00b_plane_t *grid, int32_t gap);
extern void n00b_grid_set_row_gap(n00b_plane_t *grid, int32_t row_gap);
extern void n00b_grid_set_col_gap(n00b_plane_t *grid, int32_t col_gap);
extern void n00b_grid_set_span(n00b_plane_t *grid,
                               n00b_plane_t *child,
                               int32_t       col_span,
                               int32_t       row_span);
extern void n00b_grid_get_span(n00b_plane_t *grid,
                               n00b_plane_t *child,
                               int32_t      *col_span,
                               int32_t      *row_span);
