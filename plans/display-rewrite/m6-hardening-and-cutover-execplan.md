# Harden Display Runtime And Complete Documentation Cutover (M6)

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

This plan is Milestone 6 of the umbrella plan at `plans/display-rewrite-overall-execplan.md`. There is no repository-local `PLANS.md` in this working tree, so this document follows `/home/baron/.codex/PLANS.md` and must remain compliant with it.

## Purpose / Big Picture

Milestone 6 closes the display rewrite by turning the current milestone implementation set (M0-M5) into a release-grade, documented, and reproducible cutover. After this milestone, a contributor should be able to read current docs, start one app definition through terminal and GUI selection paths, and verify behavior through deterministic artifacts without relying on migration-era caveats.

The user-visible effect is operational clarity: no hardcoded `/tmp` debug side effects in display demo paths, explicit canvas lifecycle ownership semantics, documentation that matches current APIs and backend-selection behavior, and one final cutover artifact bundle under `plans/artifacts/display-rewrite/m6/` that demonstrates terminal/GUI runtime readiness.

## Progress

- [x] (2026-03-05 14:23Z) Reviewed Milestone 6 scope and acceptance criteria from `plans/display-rewrite-overall-execplan.md`.
- [x] (2026-03-05 14:23Z) Audited current hardening/doc-cutover touchpoints in `src/tools/widget_demo.c`, `src/display/render/canvas.c`, `include/display/render/canvas.h`, `docs/render.md`, `docs/widgets.md`, and `docs/tui_prototype_intent.md`.
- [x] (2026-03-05 14:23Z) Authored this Milestone 6 child ExecPlan at `plans/display-rewrite/m6-hardening-and-cutover-execplan.md`.
- [x] (2026-03-05 14:38Z) Implemented runtime hardening: added `n00b_canvas_deinit`, made `n00b_canvas_destroy` delegate teardown+free, migrated stack canvas callers in `test/unit/test_focus.c`, and removed default `widget_demo` `/tmp` logging by adding `--debug-log` / `N00B_WIDGET_DEMO_LOG`.
- [x] (2026-03-05 14:38Z) Aligned docs with runtime reality by rewriting `docs/render.md`, `docs/widgets.md`, and `docs/tui_prototype_intent.md` around current backend policy/lifecycle behavior.
- [x] (2026-03-05 14:38Z) Added M6 lifecycle/cutover coverage and tooling: `test/unit/test_display_canvas_lifecycle.c`, `test/integration/test_display_m6_cutover_matrix.c`, `src/tools/display_m6_cutover_report.c`, with `meson.build` wiring.
- [x] (2026-03-05 14:38Z) Ran validation matrix, optional backend checks, generated `plans/artifacts/display-rewrite/m6/` outputs, and confirmed parity diffs versus M5 are empty for scene/table streams.
- [x] (2026-03-18 08:06Z) Applied the M6 review-remediation pass: added proof tests for fallback-metric refresh, terminal hitbox/local-coordinate policy, and undefined notcurses mouse coordinates; repaired the runtime geometry/input paths; extracted shared startup probing into `include/internal/display/startup_probe.h` plus `src/tools/display_startup_probe.c`; removed the unused `notcurses_probe` target; added `display_m6_artifacts`; and refreshed the tracked M6 artifacts/docs.

## Surprises & Discoveries

- Observation: `docs/render.md` still documents legacy canvas entry points such as `n00b_canvas_new(&vtable)` and omits current `n00b_canvas_init(... .backend_name=...)` selection policy.
  Evidence: `docs/render.md` Canvas and backend-registry sections versus `include/display/render/canvas.h` and `include/display/render/backend_registry.h`.
- Observation: `docs/widgets.md` still states only label is implemented, while wave 1+2 widgets are implemented and tested.
  Evidence: `docs/widgets.md` opening status text versus concrete widget sources/tests in `src/display/widgets/*.c` and `test/unit/test_{button,checkbox,input,switch,radio,list_widget,selectionlist,breadcrumb,link}.c`.
- Observation: `docs/tui_prototype_intent.md` describes pre-M5 runtime reality (registry not initialized at startup, direct tool vtable selection, `/tmp` logging in multiple display paths) that no longer matches current code.
  Evidence: `docs/tui_prototype_intent.md` “Prototype Reality” section versus `src/core/init.c` calling `n00b_renderer_registry_init()` and `src/tools/widget_demo.c` using `.backend_name` canvas init.
