# Reimplement The Wave 1 Split Widget In Production

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

There is no repository-local `PLANS.md` in this working tree at the time this plan was authored. This document follows `/home/baron/.codex/PLANS.md` and must remain compliant with that file for all future revisions.

This plan explicitly builds on `plans/notes/widget-port-priority.md`, `plans/notes/widget-wave1-design-breakdown.md`, and `docs/widgets.md`. Those files establish that after the production `zstack` and `grid` ports landed, the next missing widget in the Wave 1 queue is `split`.

## Purpose / Big Picture

The goal is to land the next production Wave 1 widget in Athens: a pixel-native `split` container with two panes, a visible divider, ratio-based sizing, minimum-size clamps, and live mouse dragging. After this change, an Athens application will be able to build resizable sidebar/content layouts, inspector panes, and editor-style two-panel shells without hand-writing layout code. A contributor or reviewer should be able to prove the result in two ways: run a dedicated `split` unit test that exercises measurement, layout, capture-driven dragging, and child replacement semantics, and run `widget_demo --widget split` to drag a divider live while both panes remain clickable.

## Progress

- [x] (2026-03-14 06:10Z) Reviewed the Wave 1 queue, the checked-in split design dossier, the prototype `split` implementation and test harness in `n00b-slop`, and the current Athens widget, mouse, event-loop, and demo patterns.
- [x] (2026-03-14 06:27Z) Identified the Athens runtime seams this widget depends on: parent-local mouse rebasing during bubbling, direct widget relayout after drag-time ratio changes, and child-list ordering for predictable focus traversal.
- [x] (2026-03-14 06:42Z) Resolved the open planning decisions left by the split dossier: keyboard resize stays out of Wave 1, divider hover/active state uses the split plane's widget state, hidden or missing panes collapse to a single-pane layout with no divider, and API ratio remains the configured ratio even when runtime min clamps temporarily narrow the effective divider position.
- [x] (2026-03-14 06:55Z) Drafted this self-contained execution plan for the production `split` reimplementation.
- [x] (2026-03-14 16:48Z) Implemented `include/display/widgets/split.h` and `src/display/widgets/split.c` with the production split API, ratio/min-size layout math, divider rendering, capture-driven dragging, non-owning pane replacement, and immediate relayout on runtime changes.
- [x] (2026-03-14 16:48Z) Updated `src/display/mouse.c` and `test/unit/test_mouse.c` so each bubbled mouse event is re-localized for the current parent plane before dispatch.
- [x] (2026-03-14 16:48Z) Added `test/unit/test_split.c`, registered the new Meson target, extended `widget_demo --widget split`, updated `docs/widgets.md`, and ran the build/test/demo validation matrix from this plan.
- [x] (2026-03-19 03:34Z) Remediated the post-landing review findings: split measurement now honors configured per-pane minimums and plain-plane children, detachment-time capture cancellation clears stale drag state in shared removal paths, and `test/unit/test_split.c` now covers vertical dragging plus removal-during-drag regressions.

## Surprises & Discoveries

- Observation: Athens currently localizes mouse coordinates exactly once for the original hit target and then bubbles the same coordinates to every parent unchanged.
  Evidence: `src/display/mouse.c` computes one `local_event` from `target` and reuses it inside the `while (cur)` bubbling loop.

- Observation: The event loop reruns layout only on startup, resize, and backend cell-metric changes; ordinary input-driven state changes only trigger rerendering.
  Evidence: `src/display/event_loop.c` calls `n00b_display_scene_run_layout(canvas)` during initial render and resize handling, but ordinary input paths only reach `n00b_display_scene_rerender_dirty(canvas)` before `n00b_canvas_render(canvas)`.

- Observation: Container `layout` callbacks in Athens receive content bounds with box insets already removed, while `plane->bounds` keeps the outer rectangle that includes borders and padding.
  Evidence: `src/display/widget.c` stores `plane->bounds = bounds`, computes viewport size by subtracting box insets, and invokes custom `layout` with `content_bounds`.

