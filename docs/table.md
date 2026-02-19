# n00b Table Library

## Overview

The n00b table library builds on the render pipeline (`render/plane.h`,
`render/box.h`) and the core layout engine (`core/layout.h`) to provide
Unicode-aware, styled tabular output with automatic column sizing, style
cascading, and ring-buffer streaming for bounded-memory log views.

The library is split into four source modules:

1. **Construction** &mdash; `table.c`: row/cell insertion, column spec
   management, ring-buffer bookkeeping.
2. **Layout** &mdash; `table_layout.c`: content scanning, constraint
   building, and the 1D layout solver invocation.
3. **Rendering** &mdash; `table_render.c`: outer box, interior borders,
   cell content alignment, title/caption placement.
4. **Styles** &mdash; `table_styles.c`: preset style factories (default,
   simple, ornate, minimal, ASCII).

### Design principles

- **Content-driven sizing.** FIT columns automatically derive preferred
  and minimum widths from cell content &mdash; longest line for preferred
  width, longest word for minimum width.
- **Constraint-based layout.** Column widths are computed by the core
  `n00b_layout_calculate()` solver, supporting FIT/FLEX/FIXED modes,
  percentage constraints, priorities, and proportional growth.
- **Style cascade.** Each cell resolves its effective box properties
  through a four-level cascade: per-cell &rarr; per-column &rarr;
  row-based (header / alternating) &rarr; table default.
- **Ring-buffer streaming.** Setting `max_rows` enables a fixed-capacity
  ring buffer &mdash; old rows are silently evicted, enabling O(1)
  memory for log-style tables.
- **Per-cell wrap control.** Word-wrapping is on by default at the table
  level, but individual cells can override via a tristate
  (`N00B_TRI_UNSPECIFIED` / `YES` / `NO`).  Non-wrapping cells truncate
  with ellipsis instead.
- **Unicode-aware.** All width calculations use display width (accounting
  for combining characters, wide characters, and emoji) via the unicode
  library.

---

## Types &mdash; `table/types.h`

### Column sizing modes

| Enum | Value | Meaning |
|------|-------|---------|
| `N00B_COL_FIT` | 0 | Preferred width computed from cell content |
| `N00B_COL_FLEX` | 1 | Proportional share of leftover space |
| `N00B_COL_FIXED` | 2 | Exact width (caller-specified) |

### Cell (`n00b_table_cell_t`)

| Field | Type | Purpose |
|-------|------|---------|
| `content` | `n00b_string_t` | Styled string (empty = blank cell) |
| `cell_props` | `n00b_box_props_t *` | Per-cell style override (nullptr = cascade) |
| `col_span` | `int32_t` | Columns spanned (1 = normal, -1 = all remaining) |
| `row_span` | `int32_t` | Rows spanned (must be 1; multi-row not yet implemented) |
| `wrap` | `n00b_tristate_t` | Wrap control: `UNSPECIFIED` = inherit table default, `YES` = wrap, `NO` = truncate |

### Row (`n00b_table_row_t`)

| Field | Type | Purpose |
|-------|------|---------|
| `cells` | `n00b_table_cell_t *` | Array of cells in this row |
| `num_cells` | `n00b_isize_t` | Populated cell count |
| `cells_cap` | `n00b_isize_t` | Capacity of cells array |

### Column spec (`n00b_table_col_spec_t`)

| Field | Type | Purpose |
|-------|------|---------|
| `mode` | `n00b_col_mode_t` | Sizing mode |
| `min` / `max` / `pref` | `n00b_layout_dim_t` | Constraints (absolute or percentage) |
| `flex_multiple` | `int64_t` | Relative growth share (FLEX mode, default 1) |
| `priority` | `int64_t` | Shrinking priority (higher = kept) |
| `col_props` | `n00b_box_props_t *` | Per-column style |

### Table style (`n00b_table_style_t`)

A coordinated set of box properties returned by style factory functions:

| Field | Purpose |
|-------|---------|
| `table_props` | Outer box style |
| `cell_props` | Default cell style |
| `header_props` | Row 0 style override |
| `alt_cell_props` | Alternating (odd) row style |

---

## Modules

### 1. Construction &mdash; `table/table.h`

#### Creating a table

```c
n00b_table_t *n00b_table_new(+);
    // keyword args:
    //   .num_cols       = 0           (0 = auto-detect from first row)
    //   .table_props    = nullptr     (outer box style)
    //   .cell_props     = nullptr     (default cell style)
    //   .header_props   = nullptr     (row 0 override)
    //   .alt_props      = nullptr     (alternating row style)
    //   .title          = nullptr     (table title)
    //   .caption        = nullptr     (table caption)
    //   .max_rows       = 0           (0 = unlimited; >0 = ring buffer)
    //   .wrap           = true        (table-level default: word-wrap cells)
    //   .allocator      = nullptr     (runtime default)
```

#### Destroying a table

```c
void n00b_table_destroy(n00b_table_t *table);
```

Pre: table not referenced by any canvas.

#### Adding cells and rows

```c
void n00b_table_add_cell(n00b_table_t *table, n00b_string_t content, +);
    // keyword args:
    //   .col_span   = 1                    (-1 = span all remaining columns)
    //   .row_span   = 1                    (must be 1)
    //   .cell_props = nullptr              (per-cell style override)
    //   .wrap       = N00B_TRI_UNSPECIFIED (inherit from table)

void n00b_table_empty_cell(n00b_table_t *table);

void n00b_table_end_row(n00b_table_t *table);

void n00b_table_add_row(n00b_table_t *table, n00b_string_t *cells, n00b_isize_t n);

void n00b_table_end(n00b_table_t *table);
```

`n00b_table_end_row()` finalizes the current row.  The first row locks
the column count; subsequent rows are padded or truncated to match.
`n00b_table_end()` flushes any partially-built row.

#### Column specifications

```c
n00b_isize_t n00b_table_col_fit(n00b_table_t *table);
n00b_isize_t n00b_table_col_flex(n00b_table_t *table, int64_t factor);
n00b_isize_t n00b_table_col_fixed(n00b_table_t *table, int64_t width);
n00b_isize_t n00b_table_col_range(n00b_table_t *table, int64_t min, int64_t max);
n00b_isize_t n00b_table_col_pct(n00b_table_t *table, double min, double max);

void n00b_table_set_col_priority(n00b_table_t *table, n00b_isize_t col, int64_t priority);
void n00b_table_set_col_props(n00b_table_t *table, n00b_isize_t col, n00b_box_props_t *props);
```

All `col_*` functions return the column index.  Column specs must be
added before the first row is finalized (after which the column count
is locked).

#### Rendering

```c
n00b_plane_t *n00b_table_render(n00b_table_t *table, int64_t width, +);
    // keyword args:
    //   .force = false   (true = re-render even if cached)
```

Returns a `n00b_plane_t *` owned by the table (do not destroy it
separately).  Returns nullptr if the table has zero rows.

```c
void n00b_table_invalidate(n00b_table_t *table);
```

Invalidates cached layout, forcing recomputation on next render.

**Example:**

```c
// Create a 3-column table with mixed sizing
n00b_table_style_t sty = n00b_table_style_default();
n00b_table_t *t = n00b_table_new(.num_cols   = 3,
                                  .table_props = sty.table_props,
                                  .cell_props  = sty.cell_props,
                                  .header_props = sty.header_props);

// Column specs: flex(2), fit, fixed(20)
n00b_table_col_flex(t, 2);
n00b_table_col_fit(t);
n00b_table_col_fixed(t, 20);

// Header row
n00b_table_add_cell(t, STR("Name"));
n00b_table_add_cell(t, STR("Age"));
n00b_table_add_cell(t, STR("Email"));
n00b_table_end_row(t);

// Data row
n00b_table_add_cell(t, STR("Alice"));
n00b_table_add_cell(t, STR("30"));
n00b_table_add_cell(t, STR("alice@example.com"));
n00b_table_end_row(t);

n00b_table_end(t);

// Render to an 80-column plane
n00b_plane_t *p = n00b_table_render(t, 80);

n00b_table_destroy(t);
```

