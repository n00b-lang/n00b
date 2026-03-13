# Reimplement The Wave 1 ZStack Widget In Production

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

There is no repository-local `PLANS.md` in this working tree at the time this plan was authored. This document follows `/home/baron/.codex/PLANS.md` and must remain compliant with that file for all future revisions.

This plan explicitly builds on `plans/notes/widget-port-priority.md`, `plans/wave1-widget-design-breakdown-execplan.md`, and `plans/notes/widget-wave1-design-breakdown.md`. Those files establish that the first missing production widget in the Wave 1 queue is the prototype `stack` port, and that its Athens public name must be `zstack`.

## Purpose / Big Picture

The goal is to land the first production Wave 1 widget in Athens: a layering container named `zstack`. After this change, an Athens application will be able to put multiple child planes in the same bounds, render them back-to-front, and reorder them at runtime with `bring_to_front` and `send_to_back`. A contributor or reviewer should be able to prove the result in two ways: run a dedicated `zstack` unit test that exercises overlapping click routing and reordering, and run `widget_demo --widget zstack` to see an overlay panel visibly rendered above a background layer.

## Progress

- [x] (2026-03-11 15:56Z) Reviewed the Wave 1 design dossier, the prototype `stack` widget, Athens plane/widget runtime files, existing widget implementations, and current test/demo wiring.
- [x] (2026-03-11 15:56Z) Resolved the implementation target for the first production widget: `stack` must ship as `zstack` because `n00b_stack_new` is already an ADT macro in `include/adt/stack.h`.
- [x] (2026-03-11 15:56Z) Drafted this self-contained execution plan with concrete file edits, validation commands, and acceptance criteria for the production `zstack` reimplementation.
- [x] (2026-03-12 00:23Z) Implemented `include/display/widgets/zstack.h`, `src/display/widgets/zstack.c`, and the `meson.build` registrations for the new widget source and unit test.
- [x] (2026-03-12 00:23Z) Added regression coverage for z-order hit-testing, measurement/layout behavior, pop/detach semantics, subtree canvas propagation during reparenting, and top-level subtree canvas clearing.
- [x] (2026-03-12 00:23Z) Added a `widget_demo --widget zstack` scene, updated `docs/widgets.md`, rebuilt the project with `N00B_CLEAN=1 bash build.sh`, and passed the targeted validation matrix in this plan.
- [x] (2026-03-12 00:42Z) Reworked the manual `zstack` demo controls so a separate top-level control panel now drives `bring_to_front` and `send_to_back` on the overlay card layer.
- [x] (2026-03-13 09:54Z) Fixed the operator-reported demo regression by restructuring `widget_demo --widget zstack` into a single top-level row layout with a sibling control column, and added a regression test for side controls plus overlay clicks in the same scene.
- [x] (2026-03-13 10:11Z) Fixed the compositor ordering bug that let deeper descendants of a back layer paint above shallower descendants of a front layer, and added render-canvas regression coverage for sibling-subtree painter order.

## Surprises & Discoveries

- Observation: In Athens, sibling child-list order is the real behavior seam for layered widgets. Mouse hit-testing walks children in reverse order, focus collection walks children in forward order, and compositing preserves insertion order when `z` ties.
  Evidence: `src/display/mouse.c` recurses through `plane->children` from `len - 1` to `0`; `src/display/focus.c` collects children from `0` upward; `src/display/render/composite.c` sorts equal `abs_z` entries by flatten order.

- Observation: `n00b_plane_add_child()` only assigns the parent canvas to the immediate child, while `n00b_canvas_add_plane()` recursively propagates canvas pointers to the whole subtree.
  Evidence: `src/display/render/plane.c` sets `child->canvas = parent->canvas` but does not recurse; `src/display/render/canvas.c` uses `propagate_canvas()` to recurse through all descendants.

- Observation: Detaching a child subtree currently clears `canvas` only on the top plane, not on descendants, which can leave stale canvas pointers on a detached tree.
  Evidence: `src/display/render/plane.c:n00b_plane_remove_child()` and `src/display/render/canvas.c:n00b_canvas_remove_plane()` clear only `child->canvas` or `p->canvas`.

