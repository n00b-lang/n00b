# Establish GUI Backend Contracts, Bridge Guards, And Parity Evidence (M3)

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

This plan is Milestone 3 of the umbrella plan at `plans/display-rewrite-overall-execplan.md`. There is no repository-local `PLANS.md` in this working tree, so this document follows `/home/baron/.codex/PLANS.md` and must remain compliant with it.

## Purpose / Big Picture

Milestone 3 makes the dual-target runtime concrete by bringing GUI backends onto the same internal-contract discipline used by terminal paths in Milestones 1 and 2. After this milestone, contributors can run one app composition through terminal and GUI pathways (Cocoa where available and X11 on Linux) and get equivalent interaction semantics for a documented subset (focus traversal, key activation, mouse press routing, resize handling, and clean stop behavior).

The user-visible effect is contract confidence and a real Linux windowed path instead of terminal-only fallbacks. Instead of leaving Cocoa-specific translation and bridge assumptions as implicit static code in `backend_cocoa.m`, and instead of mapping `gui` to non-windowed terminal backends on Linux, this milestone introduces explicit, testable contracts plus a native X11 backend and deterministic synthetic GUI-profile artifacts under `plans/artifacts/display-rewrite/m3/`.

## Progress

- [x] (2026-03-05 11:55Z) Reviewed Milestone 3 scope and acceptance criteria from `plans/display-rewrite-overall-execplan.md`.
- [x] (2026-03-05 11:55Z) Audited current GUI touchpoints: `src/display/render/backend_cocoa.m`, `include/display/render/cocoa_bridge.h`, `test/unit/test_cocoa_backend.m`, and Meson Cocoa gating.
- [x] (2026-03-05 11:55Z) Authored this Milestone 3 child ExecPlan with concrete internal contracts, tests, and artifact commands.
- [x] (2026-03-05 12:18Z) Confirmed active branch is `display-rewrite/m3-gui-backend`.
- [x] (2026-03-05 12:18Z) Implemented Cocoa contract modules (`cocoa_input`, `cocoa_bridge_contracts`, `cocoa_bridge_layout`) and rewired `backend_cocoa.m` to use them, including runtime bridge-layout guard in `cocoa_init()`.
- [x] (2026-03-05 12:18Z) Added M3 tests: `display_cocoa_input`, `display_cocoa_bridge_contracts` (Cocoa-gated), and `display_m3_backend_parity`; wired all targets in `meson.build`.
- [x] (2026-03-05 12:18Z) Added deterministic artifact tool `display_gui_parity_report` and generated M3 artifacts under `plans/artifacts/display-rewrite/m3/`.
- [x] (2026-03-05 12:18Z) Ran milestone validations, smoke checks, optional Cocoa availability probes, and M2-vs-M3 stream diffs; captured outcomes below.
- [x] (2026-03-05 13:06Z) Added native Linux GUI backend `src/display/render/backend_x11.c`, Meson X11 gating (`enable_x11`), and backend registration/alias wiring so `--backend gui` resolves to `x11` on Linux/Unix.
- [x] (2026-03-05 13:06Z) Updated `widget_demo` backend UX (`gui`, `x11`, initialization failure path) and ensured interactive `--widget all` runs via event loop on non-terminal GUI backends.
- [x] (2026-03-05 13:06Z) Fixed X11 stability/usability defects found during manual bring-up: invalid font handling (`BadFont`), mouse pixel-coordinate mapping, and border hit-testing footprint.
- [x] (2026-03-05 13:06Z) Added/ran Linux GUI-focused regression coverage: `test_display_backend_registry`, `test_x11_backend`, and expanded `test_display_event_dispatch` border-click coverage.
- [x] (2026-03-18 00:49Z) Applied M3 review remediation: added `display_x11_contracts`, pixel-space Cocoa/input regressions, shared synthetic parity fixture extraction, honest synthetic artifact wording, registry-backed `widget_demo` backend resolution, and non-interactive GUI hold behavior.

## Surprises & Discoveries

- Observation: Cocoa backend responsibilities are still concentrated in one large file that mixes rendering, AppKit lifecycle, input translation, queueing, and bridge assumptions.
  Evidence: `wc -l src/display/render/backend_cocoa.m` reports 1736 lines, and static helpers plus vtable logic co-exist in the same translation unit.
