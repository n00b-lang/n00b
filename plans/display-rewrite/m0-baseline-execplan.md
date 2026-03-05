# Establish Display Rewrite Baseline Harness And Evidence Artifacts (M0)

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

This plan is Milestone 0 of the umbrella plan at `plans/display-rewrite-overall-execplan.md`. There is no repository-local `PLANS.md` in this working tree at the time this plan was authored, so this document follows `/home/baron/.codex/PLANS.md` and must remain compliant with it.

## Purpose / Big Picture

Milestone 0 creates the stable baseline for the in-place display rewrite. After this milestone, contributors will have a deterministic way to prove what current display behavior is before refactoring starts, and they will have a repeatable artifact capture command that humans can run to inspect baseline output. This milestone does not attempt to redesign display internals. It establishes safety rails so later milestones can move fast without guessing whether behavior changed accidentally.

The user-visible result is a new baseline test harness with one new display-focused unit test, one new cross-component integration test, and one non-interactive capture tool that writes baseline evidence files under `plans/artifacts/display-rewrite/m0/`.

## Progress

- [x] (2026-03-05 09:19Z) Confirmed repository state relevant to this milestone: `test/integration/` exists, Meson already has an `integration` suite, and display unit tests are wired in `meson.build`.
- [x] (2026-03-05 09:19Z) Confirmed existing runnable tools for display evidence (`widget_demo`, `n00b_table`) and constraints (interactive `widget_demo` path blocks on keypress in TUI mode).
- [x] (2026-03-05 09:19Z) Authored Milestone 0 child ExecPlan at `plans/display-rewrite/m0-baseline-execplan.md`.
- [x] (2026-03-05 09:27Z) Created and switched to `display-rewrite/m0-baseline`; adjusted branch base to `needs-rebase` after validating this workspace's `main` does not contain the expected display subtree.
- [x] (2026-03-05 09:33Z) Added `test/unit/test_display_baseline_contract.c`, `test/integration/test_display_baseline_flow.c`, and Meson registrations `display_baseline_contract` and `display_baseline_flow`.
- [x] (2026-03-05 09:34Z) Added deterministic baseline capture tool `src/tools/display_baseline_capture.c`, default output convention `plans/artifacts/display-rewrite/m0/`, and Meson tool target `display_baseline_capture`.
- [ ] (2026-03-05 09:36Z) Execute tests and generate milestone artifacts. Attempted, but blocked by existing `ncc`/core rebuild failures in `build_debug` before new test/tool binaries are linked.

## Surprises & Discoveries

- Observation: The repository already has an integration test suite, but it currently targets allocator and core containers, not display behavior.
  Evidence: `test/integration/` contains `test_hash.c`, `test_interval_tree.c`, `test_dict_untyped.c`, `test_arena_alloc.c`, `test_pool_alloc.c`, `test_mmaps.c`, and `test_alloc_metadata.c`; no display integration files exist.
- Observation: `widget_demo` is a weak baseline artifact source in default terminal mode because it blocks on key input and writes debug files to `/tmp`.
  Evidence: `src/tools/widget_demo.c` opens `/tmp/widget_demo.log` and waits for keypress in non-event-loop TUI path.
- Observation: Display behavior can be validated without optional GUI dependencies by using stream/ANSI-inline paths.
  Evidence: current tests such as `test/unit/test_render_canvas.c` rely on stream backend helpers and run as standard unit tests.
- Observation: In this workspace, `main` does not contain the display subsystem expected by the Milestone 0 plan.
  Evidence: switching to `main` removed expected paths (`include/display`, `src/display`) required by planned test/tool work; implementation continued on `display-rewrite/m0-baseline` based on `needs-rebase`.
- Observation: Baseline test execution is currently blocked by unrelated pre-existing `ncc`/core compilation failures before test binaries build.
  Evidence: `meson test -C build_debug --print-errorlogs display_baseline_contract` fails during rebuild with errors in `src/core/hash.c` (`ncc: parse FAILED ... "__m128"`), `src/core/buffer.c` (default argument mismatch), and allocator/mmap macro expansion errors in `src/core/pool.c`, `src/core/arena.c`, and `src/core/mmaps.c`.
