# Reimplement The Wave 1 Tabs Widget In Production

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

There is no repository-local `PLANS.md` in this working tree at the time this plan was authored. This document follows `/home/baron/.codex/PLANS.md` and must remain compliant with that file for all future revisions.

This plan explicitly builds on `plans/notes/widget-port-priority.md`, `plans/notes/widget-wave1-design-breakdown.md`, and `docs/widgets.md`. Those files establish that after the production `zstack`, `grid`, `split`, and `scroll` ports landed, the next missing widget in the Wave 1 queue is `tabs`.

## Purpose / Big Picture

The goal is to land the next production Wave 1 widget in Athens: a focusable `tabs` container that draws a one-line tab header, switches pages by keyboard or mouse, preserves non-selected page state by hiding rather than destroying content planes, and emits a working selection callback. After this change, an Athens application will be able to build multi-page settings panels, inspectors, and dashboards without recreating page subtrees on every tab switch. A contributor or reviewer should be able to prove the result in two ways: run a dedicated `tabs` unit test that exercises add/remove/select behavior, header hit-testing, focus recovery, and repeated switching without content destruction, and run `widget_demo --widget tabs` to switch between pages while button state and scroll position remain intact.

## Progress

- [x] (2026-03-16 23:49Z) Reviewed the Wave 1 queue, the checked-in tabs design dossier, the prototype `tabs` implementation and tests in `n00b-slop`, and the current Athens widget, mouse, focus, event-loop, demo, and Meson patterns that constrain this port.
- [x] (2026-03-16 23:49Z) Resolved the planning decisions that the design dossier left implicit for production work: tabs is the next widget after `scroll`; content planes stay parented and toggle visibility instead of being destroyed; removing the selected tab keeps the same numeric index when a tab shifts into that slot and otherwise falls back to the previous tab; the runtime focus manager must be reachable through `canvas->focus`; and the demo must use existing Athens widgets rather than the still-missing `text` widget.
- [x] (2026-03-16 23:49Z) Drafted this self-contained execution plan at `plans/wave1-tabs-production-reimplementation-execplan.md`.
- [x] (2026-03-17 00:04Z) Implemented `include/display/widgets/tabs.h` and `src/display/widgets/tabs.c`, including visibility-based page switching, header hitboxes, wrap-around keyboard navigation, mouse selection, immediate relayout, and focus repair through `canvas->focus`.
- [x] (2026-03-17 00:09Z) Wired `canvas->focus` through `src/display/event_loop.c`, added `test/unit/test_tabs.c`, updated `meson.build`, extended `src/tools/widget_demo.c` with the `tabs` demo, and moved `tabs` into the implemented set in `docs/widgets.md`.
- [x] (2026-03-17 00:13Z) Validated the implementation with `meson compile -C build_debug test_tabs widget_demo`, `meson test -C build_debug --print-errorlogs tabs`, `./build_debug/widget_demo --widget tabs --backend stream`, and an ANSI PTY smoke run that launched the TUI demo and switched from `Counter` to `Scroll` via the Right Arrow key.
- [x] (2026-03-19 15:50Z) Follow-up review remediation landed in `plans/wave1-tabs-review-remediation-execplan.md`: `n00b_focus_mgr_new(canvas)` now owns `canvas->focus`, `n00b_focus_mgr_rebuild()` blurs dropped hidden focus, tabs measurement accepts plain planes, and tab switches/removals cancel hidden-page mouse capture before hiding content.

## Surprises & Discoveries

- Observation: Prototype `tabs` stores the selected index by reusing the `nctabbed` pointer field and destroys/recreates content planes when switching tabs.
  Evidence: `../n00b-slop/src/ctui/widgets/tabs.c` uses `tabs_get_selected_index()`/`tabs_set_selected_index()` on `nctabbed`, plus `destroy_widget_planes()` inside both `ctui_tabs_select()` and `ctui_tabs_remove()`.

- Observation: `n00b_canvas_t` already has a documented `focus` pointer, but the Athens event loop never assigns the newly created focus manager into that field.
  Evidence: `include/display/render/canvas.h` documents `struct n00b_focus_mgr_t *focus;`, while `src/display/event_loop.c` creates `n00b_focus_mgr_t *fm = n00b_focus_mgr_new(canvas);` and later destroys it without setting `canvas->focus = fm`.

- Observation: Focus traversal order is parent-first and depth-first, which means a focusable tabs plane can serve as the stable fallback target after the currently focused page is hidden.
  Evidence: `src/display/focus.c` adds the current plane to `fm->focusable` before recursing into `plane->children`.

