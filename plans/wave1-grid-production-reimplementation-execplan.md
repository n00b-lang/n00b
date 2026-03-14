# Reimplement The Wave 1 Grid Widget In Production

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

There is no repository-local `PLANS.md` in this working tree at the time this plan was authored. This document follows `/home/baron/.codex/PLANS.md` and must remain compliant with that file for all future revisions.

This plan explicitly builds on `plans/notes/widget-port-priority.md`, `plans/notes/widget-wave1-design-breakdown.md`, and `docs/widgets.md`. Those files establish that after the production `zstack` port shipped, the next missing widget in the Wave 1 queue is `grid`.

## Purpose / Big Picture

The goal is to land the next production Wave 1 widget in Athens: a pixel-native `grid` container that can place child planes into rows and columns, support fixed columns, content-sized `AUTO` tracks, proportional `FR` tracks, auto-fit columns, and per-child spans. After this change, an Athens application will be able to build real dashboard and settings-page layouts without hand-positioning every control. A contributor or reviewer should be able to prove the result in two ways: run a dedicated `grid` unit test that exercises layout math, spans, auto-fit behavior, and click routing, and run `widget_demo --widget grid` to see a visible multi-column page where a spanning header and spanning content card line up correctly and interactive controls update a status label.

## Progress

- [x] (2026-03-13 12:34Z) Reviewed the Wave 1 queue, the checked-in grid design dossier, the prototype `grid` implementation and tests in `n00b-slop`, and the current Athens widget, test, and demo patterns.
- [x] (2026-03-13 12:34Z) Resolved the planning decisions that the design dossier left open for Wave 1 grid work: hidden children are skipped during placement, explicit row tracks remain deferred, `AUTO` tracks are content-driven, and auto-fit measurement uses a deterministic one-column fallback because Athens measurement has no available-width parameter.
- [x] (2026-03-13 12:34Z) Drafted this self-contained execution plan for the production `grid` reimplementation.
- [x] (2026-03-13 11:27Z) Implemented `include/display/widgets/grid.h` and `src/display/widgets/grid.c` with the public API, span metadata, deterministic measurement, equal-column layout, track layout, auto-fit layout, and shared row/column resolution helpers.
- [x] (2026-03-13 11:27Z) Added `test/unit/test_grid.c`, registered `grid` in `meson.build`, extended `widget_demo --widget grid`, updated `docs/widgets.md`, and passed the targeted validation matrix (`grid`, `mouse`, `display_event_dispatch`, plus `widget_demo` stream and TUI launch checks).

## Surprises & Discoveries

- Observation: Athens widget measurement reports preferred and minimum size with no available-width input, while prototype `grid_preferred_size()` explicitly depended on an available width. Evidence: `include/display/widget.h` defines `measure(plane, data, pref_w, pref_h, min_w, min_h)` with no layout hint, while `../n00b-slop/src/ctui/widgets/grid.c` computes preferred size from `available.x`.
- Observation: Prototype grid behavior is covered mainly by end-to-end demo tests rather than a small dedicated unit file. Evidence: `../n00b-slop/test/ctui/test_e2e.c` contains `test_grid_*` and `test_grid_advanced_*` scenarios, and there is no separate `test_grid.c`.
- Observation: `widget_demo` only enables the interactive event loop for `zstack` and `all` today, so any clickable grid demo needs its own opt-in branch. Evidence: `src/tools/widget_demo.c` initializes `use_event_loop = false` and only sets it for `"zstack"` and `"all"`.
- Observation: Existing Athens containers skip invisible children during measurement, which gives a clear precedent for the unresolved hidden-child placement question in the grid dossier. Evidence: `src/display/widgets/box.c` ignores children without `N00B_PLANE_VISIBLE` during both measurement and flex layout.
- Observation: The checked-in `build_debug/` directory needed a Meson reconfigure before the new `grid` target appeared, even though the code changes were otherwise build-ready. Evidence: `meson compile -C build_debug test_grid ...` initially failed with `ERROR: Can't invoke target 'test_grid': target not found`; `meson setup --reconfigure build_debug` fixed the stale target table.

## Decision Log

