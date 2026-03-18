# Migrate Widgets, Table, And Hexdump Onto Shared Display Primitives (M4)

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

This plan is Milestone 4 of the umbrella plan at `plans/display-rewrite-overall-execplan.md`. There is no repository-local `PLANS.md` in this working tree, so this document follows `/home/baron/.codex/PLANS.md` and must remain compliant with it.

## Purpose / Big Picture

Milestone 4 reduces duplicated behavior in higher-level display code and proves that mixed UI/data compositions stay stable while internals are centralized. After this milestone, a contributor should be able to run one deterministic showcase that includes interactive widgets, table rendering, and hexdump output while the implementation uses shared helper modules instead of repeating the same metric/event/text-fit logic across many files.

The user-visible effect is no regression in existing widget/table/hexdump behavior plus stronger maintainability: equivalent outputs against Milestone 3 baselines for scene/table captures, deterministic hexdump showcase artifacts for review, and one mixed-flow integration test that exercises widgets + table + hexdump in a single canvas lifecycle.

## Progress

- [x] (2026-03-05 13:19Z) Reviewed Milestone 4 scope and acceptance criteria from `plans/display-rewrite-overall-execplan.md`.
- [x] (2026-03-05 13:19Z) Audited widget/table/hexdump duplication hotspots and existing test/tool coverage to ground the migration plan in concrete files.
- [x] (2026-03-05 13:19Z) Authored this Milestone 4 child ExecPlan at `plans/display-rewrite/m4-widget-table-migration-execplan.md`.
- [x] (2026-03-05 13:34Z) Implemented shared helper modules (`widget_primitives`, `table_text_primitives`, `hexdump_contracts`) and migrated widget/table/hexdump call sites without public API churn.
- [x] (2026-03-05 13:36Z) Added Milestone 4 unit/integration tests, added deterministic `display_m4_showcase` artifact tooling, and wired new sources/tests/tools in `meson.build`.
- [x] (2026-03-05 13:41Z) Ran milestone validation matrix, generated M4 artifacts under `plans/artifacts/display-rewrite/m4/`, and confirmed `scene_stream.txt` / `table_stream.txt` parity against M3 (empty diffs).
- [x] (2026-03-18 02:47Z) Applied M4 review remediation: extracted `src/tools/display_m4_showcase_fixture.[ch]`, rewired the tool and integration test to share one deterministic scenario, removed unused internal helper surface, added `display_m4_artifacts` parity coverage, refreshed the tracked M4 artifacts, and reran the focused validation matrix plus `git diff --check`.

## Surprises & Discoveries

- Observation: Widget files still repeat the same low-level event/metric patterns (left-click activation checks, Enter/Space activation checks, and character-pixel-width lookups), which is exactly the M4 centralization target.
  Evidence: `rg` hits across `src/display/widgets/*.c` show repeated `event->type == N00B_EVENT_MOUSE`, `event->mouse.button == N00B_MOUSE_LEFT`, `key == ' ' || key == N00B_KEY_ENTER`, and `n00b_plane_text_width(... "M" ...)` blocks in button/checkbox/switch/radio/list/selectionlist/input/breadcrumb files.
- Observation: Widget public helpers still duplicate plane-vtable type checks in many files.
  Evidence: `rg "widget_vtable != &n00b_widget_" src/display/widgets/*.c` reports repeated guards in `label.c`, `checkbox.c`, `input.c`, `switch.c`, `radio.c`, `list_widget.c`, `selectionlist.c`, `breadcrumb.c`, and `link.c`.
- Observation: Table layout and table render each own overlapping string-fit logic (line splitting, wrap/truncate, width metrics) that should be one shared primitive.
  Evidence: `src/display/table/table_layout.c` duplicates longest-line/word analysis and wrap decisions while `src/display/table/table_render.c` independently repeats wrap/truncate/alignment operations.
- Observation: Hexdump transform rendering currently depends on raw hexdump layout fields and hand-built style ranges instead of a shared line-region helper.
  Evidence: `src/conduit/xform_hexdump.c` builds offset/ascii style ranges from `hd->offset_cols` and `hd->ascii_start` directly during `emit_line`.