- Observation: Mouse routing already rebases bubbled events into each parent plane's local coordinates, so tabs header hit-testing can be expressed directly in tabs-local pixel rectangles.
  Evidence: `src/display/mouse.c` recomputes `local_event` inside the parent-bubbling loop with `mouse_event_to_plane_local(canvas, cur, event, &local_event)`.

- Observation: Ordinary input in the Athens event loop rerenders dirty planes but does not rerun layout, so add/remove/select operations on tabs must relayout immediately when valid bounds are already known.
  Evidence: `src/display/event_loop.c` calls `n00b_display_scene_run_layout(canvas)` during initial render and resize handling, but ordinary key and mouse events only flow through dispatch and dirty rerendering.

- Observation: The design dossier's suggested `scroll + text` demo cannot be implemented yet because `text` is still the final missing Wave 1 widget.
  Evidence: `plans/notes/widget-wave1-design-breakdown.md` suggests a `scroll + text` demo for tabs, while `docs/widgets.md` still lists `text` as missing and `scroll` as implemented.

- Observation: The stream backend is suitable for crash-free render smoke tests, but it never exercises the interactive event loop because it has no input polling.
  Evidence: `./build_debug/widget_demo --widget tabs --backend stream` printed `Backend 'stream' has no input polling; using single-frame mode.` and exited cleanly.

- Observation: In the ANSI TUI demo, the tabs header is focused on initial render and responds to Right Arrow immediately, so the selection callback and page switch are visible without an extra Tab keypress first.
  Evidence: A PTY run of `./build_debug/widget_demo --widget tabs --backend tui` showed the initial focused `Counter` header, and sending `ESC [ C` updated the status line to `Selected tab 1: Scroll (previous 0).`

- Observation: The original landing's event-loop-only `canvas->focus` wiring was not sufficient for callers that construct focus managers directly outside `n00b_canvas_run()`.
  Evidence: `plans/wave1-tabs-review-remediation-execplan.md` added failing direct-constructor regression coverage in `test/unit/test_focus.c`, and the fix moved `canvas->focus` ownership into `src/display/focus.c`.

## Decision Log

- Decision: The next Wave 1 production widget after `zstack`, `grid`, `split`, and `scroll` is `tabs`, and this plan targets that widget only.
  Rationale: `plans/notes/widget-port-priority.md` orders Wave 1 as `stack`, `grid`, `split`, `scroll`, `tabs`, `text`, and `docs/widgets.md` now shows only `tabs` and `text` still missing from Wave 1.
  Date/Author: 2026-03-17 / Codex.

- Decision: Production tabs will keep every content plane parented to the tabs widget and switch pages by toggling `N00B_PLANE_VISIBLE` rather than by destroying and recreating planes.
  Rationale: The checked-in design dossier explicitly requires visibility-based switching, and this avoids the prototype's tab-switch crash class while preserving page-local widget state such as button counters and scroll offsets.
  Date/Author: 2026-03-17 / Codex.

- Decision: `n00b_tabs_remove()` will preserve the same numeric selection slot when possible and otherwise fall back to the previous remaining tab; when no tabs remain, `selected_index` becomes `-1`.
  Rationale: This matches the prototype's effective middle-removal behavior, gives stable results for users removing the active tab, and makes the empty-state contract explicit instead of overloading `0`.
  Date/Author: 2026-03-17 / Codex.

- Decision: Tabs measurement will use the maximum content preferred and minimum sizes across all tab entries, not just the currently selected tab.
  Rationale: Athens applications should not see the widget's natural size oscillate just because the selected page changed. Hidden panes remain real children, so measuring all of them is cheap and yields a more stable container contract.
  Date/Author: 2026-03-17 / Codex.

- Decision: Add, remove, and selection changes will relayout the tabs widget immediately with `n00b_widget_layout(tabs, tabs->bounds)` whenever valid bounds already exist.
  Rationale: The Athens event loop does not rerun layout on ordinary input, and newly selected or newly added pages need current content bounds before the next render.
  Date/Author: 2026-03-17 / Codex.

- Decision: The event loop will assign `canvas->focus = fm` for the lifetime of `n00b_canvas_run()`, and tabs will use that pointer to rebuild focus after visibility changes.
  Rationale: `n00b_canvas_t` already documents a focus-manager backpointer, and tabs is the first production widget that needs to repair focus when an entire subtree becomes hidden during normal interaction.
  Date/Author: 2026-03-17 / Codex.

