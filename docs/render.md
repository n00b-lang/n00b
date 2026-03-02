# n00b Render Library

## Overview

The n00b render library provides a layered 2D rendering pipeline for
terminal and GUI output.  Content flows through a hierarchy of typed
abstractions &mdash; from individual cells through planes, boxes, and
canvases &mdash; before reaching a pluggable backend for final output.

The library is organized into seven modules:

1. **Types** &mdash; `render/types.h`: alignment, borders, overflow,
   widget state, box properties.
2. **Cells** &mdash; `render/cell.h`: the 32-byte render cell, the
   atomic unit of the grid.
3. **Planes** &mdash; `render/plane.h`: content surfaces with grids,
   viewports, write cursors, and hierarchical composition.
4. **Boxes** &mdash; `render/box.h`: border and padding decoration,
   stamped at composite time.
5. **Canvas** &mdash; `render/canvas.h`: compositing surface with double
   buffering and backend dispatch.
6. **Compositing** &mdash; `render/composite.h`: hierarchy flattening,
   z-sorting, and capability-based degradation.
7. **Backends** &mdash; `render/backend.h`, `render/backend_registry.h`:
   vtable interface, built-in renderers, and dynamic plugin loading.

### Design principles

- **Content separated from decoration.** Borders and padding are not
  stored in the plane's cell grid; they are stamped onto the frame
  buffer at composite time.  This keeps grids compact and allows
  flexible re-styling without rewriting content.
- **State-based styling.** Each plane carries a widget state (normal,
  focused, disabled, hover, active) that drives style overrides during
  compositing &mdash; enabling UI state visuals with zero grid rewrites.
- **Capability-based degradation.** The compositing pipeline queries the
  backend's capabilities and automatically degrades the frame
  (Unicode&rarr;ASCII, color stripping) to match.
- **Diff rendering.** The canvas maintains a previous frame buffer.
  Backends that support differential rendering skip unchanged cells for
  efficient terminal updates.
- **Pluggable backends.** Renderers are vtable objects registered at
  runtime.  Built-in backends cover ANSI terminals, inline output,
  plain text, and stream capture; external plugins can be loaded from
  shared libraries.

---

## Types &mdash; `render/types.h`

### Alignment

Horizontal and vertical alignment are encoded in a single `int8_t`:

| Constant | Bits | Meaning |
|----------|------|---------|
| `N00B_ALIGN_LEFT` | `0x01` | Left-align |
| `N00B_ALIGN_CENTER` | `0x02` | Center horizontally |
| `N00B_ALIGN_RIGHT` | `0x04` | Right-align |
| `N00B_ALIGN_TOP` | `0x10` | Top-align |
| `N00B_ALIGN_MIDDLE` | `0x20` | Center vertically |
| `N00B_ALIGN_BOTTOM` | `0x40` | Bottom-align |

Composite presets: `N00B_ALIGN_TOP_LEFT`, `N00B_ALIGN_TOP_CENTER`,
`N00B_ALIGN_MID_CENTER`, etc.

Masks: `N00B_HORIZONTAL_MASK` (`0x0F`), `N00B_VERTICAL_MASK` (`0xF0`).

### Borders

`n00b_border_set_t` is a `uint8_t` bitmask:

| Flag | Meaning |
|------|---------|
| `N00B_BORDER_TOP` | Top border |
| `N00B_BORDER_BOTTOM` | Bottom border |
| `N00B_BORDER_LEFT` | Left border |
| `N00B_BORDER_RIGHT` | Right border |
| `N00B_BORDER_INTERIOR_H` | Horizontal interior lines |
| `N00B_BORDER_INTERIOR_V` | Vertical interior lines |
| `N00B_BORDER_SIDES` | All four sides |
| `N00B_BORDER_ALL` | All sides + interior |

`n00b_border_theme_t` maps each border element to a Unicode codepoint:

| Field | Example (rounded) |
|-------|-------------------|
| `horizontal` | `U+2500` (`─`) |
| `vertical` | `U+2502` (`│`) |
| `upper_left` | `U+256D` (`╭`) |
| `upper_right` | `U+256E` (`╮`) |
| `lower_left` | `U+2570` (`╰`) |
| `lower_right` | `U+256F` (`╯`) |
| `cross` | `U+253C` (`┼`) |
| `top_t` / `bottom_t` | `U+252C` / `U+2534` |
| `left_t` / `right_t` | `U+251C` / `U+2524` |