- Observation: The public name `stack` is not available for the widget constructor because `include/adt/stack.h` already defines `n00b_stack_new(...)` as a macro.
  Evidence: `include/adt/stack.h`.

- Observation: Single-widget scenes in `widget_demo` render one frame unless they explicitly opt into the event loop.
  Evidence: Before this change, `src/tools/widget_demo.c` only set `use_event_loop = true` for `all`, so a clickable `zstack` demo required its own interactive branch.

- Observation: The checked-in `build_debug/` directory in this workspace was configured against `/home/baron/crash-override/n00b-athens` instead of the current repository root.
  Evidence: `build_debug/meson-info/meson-info.json` reported the stale source path, and `meson` regeneration failed until `N00B_CLEAN=1 bash build.sh` recreated `build_debug/` under `/home/baron/crash-override/n00b-tui/n00b-athens`.

- Observation: `n00b_display_scene_run_layout()` assigns full-frame bounds to every top-level plane, so a manually positioned top-level control panel becomes a full-screen hit target unless it is itself managed by a layout container or custom layout callback.
  Evidence: The operator-visible bug reproduced as a dead overlay button and misdirected control clicks in `widget_demo --widget zstack`, and the layout runner in `src/display/scene_contracts.c` calls `n00b_widget_layout()` on every top-level plane with the canvas root bounds.

- Observation: The compositor's global `abs_z` sort allowed a deeper descendant of an earlier sibling to paint above a shallower descendant of a later sibling, which breaks zstack-style whole-layer reordering.
  Evidence: In the demo, sending the overlay layer to the back still left the nested overlay button painted above the background button while hit-testing already followed the new sibling order; `src/display/render/composite.c` sorted all flattened entries by `abs_z` before painting.

## Decision Log

- Decision: The implementation will use the public surface already fixed in the design dossier: `zstack.h`, `n00b_zstack_t`, `n00b_zstack_new`, and `n00b_widget_zstack`.
  Rationale: The checked-in Wave 1 design breakdown already resolved the `stack` naming collision, and this plan should not reopen a decision that was made specifically to avoid a macro conflict.
  Date/Author: 2026-03-11 / Codex.

- Decision: Reordering inside `zstack` will mutate `stack->children` directly with list operations instead of using `n00b_plane_set_z()`.
  Rationale: Changing `z` alone would affect compositing but would not update mouse hit-testing or focus traversal, because those systems currently follow child-list order rather than the `z` field.
  Date/Author: 2026-03-11 / Codex.

- Decision: Wave 1 `zstack` will expose exactly the six public mutators/accessors from `plans/notes/widget-wave1-design-breakdown.md` and will not add `insert_at` or keyboard management shortcuts.
  Rationale: This keeps the first production widget aligned with the approved Wave 1 contract and avoids scope creep before overlay-heavy Wave 3 work exists.
  Date/Author: 2026-03-11 / Codex.

- Decision: `zstack` measurement will ignore hidden children and will compute both preferred and minimum size as the maximum visible child size across the stack.
  Rationale: This matches the prototype behavior for preferred size, matches existing Athens container practice of skipping invisible children, and avoids hidden overlays forcing container growth.
  Date/Author: 2026-03-11 / Codex.

- Decision: The production change will include a small plane-runtime fix for recursive canvas propagation and clearing when a subtree is parented or detached.
  Rationale: A `zstack` layer is allowed to be an arbitrary plane subtree. Without this fix, pushing a prebuilt overlay tree into a live `zstack` can leave descendants without font metrics, and popping a layer can leave detached descendants pointing at an old canvas.
  Date/Author: 2026-03-11 / Codex.

- Decision: The implementation will ship with both automated proof (`test_zstack`) and a human-facing `widget_demo --widget zstack` scene.
  Rationale: The first Wave 1 widget should be demonstrably useful, not only compilable. Unit tests prove behavior, while the demo provides the user-visible outcome required by `PLANS.md`.
  Date/Author: 2026-03-11 / Codex.

- Decision: The demo scene will place a background button and the overlay button at the same coordinates inside separate `zstack` layers, and `demo_zstack()` will explicitly opt into the widget-demo event loop.
  Rationale: Matching coordinates makes overlapping click precedence visible during manual testing, and the dedicated event-loop branch is required because single-widget demos otherwise only render one frame.
  Date/Author: 2026-03-12 / Codex.