- Decision: Review remediation superseded the event-loop-only focus wiring by moving `canvas->focus` ownership into `n00b_focus_mgr_new()` and `n00b_focus_mgr_destroy()`.
  Rationale: the public constructor is used by direct-dispatch and custom-loop callers as well as `n00b_canvas_run()`, so the backpointer contract needed to hold across all focus-manager entrypoints rather than only inside the event loop.
  Date/Author: 2026-03-19 / Codex.

- Decision: Tabs page transitions must cancel hidden-page mouse capture through the shared cancel API, and tabs measurement must treat plain content planes as supported public inputs.
  Rationale: review remediation proved both behaviors are part of ordinary supported tabs usage, not optional polish, because the public API accepts arbitrary `n00b_plane_t *` pages and hidden captured pages otherwise keep receiving drag events.
  Date/Author: 2026-03-19 / Codex.

- Decision: If a tab switch or tab removal hides the currently focused plane, tabs will rebuild the focus manager and then set focus explicitly back to the tabs plane.
  Rationale: Relying on a plain rebuild can move focus to an unrelated earlier widget elsewhere on the canvas. Returning focus to the header keeps left/right navigation immediately usable and is predictable for both keyboard and mouse workflows.
  Date/Author: 2026-03-17 / Codex.

- Decision: The tabs demo will use existing Athens widgets only: `box`, `label`, `button`, and `scroll`.
  Rationale: The design dossier's `scroll + text` suggestion depends on the still-unimplemented Wave 1 `text` widget, so the production demo must show the same integration points with widgets that already exist.
  Date/Author: 2026-03-17 / Codex.

- Decision: `n00b_tabs_remove()` restores a removed content plane to visible state after detaching it from the tabs widget.
  Rationale: Tabs owns hidden-page state only while a plane remains an inactive tab. Once removed, the plane should be ready for immediate reparenting elsewhere without inheriting a stale hidden flag from its former tab slot.
  Date/Author: 2026-03-17 / Codex.

## Outcomes & Retrospective

Implementation completed on 2026-03-17, with review remediation follow-up landed on 2026-03-19. Athens now has a production `tabs` widget under `include/display/widgets/tabs.h` and `src/display/widgets/tabs.c`, focus-manager lifecycle owns the live `canvas->focus` backpointer, `test_tabs` and `test_focus` cover the public API plus the hidden-focus/plain-plane/capture regressions discovered in review, `widget_demo --widget tabs` is a working entrypoint, and `docs/widgets.md` now leaves only `text` in Wave 1.

The automated validation matrix passed, including the review-remediation follow-up. `meson test -C build_debug --print-errorlogs tabs focus scroll display_event_dispatch` is now green, the stream demo smoke exited cleanly, and the ANSI PTY smoke rendered the demo and switched pages via Right Arrow with the status line updating to the selected tab name. A full human mouse-and-scroll visual pass in a real terminal is still advisable, but the implementation now meets the plan's observable production bar, exposes a consistent public focus-manager contract, and removes `tabs` from the Wave 1 backlog without the original hidden-plane edge cases.

## Context and Orientation

Athens widgets are planes with attached behavior. The plane type is `n00b_plane_t` in `include/display/render/plane.h`, and widget behavior is described by `n00b_widget_vtable_t` in `include/display/widget.h`. A widget can render draw commands, report preferred and minimum sizes, handle events, decide whether it can receive focus, and optionally lay out child planes.

The production `tabs` widget belongs under `include/display/widgets/` and `src/display/widgets/` beside the already-landed Wave 1 widgets `grid`, `split`, `scroll`, and `zstack`. Like `zstack` and `box`, it will be a container widget. Unlike those widgets, it is also focusable because left and right arrow keys switch the active page when the tabs header has focus.

In this repository, a "hidden plane" means a child plane whose `N00B_PLANE_VISIBLE` flag is cleared with `n00b_plane_set_visible(plane, false)`. Hidden planes stay parented and keep their widget-specific data, but they are skipped by rendering, hit-testing, and focus collection. That is the mechanism tabs must use for non-selected pages.

The focus manager is implemented in `include/display/focus.h` and `src/display/focus.c`. It maintains the ordered list of visible focusable planes and decides which plane receives keyboard events. The event loop in `src/display/event_loop.c` creates that manager, and mouse click-to-focus runs through `src/display/mouse.c`. Tabs needs these files because switching pages changes which descendants are visible and therefore changes which descendants are allowed to keep focus.

