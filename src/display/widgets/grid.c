/*
 * Grid widget: span-aware 2D flow layout in pixel space.
 */

#include <limits.h>

#include "n00b.h"
#include "core/alloc.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/grid.h"
#include "internal/display/widget_primitives.h"

typedef struct {
    n00b_plane_t *child;
    int32_t       pref_w;
    int32_t       pref_h;
    int32_t       min_w;
    int32_t       min_h;
    int32_t       col;
    int32_t       row;
    int32_t       col_span;
    int32_t       row_span;
} grid_item_t;

static int32_t
grid_clamp_non_negative(int32_t value)
{
    return value < 0 ? 0 : value;
}

static int32_t
grid_ceil_div_i32(int32_t value, int32_t divisor)
{
    if (divisor <= 0 || value <= 0) {
        return 0;
    }

    return 1 + (value - 1) / divisor;
}

static int32_t
grid_track_fr_units(const n00b_grid_track_t *track)
{
    if (!track || track->type != N00B_GRID_SIZE_FR) {
        return 1;
    }

    return track->value > 0 ? track->value : 1;
}

static int32_t
grid_clamp_track_width(const n00b_grid_track_t *track, int32_t width)
{
    if (!track) {
        return width;
    }

    if (track->min_px > 0 && width < track->min_px) {
        width = track->min_px;
    }
    if (track->max_px > 0 && width > track->max_px) {
        width = track->max_px;
    }

    return width;
}

static int32_t
grid_total_gap(int32_t count, int32_t gap)
{
    if (count <= 1) {
        return 0;
    }

    return (count - 1) * gap;
}

static int32_t
grid_span_extent(const int32_t *sizes,
                 int32_t        size_count,
                 int32_t        start,
                 int32_t        span,
                 int32_t        gap)
{
    int32_t extent = 0;

    if (!sizes || size_count <= 0 || start < 0 || span <= 0) {
        return 0;
    }

    for (int32_t i = 0; i < span && (start + i) < size_count; i++) {
        extent += sizes[start + i];
        if (i > 0) {
            extent += gap;
        }
    }

    return extent;
}

static int32_t
grid_sum_sizes(const int32_t *sizes, int32_t count)
{
    int32_t total = 0;

    if (!sizes || count <= 0) {
        return 0;
    }

    for (int32_t i = 0; i < count; i++) {
        total += sizes[i];
    }

    return total;
}

static n00b_grid_t *
grid_data(n00b_plane_t *plane)
{
    return n00b_widget_data_if_kind(plane, &n00b_widget_grid);
}

static void
grid_clear_tracks(n00b_grid_t *data)
{
    if (!data) {
        return;
    }

    if (data->tracks) {
        n00b_free(data->tracks);
    }
    data->tracks = nullptr;
    data->track_count = 0;
}

static void
grid_copy_tracks(n00b_grid_t            *data,
                 const n00b_grid_track_t *tracks,
                 n00b_isize_t             count)
{
    if (!data) {
        return;
    }

    grid_clear_tracks(data);

    if (!tracks || count == 0 || count > (n00b_isize_t)INT32_MAX) {
        return;
    }

    data->tracks = n00b_alloc_array(n00b_grid_track_t, count);
    memcpy(data->tracks, tracks, (size_t)count * sizeof(n00b_grid_track_t));
    data->track_count = count;
}

static void
grid_prune_stale_spans(n00b_plane_t *grid, n00b_grid_t *data)
{
    if (!grid || !data || !data->spans || data->span_count == 0) {
        return;
    }

    n00b_isize_t write_ix = 0;

    for (n00b_isize_t i = 0; i < data->span_count; i++) {
        n00b_grid_span_t span = data->spans[i];
        if (!span.child || span.child->parent != grid) {
            continue;
        }

        if (write_ix != i) {
            data->spans[write_ix] = span;
        }
        write_ix++;
    }

    data->span_count = write_ix;
}

static void
grid_child_span(n00b_grid_t  *data,
                n00b_plane_t *child,
                int32_t      *col_span,
                int32_t      *row_span)
{
    int32_t cols = 1;
    int32_t rows = 1;

    if (data && data->spans && child) {
        for (n00b_isize_t i = 0; i < data->span_count; i++) {
            if (data->spans[i].child == child) {
                cols = data->spans[i].col_span;
                rows = data->spans[i].row_span;
                break;
            }
        }
    }

    if (cols < 1) {
        cols = 1;
    }
    if (rows < 1) {
        rows = 1;
    }

    if (col_span) {
        *col_span = cols;
    }
    if (row_span) {
        *row_span = rows;
    }
}