- Observation: Existing baseline artifact tooling captures deterministic scene/table outputs but not a dedicated hexdump artifact for milestone parity review.
  Evidence: `src/tools/display_baseline_capture.c` writes `scene_stream.txt`, `table_stream.txt`, and `metadata.txt` only.
- Observation: `ncc` parsing failed for the new table helper until the file explicitly included `core/alloc.h`.
  Evidence: `ninja -C build_debug ...` failed with `unknown type name 'n00b_free'` and `use of undeclared identifier 'n00b_alloc_size'` in `src/display/table/table_text_primitives.c`; adding `#include "core/alloc.h"` fixed the build.
- Observation: The plan’s `test/data/table_sample.txt` input file does not exist in this repository.
  Evidence: `build_debug/n00b_table --style simple test/data/table_sample.txt` returned `No such file or directory`.
- Observation: Interactive smoke checks are environment-dependent in this execution context.
  Evidence: `timeout 5 ./build_debug/widget_demo --widget all --backend tui` exited with code 124 (non-interactive timeout), and `timeout 5 ./build_debug/widget_demo --widget all --backend gui` reported `x11 backend unavailable (cannot open DISPLAY)`.
- Observation: The initial M4 artifact refresh missed parity coverage for `scene_inspect.txt`, so the file drifted until remediation reran the tool and diffed the whole M4 directory.
  Evidence: `git diff -- plans/artifacts/display-rewrite/m4/scene_inspect.txt` changed only the root entry from `dirty=1` to `dirty=0` after rerunning `build_debug/display_scene_inspect --out plans/artifacts/display-rewrite/m4/scene_inspect.txt`.
- Observation: `git diff --no-index --check` returns status `1` for a clean generated file diffed against `/dev/null`, so shell-based whitespace gating must only fail on statuses greater than `1`.
  Evidence: local command probes during remediation returned `rc=1` for a one-line file with no warnings and `rc=3` plus `new blank line at EOF` for a file ending in a blank line.

## Decision Log

- Decision: Introduce one shared widget helper module under `include/internal/display/` + `src/display/widgets/` for metrics/focus/activation/data-guard primitives, then migrate event-heavy widgets first.
  Rationale: This gives immediate duplication reduction while preserving per-widget rendering choices and public headers.
  Date/Author: 2026-03-05 / Codex
- Decision: Introduce one shared table text-fit helper module used by both `table_layout` and `table_render`.
  Rationale: Table currently duplicates wrapping/truncation/width semantics across two modules; one helper keeps behavior synchronized.
  Date/Author: 2026-03-05 / Codex
- Decision: Add a hexdump line-region helper and migrate `xform_hexdump` to consume it rather than open-coding style-span offsets.
  Rationale: This centralizes hexdump layout knowledge and prevents future drift between formatter and transform styling.
  Date/Author: 2026-03-05 / Codex
- Decision: Keep Milestone 4 changes internal/additive and preserve existing public APIs (`include/display/widget.h`, `include/display/table/table.h`, `include/display/hexdump.h`).
  Rationale: M4 objective is migration to shared primitives with stable behavior, not API redesign.
  Date/Author: 2026-03-05 / Codex
- Decision: Add a deterministic `display_m4_showcase` tool for mixed widget/table/hexdump proof instead of relying only on interactive `widget_demo` sessions.
  Rationale: Deterministic text artifacts are diffable and CI-friendly; interactive smoke remains supplementary evidence.
  Date/Author: 2026-03-05 / Codex
- Decision: In `table_text_primitives.c`, use explicit array struct initialization plus `core/alloc.h`-provided allocation helpers to satisfy `ncc` parser/runtime expectations.
  Rationale: This avoided repeated parser/build failures while preserving the helper’s behavior contract.
  Date/Author: 2026-03-05 / Codex
- Decision: Replace the missing `test/data/table_sample.txt` artifact command with deterministic stdin-fed CSV for `n00b_table`.
  Rationale: The original path does not exist in this repo; stdin input keeps artifact generation deterministic and idempotent.
  Date/Author: 2026-03-05 / Codex