- Observation: Athens already supports `N00B_WSTATE_HOVER` and `N00B_WSTATE_ACTIVE` on planes even though current widgets mostly use `FOCUSED` and `ACTIVE`.
  Evidence: `include/display/render/types.h` defines `N00B_WSTATE_HOVER` and `N00B_WSTATE_ACTIVE`; `src/display/render/plane.c` exposes `n00b_plane_set_state()` and `n00b_plane_get_state()`.

- Observation: The prototype split widget relied on a dedicated notcurses divider plane, but the Wave 1 split dossier explicitly requires Athens to keep divider visuals inside the split plane itself.
  Evidence: `../n00b-slop/src/ctui/widgets/split.c` allocates `split->divider_plane`, while `plans/notes/widget-wave1-design-breakdown.md` states "No dedicated divider plane; divider visuals are rendered by split itself."

- Observation: Focus traversal order in Athens follows child-list order, so `first` and `second` pane order is observable even though the panes do not overlap.
  Evidence: `src/display/focus.c` walks `plane->children` from index `0` upward during `collect_focusable()`.

- Observation: `n00b_box_props_new()` defaults to drawing all four borders, so an internal split box must explicitly clear its borders or the divider math shifts by one cell on each side once the split is attached to a canvas.
  Evidence: The first drag test run produced `actual=0.741071 expected=0.736842` until `split_make_box()` started setting `box->borders = N00B_BORDER_NONE`.

- Observation: In this environment, `widget_demo --widget split --backend tui` resolves to the ANSI backend rather than notcurses, but the split demo still starts cleanly and renders the live divider scene under the shared event loop.
  Evidence: The PTY smoke run printed `Backend request 'tui' selected 'ansi'` before rendering the split demo frame and exiting cleanly on `Ctrl-C`.

## Decision Log

- Decision: The next Wave 1 production widget after `zstack` and `grid` is `split`, and this plan targets that widget only.
  Rationale: `plans/notes/widget-port-priority.md` orders Wave 1 as `stack`, `grid`, `split`, `scroll`, `tabs`, `text`, and `docs/widgets.md` now lists `grid` and `zstack` as implemented while `split` remains in the Wave 1 backlog.
  Date/Author: 2026-03-14 / Codex.

- Decision: Wave 1 split will ship with mouse dragging only; keyboard divider resizing is deferred to a later wave.
  Rationale: The checked-in design dossier left keyboard resize open, but Athens currently has no established non-focusable container keyboard-resize convention, and adding one here would expand scope beyond the next production widget.
  Date/Author: 2026-03-14 / Codex.

- Decision: Divider hover and active state will be represented with `n00b_plane_set_state(split, N00B_WSTATE_HOVER/ACTIVE/NORMAL)` rather than with a second internal state channel.
  Rationale: Athens already exposes widget-state styling, the split plane itself is the divider draw surface, and using existing plane state keeps render logic and future theming aligned.
  Date/Author: 2026-03-14 / Codex.

- Decision: Hidden or missing panes do not reserve split space. When fewer than two visible panes exist, the remaining visible pane fills the full split content bounds and the divider disappears.
  Rationale: This matches Athens container precedent of skipping hidden children and avoids leaving dead interactive gaps when one pane is temporarily absent.
  Date/Author: 2026-03-14 / Codex.

- Decision: `n00b_split_get_ratio()` returns the configured ratio, not a transient min-clamped runtime ratio.
  Rationale: Keeping the requested ratio stable allows a narrow layout to clamp temporarily without losing the user's intended divider position when the container grows again.
  Date/Author: 2026-03-14 / Codex.

- Decision: The plan includes a small mouse-routing fix in `src/display/mouse.c` so parent widgets receive parent-local coordinates during bubbling.
  Rationale: Without that fix, split can falsely interpret bubbled child events as divider hits because the coordinates remain in child-local space.
  Date/Author: 2026-03-14 / Codex.