- Observation: Because rebuild fails, the baseline capture executable is not produced.
  Evidence: `build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m0` returns `zsh: no such file or directory: build_debug/display_baseline_capture`.

## Decision Log

- Decision: Use a new deterministic capture tool (`display_baseline_capture`) instead of relying on `widget_demo` interaction for baseline artifacts.
  Rationale: Baseline capture must be reproducible in CI and by humans without manual keypress timing or terminal state concerns.
  Date/Author: 2026-03-05 / Codex
- Decision: Keep Milestone 0 strictly additive for display behavior, with no intentional functional changes to rendering/event semantics.
  Rationale: This milestone is a safety baseline, so changing behavior now would make future comparisons unreliable.
  Date/Author: 2026-03-05 / Codex
- Decision: Add one explicit display integration test in the existing `integration` suite rather than creating a parallel test framework.
  Rationale: Reusing existing Meson suite wiring keeps the workflow simple for novice contributors and avoids duplicated infrastructure.
  Date/Author: 2026-03-05 / Codex
- Decision: Keep implementation on `display-rewrite/m0-baseline` rooted at `needs-rebase` for this workspace instead of rebasing the milestone onto `main`.
  Rationale: This checkout's `main` branch does not include display sources required by Milestone 0 deliverables, while `needs-rebase` does.
  Date/Author: 2026-03-05 / Codex
- Decision: Do not patch unrelated `ncc`/core build failures as part of Milestone 0 baseline scope.
  Rationale: The milestone objective is additive baseline harnessing for display; broad toolchain/core fixes are orthogonal and would hide true baseline status.
  Date/Author: 2026-03-05 / Codex

## Outcomes & Retrospective

Milestone 0 implementation is complete for code deliverables but incomplete for validation artifacts due external build blockers.

Implemented:
- `meson.build` wiring for `display_baseline_contract` (unit), `display_baseline_flow` (integration), and `display_baseline_capture` (tool).
- New baseline unit contract test at `test/unit/test_display_baseline_contract.c`.
- New cross-component integration test at `test/integration/test_display_baseline_flow.c`.
- Deterministic artifact capture tool at `src/tools/display_baseline_capture.c` targeting `plans/artifacts/display-rewrite/m0/`.

Not completed:
- Passing Meson evidence for new tests.
- Generated baseline artifact files (`scene_stream.txt`, `table_stream.txt`, `metadata.txt`).

Current risk:
- Later display rewrite milestones can build on the new harness code structure, but acceptance-level baseline evidence cannot be established until the existing `ncc`/core rebuild failures are fixed in this branch lineage.

## Context and Orientation

The display subsystem is under `include/display/` and `src/display/`. It already supports planes, canvas composition, event normalization, focus routing, mouse routing, widgets, table rendering, and hexdump formatting. The rewrite goal is to evolve this subsystem in place toward one runtime that can target terminal and GUI backends. Milestone 0 is the guardrail milestone that records current behavior before architecture changes begin.

In this plan, a baseline means “observable output and test assertions that represent current behavior.” A baseline artifact means a file that a human can open and inspect (for example plain text render output) to understand what the system currently does. A cross-component integration test means one test that exercises multiple display layers together (at minimum canvas composition, one event/focus path, and backend rendering output) rather than a single isolated function.

Key files to orient yourself before editing are `meson.build`, `test/unit/test_render_canvas.c`, `test/unit/test_focus.c`, `test/unit/test_mouse.c`, `test/unit/test_table_render.c`, `test/unit/test_xform_render.c`, `src/tools/table.c`, and `src/tools/widget_demo.c`.

## Plan of Work

Begin by creating the Milestone 0 branch and keeping all changes scoped to baseline harnessing. Edit `meson.build` in two places. First, in the display unit test block near existing render/focus/widget/table registrations, register a new unit executable and test named `display_baseline_contract` that compiles `test/unit/test_display_baseline_contract.c` with `test_common_kwargs`. Second, in the integration test block near existing `hash` and `interval_tree` tests, register a new integration executable and test named `display_baseline_flow` that compiles `test/integration/test_display_baseline_flow.c` with `test_common_kwargs`.