- Observation: `widget_demo` still opens `/tmp/widget_demo.log` unconditionally, which conflicts with rewrite policy to avoid hardcoded `/tmp` side effects in user-facing display paths.
  Evidence: `src/tools/widget_demo.c` top comment and `fopen("/tmp/widget_demo.log", "w")` in `main`.
- Observation: Canvas lifecycle semantics are ambiguous: header text says “initialize a pre-allocated canvas,” while `n00b_canvas_destroy` always calls `n00b_free(c)`.
  Evidence: `include/display/render/canvas.h` lifecycle docs and `src/display/render/canvas.c::n00b_canvas_destroy`; stack-based usage remains in `test/unit/test_focus.c`.
- Observation: GUI request behavior in this Linux environment falls back from `gui` to `ansi` because X11 cannot open a display; this is now explicitly captured in M6 artifacts.
  Evidence: `build_debug/display_m6_cutover_report --out-dir plans/artifacts/display-rewrite/m6` output includes `n00b: x11 backend unavailable (cannot open DISPLAY).` and `plans/artifacts/display-rewrite/m6/cutover_metadata.txt` records environment flags.
- Observation: Legacy `/tmp/widget_demo.log` existed before M6 runs, but default `widget_demo` execution no longer mutates it; only explicit `--debug-log` creates logs.
  Evidence: `stat -c %Y /tmp/widget_demo.log` unchanged before/after `./build_debug/widget_demo --widget label --backend stream`, while `--debug-log plans/artifacts/display-rewrite/m6/widget_demo_debug.log` creates the requested file.
- Observation: The optional Cocoa command in the concrete steps can fail on non-macOS builds because only `display_cocoa_input` is present in this environment.
  Evidence: `meson test -C build_debug --print-errorlogs display_cocoa_input display_cocoa_bridge_contracts cocoa_backend` reports missing `display_cocoa_bridge_contracts`; running `display_cocoa_input` alone passes.
- Observation: Direct shell regeneration of `cutover_report.txt` is not reproducible in this workspace unless the cutover tool runs under a real terminal description instead of `TERM=dumb`.
  Evidence: the review-remediation parity test only stabilized after `test/integration/test_display_m6_artifacts.sh` invoked `display_m6_cutover_report` with `TERM=xterm-256color`; without that override, direct tool runs reported `notcurses_available=false` while the Meson test harness recorded `notcurses_available=true`.

## Decision Log

- Decision: Milestone 6 will treat `/tmp` hardcoded debug logging in display tooling as a cutover blocker and replace it with explicit opt-in logging.
  Rationale: User-facing display runtime paths must be deterministic and side-effect free by default.
  Date/Author: 2026-03-05 / Codex
- Decision: Milestone 6 will keep direct-vtable canvas initialization for harness/test use, but will document backend-name selection as the canonical user/runtime path.
  Rationale: This preserves deterministic synthetic backend tests while clarifying production startup behavior.
  Date/Author: 2026-03-05 / Codex
- Decision: Milestone 6 will harden canvas lifecycle by separating teardown from deallocation (`deinit` versus `destroy`) and migrate stack-based callers accordingly.
  Rationale: Current API/docs mismatch creates avoidable ownership ambiguity and weakens reliability for novice contributors.
  Date/Author: 2026-03-05 / Codex
- Decision: Final acceptance will require parity-style artifact continuity against M5 baseline captures in addition to new M6 cutover outputs.
  Rationale: The rewrite is complete only if hardening/docs changes do not regress established rendering behavior.
  Date/Author: 2026-03-05 / Codex
- Decision: Fallback detection in user-facing reporting should compare selected backend against the resolved primary candidate vtable name (not raw alias text) to avoid false fallback reports for aliases like `gui`.
  Rationale: Alias names (`gui`) can resolve to concrete backend names (`x11`/`cocoa`); string-only comparisons misclassify successful primary selection.
  Date/Author: 2026-03-05 / Codex
- Decision: Human-runnable final demo commands were executed under timeout in automation to capture startup/fallback evidence without requiring manual Ctrl-C.
  Rationale: `widget_demo --widget all` intentionally enters an interactive event loop and does not self-terminate in non-interactive CI-style runs.
  Date/Author: 2026-03-05 / Codex
- Decision: Keep the M6 remediation geometry helper in `display/render/composite.[ch]` instead of adding a separate terminal-geometry module.
  Rationale: the compositor already computes the authoritative absolute outer/content rects for planes, so exporting one shared rect-enclosing helper there kept mouse hit-testing and cell fallback on the same formula with less new surface area.
  Date/Author: 2026-03-18 / Codex