Predefined themes: `n00b_border_plain`, `n00b_border_bold`,
`n00b_border_double`, `n00b_border_dash`, `n00b_border_ascii`,
`n00b_border_rounded`.

### Overflow

| Constant | Meaning |
|----------|---------|
| `N00B_OVERFLOW_CLIP` | Content clipped at boundary (default) |
| `N00B_OVERFLOW_SCROLL` | Enables scrolling viewport |
| `N00B_OVERFLOW_ELLIPSIS` | Truncate with `...` on last visible line |
| `N00B_OVERFLOW_VISIBLE` | Content extends past bounds |

### Widget state

Five visual states for style resolution:

| Constant | Index | Purpose |
|----------|-------|---------|
| `N00B_WSTATE_NORMAL` | 0 | Base state |
| `N00B_WSTATE_FOCUSED` | 1 | Has keyboard focus |
| `N00B_WSTATE_DISABLED` | 2 | Inactive / grayed out |
| `N00B_WSTATE_HOVER` | 3 | Mouse pointer over |
| `N00B_WSTATE_ACTIVE` | 4 | Pressed / activated |

### State style (`n00b_state_style_t`)

Per-state overrides (nullable fields inherit from base):

| Field | Type | Purpose |
|-------|------|---------|
| `text_style` | `n00b_text_style_t *` | Text styling override |
| `border_style` | `n00b_text_style_t *` | Border styling override |
| `fill_style` | `n00b_text_style_t *` | Padding/fill styling override |
| `border_theme` | `const n00b_border_theme_t *` | Border theme override |

### Box properties (`n00b_box_props_t`)

| Field | Type | Purpose |
|-------|------|---------|
| `border_theme` | `const n00b_border_theme_t *` | Visual theme |
| `border_style` | `n00b_text_style_t *` | Border text styling |
| `fill_style` | `n00b_text_style_t *` | Padding fill styling |
| `borders` | `n00b_border_set_t` | Which borders to draw |
| `pad_top/bottom/left/right` | `int8_t` | Padding (cells) |
| `margin_top/bottom/left/right` | `int8_t` | Margin (cells) |
| `alignment` | `n00b_alignment_t` | Content alignment |
| `overflow` | `n00b_overflow_t` | Overflow handling |
| `state_styles` | `n00b_state_style_t *[5]` | Per-state overrides |
| `gui_ext` | `void *` | Opaque GUI-backend extensions |

---

## Modules

### 1. Cells &mdash; `render/cell.h`

The render cell (`n00b_rcell_t`) is a 32-byte aligned structure &mdash;
the atomic unit of every grid:

| Field | Size | Purpose |
|-------|------|---------|
| `grapheme` | 16 bytes | UTF-8 grapheme cluster (inline, NUL-terminated) |
| `style` | 8 bytes | Pointer to resolved `n00b_text_style_t` |
| `grapheme_len` | 1 byte | Byte length of grapheme (1&ndash;15) |
| `display_width` | 1 byte | Column width: 0 (empty/continuation), 1, or 2 |
| `flags` | 1 byte | Status flags |

Cell flags:

| Flag | Meaning |
|------|---------|
| `N00B_CELL_EMPTY` | No content |
| `N00B_CELL_OCCUPIED` | Contains a grapheme |
| `N00B_CELL_WIDE_CONT` | Continuation of a 2-column-wide character |
| `N00B_CELL_DIRTY` | Modified since last render |
| `N00B_CELL_BORDER` | Auto-generated border decoration |
| `N00B_CELL_PADDING` | Auto-generated padding fill |

#### Inline utilities

```c
void n00b_rcell_clear(n00b_rcell_t *c);
void n00b_rcell_set_ascii(n00b_rcell_t *c, char ch);
void n00b_rcell_set_grapheme(n00b_rcell_t *c, const char *utf8, uint8_t len, uint8_t width);
bool n00b_rcell_equal(const n00b_rcell_t *a, const n00b_rcell_t *b);
void n00b_rcell_mark_clean(n00b_rcell_t *c);
bool n00b_rcell_is_empty(const n00b_rcell_t *c);
```