- Decision: The next Wave 1 production widget after `zstack` is `grid`, and this plan targets that widget only. Rationale: `plans/notes/widget-port-priority.md` orders Wave 1 as `stack`, `grid`, `split`, `scroll`, `tabs`, `text`, and `docs/widgets.md` now lists `zstack` as implemented while `grid` remains in the missing set. Date/Author: 2026-03-13 / Codex.
- Decision: Wave 1 grid will not add explicit row-track definitions or explicit child-placement APIs. Rationale: the checked-in design dossier already scoped Wave 1 to content-driven rows and flow placement with spans, and reopening that scope would slow the next production widget unnecessarily. Date/Author: 2026-03-13 / Codex.
- Decision: Hidden children are skipped during placement, preferred-size calculation, and runtime layout. Rationale: hidden widgets should not reserve grid cells or force row/column growth, and this matches current Athens container practice. Date/Author: 2026-03-13 / Codex.
- Decision: Span metadata will be stored as an internal growable array of `{child, col_span, row_span}` records keyed by child plane pointer, pruned lazily when the child is no longer parented to the grid. Rationale: this keeps the public surface small, avoids introducing a new shared container abstraction, and is easy to reason about for the small child counts expected in Wave 1 UI scenes. Date/Author: 2026-03-13 / Codex.
- Decision: `AUTO` track widths are true content-driven widths in Athens, while `FR` tracks divide only the remaining pixels after fixed and auto tracks are resolved. Rationale: the prototype treated `AUTO` like `1fr`, but the Wave 1 design dossier intentionally tightened the contract to a more useful CSS-like meaning. Date/Author: 2026-03-13 / Codex.
- Decision: Because Athens `measure` has no available-width input, auto-fit measurement falls back to a one-column natural size; real auto-fit column counts are resolved only during layout. Rationale: this preserves determinism without changing the shared widget vtable contract just for grid. Date/Author: 2026-03-13 / Codex.
- Decision: Remainder pixels during equal-split and `FR` distribution are assigned left-to-right. Rationale: deterministic remainder handling makes the unit tests stable and avoids backend-specific drift for odd widths. Date/Author: 2026-03-13 / Codex.
- Decision: The grid port should not require changes to shared mouse, focus, or compositor code. Rationale: unlike `zstack`, grid behavior should be expressible entirely through the existing plane hierarchy, layout bounds, and normal event bubbling. Date/Author: 2026-03-13 / Codex.
- Decision: Fixed-column measurement derives its natural width from the widest per-column content contribution after span splitting, so all fixed-mode columns remain equal during measurement as well as layout. Rationale: this keeps equal-column mode deterministic and lets spanning children influence the measured width without inventing a separate width-hint API. Date/Author: 2026-03-13 / Codex.
- Decision: Track-mode natural width is computed by searching for the smallest content width whose resolved track widths satisfy the clamped per-track contributions. Rationale: this keeps `measure` and `layout` on the same resolver instead of maintaining two subtly different track-width algorithms. Date/Author: 2026-03-13 / Codex.

## Outcomes & Retrospective

As of 2026-03-13 this plan has been executed successfully. Athens now has a production `grid` container with a checked-in public header, implementation, dedicated unit test, demo entrypoint, and updated widget-status documentation.

The targeted acceptance commands in this plan now pass. `meson test -C build_debug --print-errorlogs grid mouse display_event_dispatch` completed with all three tests green, `./build_debug/widget_demo --widget grid --backend stream` exited cleanly with the expected widget branch, and the TUI demo launched under a pseudo-terminal and rendered the multi-column scene before exiting cleanly on `Ctrl-C`. The only implementation-side surprise was that the existing `build_debug/` directory needed a Meson reconfigure before it learned about the new `grid` target.

## Context and Orientation

Athens widgets are planes with attached behavior, not separate heap-managed widget objects. `include/display/widget.h` defines the `n00b_widget_vtable_t` contract, and `src/display/widget.c` handles attachment, measurement, layout, and event dispatch. A container widget receives content-space bounds through `n00b_widget_layout()`, which already subtracts any box border and padding before calling the widget-specific `layout` callback.

A grid is a 2D container. In ordinary language, a "track" is one column definition. `FIXED` means a column has an exact pixel width. `AUTO` means a column grows just large enough to fit its content. `FR` means "fractional share of the leftover width after fixed and auto columns are resolved". A "span" means one child occupies more than one adjacent cell. An "auto-fit" grid chooses how many equal-width columns can fit in the available width instead of using one fixed column count.