- Decision: Split will relayout itself immediately by calling `n00b_widget_layout(split, split->bounds)` whenever ratio, divider size, or pane assignment changes and valid bounds are already known.
  Rationale: The Athens event loop does not rerun layout for ordinary input events, so drag-time geometry changes must update child bounds synchronously before the next render pass.
  Date/Author: 2026-03-14 / Codex.

- Decision: `set_first()` and `set_second()` detach old panes without destroying them and must preserve child-list order as `first`, then `second`.
  Rationale: The design dossier fixed non-owning replacement semantics, and preserving deterministic child order keeps future focus traversal and inspection tools predictable.
  Date/Author: 2026-03-14 / Codex.

- Decision: Split uses a zero-inset internal `n00b_box_props_t` only to hold persistent state styles for divider fill/text colors.
  Rationale: The split render path needs theme-derived styles that survive past the render callback, but the widget must not introduce layout insets or visible borders that would shift divider geometry.
  Date/Author: 2026-03-14 / Codex.

- Decision: The production split measurement contract is stricter than the original landing documented: when both panes are visible, configured `min_first_px` and `min_second_px` participate in split-axis preferred and minimum measurement, and plain panes measure through the existing plain-plane helper rather than the widget-only path.
  Rationale: The post-landing review proved that the original child-only formulas under-reported the split's real runtime constraints and collapsed mixed widget/plain layouts to divider-only size.
  Date/Author: 2026-03-19 / Codex.

## Outcomes & Retrospective

The production Wave 1 split widget is now implemented in Athens together with the supporting mouse-bubbling fix the design required. Athens can now build two-pane shells with ratio-based sizing, feasible minimum clamps, a visible divider rendered inside the split plane, and live mouse dragging with capture and immediate relayout. The demo/tooling/documentation surface was updated in the same pass, so `widget_demo --widget split` is now a first-class verification path instead of a follow-up task.

The validation matrix in this plan completed successfully. `meson compile -C build_debug test_split test_mouse test_display_event_dispatch widget_demo`, `meson test -C build_debug --print-errorlogs split mouse display_event_dispatch`, and `./build_debug/widget_demo --widget split --backend stream` all passed. A short PTY run of `./build_debug/widget_demo --widget split --backend tui` also rendered the interactive split scene successfully before exiting on `Ctrl-C`. The remaining gap relative to the wider split roadmap is unchanged from the earlier design decision: keyboard divider resizing remains deferred beyond Wave 1.

The 2026-03-19 review-remediation follow-up tightened the shipped contract without changing the public API. Split measurement now treats configured per-pane minimums as real main-axis requirements when both panes are visible, plain `n00b_plane_t` children measure through their existing footprint instead of collapsing to `0x0`, and removing a captured split or its parent subtree now cancels capture before the detached tree loses its canvas pointer. The split proof surface also now covers vertical dragging and detach-during-drag cleanup directly.

## Context and Orientation

Work from the `n00b-athens/` repository root.

The Athens display system is plane-based. A "plane" is `n00b_plane_t` from `include/display/render/plane.h`: it is both a render surface and the node in the parent/child hierarchy. A "widget" is a plane with a behavior vtable attached through `include/display/widget.h`. A "canvas" is `n00b_canvas_t` from `include/display/render/canvas.h`: it owns top-level planes, input routing, render backends, and mouse capture. A "content bounds" rectangle is the box-inset-adjusted rectangle passed to a container widget's `layout` callback from `src/display/widget.c`; it is the usable inner area, not the outer border box. The "main axis" is the axis along which the split divides space: width for `N00B_SPLIT_HORIZONTAL`, height for `N00B_SPLIT_VERTICAL`. The "divider rect" is the cached split-local pixel rectangle reserved between the two panes. "Mouse capture" means `canvas->mouse_capture` is set so all later mouse events route directly to one plane during a drag.

