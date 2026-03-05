# Rewrite Display Module In Place As A Dual Terminal And GUI Runtime

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

There is no repository-local `PLANS.md` in this working tree at the time this plan was authored. This document therefore follows the global guidance in `/home/baron/.codex/PLANS.md` and must remain compliant with that file for all future revisions.

## Purpose / Big Picture

The display subsystem already has strong terminal foundations, but the product direction is larger: one UI runtime that can drive terminal and GUI presentations from the same app logic. After this rewrite, a developer should be able to author one app with planes, widgets, and event handlers, then run it either in a terminal backend or a GUI backend without rewriting widget behavior. That is the practical "Electron replacement" direction in this repository: shared UI model, backend-specific shells.

This plan is the top-level roadmap for the in-place rewrite. "In-place" means existing public paths (`include/display/**`, `src/display/**`, existing tool entry points) are evolved rather than replaced by a separate subsystem. Each milestone requires unit tests, integration tests, and a human-runnable artifact that demonstrates the milestone's acceptance criteria.

## Progress

- [x] (2026-03-05 09:05Z) Reviewed `docs/tui_prototype_intent.md` and extracted intended long-term architecture signals to preserve.
- [x] (2026-03-05 09:05Z) Reviewed `reviews/CODE_REVIEW.md` and extracted rewrite drivers: oversized backend files, duplicated widget logic, diagnostics policy gaps, and limited cross-component integration coverage.
- [x] (2026-03-05 09:05Z) Authored this umbrella ExecPlan with stacked-diff branch strategy and milestone-level acceptance criteria.
- [x] (2026-03-05 09:19Z) Authored the Milestone 0 child ExecPlan in `plans/display-rewrite/m0-baseline-execplan.md`.
- [x] (2026-03-05 09:34Z) Landed Milestone 0 harness code on `display-rewrite/m0-baseline`: `test_display_baseline_contract`, `test_display_baseline_flow`, `display_baseline_capture`, and `meson.build` wiring.
- [x] (2026-03-05 10:41Z) Completed Milestone 0 acceptance evidence: named test passes, display smoke suite pass, and deterministic artifacts in `plans/artifacts/display-rewrite/m0/`.
- [x] (2026-03-05 11:18Z) Authored the Milestone 1 child ExecPlan in `plans/display-rewrite/m1-core-contracts-execplan.md`.
- [x] (2026-03-05 11:18Z) Executed Milestone 1 and recorded parity, diagnostics cleanup, and evidence artifacts in `plans/display-rewrite/m1-core-contracts-execplan.md` and `plans/artifacts/display-rewrite/m1/`.
- [x] (2026-03-05 11:18Z) Reconfigured with installed notcurses (`3.0.17`) and reran M1/display smoke validation with `notcurses_backend` enabled.
- [x] (2026-03-05 11:23Z) Authored the Milestone 2 child ExecPlan in `plans/display-rewrite/m2-terminal-backend-execplan.md`.
- [x] (2026-03-05 11:45Z) Executed Milestone 2 and recorded terminal contract extraction, replay tooling, test validation, and parity artifacts in `plans/display-rewrite/m2-terminal-backend-execplan.md` and `plans/artifacts/display-rewrite/m2/`.
- [x] (2026-03-05 11:55Z) Authored the Milestone 3 child ExecPlan in `plans/display-rewrite/m3-gui-backend-execplan.md`.
- [x] (2026-03-05 13:06Z) Executed Milestone 3 and recorded Cocoa contract hardening plus Linux X11 GUI backend delivery, registry alias updates, and validation evidence in `plans/display-rewrite/m3-gui-backend-execplan.md`.
- [x] (2026-03-05 13:19Z) Authored the Milestone 4 child ExecPlan in `plans/display-rewrite/m4-widget-table-migration-execplan.md`.
- [x] (2026-03-05 13:41Z) Executed Milestone 4 and recorded shared widget/table/hexdump primitive migration, validation evidence, and deterministic M4 artifacts in `plans/display-rewrite/m4-widget-table-migration-execplan.md` and `plans/artifacts/display-rewrite/m4/`.
- [x] (2026-03-05 13:50Z) Authored the Milestone 5 child ExecPlan in `plans/display-rewrite/m5-runtime-selection-execplan.md`.
- [x] (2026-03-05 14:10Z) Executed Milestone 5 and recorded registry-first runtime selection, plugin-readiness coverage, validation evidence, and deterministic artifacts in `plans/display-rewrite/m5-runtime-selection-execplan.md` and `plans/artifacts/display-rewrite/m5/`.
- [x] (2026-03-05 14:23Z) Authored the Milestone 6 child ExecPlan in `plans/display-rewrite/m6-hardening-and-cutover-execplan.md`.
- [ ] Execute Milestone 6 as stacked diff, preserving this document as the top-level source of milestone ordering and acceptance scope.

## Surprises & Discoveries

- Observation: The current display implementation already signals backend-neutral design through planes, canvas composition, and a renderer vtable, so the rewrite should preserve these seams instead of replacing them.
  Evidence: `docs/tui_prototype_intent.md` sections on rendering model, backend portability, and stable direction (`include/display/render/backend.h`, `src/display/render/composite.c`).
