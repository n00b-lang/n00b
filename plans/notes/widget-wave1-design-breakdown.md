# Wave 1 Widget Design Breakdown (n00b-slop -> n00b-athens)

This document is the Wave 1 design dossier for port planning. It converts prototype behavior in `n00b-slop` into Athens-native widget contracts for `stack`, `grid`, `split`, `scroll`, `tabs`, and `text`. Each section is intentionally implementation-ready: API naming, state/layout/render/event contracts, backend constraints, and test strategy are fixed so follow-on implementation ExecPlans do not need to re-open prototype archaeology.

## stack

### Prototype Behavior Snapshot

Prototype `stack` is a z-order container that draws all visible children from bottom to top, lays each child to full container bounds, reports preferred size as max child size, and routes events from topmost child downward (`ctui_widget_hit_test` for mouse, direct child dispatch for other events).

Prototype evidence: `n00b-slop/include/ctui/widgets/stack.h`, `n00b-slop/src/ctui/widgets/stack.c`.

Athens evidence used to shape this port: `n00b-athens/include/display/widget.h`, `n00b-athens/include/display/render/plane.h`, `n00b-athens/src/display/mouse.c`.

Wave 1 dependency: this widget is the base layering primitive that later supports tab content overlays and Wave 3 modal/tooltip composition.

### Athens API Proposal

Use the public name `zstack` to avoid collision with the ADT macro `n00b_stack_new` in `n00b-athens/include/adt/stack.h`.

- Header: `n00b-athens/include/display/widgets/zstack.h`.
- Data struct: `typedef struct n00b_zstack_t { ... } n00b_zstack_t`.
- Vtable symbol: `extern const n00b_widget_vtable_t n00b_widget_zstack;`.
- Constructor:
  - `n00b_plane_t *n00b_zstack_new() _kargs { n00b_box_props_t *box = nullptr; n00b_canvas_t *canvas = nullptr; n00b_allocator_t *allocator = nullptr; };`
- Required mutators/accessors:
  - `void n00b_zstack_push(n00b_plane_t *stack, n00b_plane_t *layer);`
  - `n00b_plane_t *n00b_zstack_pop(n00b_plane_t *stack);`
  - `n00b_isize_t n00b_zstack_count(n00b_plane_t *stack);`
  - `n00b_plane_t *n00b_zstack_get(n00b_plane_t *stack, n00b_isize_t index);`
  - `bool n00b_zstack_bring_to_front(n00b_plane_t *stack, n00b_plane_t *layer);`
  - `bool n00b_zstack_send_to_back(n00b_plane_t *stack, n00b_plane_t *layer);`
- Callback signatures: none.
- Ownership/lifetime: parent/child relationship follows `n00b_plane_add_child` semantics. `zstack` does not free child allocations when reordering or popping; detach only.

### State Model

State lives in child ordering only. No extra per-layer metadata is required for Wave 1 beyond order and visibility. Visible child order defines z-order; index `0` is back, `count-1` is front.

### Layout Contract

`zstack` is a container (`can_focus=false`). On layout it passes the same content bounds to each child via `n00b_widget_layout(child, content_bounds)`. The widget's own content size is determined by normal plane bounds assignment from `n00b_widget_layout`.

### Rendering Contract

`zstack` renders no intrinsic content (`n00b_plane_clear`). Composition order is delegated to child order and plane hierarchy; no backend-specific draw primitives are needed.

### Event And Focus Contract

Mouse and key events should naturally reach the topmost visible child first through existing hit testing and bubbling in `n00b_mouse_route_event`. `zstack` itself only consumes events for future optional management shortcuts; Wave 1 keeps it non-consuming.

### Backend And Portability Notes

Do not replicate notcurses hit-testing or plane ownership code. Athens already handles pixel hit testing, clipping, and event bubbling in backend-neutral code (`src/display/mouse.c`, `src/display/event_dispatch.c`).

### Test And Demo Plan

- Unit test: build a `zstack` with three clickable children, send a click in overlapping region, assert only top layer callback fires; reorder with `bring_to_front`, repeat.
- Unit test: verify `pop` returns previous top and child parent pointer is cleared.
- Demo scenario: a box+label background with a transient overlay panel, toggling overlay order at runtime.