The key Athens files for this work are `include/display/widget.h` and `src/display/widget.c` for widget lifecycle and container layout; `include/display/mouse.h` and `src/display/mouse.c` for hit testing, bubbling, and capture; `src/display/event_loop.c` for the rerender-only input loop behavior; `src/display/widgets/zstack.c` and `src/display/widgets/grid.c` for current container-widget patterns; `src/tools/widget_demo.c` for the interactive demo entrypoint; `docs/widgets.md` for the implemented-widget list; `meson.build` for source and test target registration; and `test/unit/test_mouse.c` for mouse-routing regressions. The key prototype files are `../n00b-slop/include/ctui/widgets/split.h`, `../n00b-slop/src/ctui/widgets/split.c`, and `../n00b-slop/test/ctui/test_split.c`.

The split dossier in `plans/notes/widget-wave1-design-breakdown.md` already fixed the public API shape and the high-level behavior contract. This plan turns that design into concrete repository edits and closes the remaining implementation gaps. In particular, this plan must keep the divider inside the split plane itself, must use the existing capture API from `include/display/mouse.h`, and must not destroy pane allocations when panes are replaced or detached.

Two Athens-specific details matter for every code change in this plan. First, `plane->bounds` is the outer assigned rectangle, while `plane->width` and `plane->height` are the inner content size after box insets are subtracted. Second, `n00b_widget_layout()` updates both values and marks the plane dirty. Split code therefore needs to cache divider geometry in split-local coordinates for hit testing and rendering, while child layout calls must use absolute content-space bounds.

## Plan of Work

1. Create `include/display/widgets/split.h`. Define `n00b_split_orientation_t`, the `n00b_split_change_cb_t` callback type, and `n00b_split_t` with fields for `first`, `second`, `orientation`, `ratio`, `min_first_px`, `min_second_px`, `divider_px`, `on_change`, `on_change_data`, `divider_rect`, `divider_hovered`, `dragging`, and `drag_pointer_offset_px`. Declare `n00b_widget_split`, the constructor `n00b_split_new(n00b_plane_t *first, n00b_plane_t *second) _kargs`, and the six public mutator/accessor functions from the dossier. Keep naming and argument order exactly aligned with `plans/notes/widget-wave1-design-breakdown.md`.

2. Implement `src/display/widgets/split.c` using the existing container-widget pattern from `src/display/widgets/zstack.c` and `src/display/widgets/grid.c`. Add a private `split_data()` helper that validates `plane->widget_vtable == &n00b_widget_split`. Add a private `split_relayout_if_needed()` helper that calls `n00b_widget_layout(plane, plane->bounds)` when bounds are already known and otherwise only marks the plane dirty. Use this helper from `set_ratio()`, `set_first()`, `set_second()`, `set_min_sizes()`, `set_divider_size()`, and drag updates so the widget responds immediately during the event loop.

3. In `src/display/widgets/split.c`, implement pane ownership and child-list order carefully. `n00b_split_new()` must accept `nullptr` for either pane, assert that any non-null pane is currently unparented, parent `first` before `second`, and attach the split widget to a newly allocated plane. `set_first()` and `set_second()` must detach the previous pane with `n00b_plane_remove_child()` without destroying it, clear that old pane's parent pointer through the normal plane API, and then parent the replacement pane. After any replacement, reorder `plane->children` if necessary so the stored order is always `first` at index `0` and `second` at index `1`. This keeps focus traversal deterministic.

4. Define exact measurement behavior in `src/display/widgets/split.c`. Hidden panes are skipped. Measure each child through `n00b_widget_measure()` when it is a widget and through `n00b_widget_measure_plain_plane()` otherwise. When both panes are visible, clamp each pane's split-axis preferred and minimum size up to its configured `min_first_px` or `min_second_px` before combining by orientation:

    Horizontal:
        first_req_w = max(first_min_w, min_first_px)
        second_req_w = max(second_min_w, min_second_px)
        pref_w = max(first_pref_w, first_req_w) + divider_px + max(second_pref_w, second_req_w)
        pref_h = max(first_pref_h, second_pref_h)
        min_w  = first_req_w + divider_px + second_req_w
        min_h  = max(first_min_h, second_min_h)

    Vertical:
        first_req_h = max(first_min_h, min_first_px)
        second_req_h = max(second_min_h, min_second_px)
        pref_w = max(first_pref_w, second_pref_w)
        pref_h = max(first_pref_h, first_req_h) + divider_px + max(second_pref_h, second_req_h)
        min_w  = max(first_min_w, second_min_w)
        min_h  = first_req_h + divider_px + second_req_h

    Single visible pane:
        copy that pane's measured preferred and minimum sizes exactly; add no divider contribution.

    No visible panes:
        return `1` for all four outputs.