- Observation: Code quality risks are concentrated in backend files that combine unrelated responsibilities (lifecycle, input mapping, caching, render logic), which increases refactor risk.
  Evidence: `reviews/CODE_REVIEW.md` architecture assessment of `src/display/render/backend_notcurses.c` and `src/display/render/backend_cocoa.m`.
- Observation: Existing unit coverage is broad, but integration coverage is comparatively thin for cross-component behaviors (event loop + focus + render + backend).
  Evidence: `reviews/CODE_REVIEW.md` testing strategy section and recommendation for scenario tests.
- Observation: Runtime diagnostics currently include hardcoded `/tmp` writes in user-facing paths, which must be removed early to avoid preserving prototype side effects.
  Evidence: `reviews/CODE_REVIEW.md` quick wins and files called out (`src/display/event_loop.c`, `src/display/render/backend_ansi.c`, `src/display/widgets/label.c`).
- Observation: In this workspace, `main` does not currently carry the display tree required for rewrite milestones, so Milestone 0 had to be executed from the `needs-rebase` lineage.
  Evidence: Milestone 0 implementation and validation succeeded on `display-rewrite/m0-baseline` after rebasing onto `codex/linux-build-and-test`, while branch discovery on `main` did not provide `include/display/` and `src/display/`.
- Observation: `widget_demo` is not a deterministic baseline artifact source because the TUI path waits for user keypresses and writes debug logs to `/tmp`.
  Evidence: `src/tools/widget_demo.c` behavior observed during Milestone 0 planning; deterministic artifact capture was implemented via `src/tools/display_baseline_capture.c`.
- Observation: `meson test` for named tests does not reliably build unrelated local tool binaries.
  Evidence: `build_debug/display_baseline_capture` required explicit `ninja -C build_debug display_baseline_capture` before artifact generation.
- Observation: notcurses availability changed during M1, and Meson picked up the dependency after reconfigure without code-level fallback changes.
  Evidence: Initial M1 runs reported `Run-time dependency notcurses found: NO`; subsequent `ninja -C build_debug reconfigure` reported `Run-time dependency notcurses found: YES 3.0.17`, and `notcurses_backend` tests passed.
- Observation: Milestone 2 terminal modularization preserved baseline stream outputs while removing large duplicated parsing/style blocks from terminal backends.
  Evidence: `diff -u plans/artifacts/display-rewrite/m1/scene_stream.txt plans/artifacts/display-rewrite/m2/scene_stream.txt` and corresponding table diff both produced no output; new shared modules are wired through `src/display/event_loop.c`, `src/display/render/backend_ansi.c`, `src/display/render/backend_ansi_inline.c`, and `src/display/render/backend_notcurses.c`.
- Observation: Linux needed a true windowed backend for M3 acceptance; mapping `gui` to terminal backends did not satisfy the milestone objective.
  Evidence: User-facing validation required `./build_debug/widget_demo --widget all --backend gui` to open an X11 window and process real GUI interactions.
- Observation: X11 bring-up uncovered runtime-specific defects (font identifier validity, pixel-vs-cell mouse routing, and border hit-testing footprint) that were not exercised by prior deterministic parity-only checks.
  Evidence: Initial runtime error included `BadFont (invalid Font parameter)` from `X_ChangeGC`; subsequent fixes landed in `src/display/render/backend_x11.c` and `src/display/mouse.c` with new regression checks in `test_x11_backend` and `test_display_event_dispatch`.
- Observation: Milestone 4 table helper extraction initially failed under `ncc` parsing until allocation headers/macros were aligned with project conventions.
  Evidence: `ninja -C build_debug ...` failed in `src/display/table/table_text_primitives.c` with `unknown type name 'n00b_free'` and `use of undeclared identifier 'n00b_alloc_size'`; adding `#include "core/alloc.h"` resolved the build.
- Observation: The umbrella/milestone artifact example path `test/data/table_sample.txt` does not exist in this repository snapshot.
  Evidence: `build_debug/n00b_table --style simple test/data/table_sample.txt` returned `No such file or directory` during Milestone 4 artifact generation.
- Observation: Initial Milestone 5 candidate-list implementation returned empty selection lists because helper functions were mutating a by-value list copy.
  Evidence: `test_display_backend_selection` first failed at `assert(tui.len == 1)` with observed `tui.len=0`; changing helper signatures to pointer-based list mutation fixed the failure.
- Observation: GUI startup is unavailable in this environment when `DISPLAY` is not set, but Milestone 5 fallback policy still produced deterministic startup by selecting `ansi`.
  Evidence: `timeout 5 ./build_debug/widget_demo --widget all --backend gui` reported `x11 backend unavailable (cannot open DISPLAY)` then `Backend request 'gui' selected 'ansi' (fallback)`.

## Decision Log

- Decision: Use seven milestones (M0-M6) with additive, stackable diffs instead of a "big bang" rewrite.
  Rationale: This allows bugfixes from earlier review rounds to flow forward cleanly via rebase, while keeping each review small and behaviorally verifiable.
  Date/Author: 2026-03-05 / Codex
- Decision: Require one child ExecPlan per milestone before coding that milestone.
  Rationale: The user asked for an overall plan that can generate additional ExecPlans for specific phases; explicit child-plan generation makes that requirement concrete and repeatable.
  Date/Author: 2026-03-05 / Codex
