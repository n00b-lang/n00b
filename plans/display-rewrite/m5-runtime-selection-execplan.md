# Make Runtime Backend Selection Deterministic And Registry-First (M5)

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

This plan is Milestone 5 of the umbrella plan at `plans/display-rewrite-overall-execplan.md`. There is no repository-local `PLANS.md` in this working tree, so this document follows `/home/baron/.codex/PLANS.md` and must remain compliant with it.

## Purpose / Big Picture

Milestone 5 closes the runtime host gap left after Milestones 0-4 by making backend selection one coherent path instead of a mix of direct vtable picks, optional registry helpers, and tool-local fallback rules. After this milestone, a contributor should be able to request an explicit backend or `auto`, get predictable fallback behavior when the request cannot be satisfied, and see a concrete selection report artifact that proves what backend was chosen and why.

The user-visible effect is startup predictability. Instead of `widget_demo` containing compile-time `#if` backend routing and the registry/plugin logic living mostly unused, runtime startup will route through one policy that is testable and documented: candidate order, alias handling, dynamic plugin attempt behavior, and fallback behavior are all explicit. This milestone keeps existing rendering behavior intact while removing ambiguity in how runtime backends are chosen.

## Progress

- [x] (2026-03-05 13:50Z) Reviewed Milestone 5 scope and acceptance criteria from `plans/display-rewrite-overall-execplan.md`.
- [x] (2026-03-05 13:50Z) Audited current backend-selection paths in `src/display/render/backend_registry.c`, `src/display/render/canvas.c`, `src/core/init.c`, and `src/tools/widget_demo.c`.
- [x] (2026-03-05 13:50Z) Authored this Milestone 5 child ExecPlan at `plans/display-rewrite/m5-runtime-selection-execplan.md`.
- [x] (2026-03-05 14:02Z) Implemented registry-first backend-selection helpers (`n00b_renderer_candidate_names`, `n00b_renderer_resolve_exact`) plus runtime startup registry wiring in `src/core/init.c`.
- [x] (2026-03-05 14:04Z) Migrated `n00b_canvas_init` to support backend-name policy selection (with direct-vtable compatibility preserved) and migrated `widget_demo` to canonical backend-name startup with selected-backend reporting/default `auto`.
- [x] (2026-03-05 14:07Z) Added M5 unit/integration/plugin-fixture coverage: `test_display_backend_selection`, `test_display_backend_plugin`, `test_display_m5_runtime_selection`, and plugin fixture `test/fixtures/display/renderers/m5_test_renderer.c`.
- [x] (2026-03-05 14:10Z) Added deterministic selection artifact tool (`display_backend_selection_report`), ran M5 validation matrix, generated `plans/artifacts/display-rewrite/m5/` outputs, and confirmed empty scene/table stream diffs versus M4.

## Surprises & Discoveries

- Observation: Backend registry initialization is not currently part of runtime startup.
  Evidence: `rg -n "n00b_renderer_registry_init\(" src include test -S` returns only the declaration/definition and `test_display_backend_registry.c` calls.
- Observation: `widget_demo` bypasses the registry and picks backends with tool-local string parsing plus compile-time `#if` branches.
  Evidence: `src/tools/widget_demo.c` chooses `n00b_renderer_ansi`, `n00b_renderer_cocoa`, `n00b_renderer_x11`, and `n00b_renderer_notcurses` directly via `strcmp` blocks around lines 661-699.
- Observation: Plugin loading exists but is not in the normal runtime startup path.
  Evidence: `src/display/render/backend_registry.c` implements `n00b_renderer_load_by_name`, but no runtime or display startup code calls it.
- Observation: Current canvas initialization requires a direct vtable and cannot select by backend name.
  Evidence: `src/display/render/canvas.c` currently asserts `vtable` in `n00b_canvas_init` and has no `backend_name` path.
- Observation: Existing unit coverage checks registration and aliasing but not startup-policy fallback or plugin-path selection behavior.
  Evidence: `test/unit/test_display_backend_registry.c` asserts built-ins/`gui` alias only.
- Observation: Initial candidate-list implementation silently returned empty lists because helper functions mutated a by-value copy of `n00b_list_t`.
  Evidence: `test_display_backend_selection` first failed at `assert(tui.len == 1)` with observed `tui.len=0`; converting helper signatures to pointer parameters fixed the regression.
