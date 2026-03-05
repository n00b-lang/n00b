# Rewrite Terminal Backend Paths Around Shared Input And TTY Contracts (M2)

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

This plan is Milestone 2 of the umbrella plan at `plans/display-rewrite-overall-execplan.md`. There is no repository-local `PLANS.md` in this working tree, so this document follows `/home/baron/.codex/PLANS.md` and must remain compliant with it.

## Purpose / Big Picture

Milestone 2 rewrites terminal execution paths so terminal behavior is implemented through explicit internal contracts instead of being scattered across large backend files. After this milestone, contributors should be able to run one deterministic replay command and verify terminal semantics end to end: resize handling, focus traversal on Tab and Shift+Tab, mouse routing, widget activation, and Ctrl+C shutdown behavior.

User-visible behavior stays parity with Milestone 1. The difference is maintainability and proof: terminal-specific code is split into focused modules, and a non-interactive replay artifact demonstrates behavior without requiring a manual live terminal session every time.

## Progress

- [x] (2026-03-05 11:23Z) Reviewed Milestone 2 scope from `plans/display-rewrite-overall-execplan.md` and mapped it to concrete file touchpoints in the current M1 codebase.
- [x] (2026-03-05 11:23Z) Audited terminal hotspots in `src/display/event_loop.c`, `src/display/render/backend_ansi.c`, `src/display/render/backend_ansi_inline.c`, and `src/display/render/backend_notcurses.c`.
- [x] (2026-03-05 11:23Z) Authored this Milestone 2 child ExecPlan with concrete module boundaries, file-level edits, tests, and artifact commands.
- [ ] Create and switch to `display-rewrite/m2-terminal-backend` from `display-rewrite/m1-core-contracts`.
- [ ] Implement shared terminal modules (`terminal_lifecycle`, `terminal_input`, `ansi_sgr`) and rewire terminal backends/event loop to consume them.
- [ ] Add Milestone 2 unit and integration coverage for ANSI parsing, notcurses translation, and terminal event-loop behavior.
- [ ] Add deterministic replay tooling and produce M2 artifacts under `plans/artifacts/display-rewrite/m2/`.
- [ ] Run milestone tests, display smoke tests, optional notcurses validation, and baseline artifact diffs versus M1.

## Surprises & Discoveries

- Observation: Terminal concerns are still mixed across event loop and backends, so ownership is not yet clear.
  Evidence: `src/display/event_loop.c` still owns raw-mode/signal/alt-screen logic while `src/display/render/backend_ansi.c` and `src/display/render/backend_notcurses.c` both contain backend-specific input translation.
- Observation: `backend_notcurses.c` remains a large mixed-responsibility file, increasing regression risk for terminal changes.
  Evidence: `wc -l src/display/render/backend_notcurses.c` reports 2313 lines with input mapping, pixel rendering, cache management, and lifecycle in one unit.
- Observation: ANSI backend combines rendering, SGR style synthesis, SIGWINCH subscription, and escape parsing in one file.
  Evidence: `src/display/render/backend_ansi.c` (883 lines) includes `ansi_emit_style`, `ansi_check_sigwinch`, `ansi_parse_csi`, and `ansi_poll_event`.
- Observation: Existing ANSI/notcurses tests are mostly lifecycle/no-crash checks and do not deeply assert translation edge cases.
  Evidence: `test/unit/test_render_ansi.c` validates capabilities and render calls; `test/unit/test_notcurses_backend.c` skips many checks when no terminal and does not assert detailed key/mouse mapping tables.
- Observation: M1 already has a deterministic integration pattern that can be extended for M2 terminal replay coverage.
  Evidence: `test/integration/test_display_m1_compat.c` uses a synthetic backend queue and proves `n00b_canvas_run()` behavior deterministically.

## Decision Log

- Decision: Split Milestone 2 around three explicit contracts: terminal lifecycle, terminal input translation, and ANSI SGR emission.
  Rationale: These are the repeated concerns currently duplicated or intertwined across ANSI/notcurses/event-loop code paths.
  Date/Author: 2026-03-05 / Codex
- Decision: Keep all public renderer vtable interfaces and display public headers unchanged in Milestone 2.
  Rationale: M2 is internal modularization and parity proof, not public API redesign.
  Date/Author: 2026-03-05 / Codex
- Decision: Include notcurses input translation in M2 scope, but do not split FreeType pixel rasterization internals yet.
  Rationale: Milestone acceptance is terminal interaction parity and modular input/runtime behavior; pixel rendering decomposition is larger and can be tackled in later milestones without blocking M2 acceptance.
  Date/Author: 2026-03-05 / Codex
