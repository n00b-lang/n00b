# Produce Wave 1 Widget Design Breakdowns For Port Planning

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

There is no repository-local `PLANS.md` in this working tree at the time this plan was authored. This document follows `/home/baron/.codex/PLANS.md` and must remain compliant with that file for all future revisions.

This plan explicitly builds on `plans/widget-port-priority-execplan.md` and `plans/notes/widget-port-priority.md`. Those files define the prioritized queue and establish that Wave 1 contains `stack`, `grid`, `split`, `scroll`, `tabs`, and `text`.

## Purpose / Big Picture

The goal is to produce one design breakdown artifact that is immediately usable as input to later implementation ExecPlans for Wave 1 ports from `n00b-slop` to `n00b-athens`. After this plan is executed, a contributor should be able to open one file and, for each Wave 1 widget, see a complete port design brief: prototype behavior, Athens-native API proposal, state and layout contracts, event/focus behavior, backend constraints, and test strategy. The user-visible result is a publishable design dossier at `plans/notes/widget-wave1-design-breakdown.md` that removes ambiguity before code implementation starts.

## Progress

- [x] (2026-03-10 13:57Z) Reviewed Wave 1 scope from `plans/notes/widget-port-priority.md` and extracted architecture context from `plans/widget-port-priority-execplan.md`, `n00b-slop/src/ctui/widgets/{stack,grid,split,scroll,tabs,text}.c`, and Athens widget runtime files.
- [x] (2026-03-11 01:15Z) Created a per-widget evidence worksheet from prototype headers/sources/tests plus Athens runtime constraints (`widget.h`, `widget.c`, mouse/focus/dispatch, box/list patterns, backend capability contracts).
- [x] (2026-03-11 01:20Z) Authored `plans/notes/widget-wave1-design-breakdown.md` with six Wave 1 widget sections and the required nine-subsection schema per section.
- [x] (2026-03-11 01:22Z) Ran structure/completeness validation checks (all subsection counters == 6), confirmed prototype+Athens evidence references per section, and updated this ExecPlan living sections.

## Surprises & Discoveries

- Observation: `stack` naming collides with the existing ADT macro `n00b_stack_new` in `include/adt/stack.h`, so a direct widget constructor named `n00b_stack_new` would be ambiguous at preprocessing time.
  Evidence: `include/adt/stack.h` defines `#define n00b_stack_new(T, ...) ...`.
- Observation: Prototype `scroll` depends on notcurses plane hiding and cell-copy clipping (`hide_content_planes`, `copy_visible_content`) rather than a backend-neutral viewport model.
  Evidence: `n00b-slop/src/ctui/widgets/scroll.c`.
- Observation: Prototype `tabs` stores selected index in a field named `nctabbed` and manually destroys/recreates child planes during tab swaps, which is a migration risk when mapped onto Athens plane ownership.
  Evidence: `n00b-slop/src/ctui/widgets/tabs.c`.
- Observation: Prototype `grid` computes preferred row heights from content but then uses equal-height rows during runtime layout, so port design had to pick one canonical behavior.
  Evidence: `n00b-slop/src/ctui/widgets/grid.c` (`grid_preferred_size` vs `grid_layout`).
- Observation: Prototype `text` copy shortcut treats `ALT` and `CTRL` as copy modifiers, which risks unintended copy in terminal workflows.
  Evidence: `n00b-slop/src/ctui/widgets/text.c` (`text_handle_event`).

## Decision Log

- Decision: This plan produces one consolidated design artifact (`plans/notes/widget-wave1-design-breakdown.md`) rather than six separate files.
  Rationale: Downstream ExecPlan authors need one canonical source that keeps cross-widget dependencies visible (for example `tabs` consuming `stack`/`scroll`/`text` behaviors).
  Date/Author: 2026-03-10 / Codex.
- Decision: Each widget section must follow one fixed schema (behavior snapshot, API proposal, state, layout, rendering, events/focus, backend notes, tests, and open questions).
  Rationale: A consistent schema ensures parity across widgets and allows direct conversion into implementation milestones without re-research.
  Date/Author: 2026-03-10 / Codex.
