# Review Widget Parity And Publish A Prioritized Port Queue From n00b-slop To n00b-athens Display

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

There is no repository-local `PLANS.md` in this working tree at the time this plan was authored. This document therefore follows the global guidance in `/home/baron/.codex/PLANS.md` and must remain compliant with that file for all future revisions.

## Purpose / Big Picture

The immediate goal is to decide what to port next, not to port code in this plan. After this plan is executed, a contributor should have one concrete prioritized widget backlog that compares `n00b-slop` prototypes against `n00b-athens` display widgets and orders the missing widgets by implementation value and migration risk. The user-visible outcome is a published work queue that can be turned directly into milestone branches or issue tickets.

## Progress

- [x] (2026-03-10 01:22Z) Inventoried widget headers and sources in `n00b-slop/include/ctui/widgets`, `n00b-slop/src/ctui/widgets`, `n00b-athens/include/display/widgets`, and `n00b-athens/src/display/widgets`.
- [x] (2026-03-10 01:24Z) Computed parity baseline: 52 prototype widget headers in `n00b-slop`, 14 widget headers in `n00b-athens`, 13 direct overlaps plus one semantic rename (`list` to `list_widget`), leaving 38 missing widgets.
- [x] (2026-03-10 01:26Z) Gathered prioritization signals for missing widgets from prototype source size, notcurses coupling, and test references in `n00b-slop/test/ctui`.
- [x] (2026-03-10 01:28Z) Compared missing set against `n00b-athens/docs/widgets.md` roadmap waves and identified two missing widgets not currently represented there (`select`, `asciify`).
- [x] (2026-03-10 01:31Z) Authored initial prioritized port queue in this ExecPlan.
- [x] (2026-03-10 12:39Z) Executed plan deliverables: published `plans/notes/widget-port-inventory.md`, published `plans/notes/widget-port-priority.md`, and synchronized `docs/widgets.md` to the approved five-wave queue including explicit `select` and `asciify` treatment.

## Surprises & Discoveries

- Observation: The architectural gap is larger than the widget name diff suggests because `n00b-slop` widgets are `ctui_widget_t` objects with direct notcurses plane usage, while `n00b-athens` widgets are plane-attached `n00b_widget_vtable_t` behaviors. Evidence: `n00b-slop/include/ctui/widget.h`, `n00b-athens/include/display/widget.h`.
- Observation: Prototype docs and code diverge for tree internals; `tree.h` still describes an nctree-backed widget while `tree.c` states a custom tree rendering path. Evidence: `n00b-slop/include/ctui/widgets/tree.h`, `n00b-slop/src/ctui/widgets/tree.c`.
- Observation: High-demand candidates (`scroll`, `tabs`, `tree`) are also among the most backend-coupled widgets, so sequencing must balance user impact and migration risk. Evidence: `n00b-slop/src/ctui/widgets/scroll.c`, `n00b-slop/src/ctui/widgets/tabs.c`, `n00b-slop/src/ctui/widgets/tree.c`, and `n00b-slop/test/ctui/test_scroll.c`, `test_tabs.c`, `test_tree_widget.c`.
- Observation: The current Athens roadmap omits two prototype widgets (`select`, `asciify`) that remain in the missing set, so the roadmap and parity inventory are slightly out of sync. Evidence: `n00b-athens/docs/widgets.md` compared with `n00b-slop/include/ctui/widgets/*.h`.

## Decision Log

- Decision: Treat `list` in `n00b-slop` as already represented by `list_widget` in `n00b-athens` for parity accounting. Rationale: Athens has equivalent single-selection list behavior and public API under a renamed module (`include/display/widgets/list_widget.h`). Date/Author: 2026-03-10 / Codex.
- Decision: Prioritize by three factors in this order: dependency-unlock value, prototype confidence (existing tests/usage), and migration complexity. Rationale: This yields a queue that can start shipping quickly without getting blocked by the most coupled widgets first. Date/Author: 2026-03-10 / Codex.
- Decision: Keep backend-heavy widgets (`terminal`, `tree`, `filebrowser`, `image/canvas` family, large editor paths) in later waves even when user value is high. Rationale: These widgets currently rely on notcurses or PTY-specific assumptions that conflict with Athens' multi-backend display direction. Date/Author: 2026-03-10 / Codex.
- Decision: Validation for this plan focuses on parity accounting and queue completeness, not full runtime test execution. Rationale: the user explicitly stated testing is not critical for this planning deliverable. Date/Author: 2026-03-10 / Codex.
- Decision: Publish both inventory and priority artifacts under `plans/notes` before updating roadmap docs. Rationale: Keeping a stable inventory snapshot (`widget-port-inventory.md`) separate from queue policy (`widget-port-priority.md`) makes future parity recalculations and queue edits independent and auditable. Date/Author: 2026-03-10 / Codex.
- Decision: Normalize roadmap waves in `docs/widgets.md` to match the five-wave backlog exactly and add explicit notes for `select` and `asciify`. Rationale: This removes prior roadmap drift and makes one canonical ordering available for implementation branches and issue creation. Date/Author: 2026-03-10 / Codex.