- Decision: Make `display_m6_artifacts` run the cutover report under `TERM=xterm-256color`.
  Rationale: this workspace defaults direct shells to `TERM=dumb`, which changes notcurses startup availability and makes `cutover_report.txt` drift even when the code is unchanged.
  Date/Author: 2026-03-18 / Codex

## Outcomes & Retrospective

Milestone 6 implementation is complete on `display-rewrite/m6-hardening-and-cutover`.

Delivered outcomes:

- Runtime hardening completed: canvas lifecycle contract now has explicit teardown (`n00b_canvas_deinit`) and heap convenience destroy (`n00b_canvas_destroy`), with stack-based tests migrated.
- `widget_demo` default execution is side-effect free for logs; debug logging is explicit opt-in via CLI/env.
- Review remediation closed the remaining M6 behavior gaps: fallback metrics refresh after cell-size changes, managed-terminal hit-testing/local-coordinate policy, cell-fallback compositor coverage of partially aligned cells, and undefined notcurses mouse coordinates.
- M6 test/tool additions are wired and passing (`display_canvas_lifecycle`, `display_m6_cutover_matrix`, `display_m6_cutover_report`, `display_m6_artifacts`).
- Documentation has been cut over to current runtime/backends.
- Artifact bundle regenerated under `plans/artifacts/display-rewrite/m6/`, including new cutover report/metadata and parity automation that now proves the bundle stays synchronized.
- Parity guardrail passed: `scene_stream.txt` and `table_stream.txt` diffs versus M5 are empty.

Validation summary:

- Focused M6 remediation matrix command: pass (`render_canvas`, `mouse`, `display_terminal_input`, `display_canvas_lifecycle`, `display_m6_cutover_matrix`).
- Artifact parity command: pass (`display_m6_artifacts`).
- Optional backend tests present in this build: `notcurses_backend` and `x11_backend` pass; `display_cocoa_input` remains the only Cocoa-target test available in this Linux environment.
- GUI runtime availability in this environment is limited by missing display server (`x11` init failure), and fallback behavior is captured in M6 artifacts as intended.

## Context and Orientation

Milestones 0-5 are complete on `display-rewrite/m5-runtime-selection`. Existing deterministic evidence for parity and selection behavior lives under `plans/artifacts/display-rewrite/m0` through `m5`. M6 is the final rewrite milestone and must not invalidate that evidence without documented intent.

In this milestone, a “compatibility leftover” means migration-era behavior that was temporarily tolerated to preserve forward progress but is no longer acceptable for final cutover (for example, unconditional debug file side effects, ambiguous ownership semantics, or milestone-era documentation that no longer reflects runtime truth). A “cutover artifact” means deterministic files that prove final dual-runtime readiness and can be re-generated locally/CI without manual interaction.

Primary files and modules for M6 are:

- `include/display/render/canvas.h`
- `src/display/render/canvas.c`
- `src/tools/widget_demo.c`
- `docs/render.md`
- `docs/widgets.md`
- `docs/tui_prototype_intent.md`
- `meson.build`
- `test/unit/` (new M6 unit hardening coverage)
- `test/integration/` (new final cutover integration coverage)
- `src/tools/` (new M6 cutover report tool)

Milestone 6 is stacked on Milestone 5. Branch base must be `display-rewrite/m5-runtime-selection`, and regression comparisons should use `plans/artifacts/display-rewrite/m5/` as parent baseline.

## Plan of Work

### Workstream 1: Runtime Hardening And Compatibility Cleanup

First, remove hardcoded debug-file side effects from display demo startup. In `src/tools/widget_demo.c`, replace unconditional `/tmp/widget_demo.log` creation with explicit opt-in logging (`--debug-log <path>` and/or `N00B_WIDGET_DEMO_LOG`). Default behavior must produce no file writes outside user-requested outputs.

Second, resolve canvas ownership ambiguity by splitting teardown and deallocation responsibilities. In `include/display/render/canvas.h` and `src/display/render/canvas.c`, introduce a non-freeing lifecycle function (`n00b_canvas_deinit`) that releases backend/list resources for stack or embedded canvases. Keep `n00b_canvas_destroy` as heap object convenience (`deinit` + free). Update stack-based test callers (notably `test/unit/test_focus.c`) to use the non-freeing path.