### Open Questions

- Should Wave 1 expose an indexed insert API (`insert_at`) or defer until Wave 3 overlay widgets require it?
- Should hidden layers participate in `measure`, or should only visible layers contribute?

## grid

### Prototype Behavior Snapshot

Prototype `grid` supports fixed columns, auto-fit columns, CSS-like track definitions (`AUTO`, `FIXED`, `FR`), per-child spans, and gap/padding settings. Placement is flow-based with span-aware occupancy mapping. Preferred size uses child preferred sizes; runtime layout currently uses equal-height rows after placement.

Prototype evidence: `n00b-slop/include/ctui/widgets/grid.h`, `n00b-slop/src/ctui/widgets/grid.c`.

Athens evidence used to shape this port: `n00b-athens/include/display/widget.h`, `n00b-athens/src/display/widgets/box.c`, `n00b-athens/include/display/render/types.h`.

Wave 1 dependency: `grid` is the primary 2D container for dashboard and form layouts and should land before `tabs` page compositions become complex.

### Athens API Proposal

- Header: `n00b-athens/include/display/widgets/grid.h`.
- Data struct:
  - `typedef enum { N00B_GRID_SIZE_AUTO, N00B_GRID_SIZE_FIXED, N00B_GRID_SIZE_FR } n00b_grid_size_t;`
  - `typedef struct n00b_grid_track_t { n00b_grid_size_t type; int32_t value; int32_t min_px; int32_t max_px; } n00b_grid_track_t;`
  - `typedef struct n00b_grid_t { ... } n00b_grid_t;`
- Vtable symbol: `extern const n00b_widget_vtable_t n00b_widget_grid;`.
- Constructor:
  - `n00b_plane_t *n00b_grid_new() _kargs { int32_t columns = 1; int32_t min_col_width = 0; int32_t max_col_width = 0; int32_t row_gap = 0; int32_t col_gap = 0; int32_t gap = 0; int32_t pad_top = 0; int32_t pad_right = 0; int32_t pad_bottom = 0; int32_t pad_left = 0; n00b_canvas_t *canvas = nullptr; n00b_allocator_t *allocator = nullptr; };`
- Required mutators/accessors:
  - `void n00b_grid_set_columns(n00b_plane_t *grid, int32_t columns);`
  - `void n00b_grid_set_tracks(n00b_plane_t *grid, const n00b_grid_track_t *tracks, n00b_isize_t count);`
  - `void n00b_grid_set_auto_fit(n00b_plane_t *grid, int32_t min_col_width, int32_t max_col_width);`
  - `void n00b_grid_set_gap(n00b_plane_t *grid, int32_t gap);`
  - `void n00b_grid_set_row_gap(n00b_plane_t *grid, int32_t row_gap);`
  - `void n00b_grid_set_col_gap(n00b_plane_t *grid, int32_t col_gap);`
  - `void n00b_grid_set_span(n00b_plane_t *grid, n00b_plane_t *child, int32_t col_span, int32_t row_span);`
  - `void n00b_grid_get_span(n00b_plane_t *grid, n00b_plane_t *child, int32_t *col_span, int32_t *row_span);`
- Callback signatures: none.
- Ownership/lifetime: grid stores span metadata keyed by child plane pointer; removing a child invalidates its span record lazily on next layout pass.

### State Model

State includes: column strategy (fixed/track/auto-fit), gap/padding values, and a span map (`child -> {col_span,row_span}`). Runtime caches (resolved column widths, placement map) are ephemeral and recomputed on layout.

### Layout Contract

Layout is pixel-native. Sequence:
1. Resolve content bounds (already inset by `n00b_widget_layout`).
2. Resolve effective column count (fixed/auto-fit/tracks).
3. Build span-aware placement map in row-major order.
4. Resolve per-column widths (fixed + FR distribution + min/max clamps).
5. Resolve row heights from child `measure` results (content-driven, not equal-row fallback).
6. Call `n00b_widget_layout` on each child with computed cell bounds.

