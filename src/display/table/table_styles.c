/*
 * Named style presets and convenience constructors for tables.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "display/render/box.h"
#include "display/table/table.h"
#include "text/strings/theme.h"
#include "text/strings/style_ops.h"

// ====================================================================
// Internal: palette-based style helpers
// ====================================================================

static n00b_text_style_t *
make_text_style(n00b_palette_ix_t fg_ix,
                n00b_palette_ix_t bg_ix,
                bool bold,
                n00b_text_case_t text_case)
{
    n00b_text_style_t *s = n00b_str_style_new();

    s->fg_palette_ix = fg_ix;
    s->bg_palette_ix = bg_ix;

    if (bold) {
        s->bold = N00B_TRI_YES;
    }

    s->text_case = text_case;
    return s;
}

static n00b_text_style_t *
make_fill_style(n00b_palette_ix_t bg_ix)
{
    n00b_text_style_t *s = n00b_str_style_new();

    s->fg_palette_ix = N00B_PAL_UNSET;
    s->bg_palette_ix = bg_ix;
    return s;
}

// ====================================================================
// Style presets
// ====================================================================

n00b_table_style_t
n00b_table_style_default(void)
{
    return (n00b_table_style_t){
        .table_props = n00b_box_props_new(
            .theme        = &n00b_border_rounded,
            .borders      = N00B_BORDER_ALL,
            .border_style = make_text_style(N00B_PAL_BORDER_LIGHT,
                                            N00B_PAL_SURFACE,
                                            false, N00B_TEXT_CASE_NONE)),
        .cell_props = n00b_box_props_new(
            .borders    = N00B_BORDER_NONE,
            .pad_left   = 1,
            .pad_right  = 1,
            .alignment  = N00B_ALIGN_TOP_LEFT,
            .text_style = make_text_style(N00B_PAL_TEXT_PRIMARY,
                                          N00B_PAL_UNSET,
                                          false, N00B_TEXT_CASE_NONE),
            .fill_style = make_fill_style(N00B_PAL_SURFACE)),
        .header_props = n00b_box_props_new(
            .borders    = N00B_BORDER_NONE,
            .pad_left   = 1,
            .pad_right  = 1,
            .alignment  = N00B_ALIGN_TOP_CENTER,
            .text_style = make_text_style(N00B_PAL_TEXT_INVERSE,
                                          N00B_PAL_UNSET,
                                          true, N00B_TEXT_CASE_UPPER),
            .fill_style = make_fill_style(N00B_PAL_PRIMARY)),
        .alt_cell_props = n00b_box_props_new(
            .borders    = N00B_BORDER_NONE,
            .pad_left   = 1,
            .pad_right  = 1,
            .alignment  = N00B_ALIGN_TOP_LEFT,
            .text_style = make_text_style(N00B_PAL_TEXT_PRIMARY,
                                          N00B_PAL_UNSET,
                                          false, N00B_TEXT_CASE_NONE),
            .fill_style = make_fill_style(N00B_PAL_SURFACE_DARK)),
    };
}

n00b_table_style_t
n00b_table_style_simple(void)
{
    return (n00b_table_style_t){
        .table_props = n00b_box_props_new(
            .theme        = &n00b_border_plain,
            .borders      = (n00b_border_set_t)(N00B_BORDER_TOP
                                                | N00B_BORDER_BOTTOM
                                                | N00B_BORDER_INTERIOR_H),
            .border_style = make_text_style(N00B_PAL_BORDER,
                                            N00B_PAL_UNSET,
                                            false, N00B_TEXT_CASE_NONE)),
        .cell_props = n00b_box_props_new(
            .borders    = N00B_BORDER_NONE,
            .pad_left   = 1,
            .pad_right  = 1,
            .alignment  = N00B_ALIGN_TOP_LEFT,
            .text_style = make_text_style(N00B_PAL_TEXT_PRIMARY,
                                          N00B_PAL_UNSET,
                                          false, N00B_TEXT_CASE_NONE)),
        .header_props = n00b_box_props_new(
            .borders    = N00B_BORDER_NONE,
            .pad_left   = 1,
            .pad_right  = 1,
            .alignment  = N00B_ALIGN_TOP_CENTER,
            .text_style = make_text_style(N00B_PAL_PRIMARY,
                                          N00B_PAL_UNSET,
                                          true, N00B_TEXT_CASE_NONE)),
        .alt_cell_props = nullptr,
    };
}

n00b_table_style_t
n00b_table_style_ornate(void)
{
    return (n00b_table_style_t){
        .table_props = n00b_box_props_new(
            .theme        = &n00b_border_double,
            .borders      = N00B_BORDER_ALL,
            .border_style = make_text_style(N00B_PAL_BORDER_LIGHT,
                                            N00B_PAL_SURFACE,
                                            false, N00B_TEXT_CASE_NONE)),
        .cell_props = n00b_box_props_new(
            .borders    = N00B_BORDER_NONE,
            .pad_left   = 1,
            .pad_right  = 1,
            .alignment  = N00B_ALIGN_TOP_LEFT,
            .text_style = make_text_style(N00B_PAL_TEXT_PRIMARY,
                                          N00B_PAL_UNSET,
                                          false, N00B_TEXT_CASE_NONE),
            .fill_style = make_fill_style(N00B_PAL_SURFACE)),
        .header_props = n00b_box_props_new(
            .borders    = N00B_BORDER_NONE,
            .pad_left   = 1,
            .pad_right  = 1,
            .alignment  = N00B_ALIGN_TOP_CENTER,
            .text_style = make_text_style(N00B_PAL_TEXT_INVERSE,
                                          N00B_PAL_UNSET,
                                          true, N00B_TEXT_CASE_UPPER),
            .fill_style = make_fill_style(N00B_PAL_PRIMARY)),
        .alt_cell_props = n00b_box_props_new(
            .borders    = N00B_BORDER_NONE,
            .pad_left   = 1,
            .pad_right  = 1,
            .alignment  = N00B_ALIGN_TOP_LEFT,
            .text_style = make_text_style(N00B_PAL_TEXT_PRIMARY,
                                          N00B_PAL_UNSET,
                                          false, N00B_TEXT_CASE_NONE),
            .fill_style = make_fill_style(N00B_PAL_SURFACE_DARK)),
    };
}

n00b_table_style_t
n00b_table_style_minimal(void)
{
    return (n00b_table_style_t){
        .table_props = n00b_box_props_new(
            .borders = N00B_BORDER_NONE),
        .cell_props = n00b_box_props_new(
            .borders   = N00B_BORDER_NONE,
            .pad_left  = 1,
            .pad_right = 1,
            .alignment = N00B_ALIGN_TOP_LEFT),
        .header_props = n00b_box_props_new(
            .borders    = N00B_BORDER_NONE,
            .pad_left   = 1,
            .pad_right  = 1,
            .alignment  = N00B_ALIGN_TOP_CENTER,
            .text_style = make_text_style(N00B_PAL_UNSET,
                                          N00B_PAL_UNSET,
                                          true, N00B_TEXT_CASE_NONE)),
        .alt_cell_props = nullptr,
    };
}

n00b_table_style_t
n00b_table_style_ascii(void)
{
    return (n00b_table_style_t){
        .table_props = n00b_box_props_new(
            .theme        = &n00b_border_ascii,
            .borders      = N00B_BORDER_ALL,
            .border_style = make_text_style(N00B_PAL_BORDER,
                                            N00B_PAL_UNSET,
                                            false, N00B_TEXT_CASE_NONE)),
        .cell_props = n00b_box_props_new(
            .borders   = N00B_BORDER_NONE,
            .pad_left  = 1,
            .pad_right = 1,
            .alignment = N00B_ALIGN_TOP_LEFT),
        .header_props = n00b_box_props_new(
            .borders    = N00B_BORDER_NONE,
            .pad_left   = 1,
            .pad_right  = 1,
            .alignment  = N00B_ALIGN_TOP_CENTER,
            .text_style = make_text_style(N00B_PAL_UNSET,
                                          N00B_PAL_UNSET,
                                          true, N00B_TEXT_CASE_NONE)),
        .alt_cell_props = nullptr,
    };
}

// ====================================================================
// Convenience constructors
// ====================================================================

n00b_table_t *
n00b_table_callout(n00b_string_t *content)
{
    n00b_table_style_t style = n00b_table_style_default();
    n00b_table_t      *table = n00b_new_kargs(n00b_table_t, table,
        .num_cols    = 1,
        .style       = &style,
        .cell_props  = n00b_box_props_new(
            .borders    = N00B_BORDER_NONE,
            .pad_left   = 2,
            .pad_right  = 2,
            .pad_top    = 1,
            .pad_bottom = 1,
            .alignment  = N00B_ALIGN_TOP_LEFT));

    n00b_table_add_cell(table, content);
    n00b_table_end_row(table);

    return table;
}

n00b_table_t *
n00b_table_flow(n00b_array_t(n00b_string_t *) items)
{
    n00b_table_style_t style = n00b_table_style_minimal();
    n00b_table_t      *table = n00b_new_kargs(n00b_table_t, table, .style = &style);

    n00b_isize_t n = n00b_array_len(items);

    for (n00b_isize_t i = 0; i < n; i++) {
        n00b_table_col_flex(table, 1);
    }

    n00b_table_add_row(table, items);

    return table;
}