Third, remove remaining tool-local selection-report assumptions that duplicate runtime policy. Keep backend alias normalization and fallback reporting derived from shared selection helpers instead of ad hoc string logic where practical in `widget_demo` and M6 reporting tooling.

### Workstream 2: Documentation Alignment With Implemented Runtime

Update `docs/render.md` so its API narrative matches actual headers and behavior: canvas initialization via `n00b_canvas_init`, registry-first backend candidate/resolve policy, runtime registry init in `n00b_init`, and current backend capability notes.

Update `docs/widgets.md` to reflect implemented widget status and current backend/runtime integration expectations. Keep roadmap content, but correct “current implementation” claims and references so a novice can run real demos/tests without contradictory guidance.

Refresh `docs/tui_prototype_intent.md` from prototype-gap analysis to post-M5/M6 architecture status: which goals are now implemented, what remains optional/environment-dependent (for example GUI display server availability), and how deterministic tools validate parity.

### Workstream 3: Final Cutover Coverage And Artifacts

Add one M6 unit hardening test (canvas lifecycle ownership/deinit safety and no-regression behavior for direct-vtable and backend-name initialization) and one M6 integration cutover matrix test (explicit backend startup, auto selection, GUI request behavior with environment-aware diagnostics).

Add `src/tools/display_m6_cutover_report.c` as a deterministic final-report tool that records requested backend, selected backend, fallback usage, startup result, and environment capability notes into `plans/artifacts/display-rewrite/m6/`.

Wire new tests/tool in `meson.build`, run full M6 matrix, regenerate baseline/inspection/selection/parity artifacts in M6 directory, and diff scene/table baseline captures against M5. Any intentional diff must be documented before acceptance.

## Concrete Steps

Run all commands from `/home/baron/crash-override/n00b-athens`.

Create and switch to the milestone branch:

    git switch display-rewrite/m5-runtime-selection
    git switch -c display-rewrite/m6-hardening-and-cutover

If the branch already exists, switch directly:

    git switch display-rewrite/m6-hardening-and-cutover

Implement runtime hardening and lifecycle/docs alignment edits:

    ${EDITOR:-vi} include/display/render/canvas.h
    ${EDITOR:-vi} src/display/render/canvas.c
    ${EDITOR:-vi} src/tools/widget_demo.c
    ${EDITOR:-vi} docs/render.md
    ${EDITOR:-vi} docs/widgets.md
    ${EDITOR:-vi} docs/tui_prototype_intent.md
    ${EDITOR:-vi} test/unit/test_focus.c
    ${EDITOR:-vi} meson.build

Add M6 tests and final cutover report tool:

    ${EDITOR:-vi} test/unit/test_display_canvas_lifecycle.c
    ${EDITOR:-vi} test/integration/test_display_m6_cutover_matrix.c
    ${EDITOR:-vi} src/tools/display_m6_cutover_report.c
    ${EDITOR:-vi} meson.build

Build required M6 targets:

    if [ ! -d build_debug ]; then N00B_NATIVE=1 ./build.sh; fi
    ninja -C build_debug \
      test_display_canvas_lifecycle test_display_m6_cutover_matrix \
      display_m6_cutover_report display_baseline_capture \
      display_scene_inspect display_backend_selection_report \
      display_gui_parity_report widget_demo

Run M6 hardening unit tests:

    meson test -C build_debug --print-errorlogs \
      display_canvas_lifecycle display_backend_registry display_backend_selection \
      display_backend_plugin display_event_dispatch display_terminal_lifecycle \
      display_ansi_sgr display_terminal_input display_widget_primitives \
      display_table_text_primitives display_hexdump_contracts display_baseline_contract \
      render_plane render_canvas render_ansi event_normalize focus mouse \
      label button checkbox input list_widget selectionlist breadcrumb \
      table_build table_layout table_render table_stream hexdump xform_render

Run integration matrix including all milestone flows plus M6 cutover:

    meson test -C build_debug --print-errorlogs \
      display_baseline_flow display_m1_compat display_m2_terminal_flow \
      display_m3_backend_parity display_m4_widget_table_flow \
      display_m5_runtime_selection display_m6_cutover_matrix

Run optional backend-specific tests when built:

    if meson test -C build_debug --list | rg -q 'notcurses_backend'; then
      meson test -C build_debug --print-errorlogs notcurses_backend
    fi

    if meson test -C build_debug --list | rg -q 'x11_backend'; then
      meson test -C build_debug --print-errorlogs x11_backend
    fi

    if meson test -C build_debug --list | rg -q 'display_cocoa_input'; then
      meson test -C build_debug --print-errorlogs display_cocoa_input display_cocoa_bridge_contracts cocoa_backend
    fi