- Decision: Keep rewrite work in existing display paths and preserve existing tool entry points (`widget_demo`, `table`) as compatibility probes.
  Rationale: In-place rewrite reduces migration risk for downstream callers and keeps regression signals obvious.
  Date/Author: 2026-03-05 / Codex
- Decision: Every milestone must ship tests and a runnable artifact, even for infrastructure-heavy milestones.
  Rationale: The end goal is observable behavior, not only refactoring; this requirement prevents milestones that compile but prove nothing.
  Date/Author: 2026-03-05 / Codex
- Decision: Treat `display_baseline_capture` output as the canonical Milestone 0 artifact baseline rather than `widget_demo` transcripts.
  Rationale: The dedicated capture tool is deterministic and CI-friendly, while `widget_demo` is interactive and less stable for diffable baseline evidence.
  Date/Author: 2026-03-05 / Codex
- Decision: In this repository state, root the milestone stack on the `needs-rebase` lineage (`display-rewrite/m0-baseline` rebased onto `codex/linux-build-and-test`) rather than `main`.
  Rationale: The current `main` branch lacks the display sources needed for rewrite milestones in this workspace, while the chosen lineage supports full milestone validation.
  Date/Author: 2026-03-05 / Codex
- Decision: Treat M2 acceptance as complete only after deterministic replay output and M1 artifact parity diffs are both validated in addition to unit/integration tests.
  Rationale: M2 is primarily internal modularization; replay output plus parity diffs provide concrete evidence that behavior remained stable while architecture changed.
  Date/Author: 2026-03-05 / Codex
- Decision: Extend M3 acceptance to require one actual Linux GUI path (X11) in addition to deterministic parity harness evidence.
  Rationale: The rewrite goal is dual terminal/GUI runtime proof, so Linux must show a real windowed backend path instead of terminal emulation aliases.
  Date/Author: 2026-03-05 / Codex
- Decision: Make backend alias `gui` resolve to true GUI backends only (`cocoa` on macOS, `x11` on Linux/Unix) and keep terminal backends explicit by name.
  Rationale: This removes ambiguity for users validating GUI behavior and keeps backend selection intent unambiguous for later milestones.
  Date/Author: 2026-03-05 / Codex
- Decision: For deterministic table CLI artifacts in this repo state, feed `n00b_table` via stdin instead of the non-existent `test/data/table_sample.txt`.
  Rationale: This keeps milestone artifact commands reproducible and avoids false failures from missing fixture files.
  Date/Author: 2026-03-05 / Codex
- Decision: Set Milestone 5 deterministic auto backend order to `ansi -> gui -> notcurses -> stream -> dumb` and keep this behavior test-visible via explicit selection helpers.
  Rationale: This preserves interactive terminal-first startup while keeping fallback behavior deterministic and auditable in tests/artifacts.
  Date/Author: 2026-03-05 / Codex

## Outcomes & Retrospective

Milestones 0, 1, 2, 3, 4, and 5 are complete on the active rewrite lineage. Milestone 0 established deterministic baseline harnessing (`display_baseline_contract`, `display_baseline_flow`, `display_baseline_capture`) and baseline artifacts in `plans/artifacts/display-rewrite/m0/`. Milestone 1 then carved out internal display contracts (`diagnostics`, `scene_contracts`, `event_dispatch`, `backend_services`), removed hardcoded `/tmp` diagnostics from touched display paths, added deterministic scene inspection (`display_scene_inspect`), and captured parity artifacts in `plans/artifacts/display-rewrite/m1/`. Milestone 2 extracted shared terminal contracts (`terminal_lifecycle`, `ansi_sgr`, `terminal_input`), rewired terminal backends/event loop to consume them, added deterministic replay tooling (`display_terminal_replay`), and produced parity-checked artifacts in `plans/artifacts/display-rewrite/m2/`. Milestone 3 hardened Cocoa GUI contracts and delivered the first Linux native GUI backend (`x11`) with `gui` alias cleanup, interactive `widget_demo` window proof, and M3 validation artifacts under `plans/artifacts/display-rewrite/m3/`. Milestone 4 centralized widget/table/hexdump shared primitives, added mixed-flow integration coverage and deterministic showcase tooling (`display_m4_showcase`), and produced parity-checked artifacts under `plans/artifacts/display-rewrite/m4/`. Milestone 5 unified runtime backend selection into a registry-first path, added policy/plugin/integration coverage, and generated deterministic selection evidence under `plans/artifacts/display-rewrite/m5/`.

What remains is Milestone 6 execution only: hardening, documentation alignment, and final cutover. The key lesson through M5 is that deterministic artifact generation plus explicit internal contracts keeps parity claims defensible while refactoring runtime selection and backend startup paths.

## Context and Orientation

The current display subsystem lives in `include/display/` and `src/display/`. It already contains a plane/canvas render model, backend abstraction, input and focus routing, widgets, table rendering, and hexdump formatting. The strongest parts to preserve are called out in `docs/tui_prototype_intent.md`: one renderer contract, composed planes, normalized events, widget vtables, and stream backend support.

The code review identifies where the rewrite should focus first: large backend files with mixed responsibilities, duplicated widget behavior helpers, ad-hoc diagnostics, and insufficient end-to-end tests. Those issues are in `reviews/CODE_REVIEW.md` and form the primary rewrite scope.