## Outcomes & Retrospective

Execution completed on 2026-03-10. The inventory baseline now lives in `plans/notes/widget-port-inventory.md`, the prioritized backlog now lives in `plans/notes/widget-port-priority.md`, and `docs/widgets.md` now mirrors the same five-wave ordering.

Acceptance checks are satisfied: the published queue lists all 38 missing widgets exactly once (after semantic rename handling for `list`/`list_widget`), and roadmap docs include explicit treatment for `select` and `asciify`. The next step is implementation milestone planning beginning at Wave 1.

## Context and Orientation

`n00b-slop` contains the prototype widget surface in `include/ctui/widgets/*.h` and `src/ctui/widgets/*.c`. In that codebase, a widget is a `ctui_widget_t` object that owns a notcurses plane and class vtable (`include/ctui/widget.h`).

`n00b-athens` contains the rewritten display runtime in `include/display/**` and `src/display/**`. In Athens, a widget is behavior attached to an `n00b_plane_t` through `n00b_widget_vtable_t` (`include/display/widget.h`). This means ports are semantic migrations, not copy/paste file moves.

Current Athens widget coverage is in `docs/widgets.md` and implemented files under `src/display/widgets/`. Implemented today: `label`, `divider`, `spacer`, `button`, `checkbox`, `input`, `progress`, `switch`, `radio`, `link`, `list_widget`, `selectionlist`, `breadcrumb`, and `box`.

For this plan, "prioritized work plan" means one ordered queue of the 38 missing widgets from `n00b-slop` to `n00b-athens`, where each widget appears exactly once and each wave has a migration rationale.

## Plan of Work

Milestone 1 establishes reproducible parity inventory artifacts. Create `plans/notes/widget-port-inventory.md` that captures the full widget sets from both repositories, direct overlap, semantic rename handling (`list` to `list_widget`), and the final missing set.

Milestone 2 assigns each missing widget to one migration wave using explicit criteria: dependency unlock, confidence from prototype tests, and migration complexity from notcurses/PTY coupling. Record concise per-wave rationale and identify intentional deferrals.

Milestone 3 publishes the final backlog in `plans/notes/widget-port-priority.md` and synchronizes `docs/widgets.md` so implementation branches can follow a single queue.

The initial prioritized queue from the current evidence is:

1. Wave 1 (foundation and layout unlock): `stack`, `grid`, `split`, `scroll`, `tabs`, `text`.
2. Wave 2 (form and shell primitives): `header`, `footer`, `statusbar`, `select`, `maskedinput`, `slider`, `calendar`, `timepicker`.
3. Wave 3 (overlay and command interaction): `modal`, `tooltip`, `toast`, `collapsible`, `accordion`, `menu`, `commandpalette`.
4. Wave 4 (data visualization and decorative status): `sparkline`, `chart`, `plot`, `digits`, `gradient`, `spinner`, `log`.
5. Wave 5 (backend-heavy and complex integrations): `tree`, `filebrowser`, `editor`, `terminal`, `image`, `canvas`, `asciiart`, `asciify`, `fontify`, `colorpicker`.

## Concrete Steps

Run all commands from `/home/baron/crash-override/n00b-tui` unless noted.

