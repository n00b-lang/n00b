# Establish GUI Backend Contracts, Bridge Guards, And Parity Evidence (M3)

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

This plan is Milestone 3 of the umbrella plan at `plans/display-rewrite-overall-execplan.md`. There is no repository-local `PLANS.md` in this working tree, so this document follows `/home/baron/.codex/PLANS.md` and must remain compliant with it.

## Purpose / Big Picture

Milestone 3 makes the dual-target runtime concrete by bringing the Cocoa GUI backend onto the same internal-contract discipline used by terminal paths in Milestones 1 and 2. After this milestone, contributors should be able to prove that one app composition produces equivalent interaction semantics through terminal and GUI pathways for a documented subset (focus traversal, key activation, mouse press routing, resize handling, and clean stop behavior).

The user-visible effect is parity confidence and safer GUI evolution. Instead of leaving Cocoa-specific translation and bridge assumptions as implicit static code in `backend_cocoa.m`, this milestone introduces explicit, testable contracts plus a deterministic parity report artifact under `plans/artifacts/display-rewrite/m3/`.

## Progress

- [x] (2026-03-05 11:55Z) Reviewed Milestone 3 scope and acceptance criteria from `plans/display-rewrite-overall-execplan.md`.
- [x] (2026-03-05 11:55Z) Audited current GUI touchpoints: `src/display/render/backend_cocoa.m`, `include/display/render/cocoa_bridge.h`, `test/unit/test_cocoa_backend.m`, and Meson Cocoa gating.
- [x] (2026-03-05 11:55Z) Authored this Milestone 3 child ExecPlan with concrete internal contracts, tests, and artifact commands.
- [ ] Create and switch to branch `display-rewrite/m3-gui-backend` from `display-rewrite/m2-terminal-backend`.
- [ ] Implement shared Cocoa input-translation and bridge-layout contract modules, then rewire `backend_cocoa.m` to consume them.
- [ ] Add M3 unit tests (`display_cocoa_input`, `display_cocoa_bridge_contracts`) and integration parity coverage (`display_m3_backend_parity`).
- [ ] Add deterministic artifact tool `display_gui_parity_report` and generate Milestone 3 artifacts under `plans/artifacts/display-rewrite/m3/`.
- [ ] Run milestone validations (tests, smoke checks, parity diffs), record outcomes, and update this plan’s living sections.

## Surprises & Discoveries

- Observation: Cocoa backend responsibilities are still concentrated in one large file that mixes rendering, AppKit lifecycle, input translation, queueing, and bridge assumptions.
  Evidence: `wc -l src/display/render/backend_cocoa.m` reports 1736 lines, and static helpers plus vtable logic co-exist in the same translation unit.
- Observation: The Cocoa bridge redeclares critical core structs and ABI constants, so drift risk remains real if canonical headers evolve.
  Evidence: `include/display/render/cocoa_bridge.h` redeclares `n00b_text_style_t`, `n00b_rcell_t`, and `N00B_RENDERER_ABI_VERSION` currently mirrored from canonical headers.
- Observation: Existing Cocoa tests are mostly smoke-style and skip heavily when no window server is available.
  Evidence: `test/unit/test_cocoa_backend.m` gates most tests behind `has_display()` and prints many `[SKIP] ... (headless)` outcomes.
- Observation: Cocoa key/modifier translation is currently static inside Objective-C code, which makes it hard to validate in non-interactive C unit tests.
  Evidence: `src/display/render/backend_cocoa.m` defines `cocoa_translate_modifiers` and `cocoa_translate_function_key` as static helpers used directly in event handlers.

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

## Outcomes & Retrospective

Milestone 3 implementation has not started yet on this branch snapshot. This document now defines the complete execution path, concrete files, commands, and acceptance evidence needed to deliver M3 in one restartable pass.

Expected M3 completion outcome is: explicit Cocoa input and bridge contracts, passing unit and integration parity tests (including headless fallback), and deterministic M3 artifacts proving parity versus M2 baselines for stream captures plus GUI-path interaction semantics.

## Context and Orientation

Milestones 0-2 are complete and established the baseline harness (`display_baseline_capture`), core internal contracts (`diagnostics`, `scene_contracts`, `event_dispatch`, `backend_services`), and terminal modularization (`terminal_lifecycle`, `terminal_input`, `ansi_sgr`). M3 extends that contract-first approach to the GUI path.

For this milestone, “bridge layout” means memory layout compatibility between canonical C structs in normal headers and redeclared bridge structs in `include/display/render/cocoa_bridge.h` used by ObjC files. “Parity harness” means a deterministic, non-interactive test/tool that runs the same widget composition through terminal-like and GUI-like backend paths and compares normalized behavior outcomes. “GUI smoke” means an optional real Cocoa execution proof when the environment supports it.

