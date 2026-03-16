# Reimplement The Wave 1 Scroll Widget In Production

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

There is no repository-local `PLANS.md` in this working tree at the time this plan was authored. This document follows `/home/baron/.codex/PLANS.md` and must remain compliant with that file for all future revisions.

This plan explicitly builds on `plans/notes/widget-port-priority.md`, `plans/notes/widget-wave1-design-breakdown.md`, and `docs/widgets.md`. Those files establish that after the production `zstack`, `grid`, and `split` ports landed, the next missing widget in the Wave 1 queue is `scroll`.

## Purpose / Big Picture

The goal is to land the next production Wave 1 widget in Athens: a backend-neutral `scroll` container that can host one content subtree, clip it to a viewport, scroll by wheel and keyboard, render theme-aware scrollbars, and support vertical thumb dragging without relying on notcurses-only cell-copy tricks. After this change, an Athens application will be able to show long forms, tab pages, article text, and tall settings panels inside a real scroll viewport instead of baking ad hoc per-widget scrolling into every leaf control. A contributor or reviewer should be able to prove the result in two ways: run a dedicated `scroll` unit test that exercises layout math, offset clamping, `ensure_visible`, wheel/key handling, thumb dragging, and scrolled-content click routing, and run `widget_demo --widget scroll` to interact with a long page whose internal buttons remain clickable after scrolling.

## Progress

- [x] (2026-03-15 01:19Z) Reviewed the Wave 1 queue, the checked-in scroll design dossier, the prototype `scroll` implementation and tests in `n00b-slop`, and the current Athens widget, mouse, compositor, theme, and demo patterns.
- [x] (2026-03-15 01:19Z) Identified the Athens runtime seams that shape this port: dirty rerendering does not rerun layout on ordinary input, click-to-focus does not walk up to focusable ancestors, compositor clipping already handles oversized child subtrees, and `plane->scroll_x/y` affect rendering but not hit-testing.
- [x] (2026-03-15 01:19Z) Resolved the initial open planning decisions left by the scroll dossier: Wave 1 uses the existing theme palette instead of a new public style API, offset-changing setters relayout immediately when bounds are known, and keyboard scrolling is validated through Tab focus rather than a global click-focus behavior change.
- [x] (2026-03-15 01:19Z) Drafted this self-contained execution plan for the production `scroll` reimplementation.
- [x] (2026-03-15 01:59Z) Implemented `include/display/widgets/scroll.h` and `src/display/widgets/scroll.c` with the public API, deterministic measurement, fixed-point scrollbar visibility, theme-aware scrollbar rendering, wheel/key/track handling, vertical-thumb dragging, immediate relayout on offset changes, and a hidden viewport wrapper plane that keeps child clipping and scrollbar chrome aligned under the current compositor order.
- [x] (2026-03-15 01:59Z) Added `test/unit/test_scroll.c`, registered the Meson target, extended `widget_demo --widget scroll`, and updated `docs/widgets.md` so `scroll` moves into the implemented set.
- [x] (2026-03-15 01:59Z) Reconfigured `build_debug`, compiled `test_scroll` and `widget_demo`, passed `meson test -C build_debug --print-errorlogs scroll`, and passed `./build_debug/widget_demo --widget scroll --backend stream`.
- [x] (2026-03-15 02:06Z) Extended the production widget and unit suite to support horizontal thumb dragging on the bottom scrollbar, rebuilt `test_scroll` and `widget_demo`, reran the `scroll` Meson test target, and reran the stream demo smoke.
- [ ] (2026-03-15 01:59Z) Full human interactive verification of `./build_debug/widget_demo --widget scroll --backend tui` remains. This session completed only an automated two-second startup smoke under `timeout`, which proved startup and teardown but not the full wheel/key/drag/button workflow.

## Surprises & Discoveries

- Observation: Prototype `scroll` depends on hiding content planes and copying visible cells into a viewport plane, which is tightly coupled to notcurses and cannot survive the Athens multi-backend model.
  Evidence: `../n00b-slop/src/ctui/widgets/scroll.c` uses `hide_content_planes()`, `copy_visible_content()`, and `ncplane_at_yx_cell()`.

- Observation: Athens rerenders dirty widgets after ordinary input, but it only reruns layout on startup, resize, or backend cell-metric changes.
  Evidence: `src/display/event_loop.c` calls `n00b_display_scene_run_layout(canvas)` during initial render and resize handling, while ordinary input only reaches `n00b_display_scene_rerender_dirty(canvas)` before `n00b_canvas_render(canvas)`.

- Observation: Athens compositor clipping already handles oversized child subtrees correctly, but `plane->scroll_x` and `plane->scroll_y` are render-only offsets and do not participate in mouse hit-testing.
  Evidence: `src/display/render/composite.c` subtracts `p->scroll_x` and `p->scroll_y` while painting draw commands, while `src/display/mouse.c` computes hit regions from plane bounds and `x`/`y` without reading scroll offsets.

- Observation: Click-to-focus currently targets only the exact hit plane, not the nearest focusable ancestor.
  Evidence: `src/display/mouse.c` focuses `target` only when `n00b_widget_can_focus(target)` is true and does not walk `target->parent`.