The prototype source of truth for behavior is `../n00b-slop/include/ctui/widgets/grid.h`, `../n00b-slop/src/ctui/widgets/grid.c`, and the grid scenarios in `../n00b-slop/test/ctui/test_e2e.c`. The authoritative Athens design contract is already written in `plans/notes/widget-wave1-design-breakdown.md` under `## grid`. That dossier fixes the public API surface, the high-level state model, the layout sequence, and the Wave 1 test intent.

The most relevant Athens implementation references are:

- `include/display/widgets/box.h` and `src/display/widgets/box.c` for container-widget constructor style, measurement, and content-bounds layout patterns.
- `include/display/widgets/zstack.h`, `src/display/widgets/zstack.c`, and `test/unit/test_zstack.c` for the current Wave 1 implementation style, test harness structure, and Meson registration pattern.
- `include/display/render/plane.h` and `src/display/widget.c` for plane parenting, visibility, `n00b_widget_layout()`, and the meaning of content bounds.
- `src/tools/widget_demo.c` and `docs/widgets.md` for the user-visible demo entrypoint and widget-status documentation that must reflect the new production widget.

There is no existing Athens grid widget today. No `include/display/widgets/grid.h`, `src/display/widgets/grid.c`, or `test/unit/test_grid.c` file exists at the time this plan was authored.

## Plan of Work

Milestone 1 adds the public `grid` surface and the full internal layout engine for the widget. Create `include/display/widgets/grid.h` and `src/display/widgets/grid.c`, and register the new source in `meson.build`. The header must expose the public enum, track struct, widget data struct, vtable symbol, constructor, and eight public mutators/accessors already approved in the design dossier. The implementation must keep the widget self-contained: private helpers in `src/display/widgets/grid.c` should collect visible children, look up span metadata, build a row-major placement map, resolve column widths, resolve row heights, and finally call `n00b_widget_layout()` on each visible child with computed cell bounds.

Milestone 1 also resolves the design choices that were still open when the dossier was written. Hidden children do not reserve slots. Rows are content-driven only; there is no Wave 1 row-track API. `AUTO` tracks use measured child widths. `FR` tracks divide remaining pixels left-to-right after fixed and auto tracks are accounted for. If a configuration is over-constrained and fixed-plus-auto widths exceed the available width, the implementation should shrink only auto tracks toward their computed minimums, leave fixed widths untouched, set `FR` tracks to zero if necessary, and let any remaining impossible overflow clip normally through the compositor. That gives the widget a deterministic fallback without expanding scope into a shared layout framework.

Milestone 2 adds focused automated proof. Create `test/unit/test_grid.c` using the same style as `test_zstack.c`: a small dummy widget vtable, stream-canvas helpers, per-test `printf("  [PASS] ...")` output, and explicit assertions. The test file must prove the public API surface, fixed-column placement, span behavior, deterministic `FR` remainder handling, auto-fit column selection, hidden-child skipping, and normal click routing after layout. This port should not need shared-runtime edits, so the main regression matrix should stay focused on the new `grid` test plus the existing shared mouse and event-dispatch tests.

Milestone 3 ships the human-facing proof and updates repository status. Extend `src/tools/widget_demo.c` with `demo_grid()` and a `"grid"` dispatch branch that opts into the interactive event loop. The scene should visibly exercise the widget rather than just instantiate it. Use a three-column grid with a header card spanning all columns, a narrow card in one column, a wider details card spanning two columns, and a bottom status row that updates when buttons are clicked. Then update `docs/widgets.md` so `grid` is listed as implemented and `test_grid` appears with the representative widget tests.

## Concrete Steps

Run commands from `/home/baron/crash-override/n00b-tui/n00b-athens`.

1. Ensure the build directory exists and is current.

       N00B_TEST=1 bash build.sh

   If `build_debug/` is known to be stale, rebuild cleanly instead of trying to repair Meson state by hand:

       N00B_CLEAN=1 bash build.sh

   During this execution pass, `build_debug/` already existed but needed a metadata refresh after `meson.build` changed:

       meson setup --reconfigure build_debug

2. Re-read the approved grid contract before editing code.

       sed -n '67,137p' plans/notes/widget-wave1-design-breakdown.md

