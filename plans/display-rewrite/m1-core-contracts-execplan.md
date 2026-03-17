# Carve Out Display Core Contracts, Diagnostics, And Compatibility Boundaries (M1)

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

This plan is Milestone 1 of the umbrella plan at `plans/display-rewrite-overall-execplan.md`. There is no repository-local `PLANS.md` in this working tree, so this document follows `/home/baron/.codex/PLANS.md` and must remain compliant with it.

## Purpose / Big Picture

Milestone 1 creates clear internal contracts for scene composition, backend service access, event dispatch, and diagnostics without changing the public display API. After this milestone, contributors should still run the same app entry points (`n00b_canvas_run`, `widget_demo`, `n00b_table`) and see baseline behavior, but the implementation should be organized into smaller internal modules instead of hiding core logic inside large source files.

The user-visible effect is parity with Milestone 0 plus safer runtime behavior: touched display paths no longer hardcode debug writes to `/tmp`, and there is a deterministic scene-inspection artifact that makes later milestone diffs easier to review.

## Progress

- [x] (2026-03-05 11:08Z) Reviewed the umbrella plan and confirmed Milestone 1 scope, acceptance criteria, and branch stacking requirements.
- [x] (2026-03-05 11:10Z) Audited current display hotspots and identified concrete extraction targets: event loop internals, diagnostics side effects, and scene/composition boundary points.
- [x] (2026-03-05 11:18Z) Authored this Milestone 1 child ExecPlan with file-level implementation details, test additions, and artifact commands.
- [x] (2026-03-05 11:23Z) Confirmed work is on `display-rewrite/m1-core-contracts` and proceeded in-place from the existing Milestone 1 branch tip.
- [x] (2026-03-05 11:23Z) Implemented internal contract modules (`diagnostics`, `scene_contracts`, `event_dispatch`, `backend_services`) and rewired `event_loop.c`/`canvas.c` and backend call sites without public API changes.
- [x] (2026-03-05 11:23Z) Removed hardcoded `/tmp` diagnostics from touched display files and replaced those paths with centralized diagnostics gating.
- [x] (2026-03-05 11:23Z) Added M1 unit/integration tests, built tooling, ran milestone tests + smoke tests, and generated M1 artifacts with empty baseline diffs.
- [x] (2026-03-05 11:17Z) Reconfigured with installed notcurses (`3.0.17`) and reran notcurses backend coverage plus milestone/smoke validation with backend enabled.
- [x] (2026-03-17 18:12Z) Executed the post-review remediation: added blocking regression tests, made `display_scene_inspect --out` mandatory, added `display_scene_inspect_artifacts`, and refreshed the M0/M1 scene artifacts after trimming the trailing blank line from `scene_stream.txt`.

## Surprises & Discoveries

- Observation: Runtime debug writes are still hardcoded to `/tmp` across key display paths, including event loop and ANSI/notcurses backend paths.
  Evidence: `rg '/tmp' src/display/event_loop.c src/display/render/backend_ansi.c src/display/render/backend_notcurses.c src/display/widgets/label.c` reports active `fopen("/tmp/...")` calls.
- Observation: The largest display risk files are still very large and mixed-responsibility, especially GUI/terminal backend files.
  Evidence: `wc -l` reports `src/display/render/backend_notcurses.c` at 2314 lines and `src/display/render/backend_cocoa.m` at 1736 lines.
- Observation: `widget_demo` remains useful as a compatibility probe, but its default TUI path is interactive and includes direct `/tmp` logging.
  Evidence: `src/tools/widget_demo.c` opens `/tmp/widget_demo.log` and uses keypress waits for non-event-loop terminal mode.
- Observation: Milestone 0 added deterministic baseline capture and display integration coverage, so M1 can prove parity without depending on interactive terminal sessions.
  Evidence: `test_display_baseline_contract`, `test_display_baseline_flow`, and `src/tools/display_baseline_capture.c` are already present and green on `display-rewrite/m0-baseline`.
- Observation: `n00b_canvas_run()` warmup logic intentionally discards early key/mouse input, so deterministic loop-compat tests must queue an initial `N00B_EVENT_NONE` before behavioral key events.
  Evidence: `test/integration/test_display_m1_compat.c` uses an initial NONE event and `tick_ms=100` to avoid warmup swallowing the compatibility assertions.