- Decision: Keep the Milestone 4 copies of baseline-derived artifacts (`scene_stream.txt`, `table_stream.txt`, `metadata.txt`, `scene_inspect.txt`) and justify them with `display_m4_artifacts` parity automation instead of deleting them.
  Rationale: the milestone already uses those files as acceptance evidence, and adding parity coverage was lower risk than redefining M4 scope after review.
  Date/Author: 2026-03-18 / Codex
- Decision: Extract `src/tools/display_m4_showcase_fixture.[ch]` and let both `display_m4_showcase` and `test_display_m4_widget_table_flow` consume one deterministic scenario summary.
  Rationale: this removed duplicated setup/event logic, removed ad hoc stream-backend declarations, and let the integration test preserve its event-handled assertions without keeping the old copy-pasted scenario.
  Date/Author: 2026-03-18 / Codex
- Decision: Remove `n00b_widget_event_is_primary_activate()` and the unused `hex_start` / `hex_end` hexdump contract fields during remediation instead of keeping them for possible future use.
  Rationale: no production code consumed those helpers on this branch, so keeping them would have preserved speculative internal API surface without behavioral payoff.
  Date/Author: 2026-03-18 / Codex

## Outcomes & Retrospective

Milestone 4 implementation is complete on `display-rewrite/m4-widget-table-migration`. Internal shared primitives now back widget metrics/activation/type-guards, table text fit/alignment, and hexdump line-region styling contracts. Public APIs remained unchanged (`widget.h`, `table.h`, `hexdump.h`).

Validation passed across new primitive tests, migrated regression suites, mixed-flow integration, and shared display smoke checks. Deterministic artifacts were generated under `plans/artifacts/display-rewrite/m4/`, and parity diffs against `plans/artifacts/display-rewrite/m3/scene_stream.txt` and `table_stream.txt` were empty.

Review remediation is also complete. The deterministic showcase scenario now lives in `src/tools/display_m4_showcase_fixture.[ch]`, `display_m4_showcase` trims `showcase_stream.txt` and always reaches `n00b_shutdown()` after init, `display_m4_artifacts` now proves the entire M4 artifact directory is reproducible, and the tracked artifact refresh removed stale blank lines from `scene_stream.txt` / `showcase_stream.txt` while updating `scene_inspect.txt` to current `dirty=0` output. The focused post-remediation matrix passed `22/22`, M3 parity diffs for `scene_stream.txt` and `table_stream.txt` stayed empty, and `git diff --check` returned no output.

Remaining gap is only environment-dependent interactive smoke: TUI demo requires an interactive terminal session and GUI demo requires an available X11 display server. This was recorded with concrete command output and is non-blocking for deterministic acceptance.

## Context and Orientation

Milestones 0-3 are complete on `display-rewrite/m3-gui-backend`. Baseline/parity tooling already exists (`display_baseline_capture`, `display_scene_inspect`, `display_terminal_replay`, `display_gui_parity_report`), and GUI parity plus Linux X11 support are already validated.

For M4, “shared primitives” means internal helper contracts that multiple widget/table/hexdump modules consume. A primitive is not a new public API; it is an internal function boundary that removes repeated logic while preserving behavior.

Current hotspots relevant to M4 are:

- Widget duplication and repeated event/metric code: `src/display/widgets/button.c`, `checkbox.c`, `switch.c`, `radio.c`, `list_widget.c`, `selectionlist.c`, `input.c`, `breadcrumb.c`, plus type-guard repetition across many widget setters/getters.
- Table text-fit duplication: `src/display/table/table_layout.c` and `src/display/table/table_render.c`.
- Hexdump transform coupling: `src/display/hexdump.c` and `src/conduit/xform_hexdump.c`.
- Human/demo surfaces: `src/tools/widget_demo.c`, `src/tools/table.c`.
- Existing display tests likely to catch regressions: `test/unit/test_button.c`, `test_checkbox.c`, `test_switch.c`, `test_radio.c`, `test_list_widget.c`, `test_selectionlist.c`, `test_table_*.c`, `test_hexdump.c`, `test_xform_hexdump.c`, and existing display integration tests.