#### Exported

```c
void n00b_rcell_set_codepoint(n00b_rcell_t *c, n00b_codepoint_t cp);
```

### 2. Planes &mdash; `render/plane.h`

A plane is the primary content container: a 2D grid of cells with a
viewport, write cursor, optional box decoration, and a parent/child
hierarchy for composition.

#### Lifecycle

```c
n00b_plane_t *n00b_plane_new(n00b_isize_t cols, n00b_isize_t rows, +);
    // keyword args:
    //   .width = 0      (0 = cols)
    //   .height = 0      (0 = rows)
    //   .name    = nullptr
    //   .scroll  = N00B_SCROLL_NONE
    //   .z       = 0
    //   .box     = nullptr
    //   .style   = nullptr (default style)
    //   .allocator = nullptr

void n00b_plane_destroy(n00b_plane_t *p);
```

#### Hierarchy

```c
void n00b_plane_add_child(n00b_plane_t *parent, n00b_plane_t *child,
                           int32_t x, int32_t y);
void n00b_plane_remove_child(n00b_plane_t *parent, n00b_plane_t *child);
```

#### Content writing

```c
void n00b_plane_put_str(n00b_plane_t *p, n00b_string_t s, +);
    // keyword args: .wrap = true

void n00b_plane_put_str_at(n00b_plane_t *p, n00b_isize_t row,
                            n00b_isize_t col, n00b_string_t s);

void n00b_plane_put_cp(n00b_plane_t *p, n00b_codepoint_t cp, +);
    // keyword args: .style = nullptr

void n00b_plane_newline(n00b_plane_t *p);
```

`n00b_plane_put_str()` decodes UTF-8, handles newlines and wide
characters, and performs word-wrap when `.wrap = true`.  In auto-scroll
mode, writing past the last row scrolls the ring buffer.

#### Cell access

```c
n00b_option_t(n00b_const_rcell_ptr_t)
n00b_plane_get_cell(n00b_plane_t *p, n00b_isize_t row, n00b_isize_t col);
```

#### Clear and fill

```c
void n00b_plane_clear(n00b_plane_t *p);

void n00b_plane_fill_rect(n00b_plane_t *p, n00b_isize_t row,
                           n00b_isize_t col, n00b_isize_t rows,
                           n00b_isize_t cols, +);
    // keyword args: .cp = ' ', .style = nullptr
```

#### Cursor, viewport, and geometry

```c
void n00b_plane_cursor_move(n00b_plane_t *p, n00b_isize_t row, n00b_isize_t col);

void n00b_plane_scroll(n00b_plane_t *p, int32_t drow, int32_t dcol);
void n00b_plane_scroll_to(n00b_plane_t *p, n00b_isize_t row, n00b_isize_t col);

void n00b_plane_move(n00b_plane_t *p, int32_t x, int32_t y);
void n00b_plane_set_z(n00b_plane_t *p, int32_t z);
void n00b_plane_resize(n00b_plane_t *p, n00b_isize_t rows, n00b_isize_t cols);
void n00b_plane_set_visible(n00b_plane_t *p, bool visible);

void n00b_plane_set_box(n00b_plane_t *p, n00b_box_props_t *box);
void n00b_plane_content_size(n00b_plane_t *p, n00b_isize_t *rows, n00b_isize_t *cols);

void n00b_plane_set_state(n00b_plane_t *p, n00b_widget_state_t state);
n00b_widget_state_t n00b_plane_get_state(n00b_plane_t *p);
```

**Example:**

```c
n00b_plane_t *p = n00b_plane_new(80, 24, .name = "main",
                                  .scroll = N00B_SCROLL_AUTO);

n00b_plane_put_str(p, STR("Hello, world!"));
n00b_plane_newline(p);
n00b_plane_put_str(p, STR("Second line."));

// Child plane at (10, 5)
n00b_plane_t *child = n00b_plane_new(20, 5, .z = 1);
n00b_plane_add_child(p, child, 10, 5);
n00b_plane_put_str(child, STR("Overlay"));
```

#### Scroll modes

| Mode | Behavior |
|------|----------|
| `N00B_SCROLL_NONE` | Writes past bounds are clipped |
| `N00B_SCROLL_MANUAL` | Viewport moves on explicit `scroll()` calls |
| `N00B_SCROLL_AUTO` | Viewport follows cursor; ring buffer evicts old rows |