- Observation: GUI startup is unavailable in this environment due missing `DISPLAY`, but canonical fallback now still starts `widget_demo` by selecting `ansi`.
  Evidence: `timeout 5 ./build_debug/widget_demo --widget all --backend gui` printed `n00b: x11 backend unavailable (cannot open DISPLAY).` then `Backend request 'gui' selected 'ansi' (fallback)`.

## Decision Log

- Decision: Make runtime selection registry-first and expose explicit helper APIs in `backend_registry` for candidate ordering and exact-name resolution.
  Rationale: This centralizes policy and avoids repeating backend selection logic across tools.
  Date/Author: 2026-03-05 / Codex
- Decision: Keep direct-vtable `n00b_canvas_init(... .vtable = ...)` behavior fully compatible while adding backend-name selection as an additive path.
  Rationale: Existing tests and tools rely on direct vtable construction; M5 should not force a broad API migration.
  Date/Author: 2026-03-05 / Codex
- Decision: Call `n00b_renderer_registry_init()` during `n00b_init()` so backend registration is guaranteed before selection helpers run.
  Rationale: Selection behavior should not depend on whether a tool remembered to call registry init.
  Date/Author: 2026-03-05 / Codex
- Decision: Define one default auto candidate order and make it test-visible through a helper instead of implicit file-local `if` trees.
  Rationale: Deterministic fallback requires deterministic candidate ordering.
  Date/Author: 2026-03-05 / Codex
- Decision: Add plugin-readiness evidence with a test fixture shared library loaded through `N00B_RENDERER_PATH`.
  Rationale: M5 explicitly includes plugin readiness; positive dynamic-load proof is required, not only ENOENT failure handling.
  Date/Author: 2026-03-05 / Codex
- Decision: Set deterministic auto candidate order to `ansi -> gui -> notcurses -> stream -> dumb`.
  Rationale: This preserves interactive terminal-default behavior for `auto` while still providing deterministic fallback coverage to non-interactive backends.
  Date/Author: 2026-03-05 / Codex
- Decision: Keep `display_backend_selection_report` non-interactive by disabling dynamic-load attempts and running report cases with null output topics.
  Rationale: This prevents noisy escape-sequence/plugin-miss stderr output and keeps artifact generation deterministic for CI/local reruns.
  Date/Author: 2026-03-05 / Codex

## Outcomes & Retrospective

Milestone 5 is complete on `display-rewrite/m5-runtime-selection`. Runtime/backend startup now converges on one registry-first policy: backend candidate ordering and exact resolution are explicit APIs, runtime init guarantees registry bootstrapping, canvas supports backend-name selection with deterministic fallback, and `widget_demo` now reports requested vs selected backend through the shared path.

Acceptance evidence is complete across all required layers: `display_backend_registry`, `display_backend_selection`, `display_backend_plugin`, `display_m5_runtime_selection`, prior display integration matrix, and shared display smoke matrix all passed; `plans/artifacts/display-rewrite/m5/` contains `scene_stream.txt`, `table_stream.txt`, `metadata.txt`, `scene_inspect.txt`, `selection_report.txt`, and `selection_metadata.txt`; and M5 scene/table baseline diffs versus M4 are empty. Environment-specific GUI unavailability (`DISPLAY` missing) was handled through deterministic fallback evidence (`gui` request falling back to `ansi`) without blocking milestone acceptance.

## Context and Orientation

Milestones 0-4 are complete on `display-rewrite/m4-widget-table-migration`. Display behavior parity artifacts exist through `plans/artifacts/display-rewrite/m4/`, and M5 should preserve those outputs unless an intentional behavior change is documented.

In this milestone, “backend selection policy” means the rule set that maps a request (`stream`, `ansi`, `gui`, `auto`, or unknown name) to an ordered candidate list, then resolves candidates through registry lookup and optional dynamic loading. “Missing backend fallback” means trying later candidates when the requested backend cannot be resolved or initialized. “Plugin readiness” means `n00b_renderer_load_by_name` is actually exercised through the runtime-selection path and validated by tests.

Primary files for this milestone are:

- `include/display/render/backend_registry.h`
- `src/display/render/backend_registry.c`
- `include/display/render/canvas.h`
- `src/display/render/canvas.c`
- `src/core/init.c`
- `src/tools/widget_demo.c`
- `meson.build`
- `test/unit/test_display_backend_registry.c`
- `test/unit/` (new M5 unit tests)
- `test/integration/` (new M5 integration test)
- `src/tools/` (new M5 artifact/report tool)

Milestone 5 is stacked on Milestone 4. Branch base must be `display-rewrite/m4-widget-table-migration`, and parity comparisons should use `plans/artifacts/display-rewrite/m4/` as parent baseline.

## Plan of Work

First, add canonical selection helpers to the backend registry module. Extend `backend_registry` with one function that returns ordered candidate backend names for a runtime request (including alias normalization, optional env override, and fallback list expansion) and one function that resolves one exact candidate name via in-memory registry plus optional dynamic loading. Keep existing APIs (`n00b_renderer_find`, `n00b_renderer_load_by_name`, `n00b_renderer_list`) intact.

Second, wire registry initialization into runtime startup. In `src/core/init.c`, call `n00b_renderer_registry_init()` during `n00b_init()` after core runtime setup is complete and before tool-level startup code may request display backends.

Third, extend canvas initialization to support backend-name startup policy while preserving direct-vtable behavior. Add optional `backend_name` and backend-selection options to `n00b_canvas_init` kargs. If `.vtable` is provided, keep current behavior. If `.vtable` is null and `.backend_name` is provided (or implied as `auto`), use the new registry candidate helper to iterate candidates, resolve each candidate, and attempt backend initialization until one succeeds or candidates are exhausted. Keep failure observable by leaving `backend_ctx` null and emitting diagnostics through existing display diagnostics hooks.

Fourth, migrate `widget_demo` to use the canonical path. Remove its direct compile-time backend dispatch table and call `n00b_canvas_init` with `.backend_name` (defaulting to `auto` when not explicitly supplied). Keep user-facing CLI options and backend labels, but map aliases through the shared selection policy. Update startup logs and usage/help text so selected backend reporting reflects the backend actually chosen.

Fifth, add test coverage for policy, fallback, startup, and plugin readiness. Add a unit test focused on candidate ordering and exact resolution semantics (including alias and env override cases), extend/refresh registry tests for runtime-init behavior, add a plugin fixture test that loads a test renderer through `N00B_RENDERER_PATH`, and add one integration test that exercises full canvas startup under explicit backend, auto backend, and missing-backend fallback scenarios.

Sixth, add a deterministic M5 artifact tool that runs a small UI composition under a fixed scenario matrix and writes a selection report to `plans/artifacts/display-rewrite/m5/`. The report must include requested backend, selected backend, whether fallback was used, and whether render startup succeeded. Keep runs non-interactive and deterministic.

Finally, wire all new tests/tools in `meson.build`, run M5 validation matrix, generate M5 artifacts, and diff baseline stream captures against M4. Any intentional stream diff must be documented in this plan before acceptance.

## Concrete Steps

Run all commands from `/home/baron/crash-override/n00b-athens`.

Create and switch to the milestone branch:

    git switch display-rewrite/m4-widget-table-migration
    git switch -c display-rewrite/m5-runtime-selection

If the branch already exists, switch directly:

    git switch display-rewrite/m5-runtime-selection

Implement registry/canvas/runtime selection wiring:

    ${EDITOR:-vi} include/display/render/backend_registry.h
    ${EDITOR:-vi} src/display/render/backend_registry.c
    ${EDITOR:-vi} include/display/render/canvas.h
    ${EDITOR:-vi} src/display/render/canvas.c
    ${EDITOR:-vi} src/core/init.c
    ${EDITOR:-vi} src/tools/widget_demo.c
    ${EDITOR:-vi} meson.build

Add M5 tests, plugin fixture, and artifact tool:

    mkdir -p test/fixtures/display/renderers
    ${EDITOR:-vi} test/unit/test_display_backend_selection.c
    ${EDITOR:-vi} test/unit/test_display_backend_plugin.c
    ${EDITOR:-vi} test/fixtures/display/renderers/m5_test_renderer.c
    ${EDITOR:-vi} test/integration/test_display_m5_runtime_selection.c
    ${EDITOR:-vi} src/tools/display_backend_selection_report.c
    ${EDITOR:-vi} meson.build