5. Define exact layout behavior in `src/display/widgets/split.c`. The split `layout` callback receives content bounds, so use `bounds.width` and `bounds.height` directly as the usable size. When two visible panes exist, compute `available_main = max(0, main_axis_size - divider_px)`, compute `requested_first = floorf(ratio * available_main)`, clamp that size to `[min_first_px, available_main - min_second_px]` only when `min_first_px + min_second_px <= available_main`, and otherwise clamp only to `[0, available_main]` for the impossible case. Compute `second_size = available_main - first_size`. Cache `divider_rect` in split-local coordinates, with `x/y` measured from the split content origin. Then call `n00b_widget_layout()` on the two panes with absolute bounds derived from the content origin and the resolved sizes. When only one pane is visible, layout that pane to the full content bounds and clear `divider_rect` to a zero-size rectangle. When no panes are visible, clear `divider_rect` and stop.

6. Implement render and destroy semantics in `src/display/widgets/split.c`. `destroy` must release mouse capture if `plane->canvas` currently points at this split plane, then free the `n00b_split_t` record and nothing else. `render` must call `n00b_plane_clear(plane)`, skip drawing when `divider_rect` has zero width or height, and otherwise fill the divider rectangle with a color derived from the split plane state: `N00B_PAL_BORDER` for `NORMAL`, `N00B_PAL_HOVER` for `HOVER`, and `N00B_PAL_ACTIVE` for `ACTIVE`. Reuse the cell and line metrics helpers from `internal/display/widget_primitives.h` and the glyph ideas from `src/display/widgets/divider.c` to draw a small centered grip when the divider is at least one cell thick and three cells long on its major axis. `can_focus` must always return `false`.

7. Implement mouse handling in `src/display/widgets/split.c`. Split must only consume divider interactions. On a left-button press inside `divider_rect`, store the pointer offset within the divider on the main axis, set `dragging = true`, set the split plane state to `N00B_WSTATE_ACTIVE`, call `n00b_canvas_capture_mouse(plane->canvas, plane)`, and return `true`. While dragging, accept both `N00B_MOUSE_MOVE` and `N00B_MOUSE_DRAG`, compute a new ratio from the captured event's split-local coordinates, clamp to `[0.0f, 1.0f]`, update the stored ratio only when it changed, relayout immediately with `n00b_widget_layout(plane, plane->bounds)`, and then fire `on_change(split, ratio, data)` after geometry is current. On mouse release while dragging, clear `dragging`, release capture, recompute whether the pointer is still over the divider to choose `HOVER` versus `NORMAL`, and return `true`. On non-captured mouse move, update `divider_hovered` and the split plane state when the pointer enters or leaves `divider_rect`, but return `false` so normal event bubbling behavior is preserved. All non-divider events must return `false`.

8. Fix parent-local mouse bubbling in `src/display/mouse.c`. Extract the existing target-local coordinate math into one helper that receives the original absolute mouse event, a canvas, and the current plane, then produces a plane-local mouse event. In `n00b_mouse_route_event()`, keep the original absolute event unchanged and recompute a fresh localized event for each `cur` in the bubbling loop. Preserve the current click-to-focus behavior on the original hit target, preserve terminal grid snapping exactly as it exists today, and keep mouse capture routing unchanged. The only functional change should be that parent widgets now receive coordinates in their own local coordinate space instead of the child's local space.