In this plan, a "stacked diff" means each milestone branch is based on the previous milestone branch. In this workspace, `display-rewrite/m0-baseline` is already established as the stack root and descendants must branch from it (not from `main`) so all display rewrite context stays intact. If review feedback lands on an earlier branch, descendant branches are rebased on the updated parent branch so bugfixes carry forward cleanly.

A "human-runnable artifact" means an executable command and expected behavior that a person can run locally to see milestone capability (for example, an interactive demo, CLI renderer output, or backend parity harness with visible output files).

## Plan of Work

### Branching and Review Strategy (Mandatory For Every Milestone)

Use this branch chain and do not skip levels:

`display-rewrite/m0-baseline` -> `display-rewrite/m1-core-contracts` -> `display-rewrite/m2-terminal-backend` -> `display-rewrite/m3-gui-backend` -> `display-rewrite/m4-widget-table-migration` -> `display-rewrite/m5-runtime-selection` -> `display-rewrite/m6-hardening-and-cutover`.

Each milestone opens one review diff against its immediate parent branch. Each milestone branch contains exactly the scope for that milestone plus required tests and artifacts. When fixes are requested on an earlier milestone, apply the fix there first, then rebase each descendant milestone in order so all later milestones inherit the correction.

Before writing code for any milestone, create a child ExecPlan at `plans/display-rewrite/m<milestone>-<slug>-execplan.md` that follows this umbrella plan and includes file-level edit details.

### Milestone 0: Baseline, Safety Rails, And Rewrite Harness

Status: Complete on `display-rewrite/m0-baseline` (commits `c81d9b6` and `201028f`).

Delivered scope includes Milestone 0 Meson wiring, unit/integration baseline tests, and deterministic baseline artifact generation without changing public display semantics.

Implemented files are:
- `test/unit/test_display_baseline_contract.c`
- `test/integration/test_display_baseline_flow.c`
- `src/tools/display_baseline_capture.c`
- `meson.build` updates for new test/tool targets

Acceptance evidence was produced by passing named tests and generating artifact captures under `plans/artifacts/display-rewrite/m0/` for later milestone diff comparisons.

### Milestone 1: Core Contracts And Internal Module Boundaries

Status: Complete on `display-rewrite/m1-core-contracts`.

Milestone 1 performs the first in-place architectural carve-out. It defines explicit internal boundaries for scene composition, backend services, event dispatch, and diagnostics without breaking the external header surface that existing tools use.

Implementation scope includes extracting internal modules from monolithic files, introducing a centralized diagnostics helper under display paths, and writing compatibility adapters where old call paths must remain temporarily.

Unit tests for this milestone must validate the new internal contracts (for example scene flatten invariants, backend service callback behavior, diagnostics gating). Integration tests must verify that legacy entry points (such as `n00b_canvas_run` paths used by `widget_demo`) still behave the same while routed through new internals. The human-runnable artifact is a scene inspection tool run (new or extended tool under `src/tools/`) that prints or captures composed plane output before and after the internal split, proving parity.

Acceptance for Milestone 1 is parity with Milestone 0 behavior plus visible removal of hardcoded `/tmp` diagnostics from touched paths.

Completion evidence: `display_diagnostics`, `display_scene_contracts`, `display_event_dispatch`, `display_baseline_contract`, `display_baseline_flow`, and `display_m1_compat` all passed; baseline artifact diffs vs M0 were empty; `/tmp` scan for touched files returned no matches; and notcurses-enabled validation passed after dependency install/reconfigure.

### Milestone 2: Terminal Backend Rewrite On New Contracts

Status: Complete on `display-rewrite/m2-terminal-backend`.

Milestone 2 rewrites terminal execution paths to consume Milestone 1 internal contracts cleanly. This includes ANSI/inline ANSI rendering paths, terminal input translation, and event loop interactions that are currently scattered.

Implementation scope includes decomposing terminal backend responsibilities into dedicated internal units (event translation, render diffing, lifecycle state, optional metrics helpers) under `src/display/render/`, while preserving existing backend entry symbols.

Unit tests for this milestone must target terminal-specific helpers and renderer correctness under edge cases (resize, style degradation, cursor visibility, key normalization). Integration tests must drive an end-to-end terminal interaction sequence (focus traversal, mouse click routing, widget activation, rerender) through a deterministic harness. The human-runnable artifact is an interactive `widget_demo` terminal session plus a non-interactive replay command that emits a capture file under `plans/artifacts/display-rewrite/m2/`.

Acceptance for Milestone 2 is that terminal behavior is feature-parity with baseline while code is modularized by concern and covered by both unit and integration tests.

Completion evidence: `display_terminal_lifecycle`, `display_ansi_sgr`, `display_terminal_input`, `display_baseline_contract`, `display_event_dispatch`, `display_baseline_flow`, `display_m1_compat`, and `display_m2_terminal_flow` all passed; display smoke tests and `notcurses_backend` passed; `display_terminal_replay` artifacts were generated under `plans/artifacts/display-rewrite/m2/`; and M2 stream artifacts matched M1 (`scene_stream.txt`, `table_stream.txt`) with empty diffs.

### Milestone 3: GUI Backend Parity Path

Status: Complete on `display-rewrite/m3-gui-backend`.

Milestone 3 makes the dual-target goal concrete by bringing one GUI backend path up to parity on the same contracts used by terminal rendering. In this repository, the initial GUI path is Cocoa-backed code (`backend_cocoa.m`) with bridge constraints that must be formalized.