- Decision: The Wave 1 stack widget design will evaluate a non-conflicting public name (`stack_widget` or `zstack`) instead of assuming `stack`.
  Rationale: Athens already uses lowercase `n00b_stack_new` as a macro for the generic ADT stack container.
  Date/Author: 2026-03-10 / Codex.
- Decision: Use `zstack` as the final public Wave 1 name (`zstack.h`, `n00b_zstack_t`, `n00b_zstack_new`, `n00b_widget_zstack`).
  Rationale: It resolves the macro collision cleanly while preserving the layering intent in the API name.
  Date/Author: 2026-03-11 / Codex.
- Decision: Tabs design keeps non-selected tab panes parented and hidden, rather than detaching and destroying planes on each switch.
  Rationale: This removes prototype instability risk and aligns with Athens plane ownership and focus rebuild behavior.
  Date/Author: 2026-03-11 / Codex.
- Decision: Scroll design explicitly forbids cell-copy viewport emulation and relies on compositor clipping with offset child layout.
  Rationale: Backend-neutral behavior is required for Athens multi-backend support and avoids notcurses-only assumptions.
  Date/Author: 2026-03-11 / Codex.
- Decision: Text design standardizes keyboard copy to `Ctrl+C` in Wave 1.
  Rationale: This prevents accidental clipboard writes from `Alt+C`-style key chords while still meeting expected terminal behavior.
  Date/Author: 2026-03-11 / Codex.

## Outcomes & Retrospective

Execution completed on 2026-03-11. The Wave 1 design dossier now exists at `plans/notes/widget-wave1-design-breakdown.md` and includes implementation-ready sections for `stack`, `grid`, `split`, `scroll`, `tabs`, and `text` with fixed API proposals, state/layout/render/event contracts, portability guidance, and test plans.

Acceptance criteria were met during validation: six widget sections exist exactly once; each required subsection appears exactly six times; every widget section references both prototype and Athens evidence; each section includes concrete Athens API names and at least one test scenario; and stack naming collision resolution is explicitly documented (`zstack`).

Remaining gap after this plan: no runtime code was ported yet; follow-on implementation ExecPlans should consume this artifact directly per widget.

## Context and Orientation

Two codebases are involved. `n00b-slop` contains prototype widgets based on `ctui_widget_t` and notcurses planes. `n00b-athens` contains the current target runtime where widgets are plane-attached behaviors defined by `n00b_widget_vtable_t` in `include/display/widget.h`.

For this plan, a "design breakdown" means a per-widget document section that answers all implementation-shaping questions in advance: what behavior to preserve, what behavior to intentionally change, what public API to expose in Athens, how layout and rendering must work in pixels, how events and focus should route, how backend coupling is removed, and how to test the result.

The target widgets are fixed by Wave 1 from `plans/notes/widget-port-priority.md`:

1. `stack`
2. `grid`
3. `split`
4. `scroll`
5. `tabs`
6. `text`

Primary prototype evidence is in:

- `../n00b-slop/include/ctui/widgets/{stack,grid,split,scroll,tabs,text}.h`
- `../n00b-slop/src/ctui/widgets/{stack,grid,split,scroll,tabs,text}.c`
- `../n00b-slop/test/ctui/test_{split,scroll,tabs,tabs_switch,theme_scroll}.c`

Primary Athens constraints and conventions are in:

- `include/display/widget.h` and `src/display/widget.c`
- `include/display/render/plane.h`
- `src/display/mouse.c`, `src/display/event_dispatch.c`, `src/display/focus.c`
- Existing widget style references such as `include/display/widgets/box.h`, `src/display/widgets/box.c`, `include/display/widgets/list_widget.h`, and `src/display/widgets/list_widget.c`

## Plan of Work