9. Add `test/unit/test_split.c`. Follow the style of `test_grid.c` and `test_zstack.c`: small dummy widgets with controlled preferred/minimum sizes, captured layout bounds, and click counters. Include at least these tests:

    `test_split_create_and_api()` verifies constructor defaults, vtable attachment, `can_focus == false`, ratio getter/setter, divider-size setter, and non-owning pane replacement semantics.

    `test_split_measure_by_orientation()` verifies the exact horizontal and vertical formulas from this plan and confirms hidden panes collapse to the single-pane fallback.

    `test_split_layout_horizontal_clamps_minimums()` lays out a horizontal split at a known width with feasible minimums and asserts the exact child bounds and divider rect.

    `test_split_single_visible_pane_uses_full_bounds()` hides one pane, relayouts, and asserts that the remaining pane receives the full content rectangle and the divider rect becomes empty.

    `test_split_drag_updates_ratio_and_capture()` attaches the split to a stream canvas, lays it out, presses on the divider, sends drag/move events, asserts `n00b_canvas_get_mouse_capture(canvas) == split` during drag, verifies the ratio and callback count change, and then verifies release clears capture and leaves the split in `HOVER` or `NORMAL` state as appropriate.

10. Add one independent mouse-routing regression to `test/unit/test_mouse.c`. Define a parent widget that records the mouse coordinates it receives when a child returns `false`. Place the child at a non-zero offset inside the parent, send a click into the child, and assert that the parent receives parent-local coordinates rather than the child's local coordinates. This test must fail before the `src/display/mouse.c` bubbling fix and pass after it.

11. Update `meson.build`. Add `src/display/widgets/split.c` to the main display source list beside `grid.c` and `zstack.c`. Add a new `split_test = executable('test_split', ['test/unit/test_split.c'], kwargs: test_common_kwargs)` plus `test('split', split_test, suite: 'unit')` near the existing widget tests. Keep the ordering near the other widget targets so the file remains easy to scan.

12. Extend `src/tools/widget_demo.c`. Add `#include "display/widgets/split.h"`, add persistent demo state for one status label and optionally a cached split pointer, add callbacks that update the status label when the divider ratio changes or when pane buttons fire, add `demo_split(n00b_canvas_t *canvas)`, add `"split"` to the usage text, add a `"split"` dispatch branch in `main`, and set `use_event_loop = true` for that branch. Build the demo scene as one top-level column `box`: title label, short instructions label, status label, and then one growable `split` widget that fills the remaining height. The split scene should use a left "Navigator" card and a right "Inspector" card built with the existing `make_demo_card()` helper, and the demo should pass `.divider_px = cpw` so the divider is visibly draggable on terminal backends. The change callback should render the current ratio into the status label so manual testing confirms live updates.

13. Update `docs/widgets.md`. Move `split` from the Wave 1 backlog into the implemented widgets list, add `test_split` to the representative widget-test sentence, and remove `split` from the Wave 1 missing-widget list. Keep the remaining Wave 1 entries in order: `scroll`, `tabs`, `text`.

## Concrete Steps

Run all commands from the `n00b-athens/` directory.

1. Refresh the split design and runtime context before editing:

       sed -n '130,220p' plans/notes/widget-wave1-design-breakdown.md
       sed -n '1,260p' ../n00b-slop/src/ctui/widgets/split.c
       sed -n '1,260p' src/display/mouse.c
       sed -n '1,220p' src/display/event_loop.c

   Expected result: you can see the Wave 1 split API contract, the prototype drag behavior, the current bubbling logic in `mouse.c`, and the rerender-only ordinary input loop in `event_loop.c`.

2. Implement the new header, source, tests, and demo/docs edits described in `Plan of Work`.

3. If `build_debug/` is stale or missing targets, refresh the Meson target graph:

       meson setup --reconfigure build_debug

   Expected result: Meson finishes without complaining about unknown sources or missing targets.
   Observed on 2026-03-14 16:48Z: command succeeded and regenerated the 198-target build graph without source or target errors.

4. Build the touched targets:

       meson compile -C build_debug test_split test_mouse test_display_event_dispatch widget_demo

   Expected transcript shape:

       ninja: Entering directory `build_debug`
       [1/N] Compiling ...
       [N/N] Linking target widget_demo
   Observed on 2026-03-14 16:48Z: the build compiled `src/display/mouse.c`, `src/display/widgets/split.c`, `test/unit/test_split.c`, and `src/tools/widget_demo.c`, then linked `test_mouse`, `test_split`, `test_display_event_dispatch`, and `widget_demo`.