static void
grid_collect_visible_children(n00b_plane_t  *grid,
                              n00b_grid_t   *data,
                              grid_item_t  **out_items,
                              n00b_isize_t  *out_count)
{
    *out_items = nullptr;
    *out_count = 0;

    if (!grid || !data || !grid->children.data || grid->children.len == 0) {
        return;
    }

    grid_prune_stale_spans(grid, data);

    n00b_isize_t visible_count = 0;
    for (size_t i = 0; i < grid->children.len; i++) {
        n00b_plane_t *child = grid->children.data[i];
        if (child && (child->flags & N00B_PLANE_VISIBLE)) {
            visible_count++;
        }
    }

    if (visible_count == 0) {
        return;
    }

    grid_item_t *items = n00b_alloc_array(grid_item_t, visible_count);
    n00b_isize_t write_ix = 0;

    for (size_t i = 0; i < grid->children.len; i++) {
        n00b_plane_t *child = grid->children.data[i];
        if (!child || !(child->flags & N00B_PLANE_VISIBLE)) {
            continue;
        }

        grid_item_t *item = &items[write_ix++];
        item->child = child;
        if (child->widget_vtable && child->widget_vtable->measure) {
            n00b_widget_measure(child,
                                &item->pref_w,
                                &item->pref_h,
                                &item->min_w,
                                &item->min_h);
        }
        else {
            n00b_widget_measure_plain_plane(child,
                                            &item->pref_w,
                                            &item->pref_h,
                                            &item->min_w,
                                            &item->min_h);
        }
        grid_child_span(data, child, &item->col_span, &item->row_span);
    }

    *out_items = items;
    *out_count = visible_count;
}

static n00b_isize_t
grid_resolve_layout_columns(const n00b_grid_t *data,
                            int32_t            available_width,
                            n00b_isize_t       visible_child_count)
{
    if (!data) {
        return 1;
    }

    if (data->track_count > 0) {
        return data->track_count > 0 ? data->track_count : 1;
    }

    if (data->min_col_width <= 0) {
        return data->columns > 0 ? (n00b_isize_t)data->columns : 1;
    }

    if (visible_child_count == 0) {
        return 1;
    }

    int32_t min_with_gap = data->min_col_width + data->col_gap;
    int32_t cols = 1;

    if (min_with_gap > 0) {
        cols = (available_width + data->col_gap) / min_with_gap;
        if (cols < 1) {
            cols = 1;
        }
    }

    if ((n00b_isize_t)cols > visible_child_count) {
        cols = (int32_t)visible_child_count;
    }
    if (cols < 1) {
        cols = 1;
    }

    if (data->max_col_width > 0) {
        while ((n00b_isize_t)cols < visible_child_count) {
            int32_t total_gap = grid_total_gap(cols, data->col_gap);
            int32_t remaining = available_width - total_gap;
            int32_t base = 0;
            int32_t remainder = 0;
            int32_t widest = 0;

            if (remaining > 0) {
                base = remaining / cols;
                remainder = remaining % cols;
            }

            widest = base + (remainder > 0 ? 1 : 0);
            if (widest <= data->max_col_width) {
                break;
            }

            cols++;
        }
    }

    return cols > 0 ? (n00b_isize_t)cols : 1;
}

static void
grid_build_placements(grid_item_t  *items,
                      n00b_isize_t  item_count,
                      int32_t       col_count,
                      int32_t      *out_row_count)
{
    int32_t row_capacity = 8;
    bool   *occupied = nullptr;
    int32_t current_row = 0;
    int32_t current_col = 0;
    int32_t max_row_used = 0;

    *out_row_count = 0;

    if (!items || item_count == 0 || col_count <= 0) {
        return;
    }

    occupied = n00b_alloc_array(bool, (size_t)row_capacity * (size_t)col_count);

    for (n00b_isize_t i = 0; i < item_count; i++) {
        grid_item_t *item = &items[i];

        if (item->col_span < 1) {
            item->col_span = 1;
        }
        if (item->col_span > col_count) {
            item->col_span = col_count;
        }
        if (item->row_span < 1) {
            item->row_span = 1;
        }

        for (;;) {
            if ((current_row + item->row_span) > row_capacity) {
                int32_t new_capacity = row_capacity;
                while ((current_row + item->row_span) > new_capacity) {
                    new_capacity *= 2;
                }

                bool *new_occupied =
                    n00b_alloc_array(bool, (size_t)new_capacity * (size_t)col_count);
                memcpy(new_occupied,
                       occupied,
                       (size_t)row_capacity * (size_t)col_count * sizeof(bool));
                n00b_free(occupied);
                occupied = new_occupied;
                row_capacity = new_capacity;
            }

            bool fits = true;

            if ((current_col + item->col_span) > col_count) {
                fits = false;
            }
            else {
                for (int32_t r = 0; r < item->row_span && fits; r++) {
                    for (int32_t c = 0; c < item->col_span; c++) {
                        int32_t ix = (current_row + r) * col_count + (current_col + c);
                        if (occupied[ix]) {
                            fits = false;
                            break;
                        }
                    }
                }
            }

            if (fits) {
                item->col = current_col;
                item->row = current_row;

                for (int32_t r = 0; r < item->row_span; r++) {
                    for (int32_t c = 0; c < item->col_span; c++) {
                        int32_t ix = (current_row + r) * col_count + (current_col + c);
                        occupied[ix] = true;
                    }
                }

                if ((current_row + item->row_span) > max_row_used) {
                    max_row_used = current_row + item->row_span;
                }

                current_col += item->col_span;
                if (current_col >= col_count) {
                    current_col = 0;
                    current_row++;
                }
                break;
            }

            current_col++;
            if (current_col >= col_count) {
                current_col = 0;
                current_row++;
            }
        }
    }

    n00b_free(occupied);
    *out_row_count = max_row_used;
}