- Decision: Manual reorder controls for the demo will live in a separate top-level control panel instead of inside the reordered layers.
  Rationale: Full-frame sibling planes consume clicks even outside their visible child card, so embedding the controls inside the target layers would make one of the reorder actions unreachable after a swap. A separate panel keeps both actions available while still reordering the overlay card as the subject under test.
  Date/Author: 2026-03-12 / Codex.

- Decision: The final demo layout will use one top-level `box` root with the zstack scene on the left and a fixed control column on the right, rather than a second top-level panel.
  Rationale: A sibling control column preserves the operator workflow goal from the prior decision, but avoids the top-level full-frame relayout behavior that turned the standalone panel into an unintended click shield.
  Date/Author: 2026-03-13 / Codex.

- Decision: The compositor flatten pass will emit painter's-order entries by recursively visiting siblings in `(z, insertion order)` and painting each sibling's full subtree before the next sibling, instead of globally resorting all flattened entries by `abs_z`.
  Rationale: Whole-subtree painter order is the only model that keeps visual stacking aligned with hit-testing and focus behavior for layered containers such as `zstack`; retaining a global `abs_z` sort makes nested descendants violate the layer order the widget is supposed to control.
  Date/Author: 2026-03-13 / Codex.

## Outcomes & Retrospective

As of 2026-03-13 this plan has been executed. Athens now has a production `zstack` widget with the approved public API, list-order-based runtime reordering, recursive subtree canvas propagation on add/remove, painter's-order subtree compositing that matches layer reordering semantics, a dedicated `test_zstack` unit, an additional `test_render_plane` regression, and a `widget_demo --widget zstack` scene with a sibling control column for manual reorder testing.

The targeted validation matrix passed exactly as planned. `meson compile -C build_debug test_zstack test_render_plane test_mouse test_display_event_dispatch widget_demo` completed successfully after a clean rebuild, `meson test -C build_debug --print-errorlogs zstack render_plane mouse display_event_dispatch` passed all four tests, and `./build_debug/widget_demo --widget zstack --backend stream` exited cleanly without reporting an unknown widget or missing symbol.

The main implementation risk from planning was borne out but contained: `zstack` depends on shared ordering and subtree canvas invariants. The new tests now exercise those seams directly, and the only recovery action needed during execution was rebuilding a stale `build_debug/` directory that pointed at the wrong source tree.

## Context and Orientation

Athens widgets are not separate heap-managed objects with their own tree. A widget is a `n00b_plane_t` with a `n00b_widget_vtable_t` attached, as defined in `include/display/widget.h` and implemented in `src/display/widget.c`. The plane stores geometry, child planes, draw commands, optional box decoration, and widget-private data. Container widgets such as `box` live in `src/display/widgets/` and receive layout bounds through the `layout` vtable slot.

A "zstack" is a container that gives every child the same content bounds and relies on sibling order to decide which child is visually in front. In this repository, that sibling order matters in three places. `src/display/render/composite.c` flattens the tree in child order, so later siblings draw after earlier siblings when `z` ties. `src/display/mouse.c` hit-tests children in reverse order, so later siblings receive overlapping mouse clicks first. `src/display/focus.c` collects focusable descendants in forward order, so zstack reordering also changes tab order among descendants.

The authoritative design contract for this widget is already written in `plans/notes/widget-wave1-design-breakdown.md` under the `## stack` section. That document fixes the public API shape, the renamed public symbol family (`zstack`), the state model ("state lives in child ordering only"), the layout behavior ("every child gets the same content bounds"), and the Wave 1 test intent. This plan turns that design into a concrete implementation sequence.

The most relevant reference implementations are:

- `include/display/widgets/box.h` and `src/display/widgets/box.c` for Athens container-widget structure, constructor style, measurement, and layout callbacks.
- `include/display/widgets/button.h` and `src/display/widgets/button.c` for public widget header style, callback definitions, and attachment patterns.
- `include/display/render/plane.h` and `src/display/render/plane.c` for child parenting, visibility, dirtying, and geometry helpers.
- `src/display/render/canvas.c`, `src/display/render/composite.c`, `src/display/mouse.c`, and `src/display/focus.c` for scene construction, hit-testing, focus traversal, and canvas propagation behavior that `zstack` depends on.
- `test/unit/test_list_widget.c`, `test/unit/test_mouse.c`, `test/unit/test_display_event_dispatch.c`, and `test/unit/test_render_plane.c` for current unit-test style and the existing display harness patterns.
- `src/tools/widget_demo.c` and `docs/widgets.md` for the end-to-end demo surface and widget status documentation that must reflect the new production widget.

## Plan of Work

Milestone 1 adds the public `zstack` widget surface and the minimum runtime support it depends on. Create a new public header at `include/display/widgets/zstack.h` and a new implementation file at `src/display/widgets/zstack.c`. The header must define the `n00b_zstack_t` widget data record, `extern const n00b_widget_vtable_t n00b_widget_zstack;`, the keyword-argument constructor, and the six public API functions from the design dossier. The implementation must attach the new vtable to a plane, use the plane's child list as the sole source of layering state, and make reordering a pure list operation followed by dirtying the container. This milestone also updates `meson.build` so the library actually compiles the new widget source.

Milestone 1 also fixes the plane-runtime gap exposed by a subtree-oriented layering container. Update `src/display/render/plane.c` so adding a child to an already-canvas-backed parent recursively assigns the parent canvas to the whole child subtree, not just the root child. Update child removal in `src/display/render/plane.c` and top-level plane removal in `src/display/render/canvas.c` so a detached subtree recursively clears its canvas pointers. This is not a separate feature; it is part of making `zstack_push()` and `zstack_pop()` safe for real overlay trees.

Milestone 2 adds behavior-driven tests. Create `test/unit/test_zstack.c` as the primary contract test for the new widget. This file should define a tiny test-only widget vtable with configurable measurement values and a mouse-click counter so overlapping-hit behavior can be asserted without depending on unrelated widget internals. The tests should verify creation and API shape, count/get behavior, max-size measurement across visible children, fill-bounds layout for every child, mouse routing to the frontmost visible layer, changed routing after `bring_to_front()` or `send_to_back()`, and `pop()` detaching the top layer. Add one focused plane-runtime regression to `test/unit/test_render_plane.c` so recursive canvas propagation and clearing are covered at the plane API layer as well, not only through `zstack`.

Milestone 3 ships the user-visible proof and updates repository status. Extend `src/tools/widget_demo.c` with a `demo_zstack()` scene and a `--widget zstack` dispatch branch. The scene should be simple and intentional: a full-size background layer plus a smaller overlay card inside a second full-size layer, so the overlay is visually obvious and receives overlapping clicks first. Update `docs/widgets.md` to list `zstack` as implemented, add `test_zstack` to the representative widget tests, and leave the rest of the Wave 1 queue unchanged. Then run the validation commands from this plan, capture the outcomes in the living sections above, and only consider the milestone complete once both the automated tests and the demo launch succeed.

## Concrete Steps

Run commands from `/home/baron/crash-override/n00b-tui/n00b-athens`.

1. If `build_debug/` does not already exist or is known to be stale, create a fresh project build using the repository-supported entrypoint:

       N00B_TEST=1 bash build.sh

   If `build_debug/` already exists and is current, targeted rebuilds are fine:

       meson compile -C build_debug

   In this execution, `build_debug/` was stale and pointed at the wrong source root, so the successful recovery command was:

       N00B_CLEAN=1 bash build.sh

2. Re-read the approved `zstack` design contract before editing code:

       sed -n '1,90p' plans/notes/widget-wave1-design-breakdown.md

3. Edit `meson.build` in two places. First, add `src/display/widgets/zstack.c` to the core library source list near the other widget sources. Second, register a new unit executable and test named `zstack` using `test/unit/test_zstack.c` and `test_common_kwargs`, following the existing widget-test pattern already used for `button`, `checkbox`, `list_widget`, and similar files.