5. Run the targeted automated checks:

       meson test -C build_debug --print-errorlogs split mouse display_event_dispatch

   Expected transcript shape:

       1/3 split OK
       2/3 mouse OK
       3/3 display_event_dispatch OK
   Observed on 2026-03-14 16:48Z: all three tests passed after one implementation-time correction to remove unintended box borders from the split's internal style holder.

6. Run a one-frame smoke check of the new demo on a non-interactive backend:

       ./build_debug/widget_demo --widget split --backend stream

   Expected result: the tool renders one split scene and exits without `Unknown widget`, assertion failures, or crashes.
   Observed on 2026-03-14 16:48Z: command exited successfully after printing `Backend request 'stream' selected 'stream'` and `Backend 'stream' has no input polling; using single-frame mode.`.

7. Run the interactive manual check:

       ./build_debug/widget_demo --widget split --backend tui

   Expected result: a two-pane scene appears with a visibly draggable divider. Left-press on the divider starts drag, moving the mouse resizes both panes live and updates the status label with the new ratio, releasing the mouse ends drag, and the buttons inside both panes still fire after the drag.
   Observed on 2026-03-14 16:48Z: the PTY smoke run rendered the split demo frame under the ANSI backend and shut down cleanly on `Ctrl-C`. Full human drag/click interaction still requires an attached terminal session.

## Validation and Acceptance

The implementation is complete only when all of the following are true.

- `include/display/widgets/split.h` and `src/display/widgets/split.c` exist and expose exactly the public API promised in `plans/notes/widget-wave1-design-breakdown.md`.
- `test/unit/test_split.c` exists and the `split` Meson test target passes.
- `test/unit/test_mouse.c` includes a regression proving parent-local coordinates during mouse bubbling.
- The split divider can be dragged with capture, and release clears capture cleanly even if the pointer leaves the divider during the drag.
- The split widget does not destroy detached pane allocations during `set_first()`, `set_second()`, or normal destruction.
- Hidden or missing panes collapse to a single-pane layout with no divider gap.
- `docs/widgets.md` lists `split` as implemented, and `widget_demo --widget split` is a recognized, working demo entrypoint.

Acceptance is behavioral, not just structural:

- `test_split_drag_updates_ratio_and_capture()` must fail before the split implementation exists and pass after it.
- The new parent-local bubbling test in `test_mouse.c` must fail before the `src/display/mouse.c` change and pass after it.
- While running `widget_demo --widget split --backend tui`, the user must be able to drag the divider repeatedly without crashes or stale capture, and pane buttons must remain clickable after each drag.

## Idempotence and Recovery

All source edits in this plan are deterministic and safe to rerun. Re-running the public setters on an already-configured split should simply overwrite the current configuration and relayout the widget again.

If a build step reports an unknown target after `meson.build` was edited, rerun `meson setup --reconfigure build_debug` and repeat the compile command. If the interactive demo exits while a drag was in progress, restarting the demo is sufficient recovery because the split destructor must release mouse capture on teardown. If a partial implementation leaves tests failing, keep the new `split` files and iterate; do not delete unrelated widget code or revert existing `grid`/`zstack` work.

## Artifacts and Notes

Use these exact formulas while implementing and reviewing:

    Horizontal layout:
        available = bounds.width - divider_px
        first_px  = floor(ratio * available)
        second_px = available - first_px

    Vertical layout:
        available = bounds.height - divider_px
        first_px  = floor(ratio * available)
        second_px = available - first_px

    Feasible min clamp:
        clamp first_px to [min_first_px, available - min_second_px]
        only when min_first_px + min_second_px <= available

    Drag update:
        if horizontal:
            requested_first = event->mouse.x - drag_pointer_offset_px
        else:
            requested_first = event->mouse.y - drag_pointer_offset_px
        ratio = clamp(requested_first / available, 0.0, 1.0)