- Observation: The Cocoa bridge redeclares critical core structs and ABI constants, so drift risk remains real if canonical headers evolve.
  Evidence: `include/display/render/cocoa_bridge.h` redeclares `n00b_text_style_t`, `n00b_rcell_t`, and `N00B_RENDERER_ABI_VERSION` currently mirrored from canonical headers.
- Observation: Existing Cocoa tests are mostly smoke-style and skip heavily when no window server is available.
  Evidence: `test/unit/test_cocoa_backend.m` gates most tests behind `has_display()` and prints many `[SKIP] ... (headless)` outcomes.
- Observation: Cocoa key/modifier translation is currently static inside Objective-C code, which makes it hard to validate in non-interactive C unit tests.
  Evidence: `src/display/render/backend_cocoa.m` defines `cocoa_translate_modifiers` and `cocoa_translate_function_key` as static helpers used directly in event handlers.
- Observation: Raw control byte `0x19` does not normalize to Shift+Tab in `n00b_event_normalize`; it normalizes to Ctrl+Y.
  Evidence: Initial `display_m3_backend_parity` run failed with terminal-side key routing skew, and `parity_report.txt` showed `terminal.left.key_events=0`/`terminal.right.key_events=3` until the script used symbolic `N00B_KEY_TAB + N00B_MOD_SHIFT`.
- Observation: Cocoa-specific bridge-contract and smoke tests are unavailable in this Linux build, so deterministic fallback evidence is required and sufficient.
  Evidence: `meson test -C build_debug --list` checks produced `display_cocoa_bridge_contracts unavailable (no Cocoa build in this environment)` and `cocoa_backend unavailable (non-Darwin or Cocoa disabled)`.
- Observation: X11 core fonts can report a `None` font identifier even when `XLoadQueryFont` returns non-null, which crashes on `XSetFont` unless guarded.
  Evidence: Running `./build_debug/widget_demo --widget all --backend gui` initially failed with `BadFont (invalid Font parameter)` and `X_ChangeGC` until font candidate fallback + `fid != None` checks were added.
- Observation: Demo status messages were not reliably visible in the default X11 window because the status widget sits at the bottom of a tall vertical stack.
  Evidence: Manual run feedback showed button activation flash with no visible status-line text change; button callback was switched to self-label updates for immediate, always-visible feedback.
- Observation: Border click hit-testing can under-shoot rendered extents when inferred from content size in mixed layout states.
  Evidence: Border-click behavior improved after switching hit-test footprint to prefer `plane->bounds` outer size and after adding `test_button_border_click_focuses` in `test/unit/test_display_event_dispatch.c`.
- Observation: The original synthetic parity harness over-claimed what it proved and could not catch pixel-vs-cell regressions because the GUI profile hard-coded `1x1` cell metrics.
  Evidence: M3 review remediation replaced duplicated harness code with `src/tools/display_m3_parity_fixture.[ch]`, changed the GUI profile to `9x16`, and updated `parity_report.txt` / `parity_metadata.txt` to declare `evidence_scope=synthetic_contract_parity`.
- Observation: Live X11 smoke remained unavailable in the current sandbox even with `xvfb-run`, so the remediation had to rely on backend-specific headless tests plus refreshed artifacts.
  Evidence: `xvfb-run -a sh -c 'echo DISPLAY=$DISPLAY; xdpyinfo >/dev/null 2>&1; echo xdpyinfo=$?; ./build_debug/test_x11_backend'` reported `xdpyinfo=1` and `cannot open DISPLAY`.

## Decision Log

- Decision: Extract Cocoa key/mouse/modifier translation into a backend-neutral C contract module under `include/internal/display/` and `src/display/render/`.
  Rationale: This enables deterministic unit tests in non-GUI environments and keeps input semantics reviewable outside AppKit-heavy files.
  Date/Author: 2026-03-05 / Codex
- Decision: Add explicit bridge-layout snapshots and a runtime bridge guard that compares canonical C layouts against Cocoa-bridge redeclarations during backend initialization.
  Rationale: Bridge redeclarations are required for ObjC compiler constraints, but unguarded drift can silently corrupt rendering/event behavior.
  Date/Author: 2026-03-05 / Codex
- Decision: Treat headless parity harness evidence as mandatory for M3 acceptance, and treat real Cocoa smoke execution as required only when Cocoa is built and a display server is available.
  Rationale: Milestone acceptance must remain executable on CI and Linux/non-windowed environments while still enabling GUI smoke proof on supported systems.
  Date/Author: 2026-03-05 / Codex