Milestone 4 is stacked on Milestone 3, so this branch must be based on `display-rewrite/m3-gui-backend` and parity comparisons must use `plans/artifacts/display-rewrite/m3/` as the parent baseline.

## Plan of Work

First, add a shared widget helper module for repeated behavior that is currently copy/pasted across widgets. Create `include/internal/display/widget_primitives.h` and `src/display/widgets/widget_primitives.c` to centralize:

- character-cell pixel width lookup with safe fallback,
- line-height lookup with safe fallback,
- focus/active-state detection,
- primary activation event checks (left mouse press, Enter, Space), and
- widget-vtable/type guard data access helper used by widget setters/getters.

Then migrate the highest-duplication widget files to consume this module: `button.c`, `checkbox.c`, `switch.c`, `radio.c`, `list_widget.c`, `selectionlist.c`, and targeted public helper functions in `label.c`, `input.c`, `breadcrumb.c`, `link.c`, and `progress.c`. Keep rendered output and callback semantics unchanged.

Second, add a shared table text-fit helper module used by both layout and render phases. Create `include/internal/display/table_text_primitives.h` and `src/display/table/table_text_primitives.c` to centralize longest-line/longest-word measurement, wrap-versus-truncate line materialization, and horizontal alignment/padding behavior. Rewire `table_layout.c` and `table_render.c` to consume this helper so one implementation governs cell fitting behavior.

Third, add hexdump line-region contracts. Create `include/internal/display/hexdump_contracts.h` and `src/display/hexdump_contracts.c` with helpers that describe offset/hex/ascii span boundaries for one formatted line. Use this in `src/conduit/xform_hexdump.c` so style-tag range generation is tied to formatter-derived contracts instead of repeated offset math in transform code.

Fourth, add milestone tests and a mixed-flow integration scenario. Add unit tests for widget primitives, table text primitives, and hexdump contracts. Add one integration test that runs a deterministic canvas flow combining interactive widgets with table + hexdump-derived text content, verifies event handling, and verifies render path stability through existing event-loop contracts.

Fifth, add deterministic M4 artifact tooling. Add `src/tools/display_m4_showcase.c` that generates stable artifact files under an output directory:

- mixed widget+table stream output,
- hexdump stream output, and
- metadata describing configuration.

This tool should use stream backend and fixed dimensions/scripts so artifact output is reproducible.

After review remediation, keep the same user-visible artifact scope but change the maintenance path: put the mixed showcase scenario in `src/tools/display_m4_showcase_fixture.[ch]`, let both the tool and the M4 integration test call that fixture, trim trailing blank lines before writing `showcase_stream.txt`, and add one `display_m4_artifacts` shell test that regenerates the full M4 artifact directory and checks it for drift plus whitespace regressions.

Finally, wire all new modules/tests/tools in `meson.build`, run milestone validations, generate M4 artifacts, and compare `display_baseline_capture` scene/table outputs against M3. Any intentional diff must be documented in this plan before milestone acceptance.

## Concrete Steps

Run all commands from `/home/baron/crash-override/n00b-athens`.

Create and switch to the milestone branch:

    git switch display-rewrite/m3-gui-backend
    git switch -c display-rewrite/m4-widget-table-migration

If the branch already exists, switch directly:

    git switch display-rewrite/m4-widget-table-migration

