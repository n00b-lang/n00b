/**
 * @file types.h
 * @brief Type definitions for the table subsystem.
 *
 * Defines the cell, row, column spec, column sizing mode, and style
 * preset types used by `n00b_table_t`.
 *
 * ### Related modules
 *
 * - `table/table.h` — table struct and public API
 * - `render/types.h` — `n00b_box_props_t` used for styling
 * - `core/layout.h` — `n00b_layout_dim_t` used for column constraints
 */
#pragma once

#include "n00b.h"
#include "display/layout.h"
#include "adt/list.h"
#include "display/render/types.h"
#include "text/strings/text_style.h"

// ====================================================================
// Column sizing mode
// ====================================================================

/**
 * @brief How a column's width is determined.
 */
typedef enum : uint8_t {
    N00B_COL_FIT   = 0, /**< Preferred width computed from content. */
    N00B_COL_FLEX  = 1, /**< Proportional share of leftover space. */
    N00B_COL_FIXED = 2, /**< Exact width. */
} n00b_col_mode_t;

// ====================================================================
// Table cell
// ====================================================================

/**
 * @brief A single cell in a table row.
 *
 * Holds the content string pointer, optional per-cell styling,
 * span counts, and a tristate wrap control.
 *
 * The `wrap` field uses `n00b_tristate_t`:
 * - `N00B_TRI_UNSPECIFIED` — inherit from the table default.
 * - `N00B_TRI_YES` — force word-wrap in this cell.
 * - `N00B_TRI_NO` — disable word-wrap; content is truncated.
 *
 * @pre `row_span` must be 1 (multi-row spanning not yet implemented).
 */
typedef struct n00b_table_cell_t {
    n00b_string_t    *content;    /**< Styled string (empty = blank cell). */
    n00b_box_props_t *cell_props; /**< Per-cell style (nullptr = cascade). */
    int32_t           col_span;   /**< Columns spanned (1 = normal, -1 = all). */
    int32_t           row_span;   /**< Rows spanned (only 1 supported). */
    n00b_tristate_t   wrap;       /**< Wrap control (UNSPECIFIED = inherit). */
} n00b_table_cell_t;

// ====================================================================
// Table row
// ====================================================================

/**
 * @brief A completed row of cells.
 */
typedef struct n00b_table_row_t {
    n00b_list_t(n00b_table_cell_t) cells;
} n00b_table_row_t;

// ====================================================================
// Column specification
// ====================================================================

/**
 * @brief Sizing and style constraints for a single column.
 *
 * Columns default to `N00B_COL_FIT` mode, where `pref` and `min` are
 * automatically computed from cell content during layout.
 */
typedef struct n00b_table_col_spec_t {
    n00b_col_mode_t    mode;          /**< Sizing mode. */
    n00b_layout_dim_t  min;           /**< Minimum width. */
    n00b_layout_dim_t  max;           /**< Maximum width. */
    n00b_layout_dim_t  pref;          /**< Preferred width (FIT: from content). */
    int64_t            flex_multiple;  /**< FLEX mode: relative share (default 1). */
    int64_t            priority;       /**< Higher = kept during shrinking. */
    n00b_box_props_t  *col_props;     /**< Per-column style (nullptr = default). */
} n00b_table_col_spec_t;

// ====================================================================
// Style preset bundle
// ====================================================================

/**
 * @brief A coordinated set of box properties for a table's visual style.
 *
 * Returned by the named style factory functions (e.g., `n00b_table_style_default()`).
 */
typedef struct n00b_table_style_t {
    n00b_box_props_t *table_props;
    n00b_box_props_t *cell_props;
    n00b_box_props_t *header_props;
    n00b_box_props_t *alt_cell_props;
} n00b_table_style_t;