### 3. Boxes &mdash; `render/box.h`

Box properties describe borders and padding.  They are **not** stored
in the plane grid &mdash; they are stamped onto the frame buffer during
compositing.

```c
n00b_box_props_t *n00b_box_props_new(+);
    // keyword args:
    //   .theme, .borders = N00B_BORDER_ALL, .border_style, .fill_style,
    //   .pad_top/bottom/left/right, .margin_top/bottom/left/right,
    //   .alignment, .overflow

void n00b_box_insets(const n00b_box_props_t *box,
                     int8_t *top, int8_t *bottom,
                     int8_t *left, int8_t *right);

void n00b_box_outer_size(const n00b_box_props_t *box,
                          n00b_isize_t content_rows, n00b_isize_t content_cols,
                          n00b_isize_t *out_rows, n00b_isize_t *out_cols);

void n00b_box_stamp(const n00b_box_props_t *box,
                     n00b_rcell_t *grid, n00b_isize_t grid_cols,
                     n00b_isize_t origin_row, n00b_isize_t origin_col,
                     n00b_isize_t outer_rows, n00b_isize_t outer_cols,
                     n00b_text_style_t *border_style,
                     n00b_text_style_t *fill_style);
```

`n00b_box_stamp()` writes border characters and padding fill into a
provided cell grid, setting `N00B_CELL_BORDER` and `N00B_CELL_PADDING`
flags on the affected cells.

### 4. Canvas &mdash; `render/canvas.h`

The canvas is the compositing surface that owns frame buffers and drives
the rendering pipeline.

```c
n00b_canvas_t *n00b_canvas_new(const n00b_renderer_vtable_t *vtable, +);
    // keyword args: .allocator = nullptr, .fd = 1

void n00b_canvas_destroy(n00b_canvas_t *c);

void n00b_canvas_add_plane(n00b_canvas_t *c, n00b_plane_t *p);
void n00b_canvas_remove_plane(n00b_canvas_t *c, n00b_plane_t *p);

void n00b_canvas_render(n00b_canvas_t *c);
void n00b_canvas_invalidate(n00b_canvas_t *c);
void n00b_canvas_resize(n00b_canvas_t *c, n00b_isize_t rows, n00b_isize_t cols);
void n00b_canvas_flush(n00b_canvas_t *c);

void n00b_canvas_alt_screen_enter(n00b_canvas_t *c);
void n00b_canvas_alt_screen_leave(n00b_canvas_t *c);
```

#### Render pipeline

`n00b_canvas_render()` executes this pipeline:

1. **Query size** &mdash; ask the backend for current dimensions; resize
   frame buffers if they changed.
2. **Prepare GUI** &mdash; call `prepare_gui` hook if the backend
   supports it.
3. **Flatten** &mdash; recursively walk the plane hierarchy, computing
   absolute coordinates, z-values, and clip rectangles.
4. **Composite** &mdash; iterate z-sorted entries: stamp box
   decoration, copy viewport cells, apply state-based style resolution,
   handle overflow (ellipsis).
5. **Degrade** &mdash; adjust frame based on backend capabilities
   (Unicode&rarr;ASCII, color stripping).
6. **Diff** &mdash; compare against previous frame (if the backend
   supports differential rendering and a full redraw is not forced).
7. **Render** &mdash; send the frame to the backend.
8. **Swap** &mdash; exchange frame pointers for the next cycle.

**Example:**

```c
n00b_canvas_t *c = n00b_canvas_new(&n00b_renderer_ansi);

n00b_plane_t *root = n00b_plane_new(80, 24);
n00b_plane_put_str(root, STR("Hello from the canvas!"));
n00b_canvas_add_plane(c, root);

n00b_canvas_render(c);
n00b_canvas_flush(c);

n00b_canvas_destroy(c);
```

### 5. Compositing &mdash; `render/composite.h`