- Observation: The theme system already exposes dedicated palette slots for scrollbars, so Wave 1 can ship with coherent styling without inventing a new public scrollbar-style surface.
  Evidence: `include/text/strings/theme.h` defines `N00B_PAL_SCROLLBAR_TRACK`, `N00B_PAL_SCROLLBAR_THUMB`, and `N00B_PAL_SCROLLBAR_THUMB_HOVER`, and `src/text/strings/theme.c` supplies values for those slots across built-in themes.

- Observation: The checked-in `canvas->focus` pointer is documented as event-loop-owned, but the current event loop does not populate it, so runtime widget setters cannot safely depend on a canvas-resident focus manager today.
  Evidence: `include/display/render/canvas.h` documents `focus`, while `src/display/event_loop.c` creates `n00b_focus_mgr_t *fm` as a local variable and never assigns `canvas->focus = fm`.

- Observation: Parent planes are composited before their descendants, so drawing scrollbar chrome directly on the scroll plane only works if the scrolled content is clipped by a smaller intermediate viewport plane.
  Evidence: `src/display/render/composite.c` appends the parent entry to the flattened painter-order list before recursing into `p->children`, and the production demo rendered correctly only after the scroll implementation inserted a hidden viewport wrapper child that reserves the scrollbar gutter.

## Decision Log

- Decision: The next Wave 1 production widget after `zstack`, `grid`, and `split` is `scroll`, and this plan targets that widget only.
  Rationale: `plans/notes/widget-port-priority.md` orders Wave 1 as `stack`, `grid`, `split`, `scroll`, `tabs`, `text`, and `docs/widgets.md` now lists `grid`, `split`, and `zstack` as implemented while `scroll` remains in the Wave 1 backlog.
  Date/Author: 2026-03-15 / Codex.

- Decision: Scrollbar styling will use the existing theme palette roles instead of a new public scrollbar-style configuration API.
  Rationale: Athens already has semantic palette entries for scrollbar track, thumb, and hover colors, and adding a style API here would enlarge scope before the basic widget contract is proven.
  Date/Author: 2026-03-15 / Codex.

- Decision: Offset-changing public APIs and drag/wheel/key handlers must relayout the widget immediately by calling `n00b_widget_layout(scroll, scroll->bounds)` when valid bounds are already known.
  Rationale: The Athens event loop does not rerun layout for ordinary input, so content position, viewport size, and thumb rectangles would otherwise remain stale until an unrelated resize.
  Date/Author: 2026-03-15 / Codex.

- Decision: The production scroll path will move the content child to negative absolute coordinates rather than using `n00b_plane_scroll_to()` on the content plane.
  Rationale: Negative child placement keeps compositor clipping and mouse hit-testing aligned, while `plane->scroll_x/y` currently influence rendering only and would make clicks land in unshifted coordinates.
  Date/Author: 2026-03-15 / Codex.

- Decision: Scroll measurement will cap scroll-enabled axes to a prototype-inspired default viewport size of `20 * cell_width` by `3 * line_height`, while non-scroll axes continue to use the content widget's preferred size.
  Rationale: A scroll container should not demand its full content size as natural size, but Athens still needs a deterministic, width-agnostic measure contract because `n00b_widget_measure()` has no available-width parameter.
  Date/Author: 2026-03-15 / Codex.

- Decision: Auto scrollbar visibility will be resolved with a small fixed-point loop that recomputes visibility after subtracting the other scrollbar's thickness.
  Rationale: A vertical bar can cause horizontal overflow and vice versa, so a one-pass visibility check would be visibly wrong for some two-axis scenes.
  Date/Author: 2026-03-15 / Codex.

- Decision: Wave 1 keeps the existing global click-to-focus semantics unchanged, and keyboard scrolling acceptance is defined around Tab focus on the scroll container.
  Rationale: Focusing the nearest focusable ancestor would be a repository-wide behavior change that is useful but not necessary to ship the next production widget.
  Date/Author: 2026-03-15 / Codex.

- Decision: The production scroll widget wraps the public content subtree in an internal viewport child plane instead of attaching the content directly to the scroll plane.
  Rationale: Athens composites parent draw commands before child draw commands, so parent-drawn scrollbars would otherwise be overpainted by wide or tall child content. The hidden viewport plane keeps the content clipped to the viewport while the scroll plane still owns the scrollbar chrome and mouse/key behavior, with no shared-runtime changes.
  Date/Author: 2026-03-15 / Codex.

- Decision: Wave 1 scroll supports dragging both scrollbar thumbs when the corresponding axis overflows.
  Rationale: Once the public demo and widget existed, the lack of bottom-thumb dragging was a clear user-facing inconsistency rather than a useful scope cut. The incremental implementation cost was small because the horizontal path can reuse the same capture, anchor, and proportional-offset math as the vertical path.
  Date/Author: 2026-03-15 / Codex.

## Outcomes & Retrospective