### Rendering Contract

Container render is non-visual (`n00b_plane_clear`). Grid lines are not drawn in Wave 1; visual separators are delegated to child boxes or a future debug overlay mode.

### Event And Focus Contract

`grid` is non-focusable and does not consume events. Child widgets receive events through normal hit testing and bubbling.

### Backend And Portability Notes

Do not use backend cell APIs for sizing logic. Track math and placement must remain in pixel units so it works for ANSI, notcurses, Cocoa, and X11. Convert to character-column semantics only when measuring text widgets (via `n00b_plane_text_columns`).

### Test And Demo Plan

- Unit test: three-column fixed grid with spans (`3x1` header + regular cells), assert child bounds and no overlap.
- Unit test: `FR` tracks with min/max clamps, assert deterministic width distribution for odd pixel remainders.
- Demo scenario: settings page layout with header spanning all columns and mixed-size controls.

### Open Questions

- Should Wave 1 support explicit row track definitions, or keep row sizing content-driven until Wave 2 forms land?
- Should hidden children reserve placement slots or be skipped during layout?

## split

### Prototype Behavior Snapshot

Prototype `split` divides space into two panes, supports horizontal/vertical orientation, ratio-based sizing with min constraints, and interactive divider dragging with mouse capture. Divider hover/active visual state is tracked separately.

Prototype evidence: `n00b-slop/include/ctui/widgets/split.h`, `n00b-slop/src/ctui/widgets/split.c`, `n00b-slop/test/ctui/test_split.c`.

Athens evidence used to shape this port: `n00b-athens/include/display/mouse.h`, `n00b-athens/src/display/mouse.c`, `n00b-athens/src/display/event_dispatch.c`, `n00b-athens/include/display/widget.h`.

Wave 1 dependency: `split` is needed early because many page shells combine `tabs` with sidebars or inspector panes.

### Athens API Proposal

- Header: `n00b-athens/include/display/widgets/split.h`.
- Data struct:
  - `typedef enum { N00B_SPLIT_HORIZONTAL, N00B_SPLIT_VERTICAL } n00b_split_orientation_t;`
  - `typedef void (*n00b_split_change_cb_t)(n00b_plane_t *split, float ratio, void *data);`
  - `typedef struct n00b_split_t { ... } n00b_split_t;`
- Vtable symbol: `extern const n00b_widget_vtable_t n00b_widget_split;`.
- Constructor:
  - `n00b_plane_t *n00b_split_new(n00b_plane_t *first, n00b_plane_t *second) _kargs { n00b_split_orientation_t orientation = N00B_SPLIT_HORIZONTAL; float ratio = 0.5f; int32_t min_first_px = 64; int32_t min_second_px = 64; int32_t divider_px = 1; n00b_split_change_cb_t on_change = nullptr; void *on_change_data = nullptr; n00b_canvas_t *canvas = nullptr; n00b_allocator_t *allocator = nullptr; };`
- Required mutators/accessors:
  - `void n00b_split_set_ratio(n00b_plane_t *split, float ratio);`
  - `float n00b_split_get_ratio(n00b_plane_t *split);`
  - `void n00b_split_set_first(n00b_plane_t *split, n00b_plane_t *first);`
  - `void n00b_split_set_second(n00b_plane_t *split, n00b_plane_t *second);`
  - `void n00b_split_set_min_sizes(n00b_plane_t *split, int32_t min_first_px, int32_t min_second_px);`
  - `void n00b_split_set_divider_size(n00b_plane_t *split, int32_t divider_px);`
- Callback signatures: `on_change` fires after drag or API ratio updates.
- Ownership/lifetime: split reparents panes but does not destroy pane allocations when replacing children.

### State Model

State fields: pane references (`first`, `second`), orientation, ratio, minimum sizes, divider size, hover flag, dragging flag, and drag anchor pixel offset. No dedicated divider plane; divider visuals are rendered by split itself.

### Layout Contract

For each layout pass:
1. Compute main-axis available size minus divider size.
2. Derive first/second sizes from ratio.
3. Clamp using min sizes (ratio is effectively constrained by min-size feasibility).
4. Layout both panes into resulting bounds.
5. Cache divider rect in local state for hit tests.