Prototype behavior lives in the sibling repository under `../n00b-slop/`. The important prototype files are `../n00b-slop/include/ctui/widgets/tabs.h`, `../n00b-slop/src/ctui/widgets/tabs.c`, `../n00b-slop/test/ctui/test_tabs.c`, and `../n00b-slop/test/ctui/test_tabs_switch.c`. The prototype proves the user-facing behavior to preserve, but it also contains the notcurses-specific plane destruction path that Athens must explicitly avoid.

Current production patterns worth copying are `src/display/widgets/breadcrumb.c` for one-line text hitboxes plus keyboard navigation, `src/display/widgets/zstack.c` for container-child management, `src/display/widgets/scroll.c` for immediate relayout on state changes and the theme-aware demo pattern, and `test/unit/test_grid.c`, `test/unit/test_scroll.c`, and `test/unit/test_zstack.c` for current unit-test style.

## Plan of Work

The first edit is to add the public tabs widget surface in `include/display/widgets/tabs.h` and `src/display/widgets/tabs.c`. The header must define the public enums, entry type, callback type, public state struct, vtable symbol, constructor, and accessor/mutator functions promised by `plans/notes/widget-wave1-design-breakdown.md`. The source file must implement a focusable container that draws its own header strip, caches one header hit rectangle per tab in tabs-local pixel coordinates, parents all content planes directly under the tabs plane, and hides every non-selected page.

Inside `src/display/widgets/tabs.c`, keep all bookkeeping and geometry helpers file-local. Define helpers that grow the entry arrays, rebuild header hitboxes from measured tab-label widths, apply page visibility for the current `selected_index`, detect whether a plane belongs to a tab-content subtree, and trigger relayout/focus repair after structural changes. The widget should reuse the existing theme palette rather than inventing a new style surface: separators should use a dim/secondary palette role, unselected tabs should use secondary text, and the selected tab should render with stronger styling that becomes more prominent when the tabs plane is focused.

The layout contract in code should be simple and explicit. The tabs plane renders only the header row itself. The content rectangle is the incoming content bounds minus one line height at the top or bottom depending on `position`. Every content plane should be laid out to that same rectangle on each layout pass so that a later selection change only needs to toggle visibility and rerender. Because the event loop does not rerun layout on ordinary input, `add`, `remove`, and `select_index` must rerun `n00b_widget_layout()` immediately when `plane->bounds` already describes real bounds.

The original landing used a small runtime seam in `src/display/event_loop.c` to mirror the new focus manager into `canvas->focus`. Review remediation in `plans/wave1-tabs-review-remediation-execplan.md` later moved that ownership into `src/display/focus.c` so `n00b_focus_mgr_new(canvas)` registers itself and `n00b_focus_mgr_destroy()` unregisters it for every caller, not just `n00b_canvas_run()`. Tabs still uses `canvas->focus` to call `n00b_focus_mgr_rebuild()` after it changes page visibility and to set focus back to the tabs plane if the current focused page was just hidden; the difference is that the backpointer contract now lives with the focus-manager lifecycle instead of the event loop.

The third edit is the test suite. Add `test/unit/test_tabs.c` in the same style as `test_grid.c`, `test_scroll.c`, and `test_zstack.c`: small dummy widgets with controlled measurements, captured bounds, and click counters. The tests must prove observable behavior rather than just structure. That means creating a tabs widget, adding pages, switching by key and mouse, checking header hitboxes and content bounds, asserting repeated switching keeps the same child planes alive, and verifying focus is repaired when the active page containing the current focused plane is hidden.

The fourth edit is tooling and documentation. Update `meson.build` so the library builds `src/display/widgets/tabs.c` and the unit suite includes `test_tabs`. Extend `src/tools/widget_demo.c` with a `tabs` demo and usage/dispatch support. The demo should show at least three pages: one page with a button whose counter persists when returning to the page, one page with a scrollable long panel to prove `tabs + scroll` integration, and one static info page. Update `docs/widgets.md` so `tabs` moves into the implemented set, `test_tabs` joins the representative widget tests, and Wave 1 shows only `text` still missing.

## Concrete Steps

Run all commands from `/home/baron/crash-override/n00b-tui/n00b-athens`.

1. Refresh the tabs design and runtime seams before editing:

       sed -n '/^## tabs$/,/^## text$/p' plans/notes/widget-wave1-design-breakdown.md
       sed -n '1,260p' ../n00b-slop/src/ctui/widgets/tabs.c
       sed -n '1,240p' ../n00b-slop/test/ctui/test_tabs.c
       sed -n '1,220p' ../n00b-slop/test/ctui/test_tabs_switch.c
       sed -n '1,220p' src/display/event_loop.c
       sed -n '1,220p' src/display/focus.c
       sed -n '1,220p' src/display/mouse.c
       sed -n '1550,1810p' src/tools/widget_demo.c
       sed -n '1,220p' docs/widgets.md

   Expected result: you can see the Wave 1 tabs API contract, the prototype's plane-destruction switching path, the prototype's navigation and crash-regression tests, the missing `canvas->focus` assignment in the event loop, the parent-first focus traversal order, the current mouse-localization behavior, the widget demo dispatch table, and the docs state showing `tabs` still missing.