Implement shared helper modules and migrate call sites:

    mkdir -p include/internal/display src/display/widgets src/display/table src/display
    ${EDITOR:-vi} include/internal/display/widget_primitives.h
    ${EDITOR:-vi} src/display/widgets/widget_primitives.c
    ${EDITOR:-vi} include/internal/display/table_text_primitives.h
    ${EDITOR:-vi} src/display/table/table_text_primitives.c
    ${EDITOR:-vi} include/internal/display/hexdump_contracts.h
    ${EDITOR:-vi} src/display/hexdump_contracts.c
    ${EDITOR:-vi} src/display/widgets/button.c
    ${EDITOR:-vi} src/display/widgets/checkbox.c
    ${EDITOR:-vi} src/display/widgets/switch.c
    ${EDITOR:-vi} src/display/widgets/radio.c
    ${EDITOR:-vi} src/display/widgets/list_widget.c
    ${EDITOR:-vi} src/display/widgets/selectionlist.c
    ${EDITOR:-vi} src/display/widgets/label.c
    ${EDITOR:-vi} src/display/widgets/input.c
    ${EDITOR:-vi} src/display/widgets/breadcrumb.c
    ${EDITOR:-vi} src/display/widgets/link.c
    ${EDITOR:-vi} src/display/widgets/progress.c
    ${EDITOR:-vi} src/display/table/table_layout.c
    ${EDITOR:-vi} src/display/table/table_render.c
    ${EDITOR:-vi} src/conduit/xform_hexdump.c
    ${EDITOR:-vi} meson.build

Add Milestone 4 tests and deterministic showcase tool:

    ${EDITOR:-vi} test/unit/test_display_widget_primitives.c
    ${EDITOR:-vi} test/unit/test_display_table_text_primitives.c
    ${EDITOR:-vi} test/unit/test_display_hexdump_contracts.c
    ${EDITOR:-vi} test/integration/test_display_m4_widget_table_flow.c
    ${EDITOR:-vi} src/tools/display_m4_showcase.c
    ${EDITOR:-vi} meson.build

Build required tools/targets:

    if [ ! -d build_debug ]; then N00B_NATIVE=1 ./build.sh; fi
    ninja -C build_debug \
      display_baseline_capture display_scene_inspect display_m4_showcase \
      display_terminal_replay display_gui_parity_report widget_demo n00b_table

Run Milestone 4 targeted unit tests:

    meson test -C build_debug --print-errorlogs \
      display_widget_primitives display_table_text_primitives display_hexdump_contracts

Run existing widget/table/hexdump regression tests:

    meson test -C build_debug --print-errorlogs \
      button checkbox switch radio list_widget selectionlist input breadcrumb link \
      table_build table_layout table_render table_stream hexdump xform_hexdump \
      display_event_dispatch display_baseline_contract

Run integration coverage including prior milestones and the new mixed-flow test:

    meson test -C build_debug --print-errorlogs \
      display_baseline_flow display_m1_compat display_m2_terminal_flow \
      display_m3_backend_parity display_m4_widget_table_flow

Run shared display smoke checks:

    meson test -C build_debug --print-errorlogs \
      render_plane render_canvas render_ansi event_normalize focus mouse \
      label button checkbox input list_widget selectionlist breadcrumb \
      table_build table_layout table_render table_stream hexdump xform_render

Generate Milestone 4 artifacts and compare baseline captures to M3:

    mkdir -p plans/artifacts/display-rewrite/m4
    build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m4
    build_debug/display_scene_inspect --out plans/artifacts/display-rewrite/m4/scene_inspect.txt
    build_debug/display_m4_showcase --out-dir plans/artifacts/display-rewrite/m4
    printf 'Component,State\nwidget,ready\nhexdump,formatted\n' | build_debug/n00b_table --style simple > plans/artifacts/display-rewrite/m4/table_cli.txt
    meson test -C build_debug --print-errorlogs display_m4_artifacts
    diff -u plans/artifacts/display-rewrite/m3/scene_stream.txt plans/artifacts/display-rewrite/m4/scene_stream.txt
    diff -u plans/artifacts/display-rewrite/m3/table_stream.txt plans/artifacts/display-rewrite/m4/table_stream.txt
    git diff --check

Run human interactive smoke where environment supports it:

    ./build_debug/widget_demo --widget all --backend tui
    ./build_debug/widget_demo --widget all --backend gui

Expected success pattern is: new primitive tests pass, existing widget/table/hexdump tests remain green, mixed-flow integration passes, scene/table diffs vs M3 are empty unless documented, and M4 showcase artifacts are generated deterministically.

## Validation and Acceptance

Milestone 4 is accepted only when all validation layers pass.