As of 2026-03-15 the production `scroll` widget, its unit suite, and its demo entrypoint are implemented in Athens. The new unit test covers constructor defaults, measure caps, offset clamping, `ensure_visible`, fixed-point scrollbar visibility, keyboard scrolling, vertical thumb dragging, and translated hit-testing into scrolled content. `docs/widgets.md` now lists `scroll` as implemented, and `widget_demo --widget scroll --backend stream` plus a timed `tui` startup smoke both completed without crashes.

The main lesson from execution is that backend portability was not the hard part; compositor ordering was. The implementation needed one hidden viewport wrapper plane so parent-drawn scrollbar chrome stays visible while the content subtree remains a normal translated child tree. The main remaining gap is operational rather than structural: the full manual `tui` interaction pass still needs a human operator to verify the complete wheel, key, drag, and in-content button workflow end to end.

## Context and Orientation

Athens widgets are planes with attached behavior, not separate widget objects. `include/display/widget.h` defines the `n00b_widget_vtable_t` contract, and `src/display/widget.c` handles attachment, measurement, layout, and event dispatch. A container widget receives content-space bounds through `n00b_widget_layout()`, which already subtracts any outer box border and padding before calling the widget-specific `layout` callback.

A scroll container is a viewport over one child plane subtree. In plain language, the "viewport" is the visible rectangle the user can see right now. The "content size" is the full pixel size of the child subtree before clipping. A "track" is the full scrollbar rail, and a "thumb" is the movable handle that shows the current visible slice of the content. `AUTO` scrollbar mode means "show only when content is larger than the viewport". `ALWAYS` means "reserve scrollbar space even when content fits". `NEVER` means "never draw scrollbars, but still allow scrolling through APIs or input when overflow exists".

The prototype source of truth is `../n00b-slop/include/ctui/widgets/scroll.h`, `../n00b-slop/src/ctui/widgets/scroll.c`, `../n00b-slop/test/ctui/test_scroll.c`, and `../n00b-slop/test/ctui/test_theme_scroll.c`. The authoritative Athens contract is the `## scroll` section in `plans/notes/widget-wave1-design-breakdown.md`. The current Athens runtime pieces this widget depends on are:

- `include/display/render/plane.h` and `src/display/render/plane.c` for plane hierarchy, absolute layout bounds, and manual mouse capture.
- `src/display/mouse.c` for hit-testing and bubbling mouse dispatch in plane-local coordinates.
- `src/display/render/composite.c` for clip propagation through parent planes.
- `src/display/event_loop.c` and `src/display/event_dispatch.c` for the fact that ordinary input rerenders dirty widgets but does not rerun layout.
- `src/display/widgets/widget_primitives.c` for `n00b_widget_cell_px_width()` and `n00b_widget_line_px_height()`.
- `include/text/strings/theme.h` and `src/text/strings/theme.c` for scrollbar palette colors.
- `src/tools/widget_demo.c`, `docs/widgets.md`, and `meson.build` for demo, roadmap, and build wiring.

The implementation does not require any backend-specific plane copying or shared-runtime mouse/compositor changes. The scroll widget owns the viewport math, cached thumb rectangles, and input handling. The production code keeps the public content subtree wrapped in one hidden internal viewport plane so the compositor clips translated content before it reaches the scrollbar gutter. The other important Athens-specific compromise is measurement: because there is no width hint in `n00b_widget_measure()`, the widget reports a deterministic "reasonable viewport" size rather than trying to predict every future layout context.

## Plan of Work

Milestone 1 adds the public widget surface and the production implementation. Create `include/display/widgets/scroll.h` and `src/display/widgets/scroll.c`. The header must expose the Wave 1 axis and scrollbar-mode enums, the `n00b_scroll_t` state record, the public constructor, the content setter/getter, offset setters, `ensure_visible`, offset getters, and the directional predicate helpers. The source file must implement the vtable callbacks, immediate-relayout public setters, fixed-point scrollbar visibility resolution, an internal viewport wrapper plane plus negative child placement, cached track/thumb rectangles, theme-aware overlay rendering, key and wheel scrolling, track clicks, and thumb dragging with mouse capture on both axes. No shared runtime edits are planned for this milestone.

Milestone 2 adds behavior-driven tests. Create `test/unit/test_scroll.c` following the structure of `test_split.c` and `test_grid.c`: small test-only widgets with controlled preferred and minimum sizes, captured layout bounds, and simple click counters. The tests must prove the public API shape, measurement heuristic, offset clamping, `ensure_visible`, auto scrollbar visibility, thumb rectangle math, immediate relayout after offset changes, keyboard scrolling, thumb dragging on both axes, and correct click routing into scrolled content after the content child has been offset.

Milestone 3 ships the user-visible proof and updates repository status. Extend `src/tools/widget_demo.c` with `demo_scroll()`, a `--widget scroll` dispatch branch, and an interactive scene that demonstrates both overflow axes and content interaction after scrolling. Update `docs/widgets.md` so `scroll` moves from the Wave 1 backlog into the implemented set and `test_scroll` is added to the representative widget-test sentence. Then run the validation matrix from this plan and record the resulting evidence in the living sections above.

## Concrete Steps

Run commands from `/home/baron/crash-override/n00b-tui/n00b-athens`.