Milestone 1 establishes a reliable evidence worksheet for each Wave 1 widget. For every widget, gather prototype behavior and migration-relevant details from headers, source, and tests, then pair them with Athens runtime constraints (vtable contracts, pixel layout, focus/event routing, plane ownership, and backend portability). The result of this milestone is not final prose; it is a complete set of extracted facts that prevent guesswork in the final design doc.

Milestone 2 writes the design dossier at `plans/notes/widget-wave1-design-breakdown.md`. Each widget section must include the same mandatory subsection set so the document can be machine-checked and human-reviewed for completeness. The design writing must explicitly call out what ports directly from prototype behavior, what changes for Athens architecture, and what remains intentionally deferred.

Milestone 3 validates handoff quality. Confirm that every Wave 1 widget has a complete section, that naming and interface choices are explicit (especially stack naming), and that each section contains enough test and demo guidance for a future implementation ExecPlan to start without fresh research.

## Concrete Steps

Run commands from `/home/baron/crash-override/n00b-tui/n00b-athens` unless noted.

1. Confirm Wave 1 scope and predecessor context.

       sed -n '1,220p' plans/notes/widget-port-priority.md
       sed -n '1,260p' plans/widget-port-priority-execplan.md

2. Extract prototype behavior evidence per widget from `n00b-slop` (run from `/home/baron/crash-override/n00b-tui/n00b-athens`).

       for w in stack grid split scroll tabs text; do
         echo "===== ${w}.h ====="
         sed -n '1,220p' "../n00b-slop/include/ctui/widgets/${w}.h"
         echo "===== ${w}.c key functions ====="
         rg -n "ctui_${w}|${w}_layout|${w}_draw|${w}_handle_event|${w}_preferred_size|${w}_class" "../n00b-slop/src/ctui/widgets/${w}.c"
       done

       for t in test_split.c test_scroll.c test_tabs.c test_tabs_switch.c test_theme_scroll.c; do
         echo "===== ${t} ====="
         sed -n '1,220p' "../n00b-slop/test/ctui/${t}"
       done

3. Extract Athens constraints and patterns that the design must follow.

       sed -n '1,260p' include/display/widget.h
       sed -n '1,260p' src/display/widget.c
       sed -n '1,260p' src/display/mouse.c
       sed -n '1,220p' src/display/event_dispatch.c
       sed -n '1,220p' src/display/focus.c
       sed -n '1,260p' include/display/widgets/box.h
       sed -n '1,320p' src/display/widgets/box.c
       sed -n '1,260p' include/display/widgets/list_widget.h
       sed -n '1,340p' src/display/widgets/list_widget.c

4. Create `plans/notes/widget-wave1-design-breakdown.md` with one top-level section per widget and the exact subsection contract below for each widget:

   `### Prototype Behavior Snapshot`
   `### Athens API Proposal`
   `### State Model`
   `### Layout Contract`
   `### Rendering Contract`
   `### Event And Focus Contract`
   `### Backend And Portability Notes`
   `### Test And Demo Plan`
   `### Open Questions`

5. In each `Athens API Proposal` subsection, define at minimum:
   constructor name, expected data struct name, required mutator/accessor functions, callback signatures (if any), and ownership/lifetime expectations.

6. Add one short dependency paragraph in each widget section describing sequencing inside Wave 1, for example:
   `tabs` depends on container/focus behavior from `stack` and scrolling behavior from `scroll`; `text` design must be compatible with `scroll` viewport assumptions.

7. Update this ExecPlan `Progress`, `Decision Log`, and `Outcomes & Retrospective` sections with timestamps after the artifact is complete.

## Validation and Acceptance

A completed execution is accepted when all of the following are true:

1. `plans/notes/widget-wave1-design-breakdown.md` exists.
2. The file contains exactly six Wave 1 top-level widget sections (`stack`, `grid`, `split`, `scroll`, `tabs`, `text`), each present exactly once.
3. Each widget section contains all nine required subsections listed in `Concrete Steps`.
4. Every widget section includes explicit Athens API names and at least one concrete test scenario that can later become a unit/integration test.
5. The document references both prototype (`../n00b-slop/...`) and Athens (`include/display/...` or `src/display/...`) evidence for each widget.
6. Stack naming collision is resolved explicitly in writing (selected name + rationale).