The most important repository touchpoints for review are:

- `include/display/widgets/split.h`
- `src/display/widgets/split.c`
- `src/display/mouse.c`
- `test/unit/test_split.c`
- `test/unit/test_mouse.c`
- `src/tools/widget_demo.c`
- `docs/widgets.md`
- `meson.build`

## Interfaces and Dependencies

In `include/display/widgets/split.h`, define exactly:

    typedef enum {
        N00B_SPLIT_HORIZONTAL,
        N00B_SPLIT_VERTICAL,
    } n00b_split_orientation_t;

    typedef void (*n00b_split_change_cb_t)(n00b_plane_t *split,
                                           float         ratio,
                                           void         *data);

    typedef struct n00b_split_t {
        n00b_plane_t              *first;
        n00b_plane_t              *second;
        n00b_split_orientation_t   orientation;
        float                      ratio;
        int32_t                    min_first_px;
        int32_t                    min_second_px;
        int32_t                    divider_px;
        n00b_split_change_cb_t     on_change;
        void                      *on_change_data;
        n00b_rect_t                divider_rect;
        bool                       divider_hovered;
        bool                       dragging;
        int32_t                    drag_pointer_offset_px;
    } n00b_split_t;

    extern const n00b_widget_vtable_t n00b_widget_split;

    extern n00b_plane_t *
    n00b_split_new(n00b_plane_t *first, n00b_plane_t *second) _kargs {
        n00b_split_orientation_t orientation = N00B_SPLIT_HORIZONTAL;
        float                    ratio = 0.5f;
        int32_t                  min_first_px = 64;
        int32_t                  min_second_px = 64;
        int32_t                  divider_px = 1;
        n00b_split_change_cb_t   on_change = nullptr;
        void                    *on_change_data = nullptr;
        n00b_canvas_t           *canvas = nullptr;
        n00b_allocator_t        *allocator = nullptr;
    };

    extern void  n00b_split_set_ratio(n00b_plane_t *split, float ratio);
    extern float n00b_split_get_ratio(n00b_plane_t *split);
    extern void  n00b_split_set_first(n00b_plane_t *split, n00b_plane_t *first);
    extern void  n00b_split_set_second(n00b_plane_t *split, n00b_plane_t *second);
    extern void  n00b_split_set_min_sizes(n00b_plane_t *split,
                                          int32_t       min_first_px,
                                          int32_t       min_second_px);
    extern void  n00b_split_set_divider_size(n00b_plane_t *split,
                                             int32_t       divider_px);

In `src/display/widgets/split.c`, depend only on existing Athens facilities:

- `display/widget.h` for attach, measure, handle-event, and layout integration.
- `display/render/plane.h` for parenting, dirty flags, widget state, and draw commands.
- `display/mouse.h` for capture and release.
- `internal/display/widget_primitives.h` for line and cell metrics.
- `text/strings/theme.h` for palette-derived divider colors.

In `src/display/mouse.c`, add one file-local helper with a signature equivalent to:

    static void mouse_event_to_plane_local(n00b_canvas_t      *canvas,
                                           n00b_plane_t       *plane,
                                           const n00b_event_t *absolute,
                                           n00b_event_t       *localized);

That helper must preserve the current absolute-to-local behavior for the original hit target and make it reusable for each parent in the bubbling loop.

## Revision Notes

- 2026-03-14: Initial ExecPlan added for the Wave 1 production `split` reimplementation because `split` is now the next missing widget after `zstack` and `grid`. The plan also records the required mouse-bubbling coordinate fix so divider interaction is implementable without reopening runtime design.
- 2026-03-14: Updated the ExecPlan after implementation to mark all milestones complete, record the internal zero-border box decision, capture the drag-test discovery about default box borders, and log the successful build/test/demo validation commands.
- 2026-03-19: Updated the ExecPlan after the review-remediation follow-up to remove the accidental outer fence, correct the measurement contract for configured minima and plain planes, and record the shared detach-time capture cleanup that landed after the original production split port.