1. Refresh the approved scroll design and the runtime seams before editing code:

       sed -n '/## scroll/,/## tabs/p' plans/notes/widget-wave1-design-breakdown.md
       sed -n '300,940p' ../n00b-slop/src/ctui/widgets/scroll.c
       sed -n '1,220p' src/display/event_loop.c
       sed -n '1,340p' src/display/mouse.c
       sed -n '1,220p' include/text/strings/theme.h
       sed -n '1,220p' src/display/widgets/widget_primitives.c

   Expected result: you can see the Wave 1 public API, the prototype's cell-copy viewport path, the Athens rerender-without-layout event loop, the current mouse hit-test rules, the available scrollbar palette roles, and the pixel-to-cell helpers.

2. Edit `meson.build` in two places. Add `src/display/widgets/scroll.c` to the main display source list beside `grid.c`, `split.c`, and `zstack.c`. Add a new `scroll_test = executable('test_scroll', ['test/unit/test_scroll.c'], kwargs: test_common_kwargs)` plus `test('scroll', scroll_test, suite: 'unit')` near the existing widget tests.

3. Create `include/display/widgets/scroll.h` with the public contract below. Match the repository's normal widget-header style, including `#pragma once`, the shared includes, and the `_kargs` constructor declaration.

   The header must declare exactly these public enums and functions:

       typedef enum {
           N00B_SCROLL_AXIS_NONE       = 0,
           N00B_SCROLL_AXIS_VERTICAL   = 1 << 0,
           N00B_SCROLL_AXIS_HORIZONTAL = 1 << 1,
           N00B_SCROLL_AXIS_BOTH       = 3,
       } n00b_scroll_axis_t;

       typedef enum {
           N00B_SCROLLBAR_AUTO,
           N00B_SCROLLBAR_ALWAYS,
           N00B_SCROLLBAR_NEVER,
       } n00b_scrollbar_mode_t;

       typedef struct n00b_scroll_t {
           n00b_plane_t          *content;
           int32_t                offset_x;
           int32_t                offset_y;
           int32_t                content_width;
           int32_t                content_height;
           int32_t                viewport_width;
           int32_t                viewport_height;
           n00b_scroll_axis_t     axes;
           n00b_scrollbar_mode_t  scrollbar_mode;
           int32_t                scroll_step_lines;
           int32_t                scrollbar_thickness_px;
           n00b_rect_t            vscrollbar_rect;
           n00b_rect_t            hscrollbar_rect;
           n00b_rect_t            vthumb_rect;
           n00b_rect_t            hthumb_rect;
           bool                   show_vscrollbar;
           bool                   show_hscrollbar;
           bool                   dragging_vertical_thumb;
           bool                   hover_vertical_thumb;
           bool                   hover_horizontal_thumb;
           int32_t                drag_anchor_px;
           int32_t                drag_anchor_offset_px;
           n00b_text_style_t     *track_style;
           n00b_text_style_t     *thumb_style;
           n00b_text_style_t     *thumb_hover_style;
       } n00b_scroll_t;

       extern const n00b_widget_vtable_t n00b_widget_scroll;

       extern n00b_plane_t *
       n00b_scroll_new(n00b_plane_t *content) _kargs {
           n00b_scroll_axis_t    axes                   = N00B_SCROLL_AXIS_VERTICAL;
           n00b_scrollbar_mode_t scrollbar_mode         = N00B_SCROLLBAR_AUTO;
           int32_t               scroll_step_lines      = 3;
           int32_t               scrollbar_thickness_px = 1;
           n00b_canvas_t        *canvas                 = nullptr;
           n00b_allocator_t     *allocator              = nullptr;
       };

       extern void      n00b_scroll_set_content(n00b_plane_t *scroll, n00b_plane_t *content);
       extern n00b_plane_t *n00b_scroll_get_content(n00b_plane_t *scroll);
       extern void      n00b_scroll_to(n00b_plane_t *scroll, int32_t x_px, int32_t y_px);
       extern void      n00b_scroll_by(n00b_plane_t *scroll, int32_t dx_px, int32_t dy_px);
       extern void      n00b_scroll_to_top(n00b_plane_t *scroll);
       extern void      n00b_scroll_to_bottom(n00b_plane_t *scroll);
       extern void      n00b_scroll_to_start(n00b_plane_t *scroll);
       extern void      n00b_scroll_to_end(n00b_plane_t *scroll);
       extern void      n00b_scroll_ensure_visible(n00b_plane_t *scroll, n00b_rect_t rect_px);
       extern int32_t   n00b_scroll_get_offset_x(n00b_plane_t *scroll);
       extern int32_t   n00b_scroll_get_offset_y(n00b_plane_t *scroll);
       extern bool      n00b_scroll_can_scroll_up(n00b_plane_t *scroll);
       extern bool      n00b_scroll_can_scroll_down(n00b_plane_t *scroll);
       extern bool      n00b_scroll_can_scroll_left(n00b_plane_t *scroll);
       extern bool      n00b_scroll_can_scroll_right(n00b_plane_t *scroll);