Implementation scope includes rewriting GUI bridge internals onto the shared contracts, codifying bridge invariants, and adding compile-time or runtime guards that detect ABI drift between C headers and Objective-C bridge declarations.

Unit tests for this milestone must include bridge layout/contract guard tests and GUI event translation helpers that can run in non-interactive environments where possible. Integration tests must include a backend parity scenario that renders the same widget composition through terminal and GUI pathways (or GUI headless harness if CI lacks a window server) and compares normalized output properties. The human-runnable artifact is a GUI smoke command (for supported systems) and a generated parity report in `plans/artifacts/display-rewrite/m3/`.

Acceptance for Milestone 3 is that one app composition can run through terminal and GUI backends with equivalent interaction semantics for a documented subset.

Completion evidence: Cocoa input and bridge contracts were extracted/guarded, deterministic parity artifacts were generated under `plans/artifacts/display-rewrite/m3/`, Linux native GUI backend `x11` was added and wired into backend registry/`gui` alias resolution, and validation passed for `display_backend_registry`, `x11_backend`, and `display_event_dispatch` alongside existing M3 parity checks.

### Milestone 4: Widget, Table, And Hexdump Migration To Shared Primitives

Status: Complete on `display-rewrite/m4-widget-table-migration`.

Milestone 4 migrates higher-level UI behavior to the rewritten runtime. The focus is reducing duplication in widget behavior, aligning table/hexdump rendering with shared text/layout helpers, and preserving user-visible outputs.

Implementation scope includes introducing common widget helper modules, migrating target widgets incrementally, and converging table/hexdump formatting helpers where behavior overlap exists.

Unit tests for this milestone must cover shared helper modules and migrated widget/table/hexdump edge cases. Integration tests must exercise realistic flows that combine multiple widgets plus table/hexdump rendering in one canvas lifecycle. The human-runnable artifact is a showcase run (extended `widget_demo` scenario or dedicated display showcase tool) that demonstrates mixed widgets, table views, and hexdump views under at least one terminal and one non-terminal backend path (or parity harness output when GUI is unavailable).

Acceptance for Milestone 4 is that duplicated behavior is centralized, outputs remain stable against baseline expectations, and mixed-content flows are demonstrably functional.

Completion evidence: new primitive tests (`display_widget_primitives`, `display_table_text_primitives`, `display_hexdump_contracts`) passed; migrated regression suites and `display_m4_widget_table_flow` integration passed; `display_m4_showcase` generated deterministic artifacts in `plans/artifacts/display-rewrite/m4/`; and M4 parity diffs against M3 scene/table baseline streams were empty.

### Milestone 5: Runtime Backend Selection And Plugin Readiness

Status: Complete on `display-rewrite/m5-runtime-selection`.

Milestone 5 completes the runtime host story by making backend selection and registry behavior coherent and documented. The current mismatch between registry capabilities and runtime wiring is resolved here.

Implementation scope includes deciding and implementing the canonical backend selection path (registry-first or explicit built-in preference with documented fallback), ensuring initialization happens in the right startup path, and keeping deterministic behavior for tests.

Unit tests for this milestone must cover registry initialization, backend lookup/fallback, and error reporting. Integration tests must verify full app startup under multiple backend selection configurations (explicit backend, auto backend, missing backend fallback). The human-runnable artifact is a backend-selection demo command that prints selected backend, runs a small UI, and writes a selection report to `plans/artifacts/display-rewrite/m5/`.

Acceptance for Milestone 5 is predictable backend selection behavior with passing tests and demonstrable runtime switching.

Completion evidence: `display_backend_registry`, `display_backend_selection`, `display_backend_plugin`, and `display_m5_runtime_selection` passed; prior display integration/smoke suites remained green; deterministic artifacts (`scene_stream.txt`, `table_stream.txt`, `metadata.txt`, `scene_inspect.txt`, `selection_report.txt`, `selection_metadata.txt`) were generated under `plans/artifacts/display-rewrite/m5/`; and M5 scene/table stream parity diffs against M4 were empty.

### Milestone 6: Hardening, Documentation Alignment, And Final Cutover

Milestone 6 removes temporary compatibility layers, finalizes docs, and closes the rewrite. This milestone updates architecture docs so they match the implemented model and verifies release-grade behavior across target backends.

Implementation scope includes deleting deprecated shims introduced during migration, updating display docs to match real APIs and runtime behavior, and producing a final acceptance artifact that demonstrates the dual terminal/GUI runtime story end-to-end.

Unit tests for this milestone must confirm no deprecated paths are silently used and that core contracts remain stable. Integration tests must run the full display matrix targeted by this project (terminal, stream, and GUI-capable environments where available) and include failure diagnostics when an optional backend is unavailable. The human-runnable artifact is a final end-to-end sample command sequence showing one app definition rendered through terminal and GUI paths with matching behavior notes.

Acceptance for Milestone 6 is a fully in-place rewritten display subsystem with aligned docs, green test matrix, and a reproducible final demo.

## Concrete Steps

Run all commands from `/home/baron/crash-override/n00b-athens`.

Current branch reality:

    git branch --show-current
    # expected: display-rewrite/m5-runtime-selection