static void
grid_compute_column_contributions(const grid_item_t *items,
                                  n00b_isize_t       item_count,
                                  int32_t            col_count,
                                  int32_t            col_gap,
                                  bool               use_min_sizes,
                                  int32_t           *out_contribs)
{
    if (!out_contribs || col_count <= 0) {
        return;
    }

    for (int32_t i = 0; i < col_count; i++) {
        out_contribs[i] = 0;
    }

    if (!items || item_count == 0) {
        return;
    }

    for (n00b_isize_t i = 0; i < item_count; i++) {
        const grid_item_t *item = &items[i];
        int32_t width = use_min_sizes ? item->min_w : item->pref_w;
        int32_t internal_gap = grid_total_gap(item->col_span, col_gap);
        int32_t adjusted = width - internal_gap;
        int32_t per_track = 0;

        if (adjusted < 0) {
            adjusted = 0;
        }

        per_track = grid_ceil_div_i32(adjusted, item->col_span);

        for (int32_t c = 0; c < item->col_span && (item->col + c) < col_count; c++) {
            int32_t track_ix = item->col + c;
            if (per_track > out_contribs[track_ix]) {
                out_contribs[track_ix] = per_track;
            }
        }
    }
}

static void
grid_resolve_column_widths(const n00b_grid_t *data,
                           int32_t            col_count,
                           int32_t            available_width,
                           const int32_t     *pref_contribs,
                           const int32_t     *min_contribs,
                           int32_t           *widths)
{
    if (!data || !widths || col_count <= 0) {
        return;
    }

    if (available_width < 0) {
        available_width = 0;
    }

    if (data->track_count == 0) {
        int32_t total_gap = grid_total_gap(col_count, data->col_gap);
        int32_t remaining = available_width - total_gap;
        int32_t base = 0;
        int32_t remainder = 0;

        if (remaining < 0) {
            remaining = 0;
        }

        if (col_count > 0) {
            base = remaining / col_count;
            remainder = remaining % col_count;
        }

        for (int32_t i = 0; i < col_count; i++) {
            widths[i] = base + (i < remainder ? 1 : 0);
        }
        return;
    }

    int32_t total_gap = grid_total_gap(col_count, data->col_gap);
    int32_t track_space = available_width - total_gap;
    int32_t fixed_total = 0;
    int32_t auto_total = 0;
    int32_t total_fr_units = 0;
    int32_t *auto_mins = n00b_alloc_array(int32_t, col_count);

    if (track_space < 0) {
        track_space = 0;
    }

    for (int32_t i = 0; i < col_count; i++) {
        const n00b_grid_track_t *track = &data->tracks[i];
        int32_t pref = pref_contribs ? pref_contribs[i] : 0;
        int32_t min = min_contribs ? min_contribs[i] : 0;

        widths[i] = 0;
        auto_mins[i] = 0;

        switch (track->type) {
        case N00B_GRID_SIZE_FIXED:
            widths[i] = grid_clamp_track_width(track, track->value);
            fixed_total += widths[i];
            break;
        case N00B_GRID_SIZE_AUTO:
            widths[i] = grid_clamp_track_width(track, pref);
            auto_mins[i] = grid_clamp_track_width(track, min);
            auto_total += widths[i];
            break;
        case N00B_GRID_SIZE_FR:
        default:
            total_fr_units += grid_track_fr_units(track);
            break;
        }
    }

    int32_t remaining = track_space - fixed_total - auto_total;

    if (remaining < 0) {
        int32_t overflow = -remaining;

        for (int32_t i = 0; i < col_count && overflow > 0; i++) {
            const n00b_grid_track_t *track = &data->tracks[i];
            if (track->type != N00B_GRID_SIZE_AUTO) {
                continue;
            }

            int32_t reducible = widths[i] - auto_mins[i];
            if (reducible <= 0) {
                continue;
            }

            int32_t shrink = reducible < overflow ? reducible : overflow;
            widths[i] -= shrink;
            overflow -= shrink;
        }

        remaining = track_space - fixed_total;
        for (int32_t i = 0; i < col_count; i++) {
            if (data->tracks[i].type == N00B_GRID_SIZE_AUTO) {
                remaining -= widths[i];
            }
        }

        if (remaining < 0) {
            remaining = 0;
        }
    }

    if (remaining > 0 && total_fr_units > 0) {
        int32_t assigned = 0;
        int32_t fr_remainder = 0;

        for (int32_t i = 0; i < col_count; i++) {
            const n00b_grid_track_t *track = &data->tracks[i];
            if (track->type != N00B_GRID_SIZE_FR) {
                continue;
            }

            int32_t units = grid_track_fr_units(track);
            int32_t width = (remaining * units) / total_fr_units;
            widths[i] = grid_clamp_track_width(track, width);
            assigned += width;
        }

        fr_remainder = remaining - assigned;
        for (int32_t i = 0; i < col_count && fr_remainder > 0; i++) {
            const n00b_grid_track_t *track = &data->tracks[i];
            if (track->type != N00B_GRID_SIZE_FR) {
                continue;
            }

            widths[i]++;
            widths[i] = grid_clamp_track_width(track, widths[i]);
            fr_remainder--;
        }
    }

    n00b_free(auto_mins);
}