- Decision: Preserve public display APIs and renderer vtable shape in M3; keep all work internal or additive.
  Rationale: M3 scope is parity path hardening, not public API redesign.
  Date/Author: 2026-03-05 / Codex
- Decision: Keep terminal-like parity script aligned with real backend-normalized semantics by using symbolic Shift+Tab (`N00B_KEY_TAB` + `N00B_MOD_SHIFT`) instead of raw byte `0x19`.
  Rationale: `n00b_event_normalize` intentionally maps raw control bytes 1–26 to Ctrl+letters; using symbolic backtab avoids a false parity failure unrelated to backend behavior.
  Date/Author: 2026-03-05 / Codex
- Decision: Preserve legacy Cocoa Command-key behavior by mapping CMD into `N00B_MOD_ALT` in `n00b_cocoa_input_modifiers`.
  Rationale: Existing backend behavior already treated Command as Alt; preserving this in the new contract prevents unexpected input regressions in M3.
  Date/Author: 2026-03-05 / Codex
- Decision: Introduce a native X11 backend for Linux/Unix and map the `gui` alias to actual windowed backends only (`cocoa` on macOS, `x11` on Linux/Unix).
  Rationale: Milestone 3 acceptance requires a real GUI backend path; terminal-backed aliases (for example notcurses) do not satisfy the “actual GUI” objective.
  Date/Author: 2026-03-05 / Codex
- Decision: Keep X11 backend optional via Meson (`enable_x11`) and feature-detect X11 dependency at configure time.
  Rationale: This preserves portability for environments without X11 while still enabling native GUI support where available.
  Date/Author: 2026-03-05 / Codex
- Decision: Use visible button-label mutation (`Click Me` -> `Clicked!`) in `widget_demo` click callback during X11 demos instead of relying on bottom status-line updates.
  Rationale: It gives immediate confirmation of click handling in constrained window sizes and avoids false-negative UX during manual validation.
  Date/Author: 2026-03-05 / Codex

## Outcomes & Retrospective

Milestone 3 implementation is complete on `display-rewrite/m3-gui-backend` after review remediation. The repository now combines real backend regression coverage with a narrower, explicit synthetic GUI-profile parity artifact instead of treating the synthetic harness as stand-alone proof of live backend equivalence.

Delivered artifacts and contracts:

- Cocoa input contract extracted into `include/internal/display/cocoa_input.h` and `src/display/render/cocoa_input.c`.
- Bridge layout contracts added via `include/internal/display/cocoa_bridge_contracts.h`, `src/display/render/cocoa_bridge_contracts.c`, and ObjC bridge snapshot `src/display/render/cocoa_bridge_layout.m`.
- `backend_cocoa.m` now consumes shared input translation helpers and validates bridge layout during `cocoa_init()`.
- New tests and tools wired in `meson.build`: `display_cocoa_input`, `display_cocoa_bridge_contracts` (Cocoa-gated), `display_m3_backend_parity`, `display_x11_contracts`, and `display_gui_parity_report`.
- Native X11 backend added in `src/display/render/backend_x11.c`, with renderer registration in `src/display/render/backend_registry.c` and vtable declaration in `include/display/render/backend.h`.
- Linux GUI build/configuration wired via `meson.options` (`enable_x11`) and `meson.build` dependency detection/target wiring (`test_x11_backend`).
- Backend selection UX updated in `src/tools/widget_demo.c` so `--backend gui` resolves through a registry-backed helper, with explicit `--backend x11` support, backend-init failure diagnostics, and a non-interactive GUI hold loop instead of the previous Cocoa-only pump.
- Input/runtime fixes landed for Linux GUI validation: X11 font fallback/guards, exact-pixel resize propagation, expose-driven redraw scheduling, UTF-8 key decoding, drag mapping, truthful capability reporting, Cocoa pixel-space mouse points, and border hit-testing based on laid-out bounds.

Validation outcomes (2026-03-05):

- `meson test -C build_debug --print-errorlogs display_cocoa_input display_terminal_input display_event_dispatch display_baseline_contract` -> all pass.
- `meson test -C build_debug --print-errorlogs display_backend_registry display_cocoa_input display_event_dispatch display_x11_contracts x11_backend display_m3_backend_parity` -> all pass.
- `meson test -C build_debug --print-errorlogs render_plane ... xform_render` smoke set -> all pass.
- Cocoa-gated checks reported unavailable in this environment:
  - `display_cocoa_bridge_contracts unavailable (no Cocoa build in this environment)`
  - `cocoa_backend unavailable (non-Darwin or Cocoa disabled)`