Create `test/unit/test_display_baseline_contract.c` as a pure contract test for baseline invariants. This test should avoid fragile pixel-perfect snapshots and instead assert stable behavior seams that future milestones must preserve unless explicitly changed. Include checks for canvas initialization using stream backend, deterministic plane composition order for simple overlapping planes, key normalization invariants for Tab/Enter/Ctrl-letter paths, focus manager rebuild and traversal behavior on a small plane tree, and table/hexdump construction paths producing non-null renderable outputs. Keep assertions strict but not tied to styling internals likely to be redesigned later.

Create `test/integration/test_display_baseline_flow.c` as one scenario test that runs multiple display layers together. The scenario should create a canvas with stream backend, attach a small widget tree (for example label plus one focusable control), run focus traversal and mouse routing through public APIs, render to stream output, and assert both interaction state changes and rendered content markers. The goal is to prove “event/focus/render path works end-to-end” in one deterministic flow.

Add a human-runnable artifact tool at `src/tools/display_baseline_capture.c`. This tool should run non-interactively and write deterministic baseline files to an output directory provided by `--out-dir` (default `plans/artifacts/display-rewrite/m0`). The tool should generate at least three files: one stream-rendered scene snapshot (widgets), one table-rendered snapshot, and one metadata file that records backend name, dimensions, and tool version string. Avoid timestamps inside snapshot content so files remain diff-friendly.

Update `meson.build` in the tools section to compile `display_baseline_capture` as a local executable (install false). Keep it linked with the same `test_common_kwargs` used by existing tools to avoid environment drift.

After code changes compile, run the new unit and integration tests by name first, then run broader display smoke tests. Finally run `display_baseline_capture` to produce artifacts. Store the resulting files under `plans/artifacts/display-rewrite/m0/` and include brief transcript snippets in this plan’s `Artifacts and Notes` section.

## Concrete Steps

Run commands from `/home/baron/crash-override/n00b-athens`.

Create and enter the milestone branch.

    git switch main
    git switch -c display-rewrite/m0-baseline

Implement files and Meson wiring described above.

    ${EDITOR:-vi} meson.build
    ${EDITOR:-vi} test/unit/test_display_baseline_contract.c
    ${EDITOR:-vi} test/integration/test_display_baseline_flow.c
    ${EDITOR:-vi} src/tools/display_baseline_capture.c

Build if needed. Prefer existing `build_debug` when present.

    if [ ! -d build_debug ]; then N00B_NATIVE=1 ./build.sh; fi

Run milestone-specific unit and integration tests first.

    meson test -C build_debug --print-errorlogs display_baseline_contract
    meson test -C build_debug --print-errorlogs display_baseline_flow

Run display smoke tests to confirm no baseline harness regression.

    meson test -C build_debug --print-errorlogs \
      render_plane render_canvas render_ansi event_normalize focus mouse \
      label button checkbox input list_widget selectionlist breadcrumb \
      table_build table_layout table_render table_stream hexdump xform_render

Generate human-runnable baseline artifacts.

    mkdir -p plans/artifacts/display-rewrite/m0
    build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m0
    ls -la plans/artifacts/display-rewrite/m0

Expected output pattern for a successful run is that both new tests report `OK` in Meson output and the artifact directory contains scene, table, and metadata files.

## Validation and Acceptance

Milestone 0 is accepted only when three evidence types are present.

The first evidence type is unit testing. `display_baseline_contract` must pass and prove baseline invariants across canvas, event normalization, focus traversal, and data-presentation entry points.

The second evidence type is integration testing. `display_baseline_flow` must pass and prove one full cross-component path from interaction APIs to rendered stream output.

The third evidence type is human-runnable artifact output. Running `display_baseline_capture --out-dir plans/artifacts/display-rewrite/m0` must produce deterministic files that a reviewer can inspect and diff in later milestones.

