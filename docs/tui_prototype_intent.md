# Display Runtime Status (Post M6)

## Scope

This document now tracks delivered architecture and remaining optional/environment-dependent behavior after display rewrite milestones M0-M6.

## What Is Implemented

### Unified rendering model

- Plane-based scene authoring (`include/display/render/plane.h`, `src/display/render/plane.c`)
- Canvas composition and backend dispatch (`include/display/render/canvas.h`, `src/display/render/canvas.c`)
- Shared scene flatten/composite path (`src/display/render/composite.c`)

### Runtime backend selection

- Registry initialization during `n00b_init()` (`src/core/init.c`)
- Candidate policy + alias normalization in backend registry (`include/display/render/backend_registry.h`, `src/display/render/backend_registry.c`)
- Runtime backend-name startup in user-facing tools (for example `src/tools/widget_demo.c`)

### Input/focus/event loop

- Event normalization, focus traversal, and mouse routing are shared (`src/display/event_loop.c`, `src/display/focus.c`, `src/display/mouse.c`)
- Widget behavior dispatch is backend neutral (`src/display/widget.c`, `src/display/widgets/*.c`)

### Data UX surfaces

- Table subsystem and streaming table paths (`src/display/table/*.c`)
- Hexdump rendering and contracts (`src/display/hexdump.c`, `test/unit/test_display_hexdump_contracts.c`)
- Conduit render transforms (`src/conduit/xform_render.c`)

### Lifecycle hardening (M6)

- Explicit canvas teardown/deallocation split:
- `n00b_canvas_deinit` for teardown without free
- `n00b_canvas_destroy` for heap convenience (`deinit` + free)
- Stack canvas callers migrated to `deinit` semantics (for example `test/unit/test_focus.c`)

### Tooling hardening (M6)

- `widget_demo` no longer creates hardcoded `/tmp` logs by default
- opt-in logging via `--debug-log <path>` or `N00B_WIDGET_DEMO_LOG`
- fallback reporting derived from shared backend-selection helpers

## Optional Or Environment-Dependent Behavior

These are not architecture gaps; they depend on platform/build/runtime environment:

- GUI backend availability (`gui` alias to `x11`/`cocoa`) depends on build flags and display server/session availability.
- `notcurses` behavior depends on terminal/runtime support.
- On headless environments, GUI requests may correctly fall back according to selection policy.

## Deterministic Validation Toolchain

The rewrite now has reproducible artifact tools and milestone integration coverage:

- `display_baseline_capture`
- `display_scene_inspect`
- `display_backend_selection_report`
- `display_gui_parity_report`
- `display_m6_cutover_report`

Milestone artifacts are regenerated under `plans/artifacts/display-rewrite/m6/` and compared against M5 scene/table baselines for parity.

## Cutover Outcome

A contributor can now:

1. Start one app composition via backend policy (`auto`, explicit backend, and `gui` request with fallback).
2. Validate runtime behavior through deterministic reports/tests.
3. Rely on docs that match current APIs and selection policy.

This completes the display rewrite cutover target for M6.