Milestones 0-5 are already complete on the active lineage. Create the final milestone branch from the current milestone branch:

    git switch display-rewrite/m5-runtime-selection
    git switch -c display-rewrite/m6-hardening-and-cutover

For each milestone `Mx`, create the child ExecPlan first:

    mkdir -p plans/display-rewrite
    ${EDITOR:-vi} plans/display-rewrite/mx-<slug>-execplan.md

During each milestone, run milestone-specific tests plus shared display smoke tests. At minimum:

    meson test -C build_debug --print-errorlogs \
      render_plane render_canvas render_ansi event_normalize focus mouse \
      label button checkbox input list_widget selectionlist breadcrumb \
      table_build table_layout table_render table_stream hexdump xform_render

Run integration suite (Milestone 0 already added display coverage):

    meson test -C build_debug --suite integration --print-errorlogs

For Milestone 0 baseline evidence (already generated on this branch), regenerate deterministically with:

    ninja -C build_debug display_baseline_capture
    build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m0

For Milestones 1-6, run human-runnable artifact commands and capture outputs under milestone artifact directories:

    mkdir -p plans/artifacts/display-rewrite/mx
    build_debug/widget_demo --widget all --backend tui > plans/artifacts/display-rewrite/mx/widget_demo_tui.txt
    printf 'Component,State\nwidget,ready\nhexdump,formatted\n' | build_debug/n00b_table --style simple > plans/artifacts/display-rewrite/mx/table_simple.txt

When review feedback modifies an earlier milestone branch, rebase descendants in order:

    git switch display-rewrite/m1-core-contracts
    git rebase display-rewrite/m0-baseline
    git switch display-rewrite/m2-terminal-backend
    git rebase display-rewrite/m1-core-contracts
    git switch display-rewrite/m3-gui-backend
    git rebase display-rewrite/m2-terminal-backend
    git switch display-rewrite/m4-widget-table-migration
    git rebase display-rewrite/m3-gui-backend
    git switch display-rewrite/m5-runtime-selection
    git rebase display-rewrite/m4-widget-table-migration
    git switch display-rewrite/m6-hardening-and-cutover
    git rebase display-rewrite/m5-runtime-selection

Use `git range-diff` before pushing updated stacked diffs to verify only intended changes moved:

    git range-diff origin/needs-rebase...display-rewrite/m3-gui-backend@{1} origin/needs-rebase...display-rewrite/m3-gui-backend

## Validation and Acceptance

This umbrella plan is successful only if every milestone child ExecPlan enforces three proof types before milestone completion: unit tests, integration tests, and one human-runnable artifact with expected behavior.

The overall rewrite is accepted only when one shared app composition can be shown running on both a terminal backend and a GUI backend through the same display model and interaction semantics, with differences documented only where backend capabilities genuinely differ.

Regression acceptance requires that baseline captures created in Milestone 0 are compared against later milestone captures and material differences are either intentional (documented in milestone Decision Log) or treated as bugs.

## Idempotence and Recovery

This plan is idempotent when followed milestone-by-milestone: branch names are stable, artifact directories are additive, and test commands can be re-run repeatedly.

If a rebase in the stack fails, resolve conflicts on the current branch only, run the milestone tests/artifacts again, and continue rebasing the next descendant branch. If conflicts indicate architectural drift, pause implementation, update the active child ExecPlan and this umbrella plan's Decision Log, then continue.

If an optional backend environment is unavailable (for example GUI backend on a non-supported runner), run the milestone's fallback parity harness and record the environment limitation explicitly in `Surprises & Discoveries` with command evidence.

## Artifacts and Notes

Store milestone evidence under `plans/artifacts/display-rewrite/m0` through `plans/artifacts/display-rewrite/m6`. Each milestone must include concise transcripts for unit tests, integration tests, and artifact runs.

Milestone 0 evidence captured on this branch:

    $ meson test -C build_debug --print-errorlogs display_baseline_contract
    1/1 unit - n00b:display_baseline_contract OK

    $ meson test -C build_debug --print-errorlogs display_baseline_flow
    1/1 integration - n00b:display_baseline_flow OK

    $ meson test -C build_debug --print-errorlogs render_plane render_canvas render_ansi event_normalize focus mouse label button checkbox input list_widget selectionlist breadcrumb table_build table_layout table_render table_stream hexdump xform_render
    19/19 unit tests OK

    $ ninja -C build_debug display_baseline_capture
    [2/2] Linking target display_baseline_capture

    $ build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m0
    wrote scene_stream.txt
    wrote table_stream.txt
    wrote metadata.txt

    $ cat plans/artifacts/display-rewrite/m0/metadata.txt
    tool=display_baseline_capture
    tool_version=1
    backend=stream
    scene_rows=10
    scene_cols=52
    table_render_cols=52
    n00b_version=0.3.0

Use these files as the reference baseline for milestone parity checks:

    plans/artifacts/display-rewrite/m0/scene_stream.txt
    plans/artifacts/display-rewrite/m0/table_stream.txt
    plans/artifacts/display-rewrite/m0/metadata.txt

Keep artifact captures short and focused on proving acceptance criteria, not full logs.