### Rendering Contract

Render clears split plane and draws divider using `n00b_plane_fill_rect` plus optional grip glyphs. Divider style can reflect `NORMAL`, `HOVER`, and `ACTIVE` (dragging) states via widget state style lookup.

### Event And Focus Contract

- `can_focus=false` (container focus remains on descendants).
- Mouse press in divider rect starts drag and calls `n00b_canvas_capture_mouse`.
- Drag updates ratio continuously and emits `on_change`.
- Release ends drag and calls `n00b_canvas_release_mouse`.
- Mouse events outside divider bubble to children.

### Backend And Portability Notes

Prototype used explicit notcurses divider planes. Athens split must avoid backend-owned planes and rely on generic draw commands and mouse routing. Coordinate math remains in pixels; terminal backends are already quantized by mouse routing logic.

### Test And Demo Plan

- Unit test: drag divider from center to right, assert ratio change and callback fire count.
- Unit test: min-size clamp with narrow container; assert neither pane drops below minimum.
- Integration test: port `test_split.c` behavior into Athens display harness and verify no crash during repeated drag cycles.

### Open Questions

- Should keyboard resize bindings (for accessibility) ship in Wave 1 or Wave 2?
- Should divider hover state use `N00B_WSTATE_HOVER` on the split plane or remain internal-only?

## scroll

### Prototype Behavior Snapshot

Prototype `scroll` is a single-child viewport widget with axis flags, scrollbar modes, wheel/keyboard scrolling, dragable vertical scrollbar thumb, and helpers like `ensure_visible`. It currently hides child planes and copies visible cells into a viewport plane (`copy_visible_content`), which is tightly coupled to notcurses internals.

Prototype evidence: `n00b-slop/include/ctui/widgets/scroll.h`, `n00b-slop/src/ctui/widgets/scroll.c`, `n00b-slop/test/ctui/test_scroll.c`, `n00b-slop/test/ctui/test_theme_scroll.c`.

Athens evidence used to shape this port: `n00b-athens/include/display/render/plane.h`, `n00b-athens/include/display/mouse.h`, `n00b-athens/src/display/mouse.c`, `n00b-athens/src/display/event_dispatch.c`.

Wave 1 dependency: `scroll` is required for practical `tabs` pages and for long-form `text` content.

### Athens API Proposal

- Header: `n00b-athens/include/display/widgets/scroll.h`.
- Data struct:
  - `typedef enum { N00B_SCROLL_AXIS_NONE = 0, N00B_SCROLL_AXIS_VERTICAL = 1 << 0, N00B_SCROLL_AXIS_HORIZONTAL = 1 << 1, N00B_SCROLL_AXIS_BOTH = 3 } n00b_scroll_axis_t;`
  - `typedef enum { N00B_SCROLLBAR_AUTO, N00B_SCROLLBAR_ALWAYS, N00B_SCROLLBAR_NEVER } n00b_scrollbar_mode_t;`
  - `typedef struct n00b_scroll_t { ... } n00b_scroll_t;`
- Vtable symbol: `extern const n00b_widget_vtable_t n00b_widget_scroll;`.
- Constructor:
  - `n00b_plane_t *n00b_scroll_new(n00b_plane_t *content) _kargs { n00b_scroll_axis_t axes = N00B_SCROLL_AXIS_VERTICAL; n00b_scrollbar_mode_t scrollbar_mode = N00B_SCROLLBAR_AUTO; int32_t scroll_step_lines = 3; int32_t scrollbar_thickness_px = 1; n00b_canvas_t *canvas = nullptr; n00b_allocator_t *allocator = nullptr; };`