3. Edit `meson.build` in two places. Add `src/display/widgets/grid.c` to the widget source list near `box.c` and `zstack.c`. Register a new unit executable and test named `grid` using `test/unit/test_grid.c` and `test_common_kwargs`, following the existing `zstack` registration pattern.

4. Create `include/display/widgets/grid.h` with the exact public surface below. Follow the established widget-header style used by `box.h`, `button.h`, and `zstack.h`.

   The header must define these public interfaces:

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
       extern void n00b_grid_set_tracks(n00b_plane_t *grid,
                                        const n00b_grid_track_t *tracks,
                                        n00b_isize_t count);
       extern void n00b_grid_set_auto_fit(n00b_plane_t *grid,
                                          int32_t min_col_width,
                                          int32_t max_col_width);
       extern void n00b_grid_set_gap(n00b_plane_t *grid, int32_t gap);
       extern void n00b_grid_set_row_gap(n00b_plane_t *grid, int32_t row_gap);
       extern void n00b_grid_set_col_gap(n00b_plane_t *grid, int32_t col_gap);
       extern void n00b_grid_set_span(n00b_plane_t *grid,
                                      n00b_plane_t *child,
                                      int32_t col_span,
                                      int32_t row_span);
       extern void n00b_grid_get_span(n00b_plane_t *grid,
                                      n00b_plane_t *child,
                                      int32_t *col_span,
                                      int32_t *row_span);

5. Create `src/display/widgets/grid.c`. Keep the shared public contract small and put all layout helpers in this file. The file should define `grid_destroy`, `grid_render`, `grid_measure`, `grid_handle_event`, `grid_can_focus`, and `grid_layout`, plus private helper functions that make measurement and layout share the same placement math.

   Implement the public mutators with these exact semantics:

   `n00b_grid_new()` allocates the plane and `n00b_grid_t` record, copies constructor values into the record, applies `gap` as shorthand for both axes before any non-zero `row_gap` or `col_gap` overrides, initializes empty owned arrays for tracks and spans, attaches the widget, and marks the plane dirty.

   `grid_destroy()` frees the owned track array, span array, and the `n00b_grid_t` record itself, and nothing else.

   `n00b_grid_set_columns()` switches the widget into fixed equal-column mode, stores `max(columns, 1)`, clears any copied track array, leaves padding untouched, clears auto-fit mode by setting `min_col_width = 0` and `max_col_width = 0`, and marks the plane dirty.

   `n00b_grid_set_tracks()` copies the provided track array into widget-owned storage, sets `track_count = count`, sets `columns = count`, clears auto-fit mode, and marks the plane dirty. Passing `nullptr` or `count <= 0` clears track mode and falls back to the current fixed-column count.

   `n00b_grid_set_auto_fit()` switches the widget into auto-fit mode by storing the min and max widths, clearing any copied track array, and marking the plane dirty. In this mode `columns` is only a stale fixed-mode fallback and must not be used when `min_col_width > 0`.

   `n00b_grid_set_gap()` sets both `row_gap` and `col_gap` to the same pixel value and marks dirty. The row and column specific setters update only one axis and mark dirty.

   `n00b_grid_set_span()` clamps both spans to at least `1`, requires a non-null child that is currently parented to the grid, upserts the metadata record for that child, and marks the grid dirty.

   `n00b_grid_get_span()` returns the stored span for the given child, or `1, 1` when the child has no metadata entry. It must tolerate null output pointers.

6. Use the following private layout pipeline inside `src/display/widgets/grid.c`.

   First, prune stale span records whose child is null or whose `child->parent != grid`. Then build a temporary list of visible children only, preserving existing child-list order. Placement is always flow-based and row-major. Each visible child uses its stored span or the default `1x1`. `col_span` is clamped to the current column count. `row_span` is clamped only to `>= 1`.

   For fixed-column mode, the effective column count is `max(columns, 1)`.

   For track mode, the effective column count is `max(track_count, 1)`.

   For auto-fit layout mode, resolve the effective column count from the available content width and the visible child count. Start with the largest count that keeps each equal-width column at or above `min_col_width`:

       cols = max(1, min(visible_child_count,
                         (available_width + col_gap) / (min_col_width + col_gap)))

   If `max_col_width > 0`, then while `cols < visible_child_count` and the current equal-width result is greater than `max_col_width`, increment `cols` and recompute. This keeps wide containers from producing absurdly wide columns when enough children exist to fill more columns.