2. Edit `meson.build` in two places. Add `src/display/widgets/tabs.c` to the main display source list immediately beside `scroll.c` and `zstack.c`. Add a new unit target:

       tabs_test = executable('test_tabs',
           ['test/unit/test_tabs.c'],
           kwargs: test_common_kwargs,
       )
       test('tabs', tabs_test, suite: 'unit')

   Keep the ordering next to `scroll` and `zstack` so the Wave 1 widget block stays easy to scan.

3. Create `include/display/widgets/tabs.h` with the exact public contract below. Match the repository's existing widget-header style, including `#pragma once`, shared includes, and the `_kargs` constructor declaration.

       typedef enum {
           N00B_TABS_TOP,
           N00B_TABS_BOTTOM,
       } n00b_tabs_position_t;

       typedef struct n00b_tab_entry_t {
           n00b_string_t *name;
           n00b_plane_t  *content;
       } n00b_tab_entry_t;

       typedef void (*n00b_tabs_select_cb_t)(n00b_plane_t *tabs,
                                             int           new_index,
                                             int           old_index,
                                             void         *data);

       typedef struct n00b_tabs_t {
           n00b_tab_entry_t      *entries;
           n00b_rect_t           *header_rects;
           n00b_isize_t           count;
           n00b_isize_t           capacity;
           int32_t                selected_index;
           n00b_tabs_position_t   position;
           n00b_string_t         *separator;
           n00b_tabs_select_cb_t  on_select;
           void                  *on_select_data;
           int32_t                header_height_px;
       } n00b_tabs_t;

       extern const n00b_widget_vtable_t n00b_widget_tabs;

       extern n00b_plane_t *
       n00b_tabs_new() _kargs {
           n00b_tabs_position_t  position       = N00B_TABS_TOP;
           n00b_string_t        *separator      = nullptr;
           n00b_tabs_select_cb_t on_select      = nullptr;
           void                 *on_select_data = nullptr;
           n00b_canvas_t        *canvas         = nullptr;
           n00b_allocator_t     *allocator      = nullptr;
       };

       extern int               n00b_tabs_add(n00b_plane_t *tabs, n00b_string_t *name, n00b_plane_t *content);
       extern bool              n00b_tabs_remove(n00b_plane_t *tabs, int index);
       extern bool              n00b_tabs_select_index(n00b_plane_t *tabs, int index);
       extern int               n00b_tabs_selected_index(n00b_plane_t *tabs);
       extern int               n00b_tabs_count(n00b_plane_t *tabs);
       extern n00b_tab_entry_t *n00b_tabs_get(n00b_plane_t *tabs, int index);
       extern bool              n00b_tabs_next(n00b_plane_t *tabs);
       extern bool              n00b_tabs_prev(n00b_plane_t *tabs);

   Public contract details that must remain true:

   `n00b_tabs_selected_index()` returns `-1` when there is no selected tab or the plane is the wrong widget kind.

   `n00b_tabs_get()` returns a pointer into the widget's internal entry array. That pointer remains valid only until the next add or remove operation.

   The widget owns only parent links and its own bookkeeping arrays. It does not free or destroy content planes on removal, switching, or widget destruction.

4. Create `src/display/widgets/tabs.c`. Keep all geometry, focus, and array helpers file-local. The file must define `tabs_destroy`, `tabs_render`, `tabs_measure`, `tabs_handle_event`, `tabs_can_focus`, and `tabs_layout`, plus private helpers for header geometry, selection changes, capacity growth, and focus repair.

   Implement the public mutators with these exact semantics:

   `n00b_tabs_new()` allocates the plane and `n00b_tabs_t` record, stores the requested position/callback/configuration, leaves `selected_index = -1` until the first add, attaches the widget, and marks the plane dirty.

   `n00b_tabs_add()` appends a new entry, requires `name != nullptr`, requires `content == nullptr || content->parent == nullptr`, parents the content plane under the tabs plane when provided, makes the first added tab selected and visible, makes later tabs hidden, relayouts immediately when bounds are known, rebuilds focus ordering if `canvas->focus` exists, and returns the new zero-based index. On invalid input it returns `-1`.

   `n00b_tabs_remove()` detaches the removed content plane if it exists, never destroys that plane, compacts the entry and header-rect arrays, updates `selected_index` using the same-slot-else-previous rule from `Decision Log`, relayouts immediately when bounds are known, rebuilds focus, and if the old focused plane belonged to the removed or newly hidden subtree, explicitly focuses the tabs plane. It returns `true` only when a real entry was removed.

   `n00b_tabs_select_index()` validates the index, returns `false` on invalid or no-op requests, updates `selected_index`, toggles child visibility so exactly one page is visible, rerenders and relayouts when bounds are known, emits `on_select(tabs, new_index, old_index, data)` after state changes, rebuilds focus through `canvas->focus`, and if the old focused plane is now hidden, sets focus to the tabs plane.

   `n00b_tabs_next()` and `n00b_tabs_prev()` wrap around when `count > 0` and return the result of `n00b_tabs_select_index()`.