- Required mutators/accessors:
  - `void n00b_scroll_set_content(n00b_plane_t *scroll, n00b_plane_t *content);`
  - `n00b_plane_t *n00b_scroll_get_content(n00b_plane_t *scroll);`
  - `void n00b_scroll_to(n00b_plane_t *scroll, int32_t x_px, int32_t y_px);`
  - `void n00b_scroll_by(n00b_plane_t *scroll, int32_t dx_px, int32_t dy_px);`
  - `void n00b_scroll_to_top(n00b_plane_t *scroll);`
  - `void n00b_scroll_to_bottom(n00b_plane_t *scroll);`
  - `void n00b_scroll_to_start(n00b_plane_t *scroll);`
  - `void n00b_scroll_to_end(n00b_plane_t *scroll);`
  - `void n00b_scroll_ensure_visible(n00b_plane_t *scroll, n00b_rect_t rect_px);`
  - `int32_t n00b_scroll_get_offset_x(n00b_plane_t *scroll);`
  - `int32_t n00b_scroll_get_offset_y(n00b_plane_t *scroll);`
  - `bool n00b_scroll_can_scroll_up/down/left/right(n00b_plane_t *scroll);`
- Callback signatures: none in Wave 1.
- Ownership/lifetime: scroll reparents one content child; replacing content detaches old child but does not destroy allocation.

### State Model

State includes viewport size, content size, offset x/y in pixels, axis/mode settings, scrollbar drag state, and cached scrollbar thumb rectangles for hit testing.

### Layout Contract

- Layout content child at its measured content size (unbounded on scroll-enabled axes, bounded on disabled axes).
- Compute viewport from assigned bounds minus visible scrollbar thickness.
- Clamp offsets to `[0, content - viewport]`.
- Position content child at `(-offset_x, -offset_y)` in scroll-content coordinates so compositor clipping does the visibility work.

### Rendering Contract

Scroll widget renders only overlays: scrollbar tracks/thumbs and optional edge indicators. It must not copy backend cells or hide/move child planes offscreen. The child subtree remains a normal plane tree.

### Event And Focus Contract

- `can_focus=true` to support keyboard scrolling.
- Arrow keys and page keys move by line/page increments (line height from `n00b_widget_line_px_height`).
- `Ctrl+Home/End` maps to top/bottom.
- Wheel scrolls vertically; `Shift+wheel` scrolls horizontally.
- Dragging scrollbar thumb uses mouse capture API.
- Mouse events in content area translate by offset before dispatching to content child.

### Backend And Portability Notes

Eliminate prototype notcurses coupling (`hide_content_planes`, `copy_visible_content`, `ncplane_at_yx_cell`). Athens compositor already clips in a backend-neutral way and supports both terminal and GUI targets. This is required for parity across ANSI/notcurses/Cocoa/X11.

### Test And Demo Plan

- Port logic checks from `test_scroll.c` (offset clamping, `ensure_visible`, direction predicates) as Athens unit tests.
- Integration test: long content inside scroll in both ANSI and stream backends, verify scrollbar visibility and coordinate translation.
- Regression scenario from `test_theme_scroll.c`: keep content stable under repeated redraw/animation cycles.

### Open Questions

- Should Wave 1 include horizontal thumb dragging, or keep drag support vertical-only initially?
- Do we need explicit public API for programmatic scrollbar style customization in Wave 1?

## tabs

### Prototype Behavior Snapshot

Prototype `tabs` renders a one-row header and shows one tab content subtree at a time. Selection is changed by left/right keys or header clicks. Current implementation repurposes `nctabbed` as selected index and destroys/recreates content planes on tab switches, which caused instability workarounds in tab-switch tests.

Prototype evidence: `n00b-slop/include/ctui/widgets/tabs.h`, `n00b-slop/src/ctui/widgets/tabs.c`, `n00b-slop/test/ctui/test_tabs.c`, `n00b-slop/test/ctui/test_tabs_switch.c`.

Athens evidence used to shape this port: `n00b-athens/src/display/mouse.c`, `n00b-athens/src/display/focus.c`, `n00b-athens/include/display/widget.h`, `n00b-athens/include/display/render/plane.h`.

Wave 1 dependency: `tabs` depends on stable container behavior from `zstack/grid` and benefits directly from `scroll` for tab content with overflow.

### Athens API Proposal