7. Resolve column widths with one deterministic algorithm used by both `measure` and `layout`.

   In fixed equal-column mode, subtract total column gaps from the available width, divide the remaining pixels equally across columns, and assign any remainder pixels left-to-right.

   In track mode, compute a content contribution for each track from visible children. For a child spanning multiple columns, divide its preferred width and minimum width evenly across the spanned columns with ceiling division after subtracting internal grid gaps. Then resolve tracks in this order:

   `FIXED`: width is `value`, clamped by `min_px` and `max_px` if those fields are non-zero.

   `AUTO`: width starts from the maximum content contribution among children touching that track, then applies `min_px` and `max_px`.

   `FR`: width is not assigned until fixed and auto tracks are resolved. Sum all positive `value` fields, treating zero or negative `value` as `1`. Distribute the remaining pixels proportionally, again with left-to-right remainder assignment. Apply `min_px` and `max_px` after each provisional `FR` width is computed. If those clamps create unused leftover pixels, keep the extra space as trailing slack instead of trying to invent a second reflow pass in Wave 1.

   If fixed plus auto widths already exceed the available width, shrink only the `AUTO` tracks toward their computed minimum contributions before `FR` tracks get any space. If that is still insufficient, leave fixed widths untouched, set all `FR` widths to zero, and allow overflow to clip through normal compositor behavior. The unit tests should cover only deterministic, sane cases; this fallback exists so the widget still behaves predictably for impossible layouts.

8. Make `grid_measure()` deterministic and width-agnostic.

   `measure` must ignore hidden children. When the grid is empty, return the padding footprint with a floor of `1x1`.

   In fixed-column and track modes, preferred width is the sum of the resolved natural column widths plus column gaps and padding. Preferred height is the sum of content-driven row heights plus row gaps and padding. Minimum width and minimum height use the same placement map but resolve from child minimum sizes instead of preferred sizes.

   In auto-fit mode, because there is no parent-width hint, `measure` must fall back to a one-column natural size: treat the grid as a single-column flow layout, use `max(min_col_width, widest visible child preferred width)` as the column width, clamp it to `max_col_width` when non-zero, and sum visible child heights into stacked rows. Record this choice in a short code comment because it is an Athens-specific compromise, not prototype behavior.

9. Make `grid_layout()` use the actual content bounds from `n00b_widget_layout()`.

   `grid_layout()` receives content bounds with any outer box insets already removed. It must skip hidden children, resolve the effective column count, build the placement map, resolve column widths, compute column `x` positions, resolve content-driven row heights from child preferred heights, compute row `y` positions, and call `n00b_widget_layout()` on each visible child with the resulting cell rectangle. The grid itself does not draw grid lines or scrollbars and must keep `can_focus = false` and `handle_event = false`.

   Row heights are content-driven only. Determine each row's height by taking the maximum height contribution from children occupying that row. For multi-row spans, divide the child's preferred height evenly across the spanned rows with ceiling division. If the total row height is smaller than the available height, leave the extra space below the last row. Do not reintroduce prototype equal-height rows.

10. Create `test/unit/test_grid.c`. Use the `test_zstack.c` structure as the model and define a small dummy widget with fixed preferred and minimum sizes, click counters, and a `last_bounds` capture field.

    The test file must include at least these cases:

    `test_grid_create_and_api()` proves constructor defaults, vtable attachment, `can_focus == false`, and default span `1x1`.

    `test_grid_fixed_columns_with_span_layout()` lays out a three-column grid with one child spanning all three columns plus several regular children, then asserts each child's assigned bounds and verifies there is no overlap.

    `test_grid_fr_tracks_with_clamps()` uses a known content width and track array containing one fixed column plus two `FR` columns, then asserts the exact left-to-right width distribution including remainder-pixel behavior and min/max clamping.

    `test_grid_auto_fit_wraps_children()` creates several equal-size children, sets auto-fit min and max widths, lays the grid out at a controlled width, and asserts that wrapping occurs at the expected column count.

    `test_grid_hidden_children_do_not_reserve_slots()` hides one middle child before layout and verifies the following visible child occupies the skipped slot.

    `test_grid_mouse_routes_to_laid_out_child()` attaches the grid to a stream canvas, lays it out, sends real mouse-press events into two different cells, and proves the expected child counters increment. This confirms that grid itself does not consume events and that computed child bounds cooperate with normal mouse routing.