4. Create `src/display/widgets/scroll.c`. Keep all layout, visibility, and thumb helpers file-local. The file must define `scroll_destroy`, `scroll_render`, `scroll_measure`, `scroll_handle_event`, `scroll_can_focus`, and `scroll_layout` plus private helpers that keep the setter paths and the layout path on the same geometry logic.

   Implement the public mutators with these exact semantics:

   `n00b_scroll_new()` allocates the plane and `n00b_scroll_t` record, clamps `scroll_step_lines` and `scrollbar_thickness_px` to at least `1`, allocates three persistent style records (`track_style`, `thumb_style`, `thumb_hover_style`), attaches the widget, parents the optional initial content if provided, and marks the plane dirty.

   `scroll_destroy()` releases mouse capture if this plane currently owns it, frees the three style records, frees the `n00b_scroll_t` record itself, and nothing else.

   `n00b_scroll_set_content()` detaches the old content child without destroying it, requires `content == nullptr || content->parent == nullptr`, parents the new content when provided, clears drag and hover flags, resets both offsets to zero, relayouts immediately when `plane->bounds` is valid, and otherwise only marks the scroll plane dirty.

   `n00b_scroll_to()` and `n00b_scroll_by()` update offsets in pixels, clamp them against the current content and viewport sizes, relayout immediately when bounds are known, and otherwise mark the plane dirty.

   `n00b_scroll_to_top()`, `n00b_scroll_to_bottom()`, `n00b_scroll_to_start()`, `n00b_scroll_to_end()`, and `n00b_scroll_ensure_visible()` must all reuse the same clamp-and-relayout helper rather than duplicating geometry logic.

   `n00b_scroll_get_offset_x()` and `n00b_scroll_get_offset_y()` return `0` for null or wrong-kind planes. The directional predicate helpers return `false` when the current offset is already at the corresponding edge.

5. Use the following layout pipeline inside `src/display/widgets/scroll.c`.

   First, measure the content child when it exists. Let `content_pref_w`, `content_pref_h`, `content_min_w`, and `content_min_h` come from `n00b_widget_measure(content, ...)`. If there is no content child, treat the natural content size as `1x1`.

   Second, resolve the content size before scrollbar subtraction:

   - If horizontal scrolling is enabled, set `content_width = max(content_pref_w, bounds.width)`.
   - If horizontal scrolling is disabled, set `content_width = bounds.width`.
   - If vertical scrolling is enabled, set `content_height = max(content_pref_h, bounds.height)`.
   - If vertical scrolling is disabled, set `content_height = bounds.height`.

   Third, resolve scrollbar visibility with a two-pass fixed-point loop. Start with `show_vscrollbar = false` and `show_hscrollbar = false`. On each pass compute tentative viewport size by subtracting `scrollbar_thickness_px` for any scrollbar already marked visible, clamp the viewport to at least `1x1`, then recompute:

   - `show_vscrollbar` is allowed only when `axes` includes vertical.
   - `show_hscrollbar` is allowed only when `axes` includes horizontal.
   - `ALWAYS` forces the corresponding scrollbar visible.
   - `NEVER` forces it hidden.
   - `AUTO` shows it only when `content_height > viewport_height` for vertical or `content_width > viewport_width` for horizontal.

   Loop until the visibility bits stop changing or two passes have completed. Then recompute the final viewport size one last time from the stable visibility bits.

   Fourth, ensure `content_width >= viewport_width` and `content_height >= viewport_height`, clamp offsets into `[0, content - viewport]`, lay out the internal viewport wrapper plane at `bounds`, and then lay out the content child at absolute bounds:

       child_bounds = {
           .x = bounds.x - offset_x,
           .y = bounds.y - offset_y,
           .width = content_width,
           .height = content_height,
       };

   Call `n00b_widget_layout(content, child_bounds)` only when a content child exists. This negative placement is the production viewport mechanism; do not use backend-specific hiding or `copy_visible_content` logic.

   Fifth, cache local scrollbar and thumb rectangles in scroll-plane content coordinates, not absolute frame coordinates. When both bars are visible, the vertical track height excludes the horizontal bar thickness and the horizontal track width excludes the vertical bar thickness. When overflow on an axis is absent, leave that thumb rect empty. For the vertical thumb:

       max_offset_y = content_height - viewport_height
       thumb_h = max(scrollbar_thickness_px,
                     (viewport_height * viewport_height) / content_height)
       max_thumb_y = viewport_height - thumb_h
       thumb_y = (max_offset_y > 0) ? (offset_y * max_thumb_y) / max_offset_y : 0

   Use the same pattern for the horizontal thumb with widths and x-coordinates. Clamp each thumb so it never extends past its track.