### 2. Convenience constructors

```c
n00b_table_t *n00b_table_from_string(n00b_string_t s, +);
    // keyword args:
    //   .row_sep     = nullptr   ("\n")
    //   .col_sep     = nullptr   (",")
    //   .table_props, .cell_props, .header_props, .alt_props, .allocator

n00b_table_t *n00b_callout(n00b_string_t *content);

n00b_table_t *n00b_flow(n00b_string_t *items, n00b_isize_t n);
```

`n00b_table_from_string()` splits a string into rows and columns on the
given delimiters (defaults: newline/comma).  Trailing empty rows from a
trailing newline are skipped.

`n00b_callout()` creates a single-cell bordered callout box with extra
padding.

`n00b_flow()` creates a single-row horizontal flow layout with flex
columns.

**Example:**

```c
n00b_string_t csv = STR("Name,Age,City\nAlice,30,NYC\nBob,25,LA");
n00b_table_t *t = n00b_table_from_string(csv);
n00b_plane_t *p = n00b_table_render(t, 60);
```

---

## Style cascade

Cell style resolution uses a four-level cascade (first non-null wins):

1. `cell->cell_props` &mdash; per-cell override
2. `col_spec->col_props` &mdash; per-column style
3. Row-based override:
   - `header_props` if row index is 0
   - `alt_cell_props` if row index is odd
4. `default_cell_props` &mdash; table-wide default
5. Built-in fallback: 1-cell left/right padding, top-left alignment

This cascade is computed by `_n00b_table_resolve_cell_props()` during
rendering.  The result is never nullptr.

---

## Style presets &mdash; `table_styles.c`

| Factory | Borders | Interior | Header | Alternating |
|---------|---------|----------|--------|-------------|
| `n00b_table_style_default()` | Rounded | H + V | Center | No |
| `n00b_table_style_simple()` | Top/Bottom | H only | No | No |
| `n00b_table_style_ornate()` | Double | H + V | Center | Yes (odd rows) |
| `n00b_table_style_minimal()` | None | None | No | No |
| `n00b_table_style_ascii()` | ASCII (`-`, `|`, `+`) | H + V | Center | No |

**Example:**

```c
n00b_table_style_t sty = n00b_table_style_ornate();

n00b_table_t *t = n00b_table_new(.table_props  = sty.table_props,
                                  .cell_props   = sty.cell_props,
                                  .header_props = sty.header_props,
                                  .alt_props    = sty.alt_cell_props);
```

---

## Layout algorithm &mdash; `table_layout.c`

Layout proceeds in four phases:

### Phase 1: Content scanning

For each FIT column without an explicit preferred width:
- **Preferred width** = longest line (display width) across all cells in
  the column, plus left/right padding.
- **Minimum width** = longest word (space-delimited) across all cells,
  plus padding.
- If a column has no content, it is converted to FLEX mode.

### Phase 2: Constraint building

Column specs are converted to `n00b_layout_t` items with resolved `min`,
`max`, `pref`, `priority`, and `flex_multiple` values.

### Phase 3: 1D layout solver

`n00b_layout_calculate()` from `core/layout.h`:

1. Resolve percentage dimensions against available width.
2. Assign each column its minimum (or preferred if larger).
3. Grow columns toward their maximum, smallest-first.
4. Distribute remaining space proportionally by `flex_multiple`.
5. If over-constrained: shrink flex columns first (largest-first), then
   rigid columns.
6. If still over: force-crop by ascending priority.

### Phase 4: Row heights

For each row, for each cell: wrap content to the computed cell width,
count wrapped lines, add vertical padding.  Row height = max across all
cells in the row.

---

## Ring-buffer streaming