The first layer is unit evidence for the new shared primitives. `display_widget_primitives`, `display_table_text_primitives`, and `display_hexdump_contracts` must pass and prove helper semantics directly.

The second layer is regression evidence for migrated paths. Existing widget/table/hexdump tests must remain green (`button`, `checkbox`, `switch`, `radio`, `list_widget`, `selectionlist`, `table_*`, `hexdump`, `xform_hexdump`) to show behavior parity during migration.

The third layer is integration evidence. `display_m4_widget_table_flow` must pass and show one deterministic canvas lifecycle that includes widgets plus table/hexdump-derived content while event dispatch and rendering still behave correctly.

The fourth layer is artifact evidence. `display_m4_artifacts` must regenerate `scene_stream.txt`, `table_stream.txt`, `metadata.txt`, `scene_inspect.txt`, `showcase_stream.txt`, `hexdump_stream.txt`, `showcase_metadata.txt`, and `table_cli.txt` into a temporary directory and diff them against `plans/artifacts/display-rewrite/m4/` with no output. `scene_stream.txt` and `table_stream.txt` must still match M3 unless intentional differences are documented in this plan, and `git diff --check` must stay empty after the artifact refresh.

The fifth layer is human-runnable evidence. `widget_demo --widget all --backend tui` must remain interactive; `--backend gui` should work when a GUI backend/display server is available, otherwise the limitation must be explicitly recorded with command output.

## Idempotence and Recovery

All commands in this plan are safe to rerun. Test commands are read-only; artifact generation is overwrite-based and deterministic by design.

If `display-rewrite/m4-widget-table-migration` already exists, switch to it and continue. If stacked rebase conflicts occur later, resolve conflicts on the active branch, rerun all M4 validations, and update this document’s `Progress` and `Decision Log` with what changed.

If baseline diffs vs M3 appear unexpectedly, treat that as a regression until proven intentional. Fix the offending helper/call site, regenerate artifacts, and capture the issue in `Surprises & Discoveries`.

If GUI smoke is unavailable due environment constraints (no display server, optional backend disabled), record exact command output and rely on deterministic integration/artifact evidence for acceptance.

## Artifacts and Notes

Milestone 4 artifact directory must include at least:

- `plans/artifacts/display-rewrite/m4/scene_stream.txt`
- `plans/artifacts/display-rewrite/m4/table_stream.txt`
- `plans/artifacts/display-rewrite/m4/scene_inspect.txt`
- `plans/artifacts/display-rewrite/m4/showcase_stream.txt`
- `plans/artifacts/display-rewrite/m4/hexdump_stream.txt`
- `plans/artifacts/display-rewrite/m4/showcase_metadata.txt`
- `plans/artifacts/display-rewrite/m4/table_cli.txt`

Expected validation transcript pattern:

    $ meson test -C build_debug --print-errorlogs display_widget_primitives display_table_text_primitives display_hexdump_contracts
    3/3 tests OK

    $ meson test -C build_debug --print-errorlogs display_m4_widget_table_flow
    1/1 tests OK

    $ meson test -C build_debug --print-errorlogs display_m4_artifacts
    1/1 tests OK

    $ build_debug/display_m4_showcase --out-dir plans/artifacts/display-rewrite/m4
    wrote showcase_stream.txt
    wrote hexdump_stream.txt
    wrote showcase_metadata.txt

    $ diff -u plans/artifacts/display-rewrite/m3/scene_stream.txt plans/artifacts/display-rewrite/m4/scene_stream.txt
    (no output)

    $ diff -u plans/artifacts/display-rewrite/m3/table_stream.txt plans/artifacts/display-rewrite/m4/table_stream.txt
    (no output)

If any diff is intentional, include only relevant diff hunks here with a short rationale.

Observed interactive-smoke evidence in this environment:

    $ timeout 5 ./build_debug/widget_demo --widget all --backend tui
    (exited with 124 after timeout in non-interactive session)

    $ timeout 5 ./build_debug/widget_demo --widget all --backend gui
    n00b: x11 backend unavailable (cannot open DISPLAY).
    Failed to initialize backend 'x11'.

## Interfaces and Dependencies