- Artifact generation succeeded:
  - `build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m3`
  - `build_debug/display_scene_inspect --out plans/artifacts/display-rewrite/m3/scene_inspect.txt`
  - `build_debug/display_gui_parity_report --out-dir plans/artifacts/display-rewrite/m3`
- Baseline parity vs M2:
  - `diff -u plans/artifacts/display-rewrite/m2/scene_stream.txt plans/artifacts/display-rewrite/m3/scene_stream.txt` -> no output
  - `diff -u plans/artifacts/display-rewrite/m2/table_stream.txt plans/artifacts/display-rewrite/m3/table_stream.txt` -> no output
- Linux GUI validations:
  - `ninja -C build_debug widget_demo test_display_backend_registry test_display_x11_contracts test_x11_backend` -> build success.
  - `meson test -C build_debug --print-errorlogs display_backend_registry display_x11_contracts x11_backend display_event_dispatch` -> all pass.
  - `build_debug/widget_demo --widget label --backend gui` now resolves `gui` through the registry-backed helper and holds the GUI path open until explicit exit when a working X11 display is available.
  - `xvfb-run` smoke remained unavailable in this environment because even the ephemeral display could not be opened by X11 clients; that limitation is recorded instead of being counted as evidence.

## Context and Orientation

Milestones 0-2 are complete and established the baseline harness (`display_baseline_capture`), core internal contracts (`diagnostics`, `scene_contracts`, `event_dispatch`, `backend_services`), and terminal modularization (`terminal_lifecycle`, `terminal_input`, `ansi_sgr`). M3 extends that contract-first approach to GUI paths by hardening Cocoa bridge/input contracts and adding a native Linux X11 backend.

For this milestone, “bridge layout” means memory layout compatibility between canonical C structs in normal headers and redeclared bridge structs in `include/display/render/cocoa_bridge.h` used by ObjC files. “Parity harness” means a deterministic, non-interactive test/tool that runs the same widget composition through terminal-like and GUI-like backend paths and compares normalized behavior outcomes. “GUI smoke” means a real windowed execution proof (Cocoa on macOS, X11 on Linux/Unix) beyond terminal emulation.

Primary files to understand before editing are:

- `src/display/render/backend_cocoa.m`
- `src/display/render/backend_x11.c`
- `include/display/render/cocoa_bridge.h`
- `src/display/render/cocoa_init_bridge.c`
- `src/display/render/backend_registry.c`
- `include/display/render/backend_registry.h`
- `test/unit/test_cocoa_backend.m`
- `test/unit/test_x11_backend.c`
- `test/unit/test_display_backend_registry.c`
- `test/integration/test_display_m2_terminal_flow.c`
- `src/tools/display_terminal_replay.c`
- `src/tools/widget_demo.c`
- `meson.build`

Milestone 3 is stacked on Milestone 2, so branch base is `display-rewrite/m2-terminal-backend` and parity comparisons must use `plans/artifacts/display-rewrite/m2/` as the parent evidence baseline.

## Plan of Work

First, extract Cocoa input semantics into a dedicated internal contract. Add `include/internal/display/cocoa_input.h` and `src/display/render/cocoa_input.c` with pure C helpers for modifier mapping, function-key mapping, key normalization for Cocoa-originated characters, and mouse event construction. Then rewire `backend_cocoa.m` event handlers (`keyDown`, mouse callbacks) to call this module instead of private static translation logic.

Second, codify bridge layout invariants with explicit snapshots. Add `include/internal/display/cocoa_bridge_contracts.h` plus one canonical-layout implementation in `src/display/render/cocoa_bridge_contracts.c` (compiled with canonical headers), and one bridge-layout implementation in a new ObjC file `src/display/render/cocoa_bridge_layout.m` (compiled with `cocoa_bridge.h` redeclarations). Add a small comparison helper that reports first mismatch field. Call this guard during `cocoa_init()` before window creation; if mismatch is detected, fail initialization with an actionable diagnostic.

Third, keep Cocoa backend behavior parity while reducing static duplication. In `src/display/render/backend_cocoa.m`, remove or reduce duplicated translation helpers replaced by the new contract modules, and keep AppKit lifecycle/render logic intact. Preserve existing vtable symbol (`n00b_renderer_cocoa`) and current public behavior unless an intentional fix is documented in this plan.