Milestone 1 evidence captured on this branch:

    $ meson test -C build_debug --print-errorlogs display_diagnostics display_scene_contracts display_event_dispatch display_baseline_contract display_baseline_flow display_m1_compat
    6/6 tests OK

    $ build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m1
    wrote scene_stream.txt
    wrote table_stream.txt
    wrote metadata.txt

    $ build_debug/display_scene_inspect --out plans/artifacts/display-rewrite/m1/scene_inspect.txt
    wrote plans/artifacts/display-rewrite/m1/scene_inspect.txt

    $ diff -u plans/artifacts/display-rewrite/m0/scene_stream.txt plans/artifacts/display-rewrite/m1/scene_stream.txt
    (no output)

    $ diff -u plans/artifacts/display-rewrite/m0/table_stream.txt plans/artifacts/display-rewrite/m1/table_stream.txt
    (no output)

    $ rg --line-number '/tmp' src/display/event_loop.c src/display/render/backend_ansi.c src/display/render/backend_notcurses.c src/display/widgets/label.c
    (no output)

    $ ninja -C build_debug reconfigure
    Run-time dependency notcurses found: YES 3.0.17

    $ meson test -C build_debug --print-errorlogs notcurses_backend display_diagnostics display_scene_contracts display_event_dispatch display_baseline_contract display_baseline_flow display_m1_compat
    7/7 tests OK

Milestone 2 evidence captured on this branch:

    $ ninja -C build_debug display_terminal_replay display_baseline_capture display_scene_inspect
    [14/14] Linking target display_terminal_replay

    $ meson test -C build_debug --print-errorlogs display_terminal_lifecycle display_ansi_sgr display_terminal_input display_baseline_contract display_event_dispatch
    5/5 tests OK

    $ meson test -C build_debug --print-errorlogs display_baseline_flow display_m1_compat display_m2_terminal_flow
    3/3 tests OK

    $ meson test -C build_debug --print-errorlogs render_plane render_canvas render_ansi event_normalize focus mouse label button checkbox input list_widget selectionlist breadcrumb table_build table_layout table_render table_stream hexdump xform_render
    19/19 unit tests OK

    $ meson test -C build_debug --print-errorlogs notcurses_backend
    1/1 tests OK

    $ build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m2
    wrote scene_stream.txt
    wrote table_stream.txt
    wrote metadata.txt

    $ build_debug/display_scene_inspect --out plans/artifacts/display-rewrite/m2/scene_inspect.txt
    wrote plans/artifacts/display-rewrite/m2/scene_inspect.txt

    $ build_debug/display_terminal_replay --out-dir plans/artifacts/display-rewrite/m2
    wrote terminal_replay.txt
    wrote metadata.txt

    $ diff -u plans/artifacts/display-rewrite/m1/scene_stream.txt plans/artifacts/display-rewrite/m2/scene_stream.txt
    (no output)

    $ diff -u plans/artifacts/display-rewrite/m1/table_stream.txt plans/artifacts/display-rewrite/m2/table_stream.txt
    (no output)

Milestone 3 evidence captured on this branch:

    $ ninja -C build_debug widget_demo test_display_backend_registry test_x11_backend
    [5/5] Linking target test_x11_backend

    $ meson test -C build_debug --print-errorlogs display_backend_registry x11_backend display_event_dispatch
    3/3 tests OK

    $ ./build_debug/widget_demo --widget all --backend gui
    # expected on Linux/Unix with DISPLAY:
    # opens an X11 window and processes interactive clicks/keys

    $ diff -u plans/artifacts/display-rewrite/m2/scene_stream.txt plans/artifacts/display-rewrite/m3/scene_stream.txt
    (no output)

    $ diff -u plans/artifacts/display-rewrite/m2/table_stream.txt plans/artifacts/display-rewrite/m3/table_stream.txt
    (no output)

Milestone 4 evidence captured on this branch:

    $ meson test -C build_debug --print-errorlogs display_widget_primitives display_table_text_primitives display_hexdump_contracts
    3/3 tests OK

    $ meson test -C build_debug --print-errorlogs button checkbox switch radio list_widget selectionlist input breadcrumb link table_build table_layout table_render table_stream hexdump xform_hexdump display_event_dispatch display_baseline_contract
    17/17 tests OK

    $ meson test -C build_debug --print-errorlogs display_baseline_flow display_m1_compat display_m2_terminal_flow display_m3_backend_parity display_m4_widget_table_flow
    5/5 tests OK

    $ build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m4
    wrote scene_stream.txt
    wrote table_stream.txt
    wrote metadata.txt

    $ build_debug/display_scene_inspect --out plans/artifacts/display-rewrite/m4/scene_inspect.txt
    wrote plans/artifacts/display-rewrite/m4/scene_inspect.txt

    $ build_debug/display_m4_showcase --out-dir plans/artifacts/display-rewrite/m4
    wrote showcase_stream.txt
    wrote hexdump_stream.txt
    wrote showcase_metadata.txt

    $ diff -u plans/artifacts/display-rewrite/m3/scene_stream.txt plans/artifacts/display-rewrite/m4/scene_stream.txt
    (no output)

    $ diff -u plans/artifacts/display-rewrite/m3/table_stream.txt plans/artifacts/display-rewrite/m4/table_stream.txt
    (no output)