A reviewer should be able to run these commands and observe: tests pass, artifacts exist, and artifact contents are legible text that reflects baseline display behavior. If any baseline behavior is intentionally adjusted during this milestone (unexpected), record it in `Decision Log` and regenerate artifacts.

## Idempotence and Recovery

All milestone commands are safe to rerun. Test commands are read-only. Artifact generation overwrites files in `plans/artifacts/display-rewrite/m0/` and should be deterministic.

If build setup fails because toolchain dependencies are missing, keep this plan updated with exact failure output in `Surprises & Discoveries` and retry after satisfying dependencies. If optional GUI dependencies are unavailable, continue because Milestone 0 baseline relies only on stream/terminal-safe paths.

If an artifact file changes unexpectedly between two runs without code changes, treat that as a deterministic-baseline defect. Fix the capture tool to remove nondeterministic fields (timestamps, pointers, random order), rerun, and document the fix in `Decision Log`.

## Artifacts and Notes

Milestone command evidence for this run:

    $ meson test -C build_debug --print-errorlogs display_baseline_contract
    ninja: Entering directory `/home/baron/crash-override/n00b-athens/build_debug'
    [1/200] Compiling C object libn00b.a.p/src_core_hash.c.o
    ncc: parse FAILED (37009 tokens produced) ... "__m128"
    Could not rebuild /home/baron/crash-override/n00b-athens/build_debug

    $ meson test -C build_debug --print-errorlogs display_baseline_flow
    ninja: Entering directory `/home/baron/crash-override/n00b-athens/build_debug'
    [1/200] Compiling C object libn00b.a.p/src_core_buffer.c.o
    ncc: error: default for 'start' in 'n00b_buffer_find' differs from previous declaration
    Could not rebuild /home/baron/crash-override/n00b-athens/build_debug

    $ meson test -C build_debug --print-errorlogs render_plane render_canvas render_ansi event_normalize focus mouse ...
    ninja: Entering directory `/home/baron/crash-override/n00b-athens/build_debug'
    [1/236] Compiling C object libn00b.a.p/src_core_hash.c.o
    ncc: parse FAILED (37009 tokens produced) ... "__m128"
    Could not rebuild /home/baron/crash-override/n00b-athens/build_debug

    $ build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m0
    zsh: no such file or directory: build_debug/display_baseline_capture

Artifact directory state after attempted run:

    $ ls -la plans/artifacts/display-rewrite/m0
    total 8
    drwxr-xr-x 2 baron baron 4096 Mar  5 01:36 .
    drwxr-xr-x 3 baron baron 4096 Mar  5 01:36 ..

## Interfaces and Dependencies

Implement the following concrete interfaces so this milestone has stable seams for later milestones.

In `test/unit/test_display_baseline_contract.c`, define one `main` and at least these test functions:

    static void test_canvas_and_composition_contract(void);
    static void test_event_normalization_contract(void);
    static void test_focus_contract(void);
    static void test_table_hexdump_contract(void);

In `test/integration/test_display_baseline_flow.c`, define one scenario entry point:

    static void test_display_flow_focus_mouse_render(void);

In `src/tools/display_baseline_capture.c`, define:

    static int write_scene_snapshot(const char *out_dir);
    static int write_table_snapshot(const char *out_dir);
    static int write_metadata(const char *out_dir);
    int main(int argc, char **argv);

Dependencies for this milestone are existing display modules and Meson test wiring. Do not introduce new third-party dependencies. Keep the tool limited to already-linked project libraries so it builds wherever current `widget_demo` and `n00b_table` build.

## Revision Notes

- 2026-03-05: Initial Milestone 0 child ExecPlan created to convert the umbrella rewrite strategy into concrete baseline harness work with explicit test and artifact deliverables.
- 2026-03-05: Implemented M0 code deliverables (`meson.build` wiring, baseline unit/integration tests, deterministic capture tool) and recorded real command evidence showing validation blocked by existing `ncc`/core rebuild failures.