Fourth, add milestone test coverage. Add a pure C unit test `test/unit/test_display_cocoa_input.c` for deterministic translation edge cases (Shift+Tab, Enter/Backspace canonicalization, function keys, modifier bit combinations, mouse press/release/move mapping). Add an ObjC unit test `test/unit/test_display_cocoa_bridge_contracts.m` that compares canonical and bridge layout snapshots and fails with field-specific mismatch details.

Fifth, add one integration parity test and one artifact tool. Add `test/integration/test_display_m3_backend_parity.c` that runs one scripted interaction flow through two deterministic backends (terminal-like and GUI-like capability profiles) and asserts equivalent normalized outcomes. Add `src/tools/display_gui_parity_report.c` to run a deterministic parity scenario and write `parity_report.txt` and `parity_metadata.txt` under `plans/artifacts/display-rewrite/m3/`.

Finally, wire all new sources/tests/tools in `meson.build`, run milestone and smoke validations, generate M3 artifacts, and compare stream baseline captures against M2. Any intentional parity differences must be documented in `Decision Log` and artifact notes.

Linux GUI extension for M3 adds one native backend (`x11`), updates registry/alias semantics so `gui` maps to real GUI backends, and validates interactive `widget_demo` behavior on X11. This extension also hardens runtime behavior through robust font fallback, explicit backend-init failure messaging, and corrected mouse/hit-test behavior for border clicks.

## Concrete Steps

Run all commands from `/home/baron/crash-override/n00b-athens`.

Create and switch to the milestone branch:

    git switch display-rewrite/m2-terminal-backend
    git switch -c display-rewrite/m3-gui-backend

If the branch already exists, switch directly:

    git switch display-rewrite/m3-gui-backend

Implement Cocoa contract modules and backend rewiring:

    mkdir -p include/internal/display src/display/render
    ${EDITOR:-vi} include/internal/display/cocoa_input.h
    ${EDITOR:-vi} src/display/render/cocoa_input.c
    ${EDITOR:-vi} include/internal/display/cocoa_bridge_contracts.h
    ${EDITOR:-vi} src/display/render/cocoa_bridge_contracts.c
    ${EDITOR:-vi} src/display/render/cocoa_bridge_layout.m
    ${EDITOR:-vi} src/display/render/backend_cocoa.m
    ${EDITOR:-vi} include/display/render/cocoa_bridge.h
    ${EDITOR:-vi} meson.build

Add milestone tests and parity artifact tool:

    ${EDITOR:-vi} test/unit/test_display_cocoa_input.c
    ${EDITOR:-vi} test/unit/test_display_cocoa_bridge_contracts.m
    ${EDITOR:-vi} test/integration/test_display_m3_backend_parity.c
    ${EDITOR:-vi} src/tools/display_gui_parity_report.c
    ${EDITOR:-vi} meson.build

Build tools and run milestone-targeted tests:

    if [ ! -d build_debug ]; then N00B_NATIVE=1 ./build.sh; fi
    ninja -C build_debug display_gui_parity_report display_terminal_replay display_scene_inspect display_baseline_capture
    meson test -C build_debug --print-errorlogs \
      display_cocoa_input display_terminal_input display_event_dispatch display_baseline_contract
    meson test -C build_debug --print-errorlogs \
      display_baseline_flow display_m1_compat display_m2_terminal_flow display_m3_backend_parity

Run Cocoa-bridge unit test only when present in this build:

    if meson test -C build_debug --list | rg -q '^display_cocoa_bridge_contracts$'; then
      meson test -C build_debug --print-errorlogs display_cocoa_bridge_contracts
    else
      echo "display_cocoa_bridge_contracts unavailable (no Cocoa build in this environment)"
    fi

Run shared display smoke tests:

    meson test -C build_debug --print-errorlogs \
      render_plane render_canvas render_ansi event_normalize focus mouse \
      label button checkbox input list_widget selectionlist breadcrumb \
      table_build table_layout table_render table_stream hexdump xform_render

Run optional real Cocoa smoke test when available:

    if meson test -C build_debug --list | rg -q '^cocoa_backend$'; then
      meson test -C build_debug --print-errorlogs cocoa_backend
    else
      echo "cocoa_backend unavailable (non-Darwin or Cocoa disabled)"
    fi