static void
grid_resolve_row_heights(const grid_item_t *items,
                         n00b_isize_t       item_count,
                         int32_t            row_count,
                         int32_t            row_gap,
                         bool               use_min_sizes,
                         int32_t           *row_heights)
{
    if (!row_heights || row_count <= 0) {
        return;
    }

    for (int32_t i = 0; i < row_count; i++) {
        row_heights[i] = 0;
    }

    if (!items || item_count == 0) {
        return;
    }

    for (n00b_isize_t i = 0; i < item_count; i++) {
        const grid_item_t *item = &items[i];
        int32_t height = use_min_sizes ? item->min_h : item->pref_h;
        int32_t internal_gap = grid_total_gap(item->row_span, row_gap);
        int32_t adjusted = height - internal_gap;
        int32_t per_row = 0;

        if (adjusted < 0) {
            adjusted = 0;
        }

        per_row = grid_ceil_div_i32(adjusted, item->row_span);

        for (int32_t r = 0; r < item->row_span && (item->row + r) < row_count; r++) {
            int32_t row_ix = item->row + r;
            if (per_row > row_heights[row_ix]) {
                row_heights[row_ix] = per_row;
            }
        }
    }
}

static void
grid_build_positions(int32_t start,
                     const int32_t *sizes,
                     int32_t count,
                     int32_t gap,
                     int32_t *positions)
{
    if (!positions || count <= 0) {
        return;
    }

    positions[0] = start;
    for (int32_t i = 1; i < count; i++) {
        positions[i] = positions[i - 1] + sizes[i - 1] + gap;
    }
}

static bool
grid_widths_meet_targets(const int32_t *widths,
                         const int32_t *targets,
                         int32_t        count)
{
    if (!widths || !targets) {
        return true;
    }

    for (int32_t i = 0; i < count; i++) {
        if (widths[i] < targets[i]) {
            return false;
        }
    }

    return true;
}

static void
grid_build_required_targets(const n00b_grid_t *data,
                            const int32_t     *target_contribs,
                            int32_t            col_count,
                            int32_t           *required_targets)
{
    for (int32_t i = 0; i < col_count; i++) {
        const n00b_grid_track_t *track = &data->tracks[i];
        int32_t required = target_contribs ? target_contribs[i] : 0;

        switch (track->type) {
        case N00B_GRID_SIZE_FIXED:
            required = grid_clamp_track_width(track, track->value);
            break;
        case N00B_GRID_SIZE_AUTO:
        case N00B_GRID_SIZE_FR:
        default:
            required = grid_clamp_track_width(track, required);
            break;
        }

        required_targets[i] = required;
    }
}