4. Create `include/display/widgets/zstack.h` with the public contract below. Match the repository's normal widget header style, including `#pragma once`, the shared includes, and the `_kargs` constructor declaration.

   The header must declare exactly these public interfaces:

       typedef struct n00b_zstack_t {
           uint8_t reserved;
       } n00b_zstack_t;

       extern const n00b_widget_vtable_t n00b_widget_zstack;

       extern n00b_plane_t *
       n00b_zstack_new() _kargs {
           n00b_box_props_t *box       = nullptr;
           n00b_canvas_t    *canvas    = nullptr;
           n00b_allocator_t *allocator = nullptr;
       };

       extern void         n00b_zstack_push(n00b_plane_t *stack, n00b_plane_t *layer);
       extern n00b_plane_t *n00b_zstack_pop(n00b_plane_t *stack);
       extern n00b_isize_t n00b_zstack_count(n00b_plane_t *stack);
       extern n00b_plane_t *n00b_zstack_get(n00b_plane_t *stack, n00b_isize_t index);
       extern bool         n00b_zstack_bring_to_front(n00b_plane_t *stack, n00b_plane_t *layer);
       extern bool         n00b_zstack_send_to_back(n00b_plane_t *stack, n00b_plane_t *layer);

5. Create `src/display/widgets/zstack.c`. Implement the vtable callbacks and public functions with the following behavior:

   `destroy` frees the tiny `n00b_zstack_t` data record and nothing else.

   `render` clears the plane with `n00b_plane_clear()` and draws no intrinsic content.

   `measure` iterates visible children only, calls `n00b_widget_measure()` for each child, and reports the maximum preferred width, preferred height, minimum width, and minimum height found. If there are no visible children, return `1` for all four outputs so empty stacks remain layoutable.

   `handle_event` always returns `false`. Wave 1 keeps event routing in shared mouse/focus dispatch rather than intercepting it in `zstack`.

   `can_focus` always returns `false`.

   `layout` receives content bounds from `n00b_widget_layout()` and must call `n00b_widget_layout(child, bounds)` for every child in order, including hidden children, so all layers stay geometry-synchronized.

   `n00b_zstack_new()` allocates a plane with optional `box`, `canvas`, and `allocator`, allocates `n00b_zstack_t`, attaches the widget, and marks the plane dirty.

   `n00b_zstack_push()` requires a non-null `stack` and `layer`, requires `layer->parent == nullptr`, and parents the layer with `n00b_plane_add_child(stack, layer, 0, 0)`. Do not special-case re-push; callers already have explicit reorder APIs.

   `n00b_zstack_pop()` removes and returns the last child in `stack->children`, or `nullptr` when empty.

   `n00b_zstack_count()` returns the child count, or `0` for a null plane.

   `n00b_zstack_get()` returns the indexed child when `index < count`, otherwise `nullptr`.

   `n00b_zstack_bring_to_front()` and `n00b_zstack_send_to_back()` return `false` when `layer` is not a current child of `stack`. Otherwise they remove the existing child entry from `stack->children`, insert it at the back or front respectively, mark the stack dirty, and return `true`.

   Use `n00b_list_delete()`, `n00b_list_push()`, and `n00b_list_insert()` on `stack->children` for reordering. Do not change `child->z`.

6. Update `src/display/render/plane.c` so child reparenting behaves correctly for subtrees:

   Add a local recursive helper that assigns a canvas pointer to a plane and all descendants, marking each plane dirty. Use it inside `n00b_plane_add_child()` instead of writing only `child->canvas = parent->canvas`.

   Add a matching local recursive helper that clears the canvas pointer on a plane and all descendants. Use it inside `n00b_plane_remove_child()` after the child is deleted from the parent.

   Keep these helpers file-local. Do not add a new public API just for this feature.

7. Update `src/display/render/canvas.c` so top-level plane removal is symmetric with top-level plane addition. Reuse or mirror the existing recursive propagation logic so `n00b_canvas_remove_plane()` clears the canvas pointer on the whole removed subtree, not only on the root plane.