6. Make `scroll_measure()` deterministic and viewport-oriented.

   `measure` must ignore scrollbar visibility in `AUTO` mode because actual visibility depends on future layout bounds, but it must reserve scrollbar thickness for the corresponding axes when mode is `ALWAYS`.

   Use `default_viewport_w = 20 * n00b_widget_cell_px_width(plane)` and `default_viewport_h = 3 * n00b_widget_line_px_height(plane)`.

   Let `content_pref_*` and `content_min_*` come from the child when it exists, otherwise use `1`.

   Preferred size rules:

   - If horizontal scrolling is enabled, preferred width is `min(max(content_pref_w, 1), default_viewport_w)`.
   - If horizontal scrolling is disabled, preferred width is `max(content_pref_w, 1)`.
   - If vertical scrolling is enabled, preferred height is `min(max(content_pref_h, 1), default_viewport_h)`.
   - If vertical scrolling is disabled, preferred height is `max(content_pref_h, 1)`.

   Minimum size rules:

   - If horizontal scrolling is enabled, minimum width is `n00b_widget_cell_px_width(plane)`.
   - If horizontal scrolling is disabled, minimum width is `max(content_min_w, 1)`.
   - If vertical scrolling is enabled, minimum height is `n00b_widget_line_px_height(plane)`.
   - If vertical scrolling is disabled, minimum height is `max(content_min_h, 1)`.

   Add scrollbar thickness to the preferred and minimum size on each axis where `scrollbar_mode == N00B_SCROLLBAR_ALWAYS` and that axis is enabled.

7. Implement `scroll_render()`, `scroll_handle_event()`, and `scroll_can_focus()` with these semantics.

   `scroll_render()` clears the plane, refreshes the three persistent style records from the active theme palette, draws visible tracks with `N00B_PAL_SCROLLBAR_TRACK`, draws thumbs with `N00B_PAL_SCROLLBAR_THUMB`, switches to `N00B_PAL_SCROLLBAR_THUMB_HOVER` while the hovered or dragged thumb is active, and fills the lower-right scrollbar corner with the track style when both bars are visible. The widget does not render the content subtree itself; normal child rendering handles that.

   `scroll_can_focus()` always returns `true`.

   `scroll_handle_event()` must consume only events that change scroll state or land on scrollbar chrome. Mouse handling uses local coordinates that `src/display/mouse.c` already rebases into the scroll plane's content origin.

   Key handling:

   - `N00B_KEY_UP` and `N00B_KEY_DOWN` scroll vertically by one line height when vertical scrolling is enabled.
   - `N00B_KEY_LEFT` and `N00B_KEY_RIGHT` scroll horizontally by one cell width when horizontal scrolling is enabled.
   - `N00B_KEY_PAGE_UP` and `N00B_KEY_PAGE_DOWN` scroll vertically by one full viewport height.
   - `Ctrl+Home` maps to `n00b_scroll_to_top()`.
   - `Ctrl+End` maps to `n00b_scroll_to_bottom()`.

   Mouse handling:

   - Wheel up/down events are consumed on `N00B_MOUSE_PRESS`. Without `Shift`, wheel moves vertically by `scroll_step_lines * line_height` when vertical scrolling is enabled. With `Shift`, wheel moves horizontally by `scroll_step_lines * cell_width` when horizontal scrolling is enabled. If vertical scrolling is disabled but horizontal scrolling is enabled, wheel should still move horizontally.
   - Left press inside `vthumb_rect` starts vertical dragging, records the pointer's local y position and the current `offset_y`, calls `n00b_canvas_capture_mouse(plane->canvas, plane)`, and returns `true`.
   - Left drag or move while dragging recalculates `offset_y` proportionally from drag distance and the track's movable range, then relayouts immediately.
   - Left release while dragging stops the drag, releases mouse capture, updates hover state, and returns `true`.
   - Left press in the vertical track above or below the thumb pages by one viewport height toward the click.
   - Left press in the horizontal track left or right of the thumb pages by one viewport width toward the click.
   - Plain mouse move updates `hover_vertical_thumb` and `hover_horizontal_thumb` and marks dirty only when the hover bits change.

   All other events return `false` so child widgets can remain the primary handlers in the content area.

8. Add `test/unit/test_scroll.c`. Follow the style of `test_split.c`: one `main`, small focused test functions, helper widgets with captured layout bounds, and direct stream-canvas mouse routing where needed.

   The test file must include at least these cases:

   `test_scroll_create_and_api()` proves constructor defaults, vtable attachment, `can_focus == true`, content replacement semantics, and the zero-offset getter defaults.

   `test_scroll_measure_caps_scroll_axes()` builds a dummy content widget with large preferred size, measures vertical-only, horizontal-only, and both-axis scroll containers on a stream canvas, and asserts the `20 * cell_width` by `3 * line_height` capping behavior described in this plan.

   `test_scroll_offsets_clamp_and_direction_helpers()` lays out a both-axis scroll at a known size, uses `scroll_to`, `scroll_by`, `scroll_to_bottom`, `scroll_to_end`, and `ensure_visible`, and asserts exact offsets plus the four directional predicate helpers.

   `test_scroll_auto_scrollbars_and_thumb_rects()` creates known overflow in both axes, runs layout, and asserts the final `show_vscrollbar`, `show_hscrollbar`, `viewport_width`, `viewport_height`, and exact local thumb rectangles after the fixed-point visibility pass.

   `test_scroll_keyboard_scroll_events()` sends key events directly to the scroll widget after layout and verifies arrow, page, and `Ctrl+Home/End` behavior without involving child widgets.

   `test_scroll_vertical_thumb_drag_and_track_click()` attaches the scroll to a stream canvas, presses on the vertical thumb, verifies `n00b_canvas_get_mouse_capture(canvas) == scroll`, drags the thumb, asserts the offset changed, then releases and confirms capture clears. It must also click above or below the thumb in the track and assert page scrolling occurs.

   `test_scroll_scrolled_content_mouse_routes_to_visible_child()` uses a test-only container content widget that lays out a clickable child well below the initial viewport. After calling `n00b_scroll_to()` to reveal that child, send a real mouse press at the viewport location and assert the child's click counter increments. This is the proof that negative child placement keeps hit-testing aligned with scrolled content.