static int32_t
grid_measure_track_width(const n00b_grid_t *data,
                         int32_t            col_count,
                         const int32_t     *target_contribs,
                         const int32_t     *min_contribs)
{
    int32_t *widths = nullptr;
    int32_t *required_targets = nullptr;
    int64_t  low = 0;
    int64_t  high = 1;
    int64_t  best = 1;

    if (!data || data->track_count == 0 || col_count <= 0) {
        return 0;
    }

    widths = n00b_alloc_array(int32_t, col_count);
    required_targets = n00b_alloc_array(int32_t, col_count);
    grid_build_required_targets(data, target_contribs, col_count, required_targets);

    high = grid_total_gap(col_count, data->col_gap);
    for (int32_t i = 0; i < col_count; i++) {
        high += required_targets[i];
    }
    if (high < 1) {
        high = 1;
    }

    for (;;) {
        grid_resolve_column_widths(data,
                                   col_count,
                                   (int32_t)high,
                                   target_contribs,
                                   min_contribs,
                                   widths);
        if (grid_widths_meet_targets(widths, required_targets, col_count)) {
            break;
        }

        if (high >= (INT32_MAX / 2)) {
            high = INT32_MAX;
            break;
        }

        high *= 2;
    }

    best = high;
    while (low <= high) {
        int64_t mid = low + ((high - low) / 2);

        grid_resolve_column_widths(data,
                                   col_count,
                                   (int32_t)mid,
                                   target_contribs,
                                   min_contribs,
                                   widths);
        if (grid_widths_meet_targets(widths, required_targets, col_count)) {
            best = mid;
            high = mid - 1;
        }
        else {
            low = mid + 1;
        }
    }

    n00b_free(widths);
    n00b_free(required_targets);
    return (int32_t)best;
}

static void
grid_destroy(n00b_plane_t *plane, void *data)
{
    (void)plane;

    n00b_grid_t *grid = data;
    if (!grid) {
        return;
    }

    if (grid->tracks) {
        n00b_free(grid->tracks);
    }
    if (grid->spans) {
        n00b_free(grid->spans);
    }
    n00b_free(grid);
}

static void
grid_render(n00b_plane_t *plane, void *data)
{
    (void)data;
    n00b_plane_clear(plane);
}