Primary files to understand before editing are:

- `src/display/render/backend_cocoa.m`
- `include/display/render/cocoa_bridge.h`
- `src/display/render/cocoa_init_bridge.c`
- `test/unit/test_cocoa_backend.m`
- `test/integration/test_display_m2_terminal_flow.c`
- `src/tools/display_terminal_replay.c`
- `meson.build`

Milestone 3 is stacked on Milestone 2, so branch base is `display-rewrite/m2-terminal-backend` and parity comparisons must use `plans/artifacts/display-rewrite/m2/` as the parent evidence baseline.

## Plan of Work

First, extract Cocoa input semantics into a dedicated internal contract. Add `include/internal/display/cocoa_input.h` and `src/display/render/cocoa_input.c` with pure C helpers for modifier mapping, function-key mapping, key normalization for Cocoa-originated characters, and mouse event construction. Then rewire `backend_cocoa.m` event handlers (`keyDown`, mouse callbacks) to call this module instead of private static translation logic.

Second, codify bridge layout invariants with explicit snapshots. Add `include/internal/display/cocoa_bridge_contracts.h` plus one canonical-layout implementation in `src/display/render/cocoa_bridge_contracts.c` (compiled with canonical headers), and one bridge-layout implementation in a new ObjC file `src/display/render/cocoa_bridge_layout.m` (compiled with `cocoa_bridge.h` redeclarations). Add a small comparison helper that reports first mismatch field. Call this guard during `cocoa_init()` before window creation; if mismatch is detected, fail initialization with an actionable diagnostic.

Third, keep Cocoa backend behavior parity while reducing static duplication. In `src/display/render/backend_cocoa.m`, remove or reduce duplicated translation helpers replaced by the new contract modules, and keep AppKit lifecycle/render logic intact. Preserve existing vtable symbol (`n00b_renderer_cocoa`) and current public behavior unless an intentional fix is documented in this plan.

Fourth, add milestone test coverage. Add a pure C unit test `test/unit/test_display_cocoa_input.c` for deterministic translation edge cases (Shift+Tab, Enter/Backspace canonicalization, function keys, modifier bit combinations, mouse press/release/move mapping). Add an ObjC unit test `test/unit/test_display_cocoa_bridge_contracts.m` that compares canonical and bridge layout snapshots and fails with field-specific mismatch details.

Fifth, add one integration parity test and one artifact tool. Add `test/integration/test_display_m3_backend_parity.c` that runs one scripted interaction flow through two deterministic backends (terminal-like and GUI-like capability profiles) and asserts equivalent normalized outcomes. Add `src/tools/display_gui_parity_report.c` to run a deterministic parity scenario and write `parity_report.txt` and `parity_metadata.txt` under `plans/artifacts/display-rewrite/m3/`.

Finally, wire all new sources/tests/tools in `meson.build`, run milestone and smoke validations, generate M3 artifacts, and compare stream baseline captures against M2. Any intentional parity differences must be documented in `Decision Log` and artifact notes.

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

The fourth layer is optional real GUI smoke. If Cocoa backend tests are built and a display server is available, `cocoa_backend` must pass. If Cocoa cannot run in the current environment, acceptance remains valid only when the limitation is explicitly documented and all deterministic fallback evidence is green.

## Idempotence and Recovery

All milestone commands are safe to rerun. Artifact generation is overwrite-based and should remain deterministic.

If branch creation fails because `display-rewrite/m3-gui-backend` already exists, switch to it and continue. If stacked rebase conflicts occur later, resolve on the active branch, rerun M3 validation commands, and update this plan’s `Progress` and `Decision Log` with the conflict resolution details.

If bridge-layout guard fails at runtime, treat it as a hard compatibility defect. Fix the redeclarations or canonical structure assumptions, rerun `display_cocoa_bridge_contracts`, and capture mismatch/fix evidence in `Surprises & Discoveries`.

If Cocoa is unavailable (non-Darwin build, disabled option, or headless GUI limits), continue with deterministic parity harness plus baseline diff checks, and record the exact command output proving the limitation.

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

Dependencies remain the existing project toolchain and optional Cocoa backend build gate already present in Meson. Do not add third-party dependencies in M3.

## Revision Notes

- 2026-03-05: Initial Milestone 3 child ExecPlan authored from umbrella Milestone 3 scope, with concrete Cocoa contract extraction, bridge-layout guard strategy, parity testing plan, and artifact commands including non-GUI fallback acceptance.