9. Extend `src/tools/widget_demo.c`.

   Add `#include "display/widgets/scroll.h"`.

   Add a new `g_scroll_status_label` global alongside the existing grid and split status labels.

   Add a `demo_scroll(n00b_canvas_t *canvas)` helper and include `"scroll"` in the usage text and the widget dispatch branch in `main`. Set `use_event_loop = true` for this branch.

   Build the demo scene as one top-level column `box`: title label, instructions label, status label, and then one growable scroll widget that fills the remaining height. Use `n00b_scroll_new(content, .axes = N00B_SCROLL_AXIS_BOTH, .scrollbar_mode = N00B_SCROLLBAR_AUTO, .scroll_step_lines = 3, .scrollbar_thickness_px = cpw, .canvas = canvas)`.

   The content subtree should be a vertical `box` with enough rows to force vertical scrolling and one deliberately wide unwrapped line to force horizontal overflow. Include at least two clickable buttons inside the content, one near the top and one near the bottom, and wire both to update `g_scroll_status_label` so manual testing proves clicks still land on the correct content widgets after scrolling. The instructions label should tell the operator to Tab to the scroll frame for keyboard tests, use the mouse wheel and `Shift+wheel`, drag either scrollbar thumb, and click the bottom button after scrolling down.

10. Update `docs/widgets.md`. Move `scroll` from the Wave 1 backlog into the implemented widgets list, add `test_scroll` to the representative widget-test sentence, and keep the remaining Wave 1 backlog in order: `tabs`, `text`.

11. If `build_debug/` is stale or missing targets, refresh the Meson graph:

       meson setup --reconfigure build_debug

   Expected result: Meson completes without unknown-source or unknown-target errors.

12. Build the touched targets:

       meson compile -C build_debug test_scroll widget_demo

   Expected transcript shape:

       ninja: Entering directory `build_debug`
       [1/N] Compiling ...
       [N/N] Linking target widget_demo

13. Run the targeted automated checks:

       meson test -C build_debug --print-errorlogs scroll

   Expected transcript shape:

       1/1 scroll OK

14. Run a one-frame smoke check of the new demo on a non-interactive backend:

       ./build_debug/widget_demo --widget scroll --backend stream

   Expected result: the tool renders one scroll scene and exits without `Unknown widget`, assertion failures, or crashes.

15. Run the interactive manual check:

       ./build_debug/widget_demo --widget scroll --backend tui

   Expected result: a scrollable scene appears. After pressing Tab until the scroll viewport is focused, arrow keys and PageUp/PageDown move the content, `Ctrl+Home` and `Ctrl+End` jump to the top and bottom, the mouse wheel scrolls vertically, `Shift+wheel` scrolls horizontally, both scrollbar thumbs can be dragged repeatedly without stale mouse capture, and the bottom in-content button still fires after scrolling down to it.

## Validation and Acceptance

The implementation is complete only when all of the following are true.

- `include/display/widgets/scroll.h` and `src/display/widgets/scroll.c` exist and expose the public API promised in `plans/notes/widget-wave1-design-breakdown.md` and this plan.
- `test/unit/test_scroll.c` exists and the `scroll` Meson test target passes.
- Scroll offset setters and drag handlers update the visible layout immediately without requiring a resize event.
- The widget renders scrollbars as overlays only and does not copy backend cells or hide child planes offscreen.
- Vertical-thumb dragging captures and releases the mouse cleanly.
- Scrolled content remains clickable in the correct translated viewport position after offsets change.
- `docs/widgets.md` lists `scroll` as implemented, and `widget_demo --widget scroll` is a recognized, working demo entrypoint.

Acceptance is behavioral, not just structural:

- `test_scroll_vertical_thumb_drag_and_track_click()` must fail before the scroll implementation exists and pass after it.
- `test_scroll_scrolled_content_mouse_routes_to_visible_child()` must fail before negative child placement is implemented and pass after it.
- While running `widget_demo --widget scroll --backend tui`, the user must be able to scroll to the bottom button, click it, and then drag either scrollbar thumb again without crashes or stale mouse capture.

## Idempotence and Recovery

All source edits in this plan are deterministic and safe to rerun. Re-running the public setters on an already-configured scroll widget simply replaces the current state, updates the internal viewport wrapper if needed, and relayouts the widget again.