static void
grid_measure(n00b_plane_t *plane, void *data,
             int32_t *pref_w, int32_t *pref_h,
             int32_t *min_w, int32_t *min_h)
{
    n00b_grid_t  *grid = data;
    grid_item_t  *items = nullptr;
    n00b_isize_t  item_count = 0;
    int32_t       pad_w = 0;
    int32_t       pad_h = 0;

    if (!grid) {
        *pref_w = *pref_h = *min_w = *min_h = 1;
        return;
    }

    pad_w = grid->padding.left + grid->padding.right;
    pad_h = grid->padding.top + grid->padding.bottom;

    grid_collect_visible_children(plane, grid, &items, &item_count);
    if (item_count == 0) {
        *pref_w = n00b_max(1, pad_w);
        *pref_h = n00b_max(1, pad_h);
        *min_w  = n00b_max(1, pad_w);
        *min_h  = n00b_max(1, pad_h);
        return;
    }

    if (grid->min_col_width > 0) {
        int32_t pref_col_width = grid->min_col_width;
        int32_t min_col_width = grid->min_col_width;
        int32_t row_count = 0;
        int32_t *pref_rows = nullptr;
        int32_t *min_rows = nullptr;

        /* Athens measure has no available-width hint, so auto-fit falls
         * back to a deterministic one-column natural size. */
        for (n00b_isize_t i = 0; i < item_count; i++) {
            if (items[i].pref_w > pref_col_width) {
                pref_col_width = items[i].pref_w;
            }
            if (items[i].min_w > min_col_width) {
                min_col_width = items[i].min_w;
            }
        }

        if (grid->max_col_width > 0) {
            if (pref_col_width > grid->max_col_width) {
                pref_col_width = grid->max_col_width;
            }
            if (min_col_width > grid->max_col_width) {
                min_col_width = grid->max_col_width;
            }
        }

        grid_build_placements(items, item_count, 1, &row_count);
        pref_rows = n00b_alloc_array(int32_t, row_count > 0 ? row_count : 1);
        min_rows = n00b_alloc_array(int32_t, row_count > 0 ? row_count : 1);
        grid_resolve_row_heights(items,
                                 item_count,
                                 row_count,
                                 grid->row_gap,
                                 false,
                                 pref_rows);
        grid_resolve_row_heights(items,
                                 item_count,
                                 row_count,
                                 grid->row_gap,
                                 true,
                                 min_rows);

        *pref_w = n00b_max(1, pref_col_width + pad_w);
        *min_w  = n00b_max(1, min_col_width + pad_w);
        *pref_h = n00b_max(1, grid_sum_sizes(pref_rows, row_count)
                              + grid_total_gap(row_count, grid->row_gap)
                              + pad_h);
        *min_h  = n00b_max(1, grid_sum_sizes(min_rows, row_count)
                              + grid_total_gap(row_count, grid->row_gap)
                              + pad_h);

        n00b_free(pref_rows);
        n00b_free(min_rows);
        n00b_free(items);
        return;
    }

    int32_t col_count = (grid->track_count > 0)
                      ? (int32_t)grid->track_count
                      : n00b_max(grid->columns, 1);
    int32_t row_count = 0;
    int32_t *pref_contribs = n00b_alloc_array(int32_t, col_count);
    int32_t *min_contribs = n00b_alloc_array(int32_t, col_count);
    int32_t *pref_rows = nullptr;
    int32_t *min_rows = nullptr;
    int32_t pref_content_w = 0;
    int32_t min_content_w = 0;

    grid_build_placements(items, item_count, col_count, &row_count);
    grid_compute_column_contributions(items,
                                      item_count,
                                      col_count,
                                      grid->col_gap,
                                      false,
                                      pref_contribs);
    grid_compute_column_contributions(items,
                                      item_count,
                                      col_count,
                                      grid->col_gap,
                                      true,
                                      min_contribs);

    if (grid->track_count > 0) {
        pref_content_w = grid_measure_track_width(grid,
                                                  col_count,
                                                  pref_contribs,
                                                  min_contribs);
        min_content_w = grid_measure_track_width(grid,
                                                 col_count,
                                                 min_contribs,
                                                 min_contribs);
    }
    else {
        int32_t widest_pref = 0;
        int32_t widest_min = 0;

        for (int32_t i = 0; i < col_count; i++) {
            if (pref_contribs[i] > widest_pref) {
                widest_pref = pref_contribs[i];
            }
            if (min_contribs[i] > widest_min) {
                widest_min = min_contribs[i];
            }
        }

        pref_content_w = widest_pref * col_count
                       + grid_total_gap(col_count, grid->col_gap);
        min_content_w = widest_min * col_count
                      + grid_total_gap(col_count, grid->col_gap);
    }

    pref_rows = n00b_alloc_array(int32_t, row_count > 0 ? row_count : 1);
    min_rows = n00b_alloc_array(int32_t, row_count > 0 ? row_count : 1);
    grid_resolve_row_heights(items,
                             item_count,
                             row_count,
                             grid->row_gap,
                             false,
                             pref_rows);
    grid_resolve_row_heights(items,
                             item_count,
                             row_count,
                             grid->row_gap,
                             true,
                             min_rows);

    *pref_w = n00b_max(1, pref_content_w + pad_w);
    *min_w  = n00b_max(1, min_content_w + pad_w);
    *pref_h = n00b_max(1, grid_sum_sizes(pref_rows, row_count)
                          + grid_total_gap(row_count, grid->row_gap)
                          + pad_h);
    *min_h  = n00b_max(1, grid_sum_sizes(min_rows, row_count)
                          + grid_total_gap(row_count, grid->row_gap)
                          + pad_h);

    n00b_free(pref_contribs);
    n00b_free(min_contribs);
    n00b_free(pref_rows);
    n00b_free(min_rows);
    n00b_free(items);
}

static bool
grid_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    (void)plane;
    (void)data;
    (void)event;

    return false;
}

static bool
grid_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;

    return false;
}