- Observation: Backend-dependent coverage changed mid-milestone after installing notcurses, and reconfigure picked it up without code changes.
  Evidence: `ninja -C build_debug reconfigure` now reports `Run-time dependency notcurses found: YES 3.0.17`, and `meson test ... notcurses_backend ...` passes.

## Decision Log

- Decision: Keep all public display interfaces unchanged during Milestone 1 and perform extraction behind internal headers only.
  Rationale: M1 is about module boundaries and safety rails; public API churn belongs in later milestones only if required by proven behavior gaps.
  Date/Author: 2026-03-05 / Codex
- Decision: Introduce new internal headers under `include/internal/display/` and implement new modules under `src/display/` and `src/display/render/`.
  Rationale: `include/internal` is already in Meson include paths, so this keeps contracts explicit without exposing them as public API.
  Date/Author: 2026-03-05 / Codex
- Decision: Centralize diagnostics behind one internal module with explicit gating and sink policy; default behavior is silent unless explicitly enabled.
  Rationale: This removes accidental `/tmp` side effects and makes logging behavior deterministic in tests and CI.
  Date/Author: 2026-03-05 / Codex
- Decision: Add a deterministic scene-inspection tool artifact for M1 (`display_scene_inspect`) instead of relying on interactive probes.
  Rationale: M1 acceptance requires before/after compositing parity evidence, which is easier to diff from deterministic text output.
  Date/Author: 2026-03-05 / Codex
- Decision: Make diagnostics opt-in with `N00B_DISPLAY_DIAG`/`N00B_DISPLAY_DIAG_LEVEL`, defaulting to silent mode with no implicit `/tmp` sink.
  Rationale: This preserves baseline runtime behavior while eliminating filesystem side effects and still allows targeted tracing during debugging/tests.
  Date/Author: 2026-03-05 / Codex

## Outcomes & Retrospective

Milestone 1 implementation is complete on `display-rewrite/m1-core-contracts`. Internal contract modules were added under `include/internal/display/` and `src/display/**`, `event_loop.c` and `canvas.c` were reduced to orchestration over those contracts, and touched display files no longer contain hardcoded `/tmp` diagnostics.

Compatibility and parity evidence:

- Milestone contract tests passed: `display_diagnostics`, `display_scene_contracts`, `display_event_dispatch`, `display_baseline_contract`, `display_baseline_flow`, `display_m1_compat`.
- Display smoke suite passed for render/event/widget/table/hexdump/xform targets listed in this plan.
- Notcurses backend validation passed after install (`notcurses_backend` green with dependency detected as `3.0.17`).
- M1 artifact generation completed, including `scene_inspect.txt`.
- Baseline diffs (`scene_stream.txt`, `table_stream.txt`) were empty relative to M0.
- Final `/tmp` scan for touched files returned no matches.
- Post-review remediation added explicit-output parity coverage for `display_scene_inspect`, refreshed the M0/M1 `scene_stream.txt` artifacts to remove the trailing blank line at EOF, and regenerated `scene_inspect.txt` with the root plane correctly marked clean.

## Context and Orientation

The display subsystem already runs through `include/display/**` and `src/display/**` with public APIs that tools and tests consume directly. Milestone 0 established a baseline via `test/unit/test_display_baseline_contract.c`, `test/integration/test_display_baseline_flow.c`, and artifact captures under `plans/artifacts/display-rewrite/m0/`.

For this milestone, an internal contract means a header in `include/internal/display/` that defines module boundaries used only by display implementation files. A compatibility adapter means keeping existing public entry points and vtables in place while delegating their internal work to new modules. A diagnostics gate means logging is opt-in and routed through one policy module, instead of ad hoc `fopen("/tmp/...")` calls spread across components.

Primary files to understand before editing are:

- `src/display/event_loop.c`
- `src/display/render/canvas.c`
- `src/display/render/composite.c`
- `src/display/render/backend_ansi.c`
- `src/display/render/backend_notcurses.c`
- `src/display/widgets/label.c`
- `meson.build`
- `src/tools/display_baseline_capture.c`

Milestone 1 is rooted on `display-rewrite/m0-baseline` in this workspace lineage and must preserve parity with Milestone 0 artifacts unless explicitly documented.

## Plan of Work

Create four internal display contract headers and their implementations, then rewire existing call sites so public APIs stay unchanged while internals become modular.