If a build step reports an unknown target after `meson.build` was edited, rerun `meson setup --reconfigure build_debug` and repeat the compile command. If the interactive demo exits during a drag, restarting it is sufficient recovery because `scroll_destroy()` must release capture on teardown. If a partial implementation leaves the widget drawing but not scrolling, verify first that offset-changing paths call the relayout helper; ordinary dirty rerendering alone is not enough in the current Athens event loop.

## Artifacts and Notes

Use these exact helper formulas while implementing and reviewing:

    Default viewport heuristic:
        default_viewport_w = 20 * n00b_widget_cell_px_width(plane)
        default_viewport_h = 3 * n00b_widget_line_px_height(plane)

    Vertical thumb:
        max_offset_y = content_height - viewport_height
        thumb_h = max(scrollbar_thickness_px,
                      (viewport_height * viewport_height) / content_height)
        max_thumb_y = viewport_height - thumb_h
        thumb_y = (max_offset_y > 0) ? (offset_y * max_thumb_y) / max_offset_y : 0

    Horizontal thumb:
        max_offset_x = content_width - viewport_width
        thumb_w = max(scrollbar_thickness_px,
                      (viewport_width * viewport_width) / content_width)
        max_thumb_x = viewport_width - thumb_w
        thumb_x = (max_offset_x > 0) ? (offset_x * max_thumb_x) / max_offset_x : 0

The most important behavior proof for this port is that child interactivity survives scrolling without backend hacks. A successful implementation keeps the content subtree as a normal child plane tree, lays it out at negative coordinates when offsets are non-zero, and lets parent clipping plus ordinary hit-testing do the rest.

Collected execution evidence:

    $ meson test -C build_debug --print-errorlogs scroll
    1/1 unit - n00b:scroll OK

    $ ./build_debug/widget_demo --widget scroll --backend stream
    Backend request 'stream' selected 'stream'
    Backend 'stream' has no input polling; using single-frame mode.

    $ timeout 2 ./build_debug/widget_demo --widget scroll --backend tui
    Backend request 'tui' selected 'ansi'

## Interfaces and Dependencies

This milestone depends only on existing Athens modules. Use:

- `display/widget.h` for the vtable contract and public layout entrypoint.
- `display/render/plane.h` for child parenting, moving, content measurement helpers, and mouse capture.
- `display/mouse.h` for `n00b_canvas_capture_mouse()` and `n00b_canvas_release_mouse()`.
- `internal/display/widget_primitives.h` for `n00b_widget_cell_px_width()` and `n00b_widget_line_px_height()`.
- `text/strings/theme.h` for scrollbar palette roles.

At the end of the milestone, the following public interfaces must exist in `include/display/widgets/scroll.h` and be implemented in `src/display/widgets/scroll.c`:

    extern const n00b_widget_vtable_t n00b_widget_scroll;

    extern n00b_plane_t *
    n00b_scroll_new(n00b_plane_t *content) _kargs {
        n00b_scroll_axis_t    axes                   = N00B_SCROLL_AXIS_VERTICAL;
        n00b_scrollbar_mode_t scrollbar_mode         = N00B_SCROLLBAR_AUTO;
        int32_t               scroll_step_lines      = 3;
        int32_t               scrollbar_thickness_px = 1;
        n00b_canvas_t        *canvas                 = nullptr;
        n00b_allocator_t     *allocator              = nullptr;
    };

    extern void        n00b_scroll_set_content(n00b_plane_t *scroll, n00b_plane_t *content);
    extern n00b_plane_t *n00b_scroll_get_content(n00b_plane_t *scroll);
    extern void        n00b_scroll_to(n00b_plane_t *scroll, int32_t x_px, int32_t y_px);
    extern void        n00b_scroll_by(n00b_plane_t *scroll, int32_t dx_px, int32_t dy_px);
    extern void        n00b_scroll_to_top(n00b_plane_t *scroll);
    extern void        n00b_scroll_to_bottom(n00b_plane_t *scroll);
    extern void        n00b_scroll_to_start(n00b_plane_t *scroll);
    extern void        n00b_scroll_to_end(n00b_plane_t *scroll);
    extern void        n00b_scroll_ensure_visible(n00b_plane_t *scroll, n00b_rect_t rect_px);
    extern int32_t     n00b_scroll_get_offset_x(n00b_plane_t *scroll);
    extern int32_t     n00b_scroll_get_offset_y(n00b_plane_t *scroll);
    extern bool        n00b_scroll_can_scroll_up(n00b_plane_t *scroll);
    extern bool        n00b_scroll_can_scroll_down(n00b_plane_t *scroll);
    extern bool        n00b_scroll_can_scroll_left(n00b_plane_t *scroll);
    extern bool        n00b_scroll_can_scroll_right(n00b_plane_t *scroll);

The `n00b_scroll_t` record must expose the cached geometry a novice implementer and a unit test need to inspect: content size, viewport size, visibility bits, and the four cached track/thumb rectangles. Those fields are part of the Wave 1 contract because the tests in this plan assert them directly.

Revision note (2026-03-15 02:06Z): Updated this living ExecPlan after the follow-up bug fix that added horizontal thumb dragging, refreshed the demo instructions, and reran the scroll-specific build/test/demo checks.