5. Use the following measurement and layout rules inside `src/display/widgets/tabs.c`.

   The header strip is always exactly one line high:

       header_h = n00b_widget_line_px_height(plane)

   Header preferred width is the sum of every tab-label width plus separator widths between labels. Content preferred and minimum sizes are the maximum preferred and minimum sizes across all entries whose `content` plane is non-null.

   Report:

       pref_w = max(header_total_w, max_content_pref_w, 1)
       pref_h = header_h + max(max_content_pref_h, 1)
       min_w  = max(max_content_min_w, 1)
       min_h  = header_h + max(max_content_min_h, 1)

   When there are no tabs, report `pref_w = 1`, `min_w = 1`, and both heights equal `header_h`.

   During layout, cache `header_height_px = header_h`, compute the header strip rectangle at the top or bottom according to `position`, compute the remaining content rectangle, cache one `header_rects[i]` entry per tab in tabs-local coordinates, and lay out every content plane to the shared content rectangle. Selected content must be visible, unselected content must be hidden, and no content plane may overlap the header strip.

6. Render only the header strip from `tabs_render()`. Clear the plane first, then draw tab labels and separators using theme palette roles. At minimum the styles must distinguish these states:

   The selected tab uses stronger styling than unselected tabs.

   When the tabs plane is focused or active, the selected tab becomes visually more prominent than in the unfocused state.

   Separators use a dim or secondary palette role and are never drawn with the selected-tab style.

   Cache header rectangles in local coordinates where `x` is the measured label start and `width` is the measured label width only. Clicking separator gaps should not select a tab.

7. Handle events in `tabs_handle_event()` with these rules:

   `can_focus` returns `true`.

   `N00B_KEY_LEFT` selects the previous tab with wrap-around when `count > 0`.

   `N00B_KEY_RIGHT` selects the next tab with wrap-around when `count > 0`.

   Mouse left-press inside a cached `header_rects[i]` selects that tab.

   Mouse clicks outside the header strip return `false` so page content and ancestor containers continue to work normally.

   The widget does not consume wheel events, scroll gestures, or keyboard activate keys by default in Wave 1.

8. Edit `src/display/event_loop.c` so the canvas exposes the live focus manager to widgets:

       n00b_focus_mgr_t *fm = n00b_focus_mgr_new(canvas);
       canvas->focus = fm;
       ...
       canvas->focus = nullptr;
       n00b_focus_mgr_destroy(fm);

   No other event-loop behavior should change.

9. Add `test/unit/test_tabs.c`. Follow the style of `test_grid.c`, `test_scroll.c`, and `test_zstack.c`: deterministic dummy widgets, helper functions to build a stream canvas, and direct assertions over bounds, selection state, visibility, focus, and callback counters. Include at least these tests:

   `test_tabs_create_and_api()` verifies constructor defaults, `can_focus == true`, empty-state getters, add/count/get behavior, and first-tab auto-selection.

   `test_tabs_measure_uses_header_and_max_content()` verifies the exact measurement formulas from this plan and confirms the widget does not shrink to the currently selected page only.

   `test_tabs_layout_top_and_bottom_headers()` lays out tabs in both positions, asserts the selected and hidden child bounds, and checks that cached header rectangles sit on the expected header row.

   `test_tabs_keyboard_navigation_wraps_and_fires_callback()` drives left/right navigation through `n00b_widget_handle_event()`, asserts wrap-around, callback arguments, and visibility changes.

   `test_tabs_mouse_header_click_selects_page()` sends a click through `n00b_mouse_route_event()` using cached header rectangles and asserts the clicked tab becomes selected.

   `test_tabs_switch_preserves_content_planes_and_state()` repeatedly switches between two tabs and asserts the same content plane pointers remain alive, parented, and capable of preserving per-page state.

   `test_tabs_focus_returns_to_header_when_hidden_page_was_focused()` creates a canvas plus focus manager, stores it in `canvas->focus`, focuses a child inside the selected page, switches tabs, and asserts the focus manager rebuild leaves the tabs plane focused rather than an unrelated widget.

   `test_tabs_remove_selected_uses_same_slot_then_previous()` removes the active middle tab, then the active last tab, and verifies the selection rule from `Decision Log`.