Generate M6 artifacts and compare baseline captures against M5:

    mkdir -p plans/artifacts/display-rewrite/m6
    build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m6
    build_debug/display_scene_inspect --out plans/artifacts/display-rewrite/m6/scene_inspect.txt
    build_debug/display_backend_selection_report --out-dir plans/artifacts/display-rewrite/m6
    build_debug/display_gui_parity_report --out-dir plans/artifacts/display-rewrite/m6
    build_debug/display_m6_cutover_report --out-dir plans/artifacts/display-rewrite/m6
    diff -u plans/artifacts/display-rewrite/m5/scene_stream.txt plans/artifacts/display-rewrite/m6/scene_stream.txt
    diff -u plans/artifacts/display-rewrite/m5/table_stream.txt plans/artifacts/display-rewrite/m6/table_stream.txt

Run human-runnable final demo sequence:

    ./build_debug/widget_demo --widget all --backend auto
    ./build_debug/widget_demo --widget all --backend gui

Expected success pattern is: M6 unit/integration matrix passes, docs align with real APIs, no default hardcoded `/tmp` logging side effects remain in display demo paths, scene/table diffs versus M5 are empty unless documented, and final cutover report artifacts summarize backend selection/fallback behavior with environment notes.

## Validation and Acceptance

Milestone 6 is accepted only when all validation layers pass.

The first layer is hardening correctness. New lifecycle and runtime cleanup coverage must prove that canvas teardown semantics are explicit and safe, and that default display tool execution no longer performs hardcoded debug-file side effects.

The second layer is compatibility stability. Existing display unit and milestone integration suites must remain green (`display_baseline_flow`, `display_m1_compat`, `display_m2_terminal_flow`, `display_m3_backend_parity`, `display_m4_widget_table_flow`, `display_m5_runtime_selection`) while M6 additions pass.

The third layer is optional-backend transparency. Backend-specific tests (`notcurses_backend`, `x11_backend`, Cocoa tests when present) must pass when built; when unavailable, M6 artifacts must explicitly record environment limitations rather than silently skipping behavior.

The fourth layer is regression parity. `display_baseline_capture` M6 scene/table streams must match M5 baseline streams unless intentional and documented in this plan with evidence.

The fifth layer is final user-facing proof. The demo sequence and M6 cutover report must show one app composition can be launched through terminal and GUI requests using the shared runtime selection model, with clear fallback reporting when GUI infrastructure is unavailable.

## Idempotence and Recovery

All commands in this plan are safe to rerun. Test commands are read-only, and artifact generation overwrites files under `plans/artifacts/display-rewrite/m6/` deterministically.

If `display-rewrite/m6-hardening-and-cutover` already exists, switch to it and continue. If stacked rebase conflicts occur, resolve on the active branch, rerun the full M6 validation/artifact matrix, and update this document’s `Progress` and `Decision Log` with conflict-resolution outcomes.

If GUI backend execution is unavailable (for example missing `DISPLAY` on Linux/Unix or no GUI session on macOS CI), keep deterministic matrix/tests green, record command output and environment limitation in `Surprises & Discoveries`, and ensure cutover report metadata marks GUI availability accurately.

If scene/table parity diffs versus M5 appear unexpectedly, treat them as regressions until proven intentional, fix or document the behavior change, regenerate artifacts, and capture rationale in `Decision Log` plus `Artifacts and Notes`.

## Artifacts and Notes

Milestone 6 artifact directory must include at least:

- `plans/artifacts/display-rewrite/m6/scene_stream.txt`
- `plans/artifacts/display-rewrite/m6/table_stream.txt`
- `plans/artifacts/display-rewrite/m6/metadata.txt`
- `plans/artifacts/display-rewrite/m6/scene_inspect.txt`
- `plans/artifacts/display-rewrite/m6/selection_report.txt`
- `plans/artifacts/display-rewrite/m6/selection_metadata.txt`
- `plans/artifacts/display-rewrite/m6/parity_report.txt`
- `plans/artifacts/display-rewrite/m6/parity_metadata.txt`
- `plans/artifacts/display-rewrite/m6/cutover_report.txt`
- `plans/artifacts/display-rewrite/m6/cutover_metadata.txt`