First, add diagnostics policy as a shared internal service. Create `include/internal/display/diagnostics.h` and `src/display/diagnostics.c`. This module must own logging enable/disable state, level filtering, and sink routing, and it must not default to opening `/tmp` files. Use environment-based opt-in (for example `N00B_DISPLAY_DIAG` and `N00B_DISPLAY_DIAG_LEVEL`) plus explicit setters for tests. Then replace direct debug file handling in touched display files (`src/display/event_loop.c`, `src/display/render/backend_ansi.c`, `src/display/render/backend_notcurses.c`, `src/display/widgets/label.c`) with this service.

Second, split scene and event-loop responsibilities into internal contracts. Create `include/internal/display/scene_contracts.h` with `src/display/scene_contracts.c` for dirty-tree traversal, layout cascade, and flattening helpers currently duplicated or hidden in `event_loop.c` and `canvas.c`. Create `include/internal/display/event_dispatch.h` with `src/display/event_dispatch.c` for key/mouse dispatch decisions currently embedded in `n00b_canvas_run`. Keep `n00b_canvas_run()` in `src/display/event_loop.c`, but reduce it to orchestration that delegates to new internal modules.

Third, add backend service adapters to isolate direct vtable invocations. Create `include/internal/display/backend_services.h` and `src/display/render/backend_services.c` for polling events, reading backend size/capabilities, and cursor visibility operations. Rewire `event_loop.c` and `canvas.c` to use these adapters so later backend rewrites can swap internals without changing event-loop/canvas public code paths.

Fourth, add deterministic parity tooling and tests for the new boundaries. Add `src/tools/display_scene_inspect.c` to emit z-sorted flattened scene entries in stable text form and require `--out PATH` so callers choose the destination explicitly. Share the demo-scene construction between `display_baseline_capture` and `display_scene_inspect` through one private helper under `src/tools/`, register `display_scene_inspect` in `meson.build` as a non-installed local tool, and add parity coverage for both artifact generators. Add unit tests for diagnostics gating, scene contract invariants, and event dispatch behavior. Add one integration test that exercises a legacy-style loop path through `n00b_canvas_run` with deterministic backend events to confirm compatibility behavior remains stable.

Throughout implementation, preserve external behavior and update this plan when decisions change. Any intentional baseline output difference must be documented in this plan’s `Decision Log` and justified in artifact notes.

## Concrete Steps

Run all commands from `/home/baron/crash-override/n00b-tui/n00b-athens`.

Create and switch to the Milestone 1 branch from the Milestone 0 branch tip.

    git switch display-rewrite/m0-baseline
    git switch -c display-rewrite/m1-core-contracts

If the branch already exists, switch directly.

    git switch display-rewrite/m1-core-contracts

Implement the internal contract modules and compatibility rewiring.

    mkdir -p include/internal/display
    ${EDITOR:-vi} include/internal/display/diagnostics.h
    ${EDITOR:-vi} include/internal/display/scene_contracts.h
    ${EDITOR:-vi} include/internal/display/event_dispatch.h
    ${EDITOR:-vi} include/internal/display/backend_services.h
    ${EDITOR:-vi} src/display/diagnostics.c
    ${EDITOR:-vi} src/display/scene_contracts.c
    ${EDITOR:-vi} src/display/event_dispatch.c
    ${EDITOR:-vi} src/display/render/backend_services.c
    ${EDITOR:-vi} src/display/event_loop.c
    ${EDITOR:-vi} src/display/render/canvas.c
    ${EDITOR:-vi} src/display/render/backend_ansi.c
    ${EDITOR:-vi} src/display/render/backend_notcurses.c
    ${EDITOR:-vi} src/display/widgets/label.c
    ${EDITOR:-vi} src/tools/display_scene_inspect.c
    ${EDITOR:-vi} meson.build

Add and register Milestone 1 tests.

    ${EDITOR:-vi} test/unit/test_display_diagnostics.c
    ${EDITOR:-vi} test/unit/test_display_scene_contracts.c
    ${EDITOR:-vi} test/unit/test_display_event_dispatch.c
    ${EDITOR:-vi} test/integration/test_display_m1_compat.c
    ${EDITOR:-vi} meson.build

Build required local tools and run milestone-targeted tests.

    if [ ! -d build_debug ]; then N00B_NATIVE=1 ./build.sh; fi
    ninja -C build_debug display_scene_inspect display_baseline_capture
    meson test -C build_debug --print-errorlogs \
      display_diagnostics display_scene_contracts display_event_dispatch \
      display_baseline_contract display_baseline_flow display_m1_compat