- Decision: Require a deterministic replay tool artifact (`display_terminal_replay`) in addition to manual `widget_demo` verification.
  Rationale: Interactive terminal sessions are useful but not diff-friendly; a replay artifact gives stable review evidence.
  Date/Author: 2026-03-05 / Codex
- Decision: Preserve M1 baseline output parity (`scene_stream.txt`, `table_stream.txt`) unless an intentional terminal behavior fix is explicitly documented.
  Rationale: Milestone 2 must prove architecture changes do not silently alter established behavior.
  Date/Author: 2026-03-05 / Codex

## Outcomes & Retrospective

Milestone 2 has not been implemented yet. This plan defines the implementation and validation path needed to reach acceptance on `display-rewrite/m2-terminal-backend`.

Success for this milestone means terminal behavior remains parity with M1 while terminal-specific logic is moved into named internal contracts with dedicated tests and deterministic artifacts. Remaining milestones after M2 continue with GUI parity (M3) and higher-level widget/table/hexdump migration (M4+), so M2 is intentionally scoped to terminal runtime quality and evidence.

## Context and Orientation

The display runtime already has M1 internal contracts in `include/internal/display/` and `src/display/`. Rendering flows through `n00b_canvas_render()` in `src/display/render/canvas.c`, while interactive behavior flows through `n00b_canvas_run()` in `src/display/event_loop.c`. A renderer backend implements `n00b_renderer_vtable_t` from `include/display/render/backend.h`, including `render_planes()`, `flush()`, and optional `poll_event()`.

For Milestone 2, “terminal lifecycle” means entering and restoring terminal state safely (raw mode, alt screen, mouse protocol, signal cleanup) for backends that do not self-manage TTY state. “Terminal input translation” means converting backend-specific key and mouse payloads into canonical `n00b_event_t` values that widgets and focus code consume. “Replay harness” means a non-interactive command that feeds a deterministic event script through the same dispatch/render path and writes diffable output files.

Primary files involved before edits begin are:

- `src/display/event_loop.c`
- `src/display/render/backend_ansi.c`
- `src/display/render/backend_ansi_inline.c`
- `src/display/render/backend_notcurses.c`
- `src/display/render/backend_services.c`
- `include/internal/display/backend_services.h`
- `test/unit/test_render_ansi.c`
- `test/unit/test_notcurses_backend.c`
- `test/integration/test_display_m1_compat.c`
- `meson.build`

Milestone 2 is stacked on Milestone 1, so branch and artifact comparisons must use `display-rewrite/m1-core-contracts` and `plans/artifacts/display-rewrite/m1/` as the parent baseline.

## Plan of Work

First, extract terminal lifecycle management from `src/display/event_loop.c` into a dedicated internal module. Create `include/internal/display/terminal_lifecycle.h` and `src/display/terminal_lifecycle.c`, then move setup/teardown responsibilities there: raw mode handling for non-managing backends, alt-screen enter/leave, mouse protocol toggles, and fatal-signal cleanup hooks. Keep lifecycle semantics identical to M1 and let `n00b_canvas_run()` call the new module instead of owning terminal state directly.

Second, extract ANSI style emission into a shared helper used by both ANSI backends. Create `include/internal/display/ansi_sgr.h` and `src/display/render/ansi_sgr.c`. Move SGR reset/style serialization logic out of both backends and consume it from `backend_ansi.c` and `backend_ansi_inline.c`. The style capability behavior must remain unchanged; this extraction is to remove duplication and make future terminal backends reuse one implementation.

Third, extract terminal input parsing and mapping into a dedicated helper module. Create `include/internal/display/terminal_input.h` and `src/display/render/terminal_input.c`. Move ANSI escape parsing helpers (CSI/SS3/SGR mouse decode) from `backend_ansi.c` into this module and expose a callback-driven parser so backend poll loops can provide byte-read functions without hardcoding parser logic per backend. Also move key/mouse mapping logic used by notcurses into this module through a plain C view struct so notcurses code can map events without duplicating translation rules.

Fourth, rewire backend implementations onto the new contracts while preserving vtable symbols and behavior. In `backend_ansi.c`, keep backend context, buffering, and poll orchestration, but delegate style emission and event parsing. In `backend_ansi_inline.c`, delegate style emission to the same helper. In `backend_notcurses.c`, keep poll/read from notcurses APIs but delegate translation from notcurses event payload to canonical `n00b_event_t`.

Fifth, add deterministic terminal replay evidence and tests. Add `src/tools/display_terminal_replay.c` to run scripted event sequences against a deterministic backend harness and emit text artifacts to `plans/artifacts/display-rewrite/m2/`. Add unit tests for terminal lifecycle policy, ANSI parser behavior, and shared SGR emission, plus one integration test that runs a scripted focus/mouse/key flow through `n00b_canvas_run()` and asserts end-to-end behavior parity.