Run Linux X11 milestone checks and interactive smoke:

    ninja -C build_debug widget_demo test_display_backend_registry test_x11_backend
    meson test -C build_debug --print-errorlogs \
      display_backend_registry x11_backend display_event_dispatch
    ./build_debug/widget_demo --widget all --backend gui

Generate Milestone 3 artifacts and compare against Milestone 2 captures:

    mkdir -p plans/artifacts/display-rewrite/m3
    build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m3
    build_debug/display_scene_inspect --out plans/artifacts/display-rewrite/m3/scene_inspect.txt
    build_debug/display_gui_parity_report --out-dir plans/artifacts/display-rewrite/m3
    diff -u plans/artifacts/display-rewrite/m2/scene_stream.txt plans/artifacts/display-rewrite/m3/scene_stream.txt
    diff -u plans/artifacts/display-rewrite/m2/table_stream.txt plans/artifacts/display-rewrite/m3/table_stream.txt

Expected success pattern is: M3 tests pass, smoke tests remain green, optional Cocoa smoke is PASS or explicitly SKIP with reason, stream diffs versus M2 are empty unless documented, and parity report indicates equivalent behavior for the declared subset.

## Validation and Acceptance

Milestone 3 is accepted only when all required evidence layers pass.

The first layer is unit validation for new GUI contracts. `display_cocoa_input` must pass deterministic translation edge cases. `display_cocoa_bridge_contracts` must pass when Cocoa is built; if unavailable in the environment, that limitation must be explicitly recorded and headless fallback evidence must still pass.

The second layer is integration parity validation. `display_m3_backend_parity` must show equivalent normalized interaction results across terminal-like and GUI-like capability paths for the documented subset (focus traversal, key activation, mouse routing, resize, stop behavior).

The third layer is artifact validation. `display_gui_parity_report` must generate deterministic files in `plans/artifacts/display-rewrite/m3/`, and `display_baseline_capture` stream files in M3 should match M2 unless an intentional behavior change is documented.

The fourth layer is real GUI smoke. On Linux/Unix builds with X11 enabled, `widget_demo --widget all --backend gui` must open an X11 window and process click/key interactions. On macOS/Cocoa-capable environments, `cocoa_backend` should pass when available. If one GUI runtime is unavailable in the current environment, acceptance remains valid only when the limitation is explicitly documented and all deterministic fallback evidence is green.

## Idempotence and Recovery

All milestone commands are safe to rerun. Artifact generation is overwrite-based and should remain deterministic.

If branch creation fails because `display-rewrite/m3-gui-backend` already exists, switch to it and continue. If stacked rebase conflicts occur later, resolve on the active branch, rerun M3 validation commands, and update this plan’s `Progress` and `Decision Log` with the conflict resolution details.

If bridge-layout guard fails at runtime, treat it as a hard compatibility defect. Fix the redeclarations or canonical structure assumptions, rerun `display_cocoa_bridge_contracts`, and capture mismatch/fix evidence in `Surprises & Discoveries`.

If Cocoa is unavailable (non-Darwin build, disabled option, or headless GUI limits), continue with deterministic parity harness plus baseline diff checks, and record the exact command output proving the limitation.

If X11 initialization fails at runtime (for example missing `DISPLAY` or unavailable fonts), keep `widget_demo` failure messaging explicit (`Failed to initialize backend 'x11'.`) and verify backend tests still pass in headless mode (`x11_backend` may skip interactively but must not crash).

## Artifacts and Notes

Milestone 3 artifact directory must include at least:

- `plans/artifacts/display-rewrite/m3/scene_stream.txt`
- `plans/artifacts/display-rewrite/m3/table_stream.txt`
- `plans/artifacts/display-rewrite/m3/scene_inspect.txt`
- `plans/artifacts/display-rewrite/m3/parity_report.txt`
- `plans/artifacts/display-rewrite/m3/parity_metadata.txt`

Expected validation transcript pattern:

    $ meson test -C build_debug --print-errorlogs display_cocoa_input display_m3_backend_parity
    2/2 tests OK

    $ build_debug/display_gui_parity_report --out-dir plans/artifacts/display-rewrite/m3
    wrote parity_report.txt
    wrote parity_metadata.txt

    $ diff -u plans/artifacts/display-rewrite/m2/scene_stream.txt plans/artifacts/display-rewrite/m3/scene_stream.txt
    (no output)

    $ diff -u plans/artifacts/display-rewrite/m2/table_stream.txt plans/artifacts/display-rewrite/m3/table_stream.txt
    (no output)

    $ meson test -C build_debug --print-errorlogs display_backend_registry x11_backend display_event_dispatch
    3/3 tests OK