Implement the following internal interfaces in this milestone.

In `include/internal/display/widget_primitives.h`, define:

    extern int32_t n00b_widget_cell_px_width(n00b_plane_t *plane);
    extern int32_t n00b_widget_line_px_height(n00b_plane_t *plane);
    extern bool n00b_widget_state_is_focused_or_active(const n00b_plane_t *plane);
    extern bool n00b_widget_event_is_left_press(const n00b_event_t *event);
    extern bool n00b_widget_event_is_keyboard_activate(const n00b_event_t *event);
    extern void *n00b_widget_data_if_kind(n00b_plane_t *plane,
                                          const n00b_widget_vtable_t *expected);

In `include/internal/display/table_text_primitives.h`, define:

    typedef struct n00b_table_text_metrics_t {
        int32_t longest_line;
        int32_t longest_word;
    } n00b_table_text_metrics_t;

    extern n00b_table_text_metrics_t
    n00b_table_text_measure(n00b_string_t *text);

    extern n00b_array_t(n00b_string_t *)
    n00b_table_text_lines_for_width(n00b_string_t *text,
                                    int32_t width,
                                    bool wrap);

    extern n00b_string_t *
    n00b_table_text_align_line(n00b_string_t *line,
                               int32_t width,
                               n00b_alignment_t alignment);

In `include/internal/display/hexdump_contracts.h`, define:

    typedef struct n00b_hexdump_line_regions_t {
        uint32_t offset_start;
        uint32_t offset_end;
        uint32_t ascii_start;
        uint32_t ascii_end;
    } n00b_hexdump_line_regions_t;

    extern void n00b_hexdump_describe_line_regions(const n00b_hexdump_t *hd,
                                                   uint32_t nbytes,
                                                   n00b_hexdump_line_regions_t *out);

In `src/tools/display_m4_showcase_fixture.h`, define:

    typedef struct {
        n00b_string_t *showcase_stream;
        bool enter_handled;
        bool checkbox_click_handled;
        bool status_is_run;
        bool checkbox_checked;
        int button_clicks;
    } n00b_display_m4_showcase_summary_t;

    extern int n00b_display_m4_showcase_run(n00b_display_m4_showcase_summary_t *out);

In `src/tools/display_m4_showcase.c`, define:

    static int write_showcase_stream(const char *out_dir,
                                     const n00b_display_m4_showcase_summary_t *summary);
    static int write_hexdump_stream(const char *out_dir);
    static int write_showcase_metadata(const char *out_dir,
                                       const n00b_display_m4_showcase_summary_t *summary);
    int main(int argc, char **argv);

In `meson.build`, ensure:

- New sources are included in `n00b_display` (`widget_primitives.c`, `table_text_primitives.c`, `hexdump_contracts.c`).
- New unit tests are wired as Meson tests:
  `display_widget_primitives`, `display_table_text_primitives`, `display_hexdump_contracts`.
- New integration test is wired:
  `display_m4_widget_table_flow`.
- New artifact parity integration test is wired:
  `display_m4_artifacts`.
- New local tool target is wired:
  `display_m4_showcase`.
- The shared showcase fixture source is linked into both
  `display_m4_showcase` and `display_m4_widget_table_flow`.

Dependencies remain the existing project toolchain and already-optional backend dependencies (Cocoa, X11, Notcurses, FreeType). Do not add new third-party libraries for M4.

## Revision Notes

- 2026-03-05: Initial Milestone 4 child ExecPlan authored from umbrella Milestone 4 scope, with concrete shared-primitive module boundaries, migration targets, validation commands, and deterministic mixed-content artifact strategy.
- 2026-03-05: Updated after implementation run to mark all progress complete, capture build/test/artifact evidence, record environment/tooling discoveries, and replace the missing `test/data/table_sample.txt` command with deterministic stdin-fed table CLI input.
- 2026-03-18: Updated after M4 review remediation to record the shared showcase fixture, removed unused internal helper surface, added `display_m4_artifacts` parity coverage and whitespace gating, refreshed the tracked M4 artifacts, and capture the post-remediation validation evidence.