1. Build the raw parity baseline.

    slop=$(ls n00b-slop/include/ctui/widgets/*.h | xargs -n1 basename | sed 's/\.h$//' | sort)
    athens=$(ls n00b-athens/include/display/widgets/*.h | xargs -n1 basename | sed 's/\.h$//' | sort)
    comm -12 <(printf '%s\n' "$slop") <(printf '%s\n' "$athens")
    comm -23 <(printf '%s\n' "$slop") <(printf '%s\n' "$athens")

    Expected signal: overlap includes the 13 directly ported names and the second command yields the 38 missing prototype widgets.

2. Capture migration-risk indicators for each missing widget.

    bash -lc 'for f in n00b-slop/src/ctui/widgets/*.c; do
      w=$(basename "$f" .c)
      [ -f "n00b-athens/include/display/widgets/$w.h" ] && continue
      [ "$w" = "list" ] && continue
      loc=$(wc -l < "$f")
      nc=$( (rg -n "\b(struct nc|ncplane_|nctree|nctabbed|notcurses|ncinput|nccell_)" "$f" || true) | wc -l )
      tests=$( (rg -n "ctui_${w}|test_${w}|${w}_widget" n00b-slop/test/ctui || true) | wc -l )
      printf "%s|loc=%s|nc_refs=%s|test_refs=%s\n" "$w" "$loc" "$nc" "$tests"
    done | sort'

    Expected signal: widgets such as `scroll`, `tabs`, `tree`, `terminal`, and `editor` show higher coupling or size than basic containers.

3. Write `n00b-athens/plans/notes/widget-port-priority.md` with the five waves above and one short rationale paragraph per wave.

4. Update `n00b-athens/docs/widgets.md` roadmap waves to match the approved queue, including `select` and `asciify` disposition.

5. Record completion updates in this ExecPlan `Progress`, `Decision Log`, and `Outcomes & Retrospective` sections with timestamps.

## Validation and Acceptance

Acceptance for this plan is documentation-based and does not require a full test run.

A completed execution must satisfy all of the following:

- `n00b-athens/plans/notes/widget-port-priority.md` exists and lists every missing widget exactly once.
- The listed widget total is 38 when counting missing prototypes after treating `list` as already represented by `list_widget`.
- `n00b-athens/docs/widgets.md` reflects the same wave ordering and contains explicit treatment of `select` and `asciify`.
- The queue rationale cites both sides of the comparison (`n00b-slop` prototype files and `n00b-athens` display/widget files).

## Idempotence and Recovery

All commands in this plan are read-only until artifact-writing steps. Re-running the inventory commands is safe and should produce stable sorted outputs.

If counts drift during execution, regenerate the baseline commands in `Concrete Steps` and reconcile before updating roadmap docs. If queue edits become inconsistent, restore `plans/notes/widget-port-priority.md` and `docs/widgets.md` to the last coherent version and rerun Milestone 2 from the saved inventory.

## Artifacts and Notes

Use these concise outputs as parity checkpoints while executing the plan.

    Overlap:
    box
    breadcrumb
    button
    checkbox
    divider
    input
    label
    link
    progress
    radio
    selectionlist
    spacer
    switch

    Missing-from-Athens signal includes:
    accordion
    calendar
    grid
    scroll
    split
    tabs
    tree
    terminal
    ... (38 total)

Key reference files for this effort:

- `n00b-slop/include/ctui/widget.h`
- `n00b-slop/include/ctui/widgets/*.h`
- `n00b-slop/src/ctui/widgets/*.c`
- `n00b-slop/test/ctui/*.c`
- `n00b-athens/include/display/widget.h`
- `n00b-athens/include/display/widgets/*.h`
- `n00b-athens/src/display/widgets/*.c`
- `n00b-athens/docs/widgets.md`

## Interfaces and Dependencies

The comparison hinges on two different widget interfaces.

In prototypes, widgets are concrete `ctui_widget_t` objects with class methods and direct notcurses plane ownership (`include/ctui/widget.h`). Many widgets call notcurses APIs directly and some integrate OS/PTY behavior (`terminal`, `tree`, `tabs`, `scroll`).

In Athens, widgets attach behavior to planes through `n00b_widget_vtable_t` (`include/display/widget.h`) and are expected to run across runtime-selected backends (`include/display/render/backend.h`, `src/display/render/backend_registry.c`).

Each future port should therefore end with both of these interface-level outcomes:

- A public header in `include/display/widgets/<name>.h` exposing a constructor and runtime mutators in the Athens style (`n00b_<name>_new` and related accessors).
- A source module in `src/display/widgets/<name>.c` that renders via plane draw commands, handles events via `n00b_event_t`, and avoids backend-specific assumptions in widget logic.

## Revision Notes

- 2026-03-10: Initial ExecPlan created to compare prototype widgets in `n00b-slop` against reimplemented widgets in `n00b-athens/display` and publish a prioritized migration queue.
- 2026-03-10: Plan executed; inventory and priority artifacts published in `plans/notes/` and `docs/widgets.md` synchronized to the approved five-wave queue.