```c
n00b_composite_entry_t *
n00b_composite_flatten(n00b_plane_t **planes, n00b_isize_t num_planes);

void
n00b_composite_render(n00b_composite_entry_t *entries, n00b_isize_t count,
                       n00b_rcell_t *frame, n00b_isize_t frame_rows,
                       n00b_isize_t frame_cols,
                       n00b_text_style_t *default_style);

void
n00b_composite_degrade(n00b_rcell_t *frame, n00b_isize_t frame_rows,
                        n00b_isize_t frame_cols, n00b_render_cap_t caps);
```

Each `n00b_composite_entry_t` records a plane's absolute position and
clip rectangle:

| Field | Purpose |
|-------|---------|
| `plane` | The source plane |
| `abs_x` / `abs_y` / `abs_z` | Absolute coordinates in frame space |
| `clip_x/y/w/h` | Clip rectangle (intersection of all ancestor bounds) |

Flatten walks the tree depth-first, accumulating absolute coordinates
and intersecting clip rectangles.  The result is sorted by `abs_z`
(stable sort, low-z first = background).

Compositing renders entries in z-order using the painter's algorithm:
each plane overwrites what came before it.

### 6. Backends &mdash; `render/backend.h`

#### Capabilities (`n00b_render_cap_t`)

| Flag | Meaning |
|------|---------|
| `N00B_RCAP_COLOR_BASIC` | 8 ANSI colors |
| `N00B_RCAP_COLOR_256` | 256-color palette |
| `N00B_RCAP_COLOR_24BIT` | True color (24-bit RGB) |
| `N00B_RCAP_BOLD` / `ITALIC` / `UNDERLINE` / `STRIKETHROUGH` / `DIM` | Text decorations |
| `N00B_RCAP_CURSOR_MOVE` | Cursor positioning |
| `N00B_RCAP_ALT_SCREEN` | Alternate screen buffer |
| `N00B_RCAP_UNICODE` | Full Unicode (not ASCII-only) |
| `N00B_RCAP_WIDE_CHARS` | 2-column wide character support |
| `N00B_RCAP_DIFF_RENDER` | Differential rendering |
| `N00B_RCAP_PIXEL_COORDS` | Pixel-level positioning |
| `N00B_RCAP_FONT_METRICS` | Font size/metrics queries |
| `N00B_RCAP_GUI_EXT` | GUI-backend extensions |

#### Renderer vtable

```c
typedef struct n00b_renderer_vtable_t {
    const char *name;
    uint32_t    version;

    // Required:
    void              *(*init)(void);
    void               (*destroy)(void *ctx);
    n00b_render_cap_t  (*capabilities)(void *ctx);
    n00b_render_size_t (*get_size)(void *ctx);
    void               (*render_frame)(void *ctx, n00b_rcell_t *cells,
                                       n00b_isize_t rows, n00b_isize_t cols,
                                       n00b_rcell_t *prev_cells);
    void               (*flush)(void *ctx);

    // Optional (nullptr if unsupported):
    void (*cursor_set_visible)(void *ctx, bool visible);
    void (*cursor_move)(void *ctx, n00b_isize_t row, n00b_isize_t col);
    void (*alt_screen_enter)(void *ctx);
    void (*alt_screen_leave)(void *ctx);
    void (*on_resize)(void *ctx,
                      void (*cb)(n00b_isize_t, n00b_isize_t, void *),
                      void *user_ctx);
    void (*prepare_gui)(void *ctx, n00b_plane_t **planes, n00b_isize_t n);
} n00b_renderer_vtable_t;
```

#### Built-in backends

| Backend | Description |
|---------|-------------|
| `n00b_renderer_ansi` | Full-featured terminal: diff rendering, SGR, cursor |
| `n00b_renderer_ansi_inline` | Inline ANSI (no cursor positioning), for CLI tools |
| `n00b_renderer_dumb` | Plain text (no escapes, no color) |
| `n00b_renderer_stream` | Buffer capture for testing |

### 7. Backend registry &mdash; `render/backend_registry.h`

```c
void n00b_renderer_register(const char *name, const n00b_renderer_vtable_t *vtable);
n00b_option_t(const n00b_renderer_vtable_t *) n00b_renderer_find(const char *name);
void n00b_renderer_list(const char ***names, n00b_isize_t *count);

n00b_result_t(int) n00b_renderer_load(const char *path);
n00b_result_t(int) n00b_renderer_load_by_name(const char *name);

void n00b_renderer_registry_init(void);
```

#### Plugin interface