- Header: `n00b-athens/include/display/widgets/tabs.h`.
- Data struct:
  - `typedef enum { N00B_TABS_TOP, N00B_TABS_BOTTOM } n00b_tabs_position_t;`
  - `typedef struct n00b_tab_entry_t { n00b_string_t *name; n00b_plane_t *content; } n00b_tab_entry_t;`
  - `typedef void (*n00b_tabs_select_cb_t)(n00b_plane_t *tabs, int new_index, int old_index, void *data);`
  - `typedef struct n00b_tabs_t { ... } n00b_tabs_t;`
- Vtable symbol: `extern const n00b_widget_vtable_t n00b_widget_tabs;`.
- Constructor:
  - `n00b_plane_t *n00b_tabs_new() _kargs { n00b_tabs_position_t position = N00B_TABS_TOP; n00b_string_t *separator = nullptr; n00b_tabs_select_cb_t on_select = nullptr; void *on_select_data = nullptr; n00b_canvas_t *canvas = nullptr; n00b_allocator_t *allocator = nullptr; };`
- Required mutators/accessors:
  - `int n00b_tabs_add(n00b_plane_t *tabs, n00b_string_t *name, n00b_plane_t *content);`
  - `bool n00b_tabs_remove(n00b_plane_t *tabs, int index);`
  - `bool n00b_tabs_select_index(n00b_plane_t *tabs, int index);`
  - `int n00b_tabs_selected_index(n00b_plane_t *tabs);`
  - `int n00b_tabs_count(n00b_plane_t *tabs);`
  - `n00b_tab_entry_t *n00b_tabs_get(n00b_plane_t *tabs, int index);`
  - `bool n00b_tabs_next(n00b_plane_t *tabs);`
  - `bool n00b_tabs_prev(n00b_plane_t *tabs);`
- Callback signatures: `on_select` is required to be functional in Wave 1 (unlike prototype TODO).
- Ownership/lifetime: tabs manages parent links for all tab content planes. Switching tabs toggles visibility/active child state without destroying planes.

### State Model

State includes tab entries, selected index, header position, header separator string, and cached per-tab header hit regions. Selected index is explicit integer state (not overloaded pointer field).

### Layout Contract

Header height is one text line (`n00b_widget_line_px_height`). Remaining bounds are assigned to selected content pane. Unselected panes remain parented but hidden (`N00B_PLANE_VISIBLE` false) so state is preserved without plane re-creation.

### Rendering Contract

Tabs draws only the header strip (labels + separators + selected style). Content rendering is delegated to selected child subtree. Header style should use theme palette and focused/active states.

### Event And Focus Contract

- `can_focus=true` for keyboard navigation.
- Left/right keys cycle with wrap-around.
- Mouse left press on header hitbox selects clicked tab.
- On selection change, emit `on_select` callback and request focus-manager rebuild if selected content focusability changes.

### Backend And Portability Notes

Do not depend on notcurses `nctabbed` or tab-plane destruction. Athens must keep selection logic purely in widget state and plain plane visibility to avoid backend-specific crashes and simplify replay/testing backends.

### Test And Demo Plan

- Port `test_tabs.c` navigation checks: startup selection, right/left navigation, wrap-around.
- Add Athens regression test equivalent to `test_tabs_switch.c` that repeatedly clicks between two tabs and asserts no crash/leak.
- Demo scenario: `tabs` with one static page and one scrollable page (`scroll + text`) to validate cross-widget integration.

### Open Questions

- Should hidden tab panes keep focus if they were focused before a tab switch, or should focus always move to the tab header/select target?
- Should tab removal auto-select previous tab or nearest remaining tab to the right?

## text

### Prototype Behavior Snapshot

Prototype `text` renders wrapped multi-line text with alignment and optional hanging indent. It tracks selection in wrapped-line coordinates, supports drag selection, extracts selected UTF-8 slice text, and copies selection through clipboard helper.

Prototype evidence: `n00b-slop/include/ctui/widgets/text.h`, `n00b-slop/src/ctui/widgets/text.c`.

Athens evidence used to shape this port: `n00b-athens/src/display/widgets/label.c`, `n00b-athens/include/display/render/plane.h`, `n00b-athens/include/display/event.h`, `n00b-athens/src/display/event_dispatch.c`.