If a parity diff is intentional, include only the relevant diff lines here with a short explanation of why the behavior change is accepted for M3.

## Interfaces and Dependencies

Implement the following internal interfaces for this milestone.

In `include/internal/display/cocoa_input.h`, define:

    typedef enum : uint8_t {
        N00B_COCOA_MOD_SHIFT = 1 << 0,
        N00B_COCOA_MOD_CTRL  = 1 << 1,
        N00B_COCOA_MOD_ALT   = 1 << 2,
        N00B_COCOA_MOD_CMD   = 1 << 3,
    } n00b_cocoa_mod_mask_t;

    extern n00b_key_mod_t n00b_cocoa_input_modifiers(uint32_t cocoa_mod_flags);
    extern uint32_t n00b_cocoa_input_function_key(uint32_t cocoa_function_key);
    extern bool n00b_cocoa_input_translate_key(uint32_t key_code,
                                               uint32_t cocoa_mod_flags,
                                               n00b_event_t *out);
    extern void n00b_cocoa_input_translate_mouse(int32_t x,
                                                  int32_t y,
                                                  n00b_mouse_button_t button,
                                                  n00b_mouse_action_t action,
                                                  uint32_t cocoa_mod_flags,
                                                  n00b_event_t *out);

In `include/internal/display/cocoa_bridge_contracts.h`, define:

    typedef struct n00b_cocoa_bridge_layout_t {
        uint32_t abi_version;
        uint32_t text_style_size;
        uint32_t text_style_fg_rgb_off;
        uint32_t text_style_bg_rgb_off;
        uint32_t text_style_font_size_off;
        uint32_t rcell_size;
        uint32_t rcell_style_off;
        uint32_t rcell_grapheme_len_off;
        uint32_t rcell_display_width_off;
        uint32_t event_size;
        uint32_t event_key_off;
        uint32_t event_mouse_off;
    } n00b_cocoa_bridge_layout_t;

    extern n00b_cocoa_bridge_layout_t n00b_cocoa_bridge_layout_canonical(void);
    extern n00b_cocoa_bridge_layout_t n00b_cocoa_bridge_layout_bridge(void);
    extern bool n00b_cocoa_bridge_layout_match(const n00b_cocoa_bridge_layout_t *canonical,
                                               const n00b_cocoa_bridge_layout_t *bridge,
                                               const char **mismatch_field);

In `src/tools/display_gui_parity_report.c`, define:

    static int run_parity_case(bool gui_mode, parity_result_t *out);
    static int write_parity_report(const char *out_dir,
                                   const parity_result_t *terminal,
                                   const parity_result_t *gui);
    static int write_parity_metadata(const char *out_dir,
                                     bool cocoa_built,
                                     bool cocoa_smoke_available);
    int main(int argc, char **argv);

In `meson.options`, define:

    option('enable_x11', type: 'boolean', value: true, description: 'Enable the X11 GUI backend')

In `include/display/render/backend.h`, declare:

    #if defined(N00B_HAVE_X11)
    extern const n00b_renderer_vtable_t n00b_renderer_x11;
    #endif

In `src/display/render/backend_registry.c`, ensure:

    - `x11` backend is registered when `N00B_HAVE_X11` is defined.
    - `gui` alias resolves to `cocoa` on macOS and `x11` on Linux/Unix.
    - terminal backends remain available by explicit names (`tui`, `notcurses`, `ansi`).

Dependencies remain the existing project toolchain plus optional Cocoa and optional X11 build gates already present in Meson. Do not add third-party dependencies in M3 beyond system X11 libs.

## Revision Notes

- 2026-03-05: Initial Milestone 3 child ExecPlan authored from umbrella Milestone 3 scope, with concrete Cocoa contract extraction, bridge-layout guard strategy, parity testing plan, and artifact commands including non-GUI fallback acceptance.
- 2026-03-05: Revised Milestone 3 ExecPlan after Linux GUI execution work to include X11 backend scope, registry/alias behavior, runtime/input/hit-test fixes, and Linux-native validation commands. Reason: M3 acceptance expanded from deterministic Cocoa-contract work to include an actual Linux windowed backend path.