Expected validation transcript pattern:

    $ meson test -C build_debug --print-errorlogs display_canvas_lifecycle display_m6_cutover_matrix
    2/2 tests OK

    $ build_debug/display_m6_cutover_report --out-dir plans/artifacts/display-rewrite/m6
    wrote cutover_report.txt
    wrote cutover_metadata.txt

    $ diff -u plans/artifacts/display-rewrite/m5/scene_stream.txt plans/artifacts/display-rewrite/m6/scene_stream.txt
    (no output)

    $ diff -u plans/artifacts/display-rewrite/m5/table_stream.txt plans/artifacts/display-rewrite/m6/table_stream.txt
    (no output)

If any parity diff is intentional, include only relevant diff hunks here with a short rationale.

Observed validation evidence for this run:

- `meson test -C build_debug --print-errorlogs ...` unit matrix passed (`31/31`).
- `meson test -C build_debug --print-errorlogs ...` integration matrix passed (`7/7`, including `display_m6_cutover_matrix`).
- Optional backend tests present in this build passed: `notcurses_backend`, `x11_backend`, `display_cocoa_input`.
- `build_debug/display_m6_cutover_report --out-dir plans/artifacts/display-rewrite/m6` wrote `cutover_report.txt` and `cutover_metadata.txt` and logged GUI environment limitation (`x11 backend unavailable (cannot open DISPLAY)`).
- `diff -u` for `scene_stream.txt` and `table_stream.txt` versus M5 produced no output.
- Demo startup proof:
- `timeout 8 ./build_debug/widget_demo --widget all --backend auto` selected `ansi`.
- `timeout 8 ./build_debug/widget_demo --widget all --backend gui` selected `ansi` with fallback after `x11` unavailability.
- Default logging side-effect check:
- `/tmp/widget_demo.log` timestamp unchanged after default `widget_demo --widget label --backend stream`.
- explicit `--debug-log plans/artifacts/display-rewrite/m6/widget_demo_debug.log` produced opt-in log output.

## Interfaces and Dependencies

Implement the following interfaces in Milestone 6.

In `include/display/render/canvas.h`, define:

    extern void n00b_canvas_deinit(n00b_canvas_t *c);

`n00b_canvas_deinit` must release backend/list resources and leave the canvas in a safe reinitializable state without freeing `c` itself.

In `src/display/render/canvas.c`, enforce lifecycle contract:

- `n00b_canvas_deinit` performs teardown only (idempotent for repeated calls).
- `n00b_canvas_destroy` calls `n00b_canvas_deinit` and then frees heap-owned canvas memory.

In `src/tools/display_m6_cutover_report.c`, define:

    typedef struct cutover_case_t {
        const char *label;
        const char *requested_backend;
        bool        allow_fallback;
        bool        allow_env_override;
    } cutover_case_t;

    static int run_cutover_case(const cutover_case_t *spec,
                                FILE                 *report,
                                n00b_conduit_topic_t(n00b_buffer_t *) *output);
    static int write_cutover_metadata(const char *out_dir,
                                      bool        gui_available,
                                      bool        notcurses_available,
                                      bool        x11_built,
                                      bool        cocoa_built);
    int main(int argc, char **argv);

In `src/tools/widget_demo.c`, support explicit opt-in debug logs:

- Add `--debug-log <path>` CLI handling.
- Honor `N00B_WIDGET_DEMO_LOG` when CLI flag is absent.
- Default to no debug log file creation.

In `meson.build`, ensure:

- New unit test is wired: `display_canvas_lifecycle`.
- New integration test is wired: `display_m6_cutover_matrix`.
- New tool target is wired: `display_m6_cutover_report`.

Dependencies remain existing project toolchain and optional display backends (Notcurses, X11, Cocoa, FreeType). Do not add new third-party libraries in M6.

## Revision Notes

- 2026-03-05: Initial Milestone 6 child ExecPlan authored from umbrella Milestone 6 scope, defining runtime hardening, documentation alignment, and final cutover validation/artifact requirements.
- 2026-03-05: M6 execution completed with lifecycle/logging hardening, docs cutover, M6 tests/tool wiring, full validation pass, and regenerated `plans/artifacts/display-rewrite/m6/` evidence.
- 2026-03-18: M6 review remediation executed: proof regressions were added, terminal geometry/input/runtime fixes landed, the cutover tool and integration test now share `display_startup_probe`, `notcurses_probe` was removed, `display_m6_artifacts` was added, and the refreshed M6 artifact bundle now passes parity and whitespace validation.