11. Extend `src/tools/widget_demo.c`.

    Add `#include "display/widgets/grid.h"`.

    Add a new `demo_grid(n00b_canvas_t *canvas)` helper.

    Add a `"grid"` dispatch branch in `main` and set `use_event_loop = true` for that branch, just as `zstack` does today.

    The demo scene should use the widget meaningfully:

    create one top-level `n00b_grid_new(.canvas = canvas, .columns = 3, .gap = 2 * cpw, .pad_top = 2 * cph, .pad_right = 2 * cpw, .pad_bottom = 2 * cph, .pad_left = 2 * cpw)`;

    add a header card that spans all three columns and contains a title label;

    add a narrow left card that occupies one column and contains at least one interactive control;

    add a wider details or preview card that spans two columns and contains at least one interactive button;

    add a bottom status row spanning all columns, and update its label from button callbacks so a human can confirm click routing and focus traversal.

    Use child `box`-decorated planes or simple plane+label combinations for the cards. The important proof is visible column structure plus clickable descendants, not pixel-perfect styling.

12. Update `docs/widgets.md` so `grid` appears in the implemented widgets list, `test_grid` appears in the representative widget-test list, and Wave 1 no longer lists `grid` as pending.

13. Rebuild the touched targets:

       meson compile -C build_debug test_grid test_mouse test_display_event_dispatch widget_demo

## Validation and Acceptance

The implementation is complete only when all of the following are true.

Run the targeted automated checks first:

    meson test -C build_debug --print-errorlogs grid mouse display_event_dispatch

Acceptance for those tests means:

1. The new unit test `grid` passes and explicitly proves the public API, fixed-column placement, span behavior, `FR` distribution, auto-fit wrapping, hidden-child skipping, and click routing.
2. `mouse` still passes, proving that the grid port did not regress shared hit-testing or capture behavior.
3. `display_event_dispatch` still passes, proving that normal focus and keyboard dispatch still cooperate with a scene containing an interactive grid demo.

Then run the human-visible proof:

    ./build_debug/widget_demo --widget grid --backend tui

If a TTY backend is not convenient, a non-interactive fallback is still useful for symbol and dispatch verification:

    ./build_debug/widget_demo --widget grid --backend stream

For `tui`, acceptance means the scene launches without crashing, the header card visibly spans the full width of the three-column grid, the wide content card visibly spans two columns, and clicking buttons updates the bottom status row rather than being swallowed by the grid container. Tab navigation must move across the interactive descendants in child insertion order because grid itself is not focusable.

For `stream`, acceptance means the tool exits cleanly after rendering one frame and does not report `Unknown widget: grid` or any missing symbol.

The change is accepted only if the public `grid` API exists, the automated tests above pass, and the demo launches through the supported widget-demo entrypoint.

## Idempotence and Recovery

All edit steps in this plan are additive and safe to repeat. Re-running `meson compile` or the named `meson test` commands should only touch normal build artifacts under `build_debug/`.

If targeted builds fail because `build_debug/` is stale, rebuild from the supported wrapper:

    N00B_CLEAN=1 bash build.sh

If `test_grid` fails, fix the grid widget or the new test first before touching shared runtime tests. This widget is intentionally scoped so shared mouse or event-dispatch code should not need edits. If the demo launches but keyboard interaction does not work, check `widget_demo.c` first and confirm the `"grid"` branch sets `use_event_loop = true` before assuming a focus-manager bug. Do not revert unrelated user changes in the worktree.

## Artifacts and Notes

Files that should exist or change by the end of this plan:

- `include/display/widgets/grid.h`
- `src/display/widgets/grid.c`
- `test/unit/test_grid.c`
- `src/tools/widget_demo.c`
- `docs/widgets.md`
- `meson.build`

Observed targeted test transcript during this execution:

    $ meson test -C build_debug --print-errorlogs grid mouse display_event_dispatch
    1/3 unit - n00b:mouse                  OK
    2/3 unit - n00b:grid                   OK
    3/3 unit - n00b:display_event_dispatch OK