Build required M5 targets:

    if [ ! -d build_debug ]; then N00B_NATIVE=1 ./build.sh; fi
    ninja -C build_debug \
      test_display_backend_registry test_display_backend_selection test_display_backend_plugin \
      test_display_m5_runtime_selection display_backend_selection_report \
      display_baseline_capture display_scene_inspect widget_demo

Run M5 unit tests:

    meson test -C build_debug --print-errorlogs \
      display_backend_registry display_backend_selection display_backend_plugin

Run M5 integration tests plus previous display integration matrix:

    meson test -C build_debug --print-errorlogs \
      display_baseline_flow display_m1_compat display_m2_terminal_flow \
      display_m3_backend_parity display_m4_widget_table_flow display_m5_runtime_selection

Run shared display smoke checks:

    meson test -C build_debug --print-errorlogs \
      render_plane render_canvas render_ansi event_normalize focus mouse \
      label button checkbox input list_widget selectionlist breadcrumb \
      table_build table_layout table_render table_stream hexdump xform_render

Generate Milestone 5 artifacts and compare baseline captures against M4:

    mkdir -p plans/artifacts/display-rewrite/m5
    build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m5
    build_debug/display_scene_inspect --out plans/artifacts/display-rewrite/m5/scene_inspect.txt
    build_debug/display_backend_selection_report --out-dir plans/artifacts/display-rewrite/m5
    diff -u plans/artifacts/display-rewrite/m4/scene_stream.txt plans/artifacts/display-rewrite/m5/scene_stream.txt
    diff -u plans/artifacts/display-rewrite/m4/table_stream.txt plans/artifacts/display-rewrite/m5/table_stream.txt

Run human-runnable startup smoke checks:

    ./build_debug/widget_demo --widget all --backend auto
    ./build_debug/widget_demo --widget all --backend gui

Expected success pattern is: M5 unit/integration tests pass, startup selection behavior is deterministic and logged in artifact reports, stream parity diffs versus M4 are empty unless documented, and `widget_demo --backend auto` reports the selected backend and starts cleanly.

## Validation and Acceptance

Milestone 5 is accepted only when all evidence layers pass.

The first layer is policy-level unit evidence. `display_backend_selection` and `display_backend_registry` must prove candidate ordering, alias normalization, env override behavior, and exact backend resolution semantics.

The second layer is plugin-readiness evidence. `display_backend_plugin` must successfully load a fixture renderer through `N00B_RENDERER_PATH` and resolve it through the same runtime selection APIs.

The third layer is startup integration evidence. `display_m5_runtime_selection` must exercise explicit backend startup, auto selection startup, and missing-backend fallback startup with deterministic assertions about success/failure behavior.

The fourth layer is regression evidence. Prior display integration and smoke suites must remain green, and M5 `display_baseline_capture` scene/table outputs must match M4 unless differences are intentional and documented.

The fifth layer is human-runnable evidence. `display_backend_selection_report` must generate a readable selection report under `plans/artifacts/display-rewrite/m5/`, and `widget_demo --backend auto` must report which backend was selected and run without manual source edits.

## Idempotence and Recovery

All commands in this plan are safe to rerun. Test commands are read-only. Artifact generation overwrites files in `plans/artifacts/display-rewrite/m5/` and should remain deterministic.

If `display-rewrite/m5-runtime-selection` already exists, switch to it and continue. If stacked rebase conflicts happen later, resolve conflicts on the active branch, rerun all M5 validations/artifact generation, and update this plan’s `Progress` and `Decision Log` with what changed.

If plugin fixture loading fails due platform restrictions, record exact command output in `Surprises & Discoveries`, keep negative-path coverage for dynamic load error handling, and continue with remaining M5 acceptance checks.

If GUI startup is unavailable in the current environment (for example, missing DISPLAY), rely on deterministic selection/integration evidence and record the limitation with command output.

## Artifacts and Notes

Milestone 5 artifact directory must include at least:

- `plans/artifacts/display-rewrite/m5/scene_stream.txt`
- `plans/artifacts/display-rewrite/m5/table_stream.txt`
- `plans/artifacts/display-rewrite/m5/metadata.txt`
- `plans/artifacts/display-rewrite/m5/scene_inspect.txt`
- `plans/artifacts/display-rewrite/m5/selection_report.txt`
- `plans/artifacts/display-rewrite/m5/selection_metadata.txt`

Expected validation transcript pattern:

    $ meson test -C build_debug --print-errorlogs display_backend_registry display_backend_selection display_backend_plugin
    3/3 tests OK

    $ meson test -C build_debug --print-errorlogs display_m5_runtime_selection
    1/1 tests OK

    $ build_debug/display_backend_selection_report --out-dir plans/artifacts/display-rewrite/m5
    wrote selection_report.txt
    wrote selection_metadata.txt

    $ diff -u plans/artifacts/display-rewrite/m4/scene_stream.txt plans/artifacts/display-rewrite/m5/scene_stream.txt
    (no output)

    $ diff -u plans/artifacts/display-rewrite/m4/table_stream.txt plans/artifacts/display-rewrite/m5/table_stream.txt
    (no output)

If any baseline diff is intentional, include only the relevant diff hunks here with a short rationale.

## Interfaces and Dependencies

Implement the following interfaces for this milestone.

In `include/display/render/backend_registry.h`, define:

    extern n00b_list_t(n00b_string_t *)
    n00b_renderer_candidate_names(n00b_string_t *requested) _kargs
    {
        bool allow_fallback     = true;
        bool allow_env_override = true;
    };

    extern n00b_result_t(n00b_renderer_vtable_ptr_t)
    n00b_renderer_resolve_exact(n00b_string_t *name) _kargs
    {
        bool allow_dynamic_load = true;
    };

`n00b_renderer_candidate_names` must normalize aliases (`tui` -> `ansi`, `nc` -> `notcurses`) and apply deterministic candidate ordering for `auto` plus optional fallback expansion.

In `include/display/render/canvas.h`, extend `n00b_canvas_init` kargs with:

    n00b_string_t *backend_name             = nullptr;
    bool backend_allow_fallback             = true;
    bool backend_allow_dynamic_load         = true;
    bool backend_allow_env_override         = true;

In `src/display/render/canvas.c`, implement backend-name startup semantics:

- If `.vtable` is provided, preserve existing direct-vtable behavior.
- If `.vtable` is null, build candidates via `n00b_renderer_candidate_names`, resolve candidates via `n00b_renderer_resolve_exact`, and attempt backend initialization in order until one succeeds.
- On complete failure, leave `backend_ctx` null and emit actionable diagnostics.

In `src/tools/display_backend_selection_report.c`, define:

    typedef struct selection_case_t {
        const char *label;
        const char *requested_backend;
        const char *env_backend;
        bool        allow_fallback;
    } selection_case_t;

    static int run_selection_case(const selection_case_t *spec,
                                  FILE *report,
                                  n00b_conduit_topic_t(n00b_buffer_t *) *output);
    static int write_selection_metadata(const char *out_dir);
    int main(int argc, char **argv);

In `test/fixtures/display/renderers/m5_test_renderer.c`, export:

    extern const n00b_renderer_plugin_t n00b_renderer_plugin;

with ABI version `N00B_RENDERER_ABI_VERSION` and a minimal working vtable used by `display_backend_plugin` tests.

In `meson.build`, ensure:

- New unit tests are wired: `display_backend_selection`, `display_backend_plugin`.
- New integration test is wired: `display_m5_runtime_selection`.
- New local tool target is wired: `display_backend_selection_report`.
- Plugin fixture shared library target is built and discoverable by the plugin unit test.

Dependencies remain existing project toolchain and optional backend dependencies already used by display code (Cocoa, X11, Notcurses, FreeType). Do not add third-party libraries in M5.

## Revision Notes

- 2026-03-05: Initial Milestone 5 child ExecPlan authored from umbrella Milestone 5 scope, with concrete registry-first backend selection policy, runtime startup wiring, plugin-readiness coverage, and deterministic artifact/report requirements.
- 2026-03-05: Updated after full M5 execution to reflect completed implementation, test/artifact evidence, discovered list-mutation bug fix, and environment-specific GUI fallback behavior.