Run shared display smoke tests to verify no regression.

    meson test -C build_debug --print-errorlogs \
      render_plane render_canvas render_ansi event_normalize focus mouse \
      label button checkbox input list_widget selectionlist breadcrumb \
      table_build table_layout table_render table_stream hexdump xform_render

Generate Milestone 1 artifacts and compare with Milestone 0 baseline captures.

    mkdir -p plans/artifacts/display-rewrite/m1
    build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m1
    build_debug/display_scene_inspect --out plans/artifacts/display-rewrite/m1/scene_inspect.txt
    meson test -C build_debug --print-errorlogs \
      display_baseline_artifacts display_scene_inspect_artifacts
    diff -u plans/artifacts/display-rewrite/m0/scene_stream.txt plans/artifacts/display-rewrite/m1/scene_stream.txt
    diff -u plans/artifacts/display-rewrite/m0/table_stream.txt plans/artifacts/display-rewrite/m1/table_stream.txt

Verify hardcoded `/tmp` diagnostics are removed from touched display paths.

    rg --line-number '/tmp' \
      src/display/event_loop.c \
      src/display/render/backend_ansi.c \
      src/display/render/backend_notcurses.c \
      src/display/widgets/label.c

Expected success pattern is: milestone tests pass, smoke tests pass, baseline diffs are empty (or documented), and the final `rg` command returns no matches in the touched files.

## Validation and Acceptance

Milestone 1 is accepted only when all three evidence types are present.

The first evidence type is unit tests proving internal contracts work as designed. `display_diagnostics`, `display_scene_contracts`, and `display_event_dispatch` must pass, and Milestone 0 baseline unit tests must still pass.

The second evidence type is integration behavior proving compatibility entry paths. `display_m1_compat` and `display_baseline_flow` must pass, showing that event dispatch and render flow still behave correctly through public APIs.

The third evidence type is human-runnable artifacts. `display_baseline_capture` output in `plans/artifacts/display-rewrite/m1/` must match Milestone 0 baseline captures unless an intentional difference is documented. `display_scene_inspect` must require `--out PATH`, generate a stable, diff-friendly scene report that reviewers can inspect, and pass its parity script against the committed artifact.

In addition, diagnostics policy acceptance requires visible removal of hardcoded `/tmp` writes from touched display files and replacement with centralized diagnostics gating.

## Idempotence and Recovery

All milestone commands are safe to rerun. Test commands are read-only. Artifact generation is overwrite-based and should be deterministic.

If branch creation fails because `display-rewrite/m1-core-contracts` already exists, switch to it and continue. If rebasing later introduces conflicts, resolve on the active branch, rerun milestone tests/artifacts, and update this plan’s `Decision Log` and `Progress` with what changed.

If deterministic artifact diffs are non-empty without intentional behavior changes, treat that as a regression. Fix the offending module, regenerate artifacts, and record the fix and evidence in `Surprises & Discoveries`.

If optional backend-dependent tests are skipped due environment limits, capture that explicitly in this plan with command output and keep stream/ANSI path validation as required acceptance.

## Artifacts and Notes

Milestone 1 artifact directory must include at least:

- `plans/artifacts/display-rewrite/m1/scene_stream.txt`
- `plans/artifacts/display-rewrite/m1/table_stream.txt`
- `plans/artifacts/display-rewrite/m1/metadata.txt`
- `plans/artifacts/display-rewrite/m1/scene_inspect.txt`

Use concise transcripts to prove acceptance, for example:

    $ meson test -C build_debug --print-errorlogs display_diagnostics display_scene_contracts display_event_dispatch display_m1_compat
    4/4 tests OK

    $ build_debug/display_scene_inspect --out plans/artifacts/display-rewrite/m1/scene_inspect.txt
    wrote plans/artifacts/display-rewrite/m1/scene_inspect.txt

    $ diff -u plans/artifacts/display-rewrite/m0/scene_stream.txt plans/artifacts/display-rewrite/m1/scene_stream.txt
    (no output)

    $ meson test -C build_debug --print-errorlogs display_baseline_artifacts display_scene_inspect_artifacts
    2/2 tests OK

    $ rg --line-number '/tmp' src/display/event_loop.c src/display/render/backend_ansi.c src/display/render/backend_notcurses.c src/display/widgets/label.c
    (no output)