Setting `max_rows > 0` at construction enables ring-buffer mode.  The
table maintains a fixed-capacity row array; when a new row arrives and
the table is full, the oldest row is evicted.

```c
// Create a streaming table that retains at most 100 rows
n00b_table_t *log = n00b_table_new(.num_cols = 3, .max_rows = 100);
```

Ring-buffer state is tracked by three fields:

| Field | Purpose |
|-------|---------|
| `ring_base` | Index of the oldest visible row |
| `max_rows` | Capacity (0 = unlimited) |
| `total_added` | Lifetime row count |

Row insertion computes `slot = total_added % max_rows`.  If the slot is
occupied, the old row's cells are freed before overwriting.

All layout and rendering functions are ring-buffer-aware &mdash; they
iterate visible rows starting from `ring_base`.

---

## Rendering pipeline &mdash; `table_render.c`

`n00b_table_render()` produces a `n00b_plane_t` through five phases:

1. **Layout validation** &mdash; recompute column widths and row heights
   if the cached layout is stale.
2. **Plane creation** &mdash; create or resize the output plane to the
   total computed dimensions.
3. **Outer box** &mdash; stamp the table's border using
   `n00b_box_stamp()` with the configured border theme.
4. **Interior borders** &mdash; draw horizontal and vertical interior
   lines, T-junctions, and crossings based on the `BORDER_INTERIOR_H`
   and `BORDER_INTERIOR_V` flags.
5. **Cell content** &mdash; for each cell: resolve style via cascade,
   wrap content to the cell width, apply vertical and horizontal
   alignment, and write styled strings to the plane.
6. **Title &amp; caption** &mdash; center title in the first content row
   and caption in the last, if present.

---

## Cross-cutting patterns

### Keyword arguments

Most construction and rendering functions accept keyword arguments via
ncc's `_kargs` extension:

```c
n00b_table_new(.num_cols = 4, .max_rows = 50, .title = &my_title)
n00b_table_add_cell(t, content, .col_span = -1)
n00b_table_render(t, 120, .force = true)
```

### Thread safety

Each `n00b_table_t` has a spinlock (`lock`) protecting concurrent access
to the row array and layout cache.

### Memory ownership

- `n00b_table_render()` returns a plane owned by the table.  Do not
  destroy the plane separately.
- `n00b_table_destroy()` frees all owned resources including the plane,
  rows, cells, and column specs.
- Style preset factories return stack-allocated `n00b_table_style_t`
  values containing pointers to heap-allocated `n00b_box_props_t`
  objects; the table takes ownership when assigned.

---

## Quick reference

| Task | Function |
|------|----------|
| Create table | `n00b_table_new(.num_cols = N)` |
| Add FIT column | `n00b_table_col_fit(t)` |
| Add FLEX column | `n00b_table_col_flex(t, factor)` |
| Add FIXED column | `n00b_table_col_fixed(t, width)` |
| Add constrained column | `n00b_table_col_range(t, min, max)` |
| Add percentage column | `n00b_table_col_pct(t, min_pct, max_pct)` |
| Add cell | `n00b_table_add_cell(t, content)` |
| Add spanning cell | `n00b_table_add_cell(t, content, .col_span = -1)` |
| Add no-wrap cell | `n00b_table_add_cell(t, content, .wrap = N00B_TRI_NO)` |
| Table with no wrap | `n00b_table_new(.wrap = false)` |
| Finalize row | `n00b_table_end_row(t)` |
| Finalize table | `n00b_table_end(t)` |
| Render to plane | `n00b_table_render(t, width)` |
| Force re-render | `n00b_table_render(t, width, .force = true)` |
| Invalidate cache | `n00b_table_invalidate(t)` |
| Default style | `n00b_table_style_default()` |
| Parse CSV/delimited | `n00b_table_from_string(s)` |
| Callout box | `n00b_callout(&content)` |
| Horizontal flow | `n00b_flow(items, n)` |
| Destroy table | `n00b_table_destroy(t)` |