Throughout implementation, preserve M1 behavior unless a change is intentionally documented. If parity diffs appear, either fix the regression or log the intentional change with explicit rationale in this plan before accepting M2.

## Concrete Steps

Run all commands from `/home/baron/crash-override/n00b-athens`.

Create and switch to the milestone branch:

    git switch display-rewrite/m1-core-contracts
    git switch -c display-rewrite/m2-terminal-backend

Implement the new internal modules and backend rewiring:

    mkdir -p include/internal/display src/display/render
    ${EDITOR:-vi} include/internal/display/terminal_lifecycle.h
    ${EDITOR:-vi} src/display/terminal_lifecycle.c
    ${EDITOR:-vi} include/internal/display/ansi_sgr.h
    ${EDITOR:-vi} src/display/render/ansi_sgr.c
    ${EDITOR:-vi} include/internal/display/terminal_input.h
    ${EDITOR:-vi} src/display/render/terminal_input.c
    ${EDITOR:-vi} src/display/event_loop.c
    ${EDITOR:-vi} src/display/render/backend_ansi.c
    ${EDITOR:-vi} src/display/render/backend_ansi_inline.c
    ${EDITOR:-vi} src/display/render/backend_notcurses.c
    ${EDITOR:-vi} meson.build

Add Milestone 2 tests and deterministic replay tooling:

    ${EDITOR:-vi} test/unit/test_display_terminal_lifecycle.c
    ${EDITOR:-vi} test/unit/test_display_ansi_sgr.c
    ${EDITOR:-vi} test/unit/test_display_terminal_input.c
    ${EDITOR:-vi} test/integration/test_display_m2_terminal_flow.c
    ${EDITOR:-vi} src/tools/display_terminal_replay.c
    ${EDITOR:-vi} meson.build

Build tools and run milestone-targeted tests:

    if [ ! -d build_debug ]; then N00B_NATIVE=1 ./build.sh; fi
    ninja -C build_debug display_terminal_replay display_baseline_capture display_scene_inspect
    meson test -C build_debug --print-errorlogs \
      display_terminal_lifecycle display_ansi_sgr display_terminal_input \
      display_baseline_contract display_event_dispatch
    meson test -C build_debug --print-errorlogs \
      display_baseline_flow display_m1_compat display_m2_terminal_flow

Run shared display smoke tests:

    meson test -C build_debug --print-errorlogs \
      render_plane render_canvas render_ansi event_normalize focus mouse \
      label button checkbox input list_widget selectionlist breadcrumb \
      table_build table_layout table_render table_stream hexdump xform_render

If notcurses is enabled in this build, run backend-specific validation:

    meson test -C build_debug --print-errorlogs notcurses_backend

Generate Milestone 2 artifacts and compare against Milestone 1:

    mkdir -p plans/artifacts/display-rewrite/m2
    build_debug/display_baseline_capture --out-dir plans/artifacts/display-rewrite/m2
    build_debug/display_scene_inspect --out plans/artifacts/display-rewrite/m2/scene_inspect.txt
    build_debug/display_terminal_replay --out-dir plans/artifacts/display-rewrite/m2
    diff -u plans/artifacts/display-rewrite/m1/scene_stream.txt plans/artifacts/display-rewrite/m2/scene_stream.txt
    diff -u plans/artifacts/display-rewrite/m1/table_stream.txt plans/artifacts/display-rewrite/m2/table_stream.txt

Run the manual interactive probe once for human verification:

    build_debug/widget_demo --widget all --backend tui

Expected manual interaction sequence is Tab, Tab, Space, Ctrl+C. Record brief notes about observed focus movement and activation in `plans/artifacts/display-rewrite/m2/metadata.txt`.

## Validation and Acceptance

Milestone 2 is accepted only when all validation layers pass.

The first layer is unit validation of the new terminal contracts. The new tests for lifecycle policy, ANSI SGR serialization, and ANSI/notcurses translation behavior must pass. Existing M1 contract tests that cover baseline and dispatch behavior must remain green.

The second layer is integration validation. `display_m2_terminal_flow` must demonstrate deterministic end-to-end interaction through `n00b_canvas_run()` including resize handling, focus traversal, mouse routing, and Ctrl+C termination semantics. Existing `display_baseline_flow` and `display_m1_compat` tests must continue to pass.

The third layer is artifact validation. `display_terminal_replay` must produce stable replay output under `plans/artifacts/display-rewrite/m2/`. `scene_stream.txt` and `table_stream.txt` from `display_baseline_capture` should match M1 unless an intentional and documented behavior change is approved in this plan.

The fourth layer is human verification. Running `widget_demo --widget all --backend tui` must still behave as expected for focus movement and activation under terminal interaction.

## Idempotence and Recovery