If any diff output appears and is intentional, copy only the relevant lines into this section and explain why the change is acceptable.

Actual M1 run transcripts:

    $ meson test -C build_debug --print-errorlogs display_diagnostics display_scene_contracts display_event_dispatch display_baseline_contract display_baseline_flow display_m1_compat
    6/6 tests OK

    $ meson test -C build_debug --print-errorlogs render_plane render_canvas render_ansi event_normalize focus mouse label button checkbox input list_widget selectionlist breadcrumb table_build table_layout table_render table_stream hexdump xform_render
    19/19 tests OK

    $ ninja -C build_debug reconfigure
    Run-time dependency notcurses found: YES 3.0.17

    $ meson test -C build_debug --print-errorlogs notcurses_backend display_diagnostics display_scene_contracts display_event_dispatch display_baseline_contract display_baseline_flow display_m1_compat
    7/7 tests OK

    $ meson test -C build_debug --print-errorlogs notcurses_backend render_plane render_canvas render_ansi event_normalize focus mouse label button checkbox input list_widget selectionlist breadcrumb table_build table_layout table_render table_stream hexdump xform_render
    20/20 tests OK

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

## Interfaces and Dependencies

Implement the following internal interfaces for this milestone.

In `include/internal/display/diagnostics.h`, define:

    typedef enum : uint8_t {
        N00B_DISPLAY_DIAG_OFF = 0,
        N00B_DISPLAY_DIAG_ERROR = 1,
        N00B_DISPLAY_DIAG_INFO = 2,
        N00B_DISPLAY_DIAG_TRACE = 3,
    } n00b_display_diag_level_t;

    void n00b_display_diag_init(void);
    void n00b_display_diag_shutdown(void);
    void n00b_display_diag_set_level(n00b_display_diag_level_t level);
    void n00b_display_diag_set_stream(FILE *stream);
    bool n00b_display_diag_would_log(n00b_display_diag_level_t level);
    void n00b_display_diag_log(n00b_display_diag_level_t level,
                               const char *component,
                               const char *fmt, ...);

In `include/internal/display/scene_contracts.h`, define:

    n00b_array_t(n00b_composite_entry_t)
    n00b_display_scene_build(n00b_canvas_t *canvas);

    void n00b_display_scene_free(n00b_array_t(n00b_composite_entry_t) scene);
    bool n00b_display_scene_any_dirty(n00b_canvas_t *canvas);
    void n00b_display_scene_mark_all_dirty(n00b_canvas_t *canvas);
    void n00b_display_scene_rerender_dirty(n00b_canvas_t *canvas);
    void n00b_display_scene_run_layout(n00b_canvas_t *canvas);

In `include/internal/display/event_dispatch.h`, define:

    typedef struct n00b_display_dispatch_result_t {
        bool handled;
        bool should_stop;
        bool focus_changed;
    } n00b_display_dispatch_result_t;

    n00b_display_dispatch_result_t
    n00b_display_dispatch_event(n00b_canvas_t       *canvas,
                                n00b_focus_mgr_t    *fm,
                                const n00b_event_t  *event);

In `include/internal/display/backend_services.h`, define:

    n00b_render_size_t n00b_display_backend_get_size(n00b_canvas_t *canvas);
    n00b_render_cap_t  n00b_display_backend_caps(n00b_canvas_t *canvas);
    bool n00b_display_backend_poll_event(n00b_canvas_t *canvas,
                                         int32_t timeout_ms,
                                         n00b_event_t *out);
    void n00b_display_backend_set_cursor_visible(n00b_canvas_t *canvas,
                                                 bool visible);

Dependencies for M1 remain the existing project toolchain and optional backend deps already wired in `meson.build` (Cocoa, Notcurses, FreeType). Do not add third-party dependencies in this milestone.

## Revision Notes

- 2026-03-05: Initial Milestone 1 child ExecPlan authored from the umbrella plan and current `display-rewrite/m0-baseline` state, with explicit internal contract boundaries, diagnostics cleanup scope, and acceptance evidence requirements.
- 2026-03-17: Updated the Milestone 1 record after review remediation so the documented workspace root, explicit `display_scene_inspect --out` workflow, parity-test coverage, and refreshed scene artifacts match the repaired branch state.