10. Extend `src/tools/widget_demo.c`. Add `#include "display/widgets/tabs.h"`, add one demo status label for tab-selection and page-action messages, add one persistent counter label for a page-local button, add a `tabs` selection callback, add `demo_tabs(n00b_canvas_t *canvas)`, add `"tabs"` to the usage text, add a `"tabs"` dispatch branch in `main`, and set `use_event_loop = true` for that branch.

    The demo scene should be a root vertical `box` with a title label, a short instructions label, a status label, and a growable tabs widget. Build at least three pages:

    A page with a button that increments a visible counter so state retention is obvious after switching away and back.

    A page that embeds a `scroll` widget containing a tall `box` of labels and a bottom button so the user can prove `tabs + scroll` interaction still works after switching pages.

    A page with static summary content so the demo shows both interactive and non-interactive tabs.

    The selection callback should update the status label with the selected tab index and name. Button callbacks should update the same status label with page-local messages.

11. Update `docs/widgets.md`. Move `tabs` from the Wave 1 backlog into the implemented widgets list, add `test_tabs` to the representative widget-test sentence, and leave `text` as the only remaining Wave 1 widget.

12. If `build_debug/` does not yet know about the new target graph, refresh it:

       meson setup --reconfigure build_debug

   Expected result: Meson regenerates the build graph without complaining about unknown sources or missing targets.

13. Build the touched targets:

       meson compile -C build_debug test_tabs widget_demo

   Expected transcript shape:

       ninja: Entering directory `build_debug`
       [1/N] Compiling ...
       [N/N] Linking target widget_demo

14. Run the targeted automated checks:

       meson test -C build_debug --print-errorlogs tabs

   Expected transcript shape:

       1/1 tabs OK

15. Run a non-interactive demo smoke on the stream backend:

       ./build_debug/widget_demo --widget tabs --backend stream

   Expected result: the tool renders one tabs scene and exits without `Unknown widget`, assertion failures, or crashes.

16. Run the interactive manual check:

       ./build_debug/widget_demo --widget tabs --backend tui

   Expected result: a tabs scene appears with a visible header strip. Tab to the header and use left/right arrows to cycle pages, click tab labels with the mouse, click the counter button on the first page, switch away and back to confirm the counter persists, open the scrollable page and scroll down until the bottom button can be clicked, and verify the status label updates after every selection or page action.

## Validation and Acceptance

The implementation is complete only when all of the following are true.

- `include/display/widgets/tabs.h` and `src/display/widgets/tabs.c` exist and expose exactly the public API promised in `plans/notes/widget-wave1-design-breakdown.md` and this ExecPlan.
- `test/unit/test_tabs.c` exists and the `tabs` Meson target passes.
- The production widget never destroys content planes during tab switching or removal; it only detaches them when removed and frees only its own bookkeeping arrays on destruction.
- Exactly one page is visible at a time, and hidden pages do not participate in hit-testing or focus collection.
- Keyboard left/right navigation wraps across all tabs when the tabs header is focused.
- Mouse clicks on header labels select the correct page.
- If the currently focused descendant belongs to a page that becomes hidden, focus returns to the tabs plane.
- `widget_demo --widget tabs` is a recognized, working demo entrypoint.
- `docs/widgets.md` lists `tabs` as implemented and leaves `text` as the only missing Wave 1 widget.

Acceptance is behavioral, not just structural:

- `test_tabs_switch_preserves_content_planes_and_state()` must fail before the tabs implementation exists and pass after it.
- `test_tabs_focus_returns_to_header_when_hidden_page_was_focused()` must fail before tabs rebuilds focus through `canvas->focus` and pass after that behavior is implemented.
- While running `widget_demo --widget tabs --backend tui`, the user must be able to switch pages repeatedly without crashes, the first page's counter must persist after switching away and back, and the scrollable page's bottom button must remain clickable after scrolling.

## Idempotence and Recovery