static void
grid_layout(n00b_plane_t *plane, void *data, n00b_rect_t bounds)
{
    n00b_grid_t  *grid = data;
    grid_item_t  *items = nullptr;
    n00b_isize_t  item_count = 0;
    n00b_rect_t   content = bounds;

    if (!plane || !grid) {
        return;
    }

    content.x += grid->padding.left;
    content.y += grid->padding.top;
    content.width -= (grid->padding.left + grid->padding.right);
    content.height -= (grid->padding.top + grid->padding.bottom);

    if (content.width < 0) {
        content.width = 0;
    }
    if (content.height < 0) {
        content.height = 0;
    }

    grid_collect_visible_children(plane, grid, &items, &item_count);
    if (item_count == 0) {
        return;
    }

    int32_t col_count = (int32_t)grid_resolve_layout_columns(grid,
                                                             content.width,
                                                             item_count);
    int32_t row_count = 0;
    int32_t *widths = n00b_alloc_array(int32_t, col_count);
    int32_t *rows = nullptr;
    int32_t *x_positions = nullptr;
    int32_t *y_positions = nullptr;
    int32_t *pref_contribs = nullptr;
    int32_t *min_contribs = nullptr;

    grid_build_placements(items, item_count, col_count, &row_count);

    if (grid->track_count > 0) {
        pref_contribs = n00b_alloc_array(int32_t, col_count);
        min_contribs = n00b_alloc_array(int32_t, col_count);
        grid_compute_column_contributions(items,
                                          item_count,
                                          col_count,
                                          grid->col_gap,
                                          false,
                                          pref_contribs);
        grid_compute_column_contributions(items,
                                          item_count,
                                          col_count,
                                          grid->col_gap,
                                          true,
                                          min_contribs);
    }

    grid_resolve_column_widths(grid,
                               col_count,
                               content.width,
                               pref_contribs,
                               min_contribs,
                               widths);

    rows = n00b_alloc_array(int32_t, row_count > 0 ? row_count : 1);
    x_positions = n00b_alloc_array(int32_t, col_count);
    y_positions = n00b_alloc_array(int32_t, row_count > 0 ? row_count : 1);

    grid_resolve_row_heights(items,
                             item_count,
                             row_count,
                             grid->row_gap,
                             false,
                             rows);
    grid_build_positions(content.x, widths, col_count, grid->col_gap, x_positions);
    grid_build_positions(content.y, rows, row_count, grid->row_gap, y_positions);

    for (n00b_isize_t i = 0; i < item_count; i++) {
        grid_item_t *item = &items[i];
        n00b_rect_t child_bounds = {
            .x = x_positions[item->col],
            .y = y_positions[item->row],
            .width = grid_span_extent(widths,
                                      col_count,
                                      item->col,
                                      item->col_span,
                                      grid->col_gap),
            .height = grid_span_extent(rows,
                                       row_count,
                                       item->row,
                                       item->row_span,
                                       grid->row_gap),
        };

        n00b_widget_layout(item->child, child_bounds);
    }

    if (pref_contribs) {
        n00b_free(pref_contribs);
    }
    if (min_contribs) {
        n00b_free(min_contribs);
    }
    n00b_free(widths);
    n00b_free(rows);
    n00b_free(x_positions);
    n00b_free(y_positions);
    n00b_free(items);
}

const n00b_widget_vtable_t n00b_widget_grid = {
    .kind         = "grid",
    .destroy      = grid_destroy,
    .render       = grid_render,
    .measure      = grid_measure,
    .handle_event = grid_handle_event,
    .can_focus    = grid_can_focus,
    .layout       = grid_layout,
};

n00b_plane_t *
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
}
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                         .canvas    = canvas,
                                         .allocator = allocator);
    n00b_grid_t *grid = n00b_alloc(n00b_grid_t);

    grid->columns       = columns > 0 ? columns : 1;
    grid->min_col_width = grid_clamp_non_negative(min_col_width);
    grid->max_col_width = grid_clamp_non_negative(max_col_width);
    grid->row_gap       = 0;
    grid->col_gap       = 0;
    grid->padding.top   = grid_clamp_non_negative(pad_top);
    grid->padding.right = grid_clamp_non_negative(pad_right);
    grid->padding.bottom = grid_clamp_non_negative(pad_bottom);
    grid->padding.left  = grid_clamp_non_negative(pad_left);

    if (gap > 0) {
        grid->row_gap = gap;
        grid->col_gap = gap;
    }
    if (row_gap > 0) {
        grid->row_gap = row_gap;
    }
    if (col_gap > 0) {
        grid->col_gap = col_gap;
    }

    n00b_widget_attach(plane, &n00b_widget_grid, grid);
    n00b_plane_mark_dirty(plane);

    return plane;
}

static bool
grid_has_valid_bounds(const n00b_plane_t *plane)
{
    return plane && plane->bounds.width > 0 && plane->bounds.height > 0;
}

static void
grid_relayout_or_mark_dirty(n00b_plane_t *grid_plane)
{
    if (!grid_plane) {
        return;
    }

    if (grid_has_valid_bounds(grid_plane)) {
        n00b_widget_layout(grid_plane, grid_plane->bounds);
    }

    n00b_plane_mark_dirty(grid_plane);
}

void
n00b_grid_set_columns(n00b_plane_t *grid_plane, int32_t columns)
{
    n00b_grid_t *grid = grid_data(grid_plane);
    if (!grid) {
        return;
    }

    grid->columns = columns > 0 ? columns : 1;
    grid_clear_tracks(grid);
    grid->min_col_width = 0;
    grid->max_col_width = 0;
    grid_relayout_or_mark_dirty(grid_plane);
}