Milestone 5 evidence captured on this branch:

    $ meson test -C build_debug --print-errorlogs display_backend_registry display_backend_selection display_backend_plugin
    3/3 tests OK

    $ meson test -C build_debug --print-errorlogs display_baseline_flow display_m1_compat display_m2_terminal_flow display_m3_backend_parity display_m4_widget_table_flow display_m5_runtime_selection
    6/6 tests OK

    $ meson test -C build_debug --print-errorlogs render_plane render_canvas render_ansi event_normalize focus mouse label button checkbox input list_widget selectionlist breadcrumb table_build table_layout table_render table_stream hexdump xform_render
    19/19 tests OK

    $ build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m5
    wrote scene_stream.txt
    wrote table_stream.txt
    wrote metadata.txt

    $ build_debug/display_scene_inspect --out plans/artifacts/display-rewrite/m5/scene_inspect.txt
    wrote plans/artifacts/display-rewrite/m5/scene_inspect.txt

    $ build_debug/display_backend_selection_report --out-dir plans/artifacts/display-rewrite/m5
    wrote selection_report.txt
    wrote selection_metadata.txt

    $ diff -u plans/artifacts/display-rewrite/m4/scene_stream.txt plans/artifacts/display-rewrite/m5/scene_stream.txt
    (no output)

    $ diff -u plans/artifacts/display-rewrite/m4/table_stream.txt plans/artifacts/display-rewrite/m5/table_stream.txt
    (no output)

## Interfaces and Dependencies

This rewrite must preserve and evolve the existing display-facing interfaces in place:

- Render contracts and types in `include/display/render/*.h` with implementations in `src/display/render/*.c`, `src/display/render/backend_cocoa.m`, and `src/display/render/backend_x11.c`.
- Interaction contracts in `include/display/event.h`, `include/display/event_loop.h`, `include/display/focus.h`, and `include/display/mouse.h` with implementations in `src/display/`.
- Widget contracts in `include/display/widget.h` and concrete widgets in `src/display/widgets/*.c`.
- Data presentation modules in `include/display/table/table.h`, `src/display/table/*.c`, `include/display/hexdump.h`, and `src/display/hexdump.c`.
- Tool and integration surfaces in `src/tools/widget_demo.c`, `src/tools/table.c`, and `src/conduit/xform_render.c`.

Dependencies that materially affect milestone planning include optional GUI/render backends (Cocoa, X11, Notcurses, FreeType), Meson test wiring in `meson.build`, and any environment prerequisites needed to run GUI or advanced backend tests. Each child ExecPlan must spell out environment assumptions and fallback validation paths.

## Revision Notes

- 2026-03-05: Initial umbrella ExecPlan created to guide an in-place display rewrite with stacked diffs, milestone-specific child ExecPlans, and mandatory unit/integration/artifact acceptance requirements per milestone.
- 2026-03-05: Updated umbrella plan to match the current `display-rewrite/m0-baseline` branch state: marked Milestone 0 complete, recorded real validation/artifact evidence, adjusted stack-root guidance to current branch lineage, and set Milestone 1 as the next pending step.
- 2026-03-05: Marked Milestone 1 child ExecPlan authoring as complete after creating `plans/display-rewrite/m1-core-contracts-execplan.md`, so umbrella progress reflects current branch planning state.
- 2026-03-05: Marked Milestone 1 execution complete in the umbrella plan, added M1 artifact/test evidence (including notcurses-enabled rerun), and moved remaining scope to M2-M6.
- 2026-03-05: Marked Milestone 2 child ExecPlan authoring as complete after creating `plans/display-rewrite/m2-terminal-backend-execplan.md`, and narrowed the remaining umbrella progress item to milestone execution.
- 2026-03-05: Updated umbrella plan after Milestone 2 execution to mark M2 complete, add M2 validation/artifact evidence, and narrow remaining execution scope to Milestones 3-6.
- 2026-03-05: Marked Milestone 3 child ExecPlan authoring as complete after creating `plans/display-rewrite/m3-gui-backend-execplan.md`, while keeping remaining execution scope at Milestones 3-6.
- 2026-03-05: Updated umbrella plan after Milestone 3 execution to mark M3 complete, record Linux X11 GUI backend evidence, update branch guidance to start at M4, and narrow remaining scope to Milestones 4-6. Reason: milestone planning state must match implemented GUI deliverables and current branch reality.
- 2026-03-05: Marked Milestone 4 child ExecPlan authoring as complete after creating `plans/display-rewrite/m4-widget-table-migration-execplan.md`, while keeping remaining execution scope at Milestones 4-6.
- 2026-03-05: Updated umbrella plan after Milestone 4 execution to mark M4 complete, add M4 validation/artifact evidence, adjust branch/remaining scope guidance to Milestones 5-6, and replace the missing table sample path in generic artifact commands with deterministic stdin-fed table input.
- 2026-03-05: Marked Milestone 5 child ExecPlan authoring as complete after creating `plans/display-rewrite/m5-runtime-selection-execplan.md`, while keeping remaining execution scope at Milestones 5-6.
- 2026-03-05: Updated umbrella plan after Milestone 5 execution to mark M5 complete, add M5 validation/artifact evidence, capture M5 surprises/decisions, and narrow remaining execution scope to Milestone 6 only.
- 2026-03-05: Marked Milestone 6 child ExecPlan authoring as complete after creating `plans/display-rewrite/m6-hardening-and-cutover-execplan.md`, while keeping remaining execution scope at Milestone 6 execution.