8. Create `test/unit/test_zstack.c`. The file should follow the existing widget-test style: one `main`, small focused test functions, `printf("  [PASS] ...")` lines, and `n00b_init()` / `n00b_shutdown()` around the test run. Define a small dummy widget type in the test file with:

   a `measure` callback that returns fixed dimensions from test-controlled state,

   a `handle_event` callback that increments a click counter on left mouse press and optionally consumes the event,

   and a `can_focus` callback that can stay `false`.

   Include at least these tests:

   `test_zstack_create_and_api()` verifies constructor result, vtable attachment, empty count/get/pop behavior, and `can_focus == false`.

   `test_zstack_measure_visible_max()` builds multiple child widgets with distinct preferred/minimum sizes, hides one larger child, and asserts the stack reports the maximum visible sizes only.

   `test_zstack_layout_fills_all_children()` assigns a known bounds rectangle to the stack and asserts every child receives those exact bounds.

   `test_zstack_mouse_hits_frontmost_layer()` creates overlapping layers on a stream canvas, dispatches one mouse press into the overlap, and asserts only the topmost layer's counter increments.

   `test_zstack_reorder_changes_front_layer()` calls `bring_to_front()` and `send_to_back()` and repeats the overlapping click, proving the target changes exactly as the reordered child list predicts.

   `test_zstack_pop_detaches_top_layer()` verifies `pop()` returns the prior front layer, clears `parent`, and leaves the remaining order intact.

9. Add one plane-runtime regression to `test/unit/test_render_plane.c` for recursive canvas handling. Build a child plane with its own descendant before parenting it into a canvas-backed parent, then assert that both the child and grandchild gain the canvas pointer on add and lose it on remove. This test proves the subtree fix independently of `zstack`.

10. Update `src/tools/widget_demo.c`:

    add `#include "display/widgets/zstack.h"`;

    add a new `demo_zstack(n00b_canvas_t *canvas)` helper;

    add a `strcmp(widget_name, "zstack") == 0` dispatch branch in `main`.

    The demo scene should create one root `n00b_zstack_new(.canvas = canvas)` plane, add a full-frame background layer with explanatory text, then add a second full-frame layer whose own children render a smaller overlay card near the center. Put a clickable button on the overlay card so overlapping clicks exercise the top layer during manual testing.

11. Update `docs/widgets.md` so `zstack` appears in the implemented widgets list and `test_zstack` is mentioned with the other representative widget tests.

12. Rebuild the touched targets:

       meson compile -C build_debug test_zstack test_render_plane test_mouse test_display_event_dispatch widget_demo

## Validation and Acceptance

The implementation is complete only when all of the following are true.

Run the targeted automated checks first:

    meson test -C build_debug --print-errorlogs zstack render_plane mouse display_event_dispatch

Acceptance for those tests means:

1. The new unit test `zstack` passes and explicitly proves overlapping click routing, reordering, measurement, layout, and pop/detach behavior.
2. `render_plane` still passes after the subtree canvas-propagation change.
3. `mouse` still passes, proving that the zstack work did not regress shared hit-testing/capture behavior.
4. `display_event_dispatch` still passes, proving that shared display dispatch still cooperates with the new layering container.

Then run the human-visible proof:

    ./build_debug/widget_demo --widget zstack --backend tui

If a TTY backend is not convenient, a non-interactive fallback is acceptable:

    ./build_debug/widget_demo --widget zstack --backend stream

For `tui`, acceptance means the scene launches without crashing, the overlay card is visibly above the background layer, and clicks in the overlapping region activate the overlay button instead of the background content. For `stream`, acceptance means the tool exits cleanly after rendering one frame and does not report an unknown widget or missing symbol.

The change is accepted only if the public `zstack` API exists, the automated tests above pass, and the demo can launch through the supported widget-demo entrypoint.

## Idempotence and Recovery

All edit steps in this plan are additive and safe to repeat. Re-running `meson compile` or the named `meson test` commands is expected and should not mutate repository state beyond normal build artifacts under `build_debug/`.

If targeted builds fail because `build_debug/` is stale, rebuild from the supported wrapper rather than trying to hand-recreate Meson configuration:

    N00B_CLEAN=1 bash build.sh

If the new `zstack` source compiles but the test registration is wrong, fix `meson.build` first and rerun only the named targets from `Concrete Steps`. If the subtree canvas fix causes broader rendering regressions, keep the `zstack` widget code intact and temporarily narrow diagnosis to `test_render_plane`, `mouse`, and `display_event_dispatch` before touching unrelated widgets. Do not revert unrelated user changes in the worktree.