void
n00b_grid_set_tracks(n00b_plane_t            *grid_plane,
                     const n00b_grid_track_t *tracks,
                     n00b_isize_t             count)
{
    n00b_grid_t *grid = grid_data(grid_plane);
    if (!grid) {
        return;
    }

    if (!tracks || count == 0 || count > (n00b_isize_t)INT32_MAX) {
        grid_clear_tracks(grid);
        grid->min_col_width = 0;
        grid->max_col_width = 0;
        grid_relayout_or_mark_dirty(grid_plane);
        return;
    }

    grid_copy_tracks(grid, tracks, count);
    grid->track_count = count;
    grid->columns = (int32_t)count;
    grid->min_col_width = 0;
    grid->max_col_width = 0;
    grid_relayout_or_mark_dirty(grid_plane);
}

void
n00b_grid_set_auto_fit(n00b_plane_t *grid_plane,
                       int32_t       min_col_width,
                       int32_t       max_col_width)
{
    n00b_grid_t *grid = grid_data(grid_plane);
    if (!grid) {
        return;
    }

    grid->min_col_width = grid_clamp_non_negative(min_col_width);
    grid->max_col_width = grid_clamp_non_negative(max_col_width);
    grid_clear_tracks(grid);
    grid_relayout_or_mark_dirty(grid_plane);
}

void
n00b_grid_set_gap(n00b_plane_t *grid_plane, int32_t gap)
{
    n00b_grid_t *grid = grid_data(grid_plane);
    if (!grid) {
        return;
    }

    gap = grid_clamp_non_negative(gap);
    grid->row_gap = gap;
    grid->col_gap = gap;
    grid_relayout_or_mark_dirty(grid_plane);
}

void
n00b_grid_set_row_gap(n00b_plane_t *grid_plane, int32_t row_gap)
{
    n00b_grid_t *grid = grid_data(grid_plane);
    if (!grid) {
        return;
    }

    grid->row_gap = grid_clamp_non_negative(row_gap);
    grid_relayout_or_mark_dirty(grid_plane);
}

void
n00b_grid_set_col_gap(n00b_plane_t *grid_plane, int32_t col_gap)
{
    n00b_grid_t *grid = grid_data(grid_plane);
    if (!grid) {
        return;
    }

    grid->col_gap = grid_clamp_non_negative(col_gap);
    grid_relayout_or_mark_dirty(grid_plane);
}

void
n00b_grid_set_span(n00b_plane_t *grid_plane,
                   n00b_plane_t *child,
                   int32_t       col_span,
                   int32_t       row_span)
{
    n00b_grid_t *grid = grid_data(grid_plane);
    if (!grid || !child || child->parent != grid_plane) {
        return;
    }

    if (col_span < 1) {
        col_span = 1;
    }
    if (row_span < 1) {
        row_span = 1;
    }

    for (n00b_isize_t i = 0; i < grid->span_count; i++) {
        if (grid->spans[i].child == child) {
            grid->spans[i].col_span = col_span;
            grid->spans[i].row_span = row_span;
            grid_relayout_or_mark_dirty(grid_plane);
            return;
        }
    }

    if (grid->span_count == grid->span_capacity) {
        n00b_isize_t new_capacity = grid->span_capacity == 0
                                  ? 4
                                  : grid->span_capacity * 2;
        n00b_grid_span_t *new_spans =
            n00b_alloc_array(n00b_grid_span_t, new_capacity);

        if (grid->spans && grid->span_count > 0) {
            memcpy(new_spans,
                   grid->spans,
                   (size_t)grid->span_count * sizeof(n00b_grid_span_t));
            n00b_free(grid->spans);
        }

        grid->spans = new_spans;
        grid->span_capacity = new_capacity;
    }

    grid->spans[grid->span_count++] = (n00b_grid_span_t){
        .child = child,
        .col_span = col_span,
        .row_span = row_span,
    };
    grid_relayout_or_mark_dirty(grid_plane);
}

void
n00b_grid_get_span(n00b_plane_t *grid_plane,
                   n00b_plane_t *child,
                   int32_t      *col_span,
                   int32_t      *row_span)
{
    n00b_grid_t *grid = grid_data(grid_plane);
    int32_t cols = 1;
    int32_t rows = 1;

    if (grid && child && child->parent == grid_plane) {
        grid_prune_stale_spans(grid_plane, grid);
        grid_child_span(grid, child, &cols, &rows);
    }

    if (col_span) {
        *col_span = cols;
    }
    if (row_span) {
        *row_span = rows;
    }
}