Wave 1 dependency: `text` should be compatible with `scroll` viewport behavior because most long-form text usage is expected inside scroll containers.

### Athens API Proposal

- Header: `n00b-athens/include/display/widgets/text.h`.
- Data struct:
  - `typedef struct n00b_text_selection_t { int32_t start_line; int32_t start_col; int32_t end_line; int32_t end_col; bool active; } n00b_text_selection_t;`
  - `typedef struct n00b_text_t { ... } n00b_text_t;`
- Vtable symbol: `extern const n00b_widget_vtable_t n00b_widget_text;`.
- Constructor:
  - `n00b_plane_t *n00b_text_new(n00b_string_t *text) _kargs { n00b_alignment_t alignment = N00B_ALIGN_LEFT; bool wrap = true; int32_t hang_indent_cols = 0; bool selectable = false; bool copy_on_release = true; n00b_canvas_t *canvas = nullptr; n00b_allocator_t *allocator = nullptr; };`
- Required mutators/accessors:
  - `void n00b_text_set_text(n00b_plane_t *text_plane, n00b_string_t *text);`
  - `n00b_string_t *n00b_text_get_text(n00b_plane_t *text_plane);`
  - `void n00b_text_set_alignment(n00b_plane_t *text_plane, n00b_alignment_t alignment);`
  - `void n00b_text_set_hang_indent(n00b_plane_t *text_plane, int32_t hang_indent_cols);`
  - `void n00b_text_set_selectable(n00b_plane_t *text_plane, bool selectable);`
  - `bool n00b_text_has_selection(n00b_plane_t *text_plane);`
  - `void n00b_text_clear_selection(n00b_plane_t *text_plane);`
  - `n00b_string_t *n00b_text_get_selection(n00b_plane_t *text_plane);`
  - `bool n00b_text_copy_selection(n00b_plane_t *text_plane);`
  - `int32_t n00b_text_get_wrapped_line_count(n00b_plane_t *text_plane);`
- Callback signatures: none required for Wave 1.
- Ownership/lifetime: text widget stores references to immutable `n00b_string_t`; wrapped-line cache is widget-owned and invalidated on text/width/style change.

### State Model

State includes source text, cached wrapped line slices, cache width key (columns), alignment/hanging indent config, selectable flag, and active selection range in wrapped-line coordinates.

### Layout Contract

Width-sensitive wrapping uses Unicode line breaking (`n00b_unicode_linebreak_wrap`) with width in text columns derived from pixel width (`n00b_plane_text_columns`). Preferred height is `line_count * line_height`. Cache invalidates whenever content width or text changes.

### Rendering Contract

Render uses `n00b_plane_draw_text` for each wrapped line. Selected ranges are rendered with explicit highlight style overlays (inverse or theme selection colors). Rendering avoids backend-specific plane cell writes.

### Event And Focus Contract

- `can_focus` returns `true` only when `selectable`.
- Mouse press/drag/release updates selection range.
- `Ctrl+C` copies active selection (`n00b_text_copy_selection`); this intentionally narrows prototype behavior (which also accepted Alt/Cmd-mapped path) to reduce accidental copy triggers.
- Non-selection events bubble upward.

### Backend And Portability Notes

Clipboard support must be abstracted behind Athens runtime APIs so ANSI/notcurses/Cocoa/X11 can provide backend-appropriate implementations. Widget logic should not emit backend escape sequences directly.

### Test And Demo Plan

- Unit test: wrap identical UTF-8 paragraph at two widths, assert deterministic wrapped line counts and selection extraction boundaries.
- Unit test: drag selection over multi-line content, call `get_selection`, assert expected newline-joined substring.
- Integration scenario: `text` inside `scroll`; wheel scroll + selection + `Ctrl+C` should preserve offsets and not crash.

### Open Questions

- Should Wave 1 expose a read-only mode that is focusable for keyboard copy but ignores mouse selection edits?
- Should selection coordinates be stored as codepoint indices only (instead of line/col pairs) for easier future bidi handling?