Use these checks as a quick verification pass:

    rg -n '^## (stack|grid|split|scroll|tabs|text)$' plans/notes/widget-wave1-design-breakdown.md | wc -l
    rg -n '^### Prototype Behavior Snapshot$' plans/notes/widget-wave1-design-breakdown.md | wc -l
    rg -n '^### Athens API Proposal$' plans/notes/widget-wave1-design-breakdown.md | wc -l
    rg -n '^### State Model$' plans/notes/widget-wave1-design-breakdown.md | wc -l
    rg -n '^### Layout Contract$' plans/notes/widget-wave1-design-breakdown.md | wc -l
    rg -n '^### Rendering Contract$' plans/notes/widget-wave1-design-breakdown.md | wc -l
    rg -n '^### Event And Focus Contract$' plans/notes/widget-wave1-design-breakdown.md | wc -l
    rg -n '^### Backend And Portability Notes$' plans/notes/widget-wave1-design-breakdown.md | wc -l
    rg -n '^### Test And Demo Plan$' plans/notes/widget-wave1-design-breakdown.md | wc -l
    rg -n '^### Open Questions$' plans/notes/widget-wave1-design-breakdown.md | wc -l

Expected output for each `wc -l` command above is `6`.

## Idempotence and Recovery

Research commands in this plan are read-only and safe to rerun. Writing is limited to one artifact file under `plans/notes/`.

If the breakdown file drifts into partial or inconsistent structure, delete only `plans/notes/widget-wave1-design-breakdown.md` and regenerate it from the subsection contract in `Concrete Steps`. If Wave membership changes upstream, update `plans/notes/widget-port-priority.md` first, then rerun this plan so the breakdown stays aligned with the queue source of truth.

## Artifacts and Notes

Target artifact produced by this plan:

- `plans/notes/widget-wave1-design-breakdown.md`

Recommended section starter for each widget in the output file:

    ## <widget-name>

    ### Prototype Behavior Snapshot

    ### Athens API Proposal

    ### State Model

    ### Layout Contract

    ### Rendering Contract

    ### Event And Focus Contract

    ### Backend And Portability Notes

    ### Test And Demo Plan

    ### Open Questions

## Interfaces and Dependencies

The design dossier must define concrete intended Athens interfaces for all six widgets so downstream implementation plans do not need to invent names or signatures.

For `stack`, the design must pick a non-conflicting public constructor name because `n00b_stack_new` is already a macro in `include/adt/stack.h`. The selected naming should be applied consistently to header path, struct name, constructor, and mutators.

For `grid`, `split`, `scroll`, `tabs`, and `text`, the design must define public headers under `include/display/widgets/`, matching source files under `src/display/widgets/`, and vtable symbols following existing conventions (`n00b_widget_<kind>`). The design must state measurement semantics in pixels, event handling behavior through `n00b_event_t`, focus expectations via `can_focus`, and any container-specific `layout` behavior.

Cross-widget dependency assumptions to capture in the artifact:

1. `stack` and `grid` provide baseline container composition primitives.
2. `split` requires drag-aware mouse capture semantics from Athens mouse routing.
3. `scroll` requires a backend-neutral viewport model and must avoid notcurses-only plane-copy behavior.
4. `tabs` requires explicit focus and selection routing and may consume `stack` or `scroll` behaviors for tab content.
5. `text` must align wrapping, alignment, and optional selection behavior with Athens text measurement APIs and work correctly inside `scroll`.

## Revision Notes

- 2026-03-10: Initial ExecPlan added to produce a Wave 1 widget design breakdown artifact (`plans/notes/widget-wave1-design-breakdown.md`) using `plans/widget-port-priority-execplan.md` and `plans/notes/widget-port-priority.md` as scope context.
- 2026-03-11: Plan executed and finalized. Updated `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` with completion evidence and final design decisions after authoring and validating `plans/notes/widget-wave1-design-breakdown.md`.