All test and artifact commands in this plan are safe to rerun. Artifact generation is overwrite-based and should be deterministic.

If branch creation fails because `display-rewrite/m2-terminal-backend` already exists, switch to it and continue. If rebase conflicts occur later in the stack, resolve conflicts on the active branch, rerun milestone tests and artifact generation, and update this document’s `Decision Log` and `Progress` with what changed.

If parity artifact diffs are non-empty unexpectedly, treat that as a regression until proven intentional. Fix the relevant module, regenerate artifacts, and capture the regression and fix details in `Surprises & Discoveries`.

If notcurses is unavailable in the build environment, keep M2 moving with ANSI and deterministic replay validation, and document the notcurses gap explicitly in this plan with exact command output.

## Artifacts and Notes

Milestone 2 artifact directory must include at least:

- `plans/artifacts/display-rewrite/m2/scene_stream.txt`
- `plans/artifacts/display-rewrite/m2/table_stream.txt`
- `plans/artifacts/display-rewrite/m2/scene_inspect.txt`
- `plans/artifacts/display-rewrite/m2/terminal_replay.txt`
- `plans/artifacts/display-rewrite/m2/metadata.txt`

Expected validation transcript pattern:

    $ meson test -C build_debug --print-errorlogs display_terminal_lifecycle display_ansi_sgr display_terminal_input display_m2_terminal_flow
    4/4 tests OK

    $ build_debug/display_terminal_replay --out-dir plans/artifacts/display-rewrite/m2
    wrote terminal_replay.txt
    wrote metadata.txt

    $ diff -u plans/artifacts/display-rewrite/m1/scene_stream.txt plans/artifacts/display-rewrite/m2/scene_stream.txt
    (no output)

    $ diff -u plans/artifacts/display-rewrite/m1/table_stream.txt plans/artifacts/display-rewrite/m2/table_stream.txt
    (no output)

Manual probe note template:

    widget_demo_tui_manual=PASS
    sequence=Tab,Tab,Space,Ctrl-C
    observed=focus advanced twice, activation fired, clean exit

If artifact diffs are intentional, copy only relevant diff hunks here and explain why they are acceptable.

## Interfaces and Dependencies

Implement the following internal interfaces in this milestone.

In `include/internal/display/terminal_lifecycle.h`, define:

    extern void n00b_display_terminal_setup(n00b_canvas_t *canvas);
    extern void n00b_display_terminal_teardown(n00b_canvas_t *canvas);

In `include/internal/display/ansi_sgr.h`, define:

    typedef void (*n00b_ansi_emit_fn)(void *ctx, const char *data, size_t len);
    extern void n00b_display_ansi_emit_reset(n00b_ansi_emit_fn emit, void *ctx);
    extern void n00b_display_ansi_emit_style(const n00b_text_style_t *style,
                                             n00b_ansi_emit_fn emit,
                                             void *ctx);

In `include/internal/display/terminal_input.h`, define:

    typedef struct n00b_terminal_input_state_t {
        bool mouse_button_down;
    } n00b_terminal_input_state_t;

    typedef int (*n00b_terminal_read_byte_fn)(void *ctx, int32_t timeout_ms);

    typedef struct n00b_terminal_ncinput_view_t {
        uint32_t id;
        uint32_t evtype;
        int32_t x;
        int32_t y;
        bool shift;
        bool ctrl;
        bool alt;
        uint32_t eff_text0;
    } n00b_terminal_ncinput_view_t;

    extern void n00b_terminal_input_reset(n00b_terminal_input_state_t *state);
    extern bool n00b_terminal_parse_ansi_event(n00b_terminal_input_state_t *state,
                                               n00b_terminal_read_byte_fn read_byte,
                                               void *io_ctx,
                                               int32_t timeout_ms,
                                               n00b_event_t *out);
    extern uint32_t n00b_terminal_map_key(uint32_t raw_key);
    extern bool n00b_terminal_translate_notcurses(const n00b_terminal_ncinput_view_t *in,
                                                  n00b_terminal_input_state_t *state,
                                                  n00b_isize_t cell_px_w,
                                                  n00b_isize_t cell_px_h,
                                                  n00b_event_t *out);

In `src/tools/display_terminal_replay.c`, define:

    static int run_replay(const char *out_dir);
    static int write_replay_log(const char *out_dir, const char *text);
    static int write_metadata(const char *out_dir);
    int main(int argc, char **argv);

Dependencies remain the existing display runtime and optional notcurses dependency gate in Meson. Do not add new external libraries for M2.

## Revision Notes

- 2026-03-05: Initial Milestone 2 child ExecPlan authored from umbrella Milestone 2 scope, with concrete module boundaries, validation commands, and deterministic replay artifact requirements.