All edits in this plan are additive and deterministic. Re-running the public tabs mutators on an already-configured widget should only recompute visibility, geometry, and focus state; it should not duplicate children or free content planes.

If the build reports an unknown `tabs` target after `meson.build` was edited, rerun `meson setup --reconfigure build_debug` and then repeat the compile command. If the demo shows correct rendering but keyboard focus does not return to the header after switching away from a focused child page, inspect `src/display/event_loop.c` first for a missing `canvas->focus` assignment before changing the tabs widget itself. If a partial implementation leaves `test_tabs` failing, keep the new files and iterate; do not delete or revert the already-landed Wave 1 widgets.

## Artifacts and Notes

Use these exact user-visible behaviors as the review checklist during implementation:

    Tabs header:
        one text line high
        top or bottom according to `position`
        draws labels + separators on the tabs plane itself

    Page switching:
        hides old page
        shows new page
        keeps page subtrees parented and alive
        invokes `on_select`

    Focus repair:
        rebuild focus manager after visibility changes
        if the old focused plane is now hidden, focus the tabs plane

Use these exact selection rules for removal:

    remove selected index i:
        if another tab shifts into index i, select that tab
        else select index i - 1
        else selected index becomes -1

## Interfaces and Dependencies

In `include/display/widgets/tabs.h`, define exactly the public types and functions listed in `Concrete Steps`. Do not add a public `set_content` or `insert_after` API in this production pass; the checked-in design dossier fixed the Wave 1 surface already, and the implementation should not reopen it.

In `src/display/widgets/tabs.c`, define the widget vtable as:

    const n00b_widget_vtable_t n00b_widget_tabs = {
        .kind         = "tabs",
        .destroy      = tabs_destroy,
        .render       = tabs_render,
        .measure      = tabs_measure,
        .handle_event = tabs_handle_event,
        .can_focus    = tabs_can_focus,
        .layout       = tabs_layout,
    };

Keep the following helper interfaces file-local and implement them in plain C inside `src/display/widgets/tabs.c`:

    static n00b_tabs_t *tabs_data(n00b_plane_t *plane);
    static bool tabs_has_valid_bounds(const n00b_plane_t *plane);
    static void tabs_ensure_capacity(n00b_tabs_t *tabs, n00b_isize_t needed);
    static void tabs_recompute_header_rects(n00b_plane_t *plane, n00b_tabs_t *tabs);
    static void tabs_apply_visibility(n00b_tabs_t *tabs);
    static bool tabs_plane_is_descendant_of(n00b_plane_t *plane, n00b_plane_t *ancestor);
    static void tabs_relayout_if_needed(n00b_plane_t *plane);
    static void tabs_rebuild_focus_after_visibility_change(n00b_plane_t *plane,
                                                           n00b_plane_t *old_focus,
                                                           n00b_plane_t *old_content,
                                                           bool          force_tabs_focus);

`tabs_rebuild_focus_after_visibility_change()` must use `plane->canvas->focus` only when that pointer is non-null. It should be safe in unit tests that manually assign `canvas->focus`, and in the interactive event loop after `src/display/event_loop.c` is updated to populate that field.

The implementation depends only on already-landed subsystems and widgets:

- `include/display/widget.h` and `src/display/widget.c` for attach, measure, event dispatch, and layout entry points.
- `include/display/render/plane.h` and `src/display/render/plane.c` for child parenting, visibility toggling, and drawing.
- `include/display/focus.h` and `src/display/focus.c` for rebuilding and setting focus after hidden-page transitions.
- `src/display/mouse.c` for parent-local mouse coordinates during header hit-testing.
- `include/internal/display/widget_primitives.h` for line-height, cell-width, widget-state, and keyboard/mouse helper functions.
- `include/text/strings/theme.h` for palette-based header styling.
- Existing widgets `box`, `button`, `label`, and `scroll` for the tabs demo.

## Revision Notes

- 2026-03-17: Initial ExecPlan added for the Wave 1 production `tabs` reimplementation after confirming `tabs` is the next missing widget in `docs/widgets.md`. The plan resolves the production focus-rebuild strategy, tab-removal selection rule, and demo composition strategy that avoids depending on the still-missing `text` widget.
- 2026-03-17: Updated the ExecPlan after implementation to record the shipped widget/files, the validation commands and observed outcomes, the stream/TUI demo findings, and the final visibility-reset decision for removed tab content.
- 2026-03-19: Updated the historical production plan after `plans/wave1-tabs-review-remediation-execplan.md` landed so this document records the lifecycle-owned `canvas->focus` contract plus the plain-plane and hidden-capture fixes that were required to complete the public tabs behavior.