Shared libraries export a `n00b_renderer_plugin_t`:

```c
typedef struct {
    uint32_t abi_version;
    const char *name;
    const n00b_renderer_vtable_t *vtable;
} n00b_renderer_plugin_t;

// In plugin .so/.dylib:
extern const n00b_renderer_plugin_t n00b_renderer_plugin;
```

`n00b_renderer_load_by_name()` searches these directories:
1. `$N00B_RENDERER_PATH` (colon-separated)
2. `$HOME/.n00b/renderers/`
3. `/usr/local/lib/n00b/renderers/`

Looking for `libn00b_render_<name>.so` (Linux) or `.dylib` (macOS).

---

## Data flow

```
Application code
    │
    ▼
┌─────────────────────────────────────────────────┐
│ Planes: content grids + viewports + hierarchy   │
│   n00b_plane_put_str(), n00b_plane_put_cp()     │
│   Box properties (borders, padding)             │
│   Widget state (normal, focused, hover, ...)    │
└─────────────────────┬───────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────┐
│ Canvas: frame buffer + backend reference        │
│   n00b_canvas_render() triggers:                │
│   1. Flatten hierarchy → z-sorted entries       │
│   2. Composite → stamp boxes, copy cells        │
│   3. Degrade → adjust for backend capabilities  │
│   4. Diff → skip unchanged cells                │
│   5. Render → backend::render_frame()           │
│   6. Swap → exchange frame pointers             │
└─────────────────────┬───────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────┐
│ Backend: ANSI terminal, plain text, GUI, ...    │
│   render_frame(), flush()                       │
└─────────────────────────────────────────────────┘
```

---

## Cross-cutting patterns

### Thread safety

- Each plane has a `n00b_spin_lock_t` protecting grid access, child
  array manipulation, and state changes.
- The canvas has its own spinlock protecting the frame buffer and plane
  list.
- Operations that modify the plane hierarchy (add/remove child) acquire
  the parent's lock.

### Memory layout

Cells are 32 bytes, aligned, with a 16-byte inline grapheme buffer.
This avoids heap indirection for ~99% of characters and provides
cache-friendly row-major grid access.

A 200&times;50 terminal frame = 10,000 cells &times; 32 bytes = 320 KB.

Grids are allocated with `.no_scan = true` to avoid GC scanning.

### Clipping

Child planes are clipped to the intersection of all ancestor boundaries.
A fully-clipped plane (zero-area clip rectangle) is skipped entirely
during compositing.

### Ring-buffer scrolling

In `N00B_SCROLL_AUTO` mode, the plane's grid acts as a circular buffer.
Writing past the last row advances `ring_base` instead of memcpy-ing
the grid &mdash; O(1) per line.

---

## Quick reference

| Task | Function |
|------|----------|
| Create plane | `n00b_plane_new(cols, rows)` |
| Write string | `n00b_plane_put_str(p, s)` |
| Write codepoint | `n00b_plane_put_cp(p, cp)` |
| Newline | `n00b_plane_newline(p)` |
| Clear plane | `n00b_plane_clear(p)` |
| Fill rectangle | `n00b_plane_fill_rect(p, row, col, rows, cols)` |
| Move cursor | `n00b_plane_cursor_move(p, row, col)` |
| Scroll viewport | `n00b_plane_scroll(p, drow, dcol)` |
| Add child plane | `n00b_plane_add_child(parent, child, x, y)` |
| Set z-order | `n00b_plane_set_z(p, z)` |
| Resize plane | `n00b_plane_resize(p, rows, cols)` |
| Show/hide | `n00b_plane_set_visible(p, visible)` |
| Set box decoration | `n00b_plane_set_box(p, box)` |
| Set widget state | `n00b_plane_set_state(p, state)` |
| Create canvas | `n00b_canvas_new(&vtable)` |
| Add plane to canvas | `n00b_canvas_add_plane(c, p)` |
| Render frame | `n00b_canvas_render(c)` |
| Force full redraw | `n00b_canvas_invalidate(c)` |
| Enter alt screen | `n00b_canvas_alt_screen_enter(c)` |
| Register backend | `n00b_renderer_register(name, vtable)` |
| Find backend | `n00b_renderer_find(name)` |
| Load plugin | `n00b_renderer_load_by_name(name)` |