## Artifacts and Notes

Files that should exist or change by the end of this plan:

- `include/display/widgets/zstack.h`
- `src/display/widgets/zstack.c`
- `src/display/render/plane.c`
- `src/display/render/canvas.c`
- `test/unit/test_zstack.c`
- `test/unit/test_render_plane.c`
- `src/tools/widget_demo.c`
- `docs/widgets.md`
- `meson.build`

Observed targeted test transcript after successful implementation:

    $ meson test -C build_debug --print-errorlogs zstack render_plane mouse display_event_dispatch
    1/4 unit - n00b:render_plane           OK
    2/4 unit - n00b:mouse                  OK
    3/4 unit - n00b:zstack                 OK
    4/4 unit - n00b:display_event_dispatch OK

Observed non-interactive demo transcript after successful implementation:

    $ ./build_debug/widget_demo --widget zstack --backend stream
    Backend request 'stream' selected 'stream'
    Backend 'stream' has no input polling; using single-frame mode.

Observed interactive scene behavior in code:

    A full-screen background layer renders first.
    A centered overlay card renders on top of it.
    The overlay button sits directly above a background button at the same coordinates.
    Clicking the overlapping button region triggers the overlay status label instead of the background status label.

## Interfaces and Dependencies

The feature depends only on existing Athens modules: the widget vtable API in `include/display/widget.h`, plane hierarchy and geometry in `include/display/render/plane.h`, scene flattening in `src/display/render/composite.c`, mouse routing in `src/display/mouse.c`, focus traversal in `src/display/focus.c`, and the generic list container in `include/adt/list.h`.

At the end of this plan, these public interfaces must exist exactly as named:

In `include/display/widgets/zstack.h`:

    typedef struct n00b_zstack_t {
        uint8_t reserved;
    } n00b_zstack_t;

    extern const n00b_widget_vtable_t n00b_widget_zstack;

    extern n00b_plane_t *
    n00b_zstack_new() _kargs {
        n00b_box_props_t *box       = nullptr;
        n00b_canvas_t    *canvas    = nullptr;
        n00b_allocator_t *allocator = nullptr;
    };

    extern void         n00b_zstack_push(n00b_plane_t *stack, n00b_plane_t *layer);
    extern n00b_plane_t *n00b_zstack_pop(n00b_plane_t *stack);
    extern n00b_isize_t n00b_zstack_count(n00b_plane_t *stack);
    extern n00b_plane_t *n00b_zstack_get(n00b_plane_t *stack, n00b_isize_t index);
    extern bool         n00b_zstack_bring_to_front(n00b_plane_t *stack, n00b_plane_t *layer);
    extern bool         n00b_zstack_send_to_back(n00b_plane_t *stack, n00b_plane_t *layer);

In `src/display/widgets/zstack.c`, the vtable must be:

    const n00b_widget_vtable_t n00b_widget_zstack = {
        .kind         = "zstack",
        .destroy      = zstack_destroy,
        .render       = zstack_render,
        .measure      = zstack_measure,
        .handle_event = zstack_handle_event,
        .can_focus    = zstack_can_focus,
        .layout       = zstack_layout,
    };

Do not add any additional public `zstack` API in this milestone. In particular, do not add indexed insertion, layer-specific metadata, keyboard shortcuts, or a dedicated divider/overlay plane abstraction. The first production widget should stay aligned with the approved Wave 1 contract.

The subtree canvas fix must remain internal. If helper recursion is introduced in `src/display/render/plane.c` or `src/display/render/canvas.c`, keep it file-local and avoid changing public headers unless a concrete compile error forces it.

## Revision Notes

- 2026-03-11: Initial ExecPlan added for the first Wave 1 production widget reimplementation, using `plans/notes/widget-wave1-design-breakdown.md` as the source-of-truth design contract for `zstack`.
- 2026-03-12: Marked the plan executed after landing the production `zstack` widget, runtime subtree canvas fixes, tests, demo wiring, documentation updates, and the passing validation results. Added the clean-build recovery note because the existing `build_debug/` directory pointed at a stale source root.