Observed non-interactive demo transcript during this execution:

    $ ./build_debug/widget_demo --widget grid --backend stream
    Backend request 'stream' selected 'stream'
    Backend 'stream' has no input polling; using single-frame mode.

Observed TUI proof during this execution:

    `./build_debug/widget_demo --widget grid --backend tui` launched under a pseudo-terminal, rendered the spanning header banner and lower cards, and exited cleanly on `Ctrl-C`.

Expected interactive scene behavior in code:

    A title/header card spans all three columns at the top.
    A narrow left card occupies one column below it.
    A wider details card occupies the next two columns on the same row.
    A status row spans all three columns at the bottom.
    Clicking descendant buttons updates the status row text.

## Interfaces and Dependencies

The feature depends on existing Athens modules only: the widget vtable API in `include/display/widget.h`, plane hierarchy and geometry in `include/display/render/plane.h`, common render/layout types in `include/display/render/types.h`, the current container pattern in `src/display/widgets/box.c`, the existing Wave 1 implementation pattern in `src/display/widgets/zstack.c`, and the shared input systems exercised by `src/display/mouse.c` and `src/display/event_dispatch.c`.

At the end of this plan, these public interfaces must exist exactly as named in `include/display/widgets/grid.h`:

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
    extern n00b_plane_t *n00b_grid_new() _kargs { ... };
    extern void n00b_grid_set_columns(n00b_plane_t *grid, int32_t columns);
    extern void n00b_grid_set_tracks(n00b_plane_t *grid,
                                     const n00b_grid_track_t *tracks,
                                     n00b_isize_t count);
    extern void n00b_grid_set_auto_fit(n00b_plane_t *grid,
                                       int32_t min_col_width,
                                       int32_t max_col_width);
    extern void n00b_grid_set_gap(n00b_plane_t *grid, int32_t gap);
    extern void n00b_grid_set_row_gap(n00b_plane_t *grid, int32_t row_gap);
    extern void n00b_grid_set_col_gap(n00b_plane_t *grid, int32_t col_gap);
    extern void n00b_grid_set_span(n00b_plane_t *grid,
                                   n00b_plane_t *child,
                                   int32_t col_span,
                                   int32_t row_span);
    extern void n00b_grid_get_span(n00b_plane_t *grid,
                                   n00b_plane_t *child,
                                   int32_t *col_span,
                                   int32_t *row_span);

In `src/display/widgets/grid.c`, the vtable must be:

    const n00b_widget_vtable_t n00b_widget_grid = {
        .kind         = "grid",
        .destroy      = grid_destroy,
        .render       = grid_render,
        .measure      = grid_measure,
        .handle_event = grid_handle_event,
        .can_focus    = grid_can_focus,
        .layout       = grid_layout,
    };

The most important private helpers that should exist by the end of the implementation are:

    static n00b_grid_t *grid_data(n00b_plane_t *plane);
    static void grid_prune_stale_spans(n00b_plane_t *grid, n00b_grid_t *data);
    static void grid_child_span(n00b_grid_t *data, n00b_plane_t *child,
                                int32_t *col_span, int32_t *row_span);
    static n00b_isize_t grid_resolve_layout_columns(...);
    static void grid_build_placements(...);
    static void grid_resolve_column_widths(...);
    static void grid_resolve_row_heights(...);

Do not add row-track setters, explicit child placement coordinates, or backend-specific grid drawing APIs in this milestone. The first production `grid` should stay aligned with the approved Wave 1 contract and remain a normal Athens container widget.

## Revision Notes

- 2026-03-13: Initial ExecPlan added for the next Wave 1 production widget reimplementation after `zstack`, using `plans/notes/widget-wave1-design-breakdown.md` as the source-of-truth design contract for `grid`.
- 2026-03-13: Clarified `n00b_grid_new()` and `grid_destroy()` semantics and added `n00b_grid_span_t` to the interface summary so the plan remains fully self-contained for a novice implementer.
- 2026-03-13: Executed the plan, marked the implementation and validation work complete, and recorded the Meson reconfigure requirement so a future contributor can restart from the plan without rediscovering the stale-build-dir failure mode.
