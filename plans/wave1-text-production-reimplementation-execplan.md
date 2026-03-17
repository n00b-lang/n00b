# Reimplement The Wave 1 Text Widget In Production

This ExecPlan is a living document. The sections `Progress`, `Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must be kept up to date as work proceeds.

There is no repository-local `PLANS.md` in this working tree at the time this plan was authored. This document follows `/home/baron/.codex/PLANS.md` and must remain compliant with that file for all future revisions.

This plan explicitly builds on `plans/notes/widget-port-priority.md`, `plans/notes/widget-wave1-design-breakdown.md`, and `docs/widgets.md`. Those files establish that after the production `zstack`, `grid`, `split`, `scroll`, and `tabs` ports landed, the final missing widget in the Wave 1 queue is `text`.

## Purpose / Big Picture

The goal is to land the final production Wave 1 widget in Athens: a selectable `text` widget that renders wrapped multi-line rich text, supports alignment and hanging indent, preserves backend-neutral rendering, and can copy the current visual selection through an Athens clipboard abstraction instead of hard-coding terminal escape sequences inside widget code. After this change, an Athens application will be able to show help panels, article-style content, logs, release notes, and tab pages as real wrapped text instead of stacking many labels or inventing one-off text views. A contributor or reviewer should be able to prove the result in two ways: run a dedicated `text` unit test that exercises wrapping, selection extraction, clipboard copy, and `scroll + text` integration on the stream backend, and run `widget_demo --widget text` to see a long selectable paragraph inside a real scroll viewport where dragging highlights visual lines and `Ctrl+C` copies the current selection on interactive backends without quitting the event loop.

## Progress

- [x] (2026-03-17 09:44Z) Reviewed the Wave 1 queue, the checked-in text design dossier, the prototype `text` implementation in `../n00b-slop`, and the current Athens widget, render, Unicode, event-dispatch, and backend seams that constrain this port.
- [x] (2026-03-17 09:44Z) Resolved the production planning decisions that the text dossier left open: `text` is the final Wave 1 widget; selection endpoints are stored as wrapped-line caret slots between grapheme clusters; selection extraction is visual and newline-joined across wrapped lines; `Ctrl+C` must dispatch to the focused widget before falling back to event-loop quit; and Wave 1 clipboard copy will use a renderer-backed Athens abstraction with stream-backend capture for tests and ANSI-family OSC 52 support for interactive terminal demos.
- [x] (2026-03-17 09:44Z) Drafted this self-contained execution plan for the production `text` reimplementation.
- [x] (2026-03-17 02:12Z) Implemented the shared clipboard/runtime seam in the renderer vtable, backend services, canvas helper, stream/ANSI-family backends, and the `Ctrl+C` dispatch path so focused widgets can consume copy before the event loop falls back to quit.
- [x] (2026-03-17 02:12Z) Added `include/display/widgets/text.h` and `src/display/widgets/text.c` with wrapped rich-text rendering, hanging indent, visual selection, stream/renderer-backed clipboard copy, and scroll-compatible mouse selection behavior.
- [x] (2026-03-17 02:12Z) Added `test/unit/test_text.c`, updated the dispatch regression test, extended `widget_demo --widget text`, updated `meson.build`, and marked Wave 1 complete in `docs/widgets.md`.

## Surprises & Discoveries

- Observation: Athens currently has no clipboard abstraction anywhere in the display stack.
  Evidence: `rg -n "clipboard|OSC 52|pbcopy|copy_selection" include src test` returned no Athens clipboard implementation hits before this plan was written.

- Observation: `n00b_display_dispatch_event()` currently intercepts `Ctrl+C` before the focused widget sees the key event.
  Evidence: `src/display/event_dispatch.c` returns `should_stop = true` as soon as it sees `key == 'c' && (mods & N00B_MOD_CTRL)`.

- Observation: Mouse routing already rebases events into each widget's local pixel coordinates during bubbling and during mouse capture.
  Evidence: `src/display/mouse.c` calls `mouse_event_to_plane_local()` for the target and each parent before invoking `n00b_widget_handle_event()`.

- Observation: Athens already has the Unicode pieces needed for production-quality wrapped selection logic.
  Evidence: `include/text/unicode/linebreak.h` exposes `n00b_unicode_linebreak_wrap()`, `include/text/unicode/segmentation.h` exposes `n00b_unicode_grapheme_iter()`, and `include/text/strings/string_style.h` exposes `n00b_str_add_style()` for range highlighting.

- Observation: The widget `measure` vtable has no available-width parameter, so wrapped-text preferred height cannot be computed from parent width the way the prototype did.
  Evidence: `include/display/widget.h` defines `measure(plane, data, pref_w, pref_h, min_w, min_h)` with no width hint, while the prototype `../n00b-slop/src/ctui/widgets/text.c` used an `available.x` width to compute wrapped height.

- Observation: The stream backend is already the standard harness for display/widget tests and can safely double as an in-memory clipboard capture backend for `text` tests.
  Evidence: existing tests such as `test/unit/test_grid.c`, `test/unit/test_scroll.c`, `test/unit/test_tabs.c`, and `test/unit/test_display_event_dispatch.c` already instantiate `n00b_renderer_stream` canvases and query helper functions from `src/display/render/backend_stream.c`.

- Observation: `n00b_unicode_str_split_lines()`, `n00b_unicode_str_wrap()`, and the basic byte-slice helpers do not preserve per-range styling metadata.
  Evidence: `src/text/strings/string_ops.c` constructs new strings with `n00b_string_from_raw(...)` in all three paths, so the production widget had to rebuild clipped style ranges while caching wrapped visual lines.

- Observation: Requesting `widget_demo --widget text --backend ansi_inline` in this checkout currently falls back to the full-screen ANSI backend because there is no standalone `ansi_inline` renderer plugin on disk.
  Evidence: the smoke run printed `Backend request 'ansi_inline' selected 'ansi' (fallback)` before rendering the text demo scene.

## Decision Log

- Decision: The next and final Wave 1 production widget is `text`, and this plan targets that widget only.
  Rationale: `plans/notes/widget-port-priority.md` orders Wave 1 as `stack`, `grid`, `split`, `scroll`, `tabs`, `text`, and `docs/widgets.md` now shows `text` as the only remaining Wave 1 backlog entry.
  Date/Author: 2026-03-17 / Codex.

- Decision: Selection coordinates remain `line + col` as the design dossier required, but each `col` is a caret slot between grapheme clusters in a wrapped visual line rather than a raw byte offset or codepoint index.
  Rationale: grapheme-slot coordinates avoid splitting combined emoji or accent clusters, make end-of-line clicks representable, and keep the public API stable without forcing source-text byte offsets into the widget surface.
  Date/Author: 2026-03-17 / Codex.

- Decision: `n00b_text_get_selection()` returns the visual selection, joining wrapped-line slices with `\n` between wrapped lines instead of reconstructing the original unwrapped source spacing.
  Rationale: the checked-in dossier and the prototype selection contract are both visual-selection oriented, and that behavior is simpler and more predictable for users selecting what they actually see on screen.
  Date/Author: 2026-03-17 / Codex.

- Decision: Clipboard copy is exposed through a shared Athens runtime path rooted at the canvas and renderer backend interface instead of widget-specific shell commands or raw escape-sequence emission.
  Rationale: the design dossier explicitly requires backend-neutral clipboard support, and the widget layer must remain ignorant of whether the active renderer is ANSI, notcurses, Cocoa, X11, or the stream test harness.
  Date/Author: 2026-03-17 / Codex.

- Decision: The wrapped-line cache stores style-preserving slices clipped from the original source string instead of the raw outputs from `n00b_unicode_str_split_lines()` and `n00b_unicode_str_wrap()`.
  Rationale: Athens rich text attaches styles as byte-range metadata on `n00b_string_t`, and the stock split/wrap helpers discard that metadata; rebuilding clipped style ranges in the cache keeps selection overlays and styled demo spans intact.
  Date/Author: 2026-03-17 / Codex.

- Decision: When text content or hanging indent changes and the widget already lives on a canvas with valid bounds, the implementation reruns full scene layout through `n00b_display_scene_run_layout(canvas)` instead of relaying out only the leaf plane.
  Rationale: a width-sensitive leaf widget cannot change its parent-assigned height by relaying out itself in isolation, while a scene relayout lets containers such as `box` and `scroll` remeasure the text widget and propagate the new preferred height correctly.
  Date/Author: 2026-03-17 / Codex.

- Decision: `Ctrl+C` stops the event loop only when the focused widget declines to consume it.
  Rationale: `text` needs `Ctrl+C` for copy, but the broader TUI still needs a quit fallback when no widget uses that shortcut. Dispatch-first then quit-fallback preserves both behaviors without introducing a new global keybinding.
  Date/Author: 2026-03-17 / Codex.

- Decision: Wrapped-text measurement uses the current known content width when one exists and otherwise falls back to an 80-column natural-width wrap.
  Rationale: the vtable offers no available-width input, so Athens needs a deterministic fallback. Eighty columns matches the prototype's de facto fallback behavior and keeps `measure` stable instead of oscillating with later layout passes.
  Date/Author: 2026-03-17 / Codex.

- Decision: Wave 1 clipboard support must be fully testable on the stream backend even though the stream backend has no real system clipboard.
  Rationale: automated tests need an observable copy target. The stream backend can safely store the last copied UTF-8 payload in memory while ANSI-family backends perform real terminal clipboard writes.
  Date/Author: 2026-03-17 / Codex.

- Decision: `copy_on_release` remains enabled by default, but mouse release copies only when the current selection range is non-empty; an empty click selection is not copied and does not quit the app.
  Rationale: this keeps ordinary click-to-focus or click-to-place gestures from spamming clipboard writes while still preserving the prototype's drag-to-select-and-copy flow.
  Date/Author: 2026-03-17 / Codex.

## Outcomes & Retrospective

As of 2026-03-17, the Wave 1 production `text` port is implemented end-to-end. Athens now has a backend-neutral clipboard seam, `Ctrl+C` dispatch that gives focused widgets first refusal, a selectable wrapped rich-text widget with hanging indent and visual copy semantics, a dedicated `text` unit suite, a `widget_demo --widget text` scene, and updated Wave 1 documentation. The required automated validation passed in `build_debug`, including the new `text` suite plus nearby `render_canvas`, `mouse`, and `scroll` regressions. The remaining gap is manual end-to-end clipboard verification in a live interactive terminal session outside this execution environment.

## Context and Orientation

The production `text` widget belongs under `include/display/widgets/` and `src/display/widgets/` beside the already-landed Wave 1 widgets `scroll`, `tabs`, and `zstack`. Unlike `label`, which is a lightweight render-only leaf that can wrap text but does not maintain selection state, `text` is a stateful leaf widget with a wrapped-line cache and optional focus. It must remain compatible with the existing plane-based rendering system, so it renders by issuing draw commands on its own plane rather than creating backend-owned subplanes.

In this repository, a "wrapped-line cache" means widget-owned data that stores the current visual lines derived from the source string and the current content width. For `text`, each cached visual line must carry enough data to do three things without re-tokenizing on every draw: measure its rendered width, map mouse x-coordinates to grapheme boundaries, and build a styled substring when part of the line is selected. The cache is invalid whenever the source text, wrapping mode, hanging indent, or content width changes.

A "grapheme cluster" is a user-perceived character, which may be one codepoint or several codepoints joined together, such as an accented letter or emoji sequence. The selection model in this plan intentionally works in grapheme boundaries rather than raw bytes so a mouse drag cannot split a combined glyph into invalid pieces. The grapheme iterator lives in `include/text/unicode/segmentation.h` and `src/text/unicode/segmentation.c`.

Prototype behavior lives in the sibling repository under `../n00b-slop/`. The important prototype files are `../n00b-slop/include/ctui/widgets/text.h` and `../n00b-slop/src/ctui/widgets/text.c`. They prove the core user-visible behavior to preserve, but they also show the two things Athens must not copy verbatim: direct clipboard coupling through the prototype app and the assumption that preferred size always knows an available width.

Current Athens files that directly constrain this work are:

- `include/display/widget.h` and `src/display/widget.c` for the widget vtable, `measure`, and `layout` entry points.
- `include/display/render/plane.h` and `src/display/render/plane.c` for text drawing, text measurement, and plane content bounds.
- `src/display/widgets/label.c` for the closest existing wrapped-text render path.
- `src/display/widgets/scroll.c` for the current Wave 1 integration target and for the immediate-relayout pattern used when widget state affects layout.
- `src/display/event_dispatch.c` and `include/display/event_loop.h` for the current `Ctrl+C` quit path that must be generalized.
- `src/display/mouse.c` for mouse localization and capture behavior.
- `include/display/render/backend.h`, `include/internal/display/backend_services.h`, `src/display/render/backend_services.c`, `include/display/render/canvas.h`, and `src/display/render/canvas.c` for the new clipboard abstraction.
- `src/display/render/backend_stream.c` and `src/display/render/backend_ansi*.c` for the first concrete clipboard implementations.
- `src/tools/widget_demo.c`, `meson.build`, and `docs/widgets.md` for the end-to-end proof and roadmap state.

An "OSC 52" clipboard copy is a terminal escape sequence that asks the terminal emulator to place text on the system clipboard. Athens may use that mechanism inside ANSI-family backends because it is renderer-specific terminal behavior, but widget code must never emit the sequence directly. The widget should only call an Athens clipboard helper on the active canvas.

## Plan of Work

The first edit is a small but necessary runtime seam below the widget layer. Extend the renderer backend interface with an optional clipboard-copy slot, add a public canvas helper that forwards UTF-8 text to the active renderer, and add the matching backend-services wrapper. The stream backend should implement that slot by storing the copied text in memory so tests can assert on it. The ANSI and ANSI-inline backends should implement it by emitting an OSC 52 clipboard sequence and flushing the backend immediately after a successful copy. Backends that do not support clipboard copy in Wave 1 may return `false`, but they must do so safely and without crashing or emitting partial escapes.

The second edit is the event-dispatch change that lets copy work without regressing quit behavior. `src/display/event_dispatch.c` must dispatch `Ctrl+C` to the currently focused widget first. If the widget consumes the key, dispatch stops normally and the event loop continues. If the focused widget declines the key, dispatch returns `should_stop = true` exactly as it does today. Update `include/display/event_loop.h` and the dispatch unit test so the documented behavior and test suite match the new contract.

The third edit is the public widget surface in `include/display/widgets/text.h` and the production implementation in `src/display/widgets/text.c`. The header must define the selection record, the public widget state record, the vtable symbol, the constructor, and the exact accessors/mutators promised by the design dossier. The source file must define a private implementation record that embeds the public state and owns the wrapped-line cache. Keep cache helpers file-local; only the public state and functions belong in the header.

The cache pipeline should reuse Athens Unicode helpers instead of reimplementing text segmentation. Split the source string into hard lines with `n00b_unicode_str_split_lines()`. When wrapping is enabled, wrap each hard line with `n00b_unicode_str_wrap()` using the current wrap width in text columns and the configured hanging indent. For each resulting visual line, cache the rendered substring, the display-column width, the continuation indent columns, and an array of grapheme-boundary byte offsets computed with `n00b_unicode_grapheme_iter()`. That cache is then reused by render, hit testing, selection extraction, and wrapped-line counting.

Rendering should stay fully Athens-native. `text_render()` clears the plane, resolves the current content width, ensures the cache is valid, and draws each visible line with `n00b_plane_draw_text()`. Alignment is applied inside the usable width for that line: continuation lines reserve `hang_indent_cols * cell_px_w` pixels on the left, then left/center/right alignment is computed inside the remaining width and added to the indent offset. When part of a line is selected, build a styled copy of that cached line substring with `n00b_str_add_style()` over the selected byte range, using `N00B_PAL_SELECTION_BG` and `N00B_PAL_SELECTION_FG` so the existing rich-text styling remains intact under the selection overlay.

Selection and copy behavior belong entirely inside `src/display/widgets/text.c`. Mouse press starts a selection if the widget is selectable, captures the mouse on the canvas, and records the anchor caret slot. Mouse drag updates the tail slot even when the pointer leaves the original line bounds, clamping to the nearest legal line and slot. Mouse release finalizes the range, releases mouse capture, and triggers `n00b_text_copy_selection()` only when `copy_on_release` is enabled and the range is non-empty. `Ctrl+C` should attempt a copy whenever the widget is selectable and currently has a non-empty selection, and the event must be reported as consumed even if the active backend returns `false` for clipboard support so the fallback quit path does not terminate the application unexpectedly.

The fourth edit is the test suite. Add `test/unit/test_text.c` in the same style as `test_scroll.c`, `test_tabs.c`, and `test_label.c`, using stream-backend canvases for deterministic layout and clipboard assertions. Also update `test/unit/test_display_event_dispatch.c` so it verifies the new "dispatch first, quit only if unhandled" `Ctrl+C` rule. The text test file should prove observable behavior: wrapped line counts at different widths, hanging-indent positioning, selection extraction across wrapped lines, `Ctrl+C` copy via the stream clipboard helper, and at least one `scroll + text` smoke scenario showing that selection does not reset scroll offsets or crash after a scroll operation.

The fifth edit is tooling and documentation. Update `meson.build` so the library builds `src/display/widgets/text.c` and the unit suite includes `test_text`. Extend `src/tools/widget_demo.c` with a selectable `text` demo and `--widget text` dispatch support. The demo should place a long styled `text` widget inside a `scroll` container so the user can verify wrapping, hanging indent, selection highlight, wheel scrolling, and `Ctrl+C` copy behavior in one scene. Update `docs/widgets.md` so `text` moves into the implemented widget list, `test_text` joins the representative widget-test list, and the Wave 1 roadmap section states that Wave 1 is complete rather than leaving an empty bullet list.

## Concrete Steps

Run all commands from `/home/baron/crash-override/n00b-tui/n00b-athens`.

1. Refresh the text design, prototype, and Athens runtime seams before editing:

       sed -n '/^## text$/,$p' plans/notes/widget-wave1-design-breakdown.md
       sed -n '1,220p' ../n00b-slop/include/ctui/widgets/text.h
       sed -n '1,520p' ../n00b-slop/src/ctui/widgets/text.c
       sed -n '1,240p' src/display/widgets/label.c
       sed -n '1,220p' src/display/event_dispatch.c
       sed -n '1,220p' include/display/event_loop.h
       sed -n '1,320p' src/display/mouse.c
       sed -n '1,260p' include/display/render/backend.h
       sed -n '1,220p' include/internal/display/backend_services.h
       sed -n '1,220p' src/display/render/backend_services.c
       sed -n '1,260p' src/display/render/backend_stream.c
       sed -n '1,220p' include/text/unicode/linebreak.h
       sed -n '1,220p' include/text/unicode/segmentation.h
       sed -n '1,220p' include/text/strings/string_style.h
       sed -n '1,220p' docs/widgets.md

   Expected result: you can see the Wave 1 text API contract, the prototype's wrapped-line and clipboard behavior, the current Athens wrapped-text rendering path in `label`, the unconditional `Ctrl+C` stop branch in dispatch, the mouse-localization behavior that makes text-local hit testing possible, the lack of a clipboard slot in the renderer backend interface, and the docs state showing `text` as the final missing Wave 1 widget.

2. Edit the render runtime so widgets can copy text through the active backend.

   In `include/display/render/backend.h`, extend `n00b_renderer_vtable_t` with:

       bool (*clipboard_copy)(void *ctx, const char *utf8, size_t len);

   In `include/internal/display/backend_services.h` and `src/display/render/backend_services.c`, add:

       extern bool n00b_display_backend_copy_text(n00b_canvas_t *canvas,
                                                  const char    *utf8,
                                                  size_t         len);

   In `include/display/render/canvas.h` and `src/display/render/canvas.c`, add:

       extern bool n00b_canvas_clipboard_copy(n00b_canvas_t *canvas,
                                              n00b_string_t *text);

   `n00b_canvas_clipboard_copy()` must return `false` for null canvas, null text, missing backend support, or failed backend copy. When backend copy succeeds, it must immediately call `n00b_canvas_flush(canvas)` so a `Ctrl+C` copy on an otherwise static frame still reaches the backend.

   Implement the backend slot with these exact Wave 1 semantics:

   - `src/display/render/backend_stream.c`: store the copied UTF-8 text in backend context and return `true`. Add an accessor helper:

         extern n00b_string_t *n00b_stream_backend_get_clipboard(void *ctx);

     The helper should return an owned copy of the last copied text, or `nullptr` when nothing has been copied yet.

   - `src/display/render/backend_ansi.c` and `src/display/render/backend_ansi_inline.c`: emit one OSC 52 clipboard sequence containing the base64-encoded UTF-8 payload and return `true` when the payload is emitted into the backend output buffer successfully.

   - `src/display/render/backend_dumb.c`: return `false`.

   - Optional GUI or notcurses backends that are easy to support in this repository may implement the slot, but Wave 1 does not require new platform clipboard code beyond the ANSI-family and stream backends. Unsupported backends must leave the slot null or return `false` safely.

3. Update the event-dispatch contract so `Ctrl+C` can be used by focused widgets.

   In `src/display/event_dispatch.c`, keep Tab and Shift+Tab handling exactly as it is, but move the `Ctrl+C` stop check to after focused-widget dispatch. The new key-flow contract must be:

   - Dispatch `Ctrl+C` to the focused widget just like any other key.
   - If the focused widget returns `true`, do not set `should_stop`.
   - If the focused widget returns `false`, set `handled = true` and `should_stop = true`.

   Update `include/display/event_loop.h` so the loop behavior description says `Ctrl+C` quits only when unhandled by the focused widget. Update `test/unit/test_display_event_dispatch.c` so it proves both branches: a focusable widget that consumes `Ctrl+C` keeps the loop running, and an unconsumed `Ctrl+C` still triggers stop.

4. Create `include/display/widgets/text.h` with the exact public widget surface below. Match the repository's existing widget-header style and keep the public state record small and user-facing.

       typedef struct n00b_text_selection_t {
           int32_t start_line;
           int32_t start_col;
           int32_t end_line;
           int32_t end_col;
           bool    active;
       } n00b_text_selection_t;

       typedef struct n00b_text_t {
           n00b_string_t        *text;
           n00b_alignment_t      alignment;
           bool                  wrap;
           int32_t               hang_indent_cols;
           bool                  selectable;
           bool                  copy_on_release;
           n00b_text_selection_t selection;
           int32_t               wrapped_line_count;
           int32_t               cached_wrap_cols;
       } n00b_text_t;

       extern const n00b_widget_vtable_t n00b_widget_text;

       extern n00b_plane_t *
       n00b_text_new(n00b_string_t *text) _kargs {
           n00b_alignment_t  alignment        = N00B_ALIGN_LEFT;
           bool              wrap             = true;
           int32_t           hang_indent_cols = 0;
           bool              selectable       = false;
           bool              copy_on_release  = true;
           n00b_canvas_t    *canvas           = nullptr;
           n00b_allocator_t *allocator        = nullptr;
       };

       extern void           n00b_text_set_text(n00b_plane_t *text_plane, n00b_string_t *text);
       extern n00b_string_t *n00b_text_get_text(n00b_plane_t *text_plane);
       extern void           n00b_text_set_alignment(n00b_plane_t *text_plane, n00b_alignment_t alignment);
       extern void           n00b_text_set_hang_indent(n00b_plane_t *text_plane, int32_t hang_indent_cols);
       extern void           n00b_text_set_selectable(n00b_plane_t *text_plane, bool selectable);
       extern bool           n00b_text_has_selection(n00b_plane_t *text_plane);
       extern void           n00b_text_clear_selection(n00b_plane_t *text_plane);
       extern n00b_string_t *n00b_text_get_selection(n00b_plane_t *text_plane);
       extern bool           n00b_text_copy_selection(n00b_plane_t *text_plane);
       extern int32_t        n00b_text_get_wrapped_line_count(n00b_plane_t *text_plane);

   Public contract details that must remain true:

   - All wrong-kind or null-plane accessors are harmless: setters are no-ops, booleans return `false`, integers return `0`, and pointer getters return `nullptr`.
   - `n00b_text_get_selection()` returns a newly allocated UTF-8 string representing the current visual selection, or `nullptr` when there is no non-empty selection.
   - `n00b_text_copy_selection()` returns `false` when there is no selection or no backend clipboard support. It never emits terminal escape sequences directly from widget code.
   - `n00b_text_get_wrapped_line_count()` must ensure the cache exists before returning, using the current content width if known and the 80-column fallback otherwise.

5. Create `src/display/widgets/text.c`. Keep cache records and geometry helpers file-local. The file should define `text_destroy`, `text_render`, `text_measure`, `text_handle_event`, `text_can_focus`, and `text_layout`, plus private helpers for cache invalidation, hard-line wrapping, grapheme-boundary caching, selection normalization, line/slot hit testing, selection-style overlay, and immediate relayout.

   Use these exact private records inside `src/display/widgets/text.c`:

       typedef struct n00b_text_line_t {
           n00b_string_t *text;
           uint32_t      *grapheme_boundaries;
           n00b_isize_t   grapheme_count;
           int32_t        display_cols;
           int32_t        indent_cols;
       } n00b_text_line_t;

       typedef struct n00b_text_impl_t {
           n00b_text_t      public_state;
           n00b_text_line_t *lines;
           n00b_isize_t      line_capacity;
           int32_t           cached_content_width_px;
           bool              dragging_selection;
       } n00b_text_impl_t;

   Implement the public mutators with these exact semantics:

   - `n00b_text_new()` allocates the plane and `n00b_text_impl_t`, stores the requested configuration, attaches the widget, and marks the plane dirty.
   - `n00b_text_set_text()` stores the new string reference, clears the cache and current selection, marks the plane dirty, and reruns `n00b_widget_layout()` immediately when valid bounds are already known because height may change.
   - `n00b_text_set_alignment()` stores the new alignment, marks dirty, and does not clear the selection.
   - `n00b_text_set_hang_indent()` clamps negative values to zero, clears the cache, and relayouts immediately when bounds are known.
   - `n00b_text_set_selectable()` stores the new flag, clears any active selection when set to `false`, releases mouse capture if this widget owns it, and rebuilds focus through `canvas->focus` when that pointer is available.
   - `n00b_text_clear_selection()` makes the selection inactive and marks the plane dirty.

6. Use the following wrap, measure, layout, render, and selection rules inside `src/display/widgets/text.c`.

   Cache invalidation must happen whenever the source text pointer changes, the content width in pixels changes, `wrap` changes, or `hang_indent_cols` changes. Convert content width from pixels to text columns with `n00b_plane_text_columns()`, and clamp the result to at least `1`.

   Cache construction works like this:

   - Split the source string into hard lines with `n00b_unicode_str_split_lines()`. If the source is null or empty, synthesize one empty visual line so height never collapses to zero.
   - For each hard line:
     - If `wrap == false`, keep one visual line with `indent_cols = 0`.
     - If `wrap == true`, call `n00b_unicode_str_wrap(hard_line, .width = wrap_cols, .hang = hang_indent_cols)` and set `indent_cols = 0` for the first visual line from that hard line and `hang_indent_cols` for every continuation line.
   - For each visual line, compute `display_cols = n00b_unicode_display_width(line)` and fill `grapheme_boundaries` with byte offsets for every grapheme boundary, including `0` and `line->u8_bytes`.

   Measurement rules:

   - `wrap == true`: if `plane->bounds.width > 0` or `plane->width > 0`, derive `measure_cols` from that current width; otherwise use 80 columns. Build the cache for `measure_cols`.
   - `wrap == false`: build the cache from hard lines only.
   - `pref_w` is the maximum rendered line width in pixels, including hanging-indent offset for continuation lines, with a floor of `1`.
   - `pref_h` is `max(wrapped_line_count, 1) * line_height_px`.
   - `min_w` is one cell width in pixels and `min_h` is one line height in pixels. Keep this explicit in a short code comment because it is the deterministic Athens fallback for a width-sensitive widget without an available-width parameter.

   Layout rules:

   - `text_layout()` receives already-inset content bounds from `n00b_widget_layout()`. Use the actual content width from the plane to refresh the cache immediately so `n00b_text_get_wrapped_line_count()` and subsequent renders are already synchronized with the real width.
   - The widget remains a leaf. Do not add child planes.

   Render rules:

   - Clear the plane before drawing.
   - Draw at most the lines that fit in the visible content height.
   - For each line, convert `indent_cols` to pixels with `n00b_widget_cell_px_width(plane)`.
   - For left alignment, draw at `indent_px`.
   - For center or right alignment, align inside the remaining width `content_w - indent_px` and then add `indent_px`.
   - When the current selection intersects the line, build a styled copy of that line using `n00b_str_add_style()` across the selected byte range with a style whose foreground and background come from `N00B_PAL_SELECTION_FG` and `N00B_PAL_SELECTION_BG`.

   Selection rules:

   - Selection coordinates are stored as caret slots from `0` to `grapheme_count`, not inclusive grapheme indices.
   - `n00b_text_has_selection()` returns `true` only when the selection is active and the normalized start and end positions differ.
   - `n00b_text_get_selection()` normalizes the range, slices each selected visual line between the selected grapheme boundaries, and joins the resulting fragments with `\n`.
   - Mouse press starts selection, records the anchor slot, captures the mouse, and consumes the event.
   - Mouse drag updates the tail slot while capture is held and consumes the event.
   - Mouse release updates the tail slot one last time, releases capture, optionally calls `n00b_text_copy_selection()` when `copy_on_release == true` and the range is non-empty, and consumes the event.
   - `Ctrl+C` consumes the key whenever the widget is selectable and has a non-empty selection. It should call `n00b_text_copy_selection()` but return `true` even when the backend copy reports failure.

7. Create `test/unit/test_text.c` and update `test/unit/test_display_event_dispatch.c`.

   `test/unit/test_text.c` must include at least these cases:

   - `test_text_create_and_api()` proves constructor defaults, vtable attachment, wrong-kind tolerance, setter/getter behavior, and the initial no-selection state.
   - `test_text_wrap_count_changes_with_width()` lays out the same UTF-8 paragraph at two widths and asserts that the narrow layout produces more wrapped lines. It should also assert that a continuation line is indented when `hang_indent_cols > 0`.
   - `test_text_selection_extract_and_autocopy()` uses a stream canvas, sends localized mouse press/drag/release events through `n00b_mouse_route_event()`, and asserts that `n00b_text_get_selection()` returns the expected newline-joined visual slice and that `n00b_stream_backend_get_clipboard()` captured the same string after release.
   - `test_text_ctrl_c_copy()` focuses a selectable text widget with an existing selection, sends `Ctrl+C` through `n00b_widget_handle_event()` or the dispatch path, and asserts that the stream clipboard capture changed.
   - `test_text_inside_scroll_smoke()` places `text` inside a real `scroll` widget, scrolls down, performs a selection, and asserts that the scroll offset remains stable after copy and that the scene does not crash.

   Update `test/unit/test_display_event_dispatch.c` so the `Ctrl+C` contract is explicit: a consuming focused widget keeps the loop running, while an unconsumed `Ctrl+C` still sets `should_stop`.

8. Wire the build, demo, and docs.

   In `meson.build`, add `src/display/widgets/text.c` immediately after `src/display/widgets/tabs.c` and before `src/display/widgets/zstack.c`. Add:

       text_test = executable('test_text',
           ['test/unit/test_text.c'],
           kwargs: test_common_kwargs,
       )
       test('text', text_test, suite: 'unit')

   In `src/tools/widget_demo.c`, add a `demo_text(n00b_canvas_t *canvas)` scene and a `--widget text` dispatch branch. The demo should:

   - Create a long multi-paragraph `n00b_string_t` with at least one styled span so selection overlay merges with rich text.
   - Place that text widget inside a `scroll` container.
   - Enable `selectable = true`, `copy_on_release = true`, `wrap = true`, and a visible `hang_indent_cols` value.
   - Include a small explanatory label telling the user to drag-select and press `Ctrl+C`.
   - Opt into the interactive event loop the same way the existing `scroll` and `tabs` demos do.

   In `docs/widgets.md`, move `text` into the implemented widgets list, add `text` to the representative widget-test sentence, and replace the Wave 1 backlog bullets with a sentence that Wave 1 is complete.

9. Reconfigure, build, and run the validation targets after the edits land:

       meson setup --reconfigure build_debug -Dusing_build_script=true
       meson compile -C build_debug test_text test_display_event_dispatch widget_demo
       meson test -C build_debug --print-errorlogs text display_event_dispatch
       meson test -C build_debug --print-errorlogs render_canvas scroll mouse
       timeout 5s ./build_debug/widget_demo --widget text --backend stream

   Actual result from this execution: the Meson reconfigure succeeded, the targeted compile completed without errors, `text`, `display_event_dispatch`, `render_canvas`, `scroll`, and `mouse` all reported `OK`, and the stream demo exited cleanly after one frame with `Backend request 'stream' selected 'stream'` followed by `Backend 'stream' has no input polling; using single-frame mode.`.

10. Optionally smoke the ANSI-family demo scene even when a full interactive clipboard verification is not available:

       ./build_debug/widget_demo --widget text --backend ansi_inline

   Actual result from this execution: the scene rendered successfully, but backend resolution fell back to the full-screen ANSI backend in this checkout because no standalone `ansi_inline` renderer plugin was present on disk. That smoke run proved the text demo scene builds on an interactive renderer, but it was not used as the final manual clipboard acceptance check.

## Validation and Acceptance

Run all validation commands from `/home/baron/crash-override/n00b-tui/n00b-athens`.

First, if `meson` does not know about the new target after editing `meson.build`, reconfigure once:

    meson setup --reconfigure build_debug -Dusing_build_script=true

Then build the affected targets:

    meson compile -C build_debug test_text test_display_event_dispatch widget_demo

Run the targeted unit tests:

    meson test -C build_debug --print-errorlogs text display_event_dispatch

Expected result: `text` and `display_event_dispatch` both report `OK`, and the new `text` test output shows explicit pass lines for wrap counting, selection extraction, clipboard capture, and scroll integration.

Run the non-interactive demo smoke:

    timeout 5s ./build_debug/widget_demo --widget text --backend stream

Expected result: the command exits cleanly after one frame and prints the backend-selection notice instead of crashing or hanging.

Run the interactive terminal smoke on an ANSI-capable backend:

    ./build_debug/widget_demo --widget text --backend tui

Manual acceptance in that session:

- Drag-select across at least two wrapped lines and verify that the selection highlight follows the drag.
- Scroll the content and verify the selection interaction still targets the visible text rather than the pre-scroll coordinates.
- Press `Ctrl+C` with a non-empty selection and verify the app stays open.
- Paste into another application or terminal and verify the copied text matches the selected visual slice.
- Clear the selection, press `Ctrl+C` again, and verify the event loop now exits via the existing quit fallback.

The feature is accepted when the targeted tests pass, the stream demo smoke exits cleanly, and the manual TUI smoke proves that selection highlighting, copy, and quit fallback all behave as described above. In this execution, the automated criteria passed and the manual TUI criteria remain the only outstanding operator check.

## Idempotence and Recovery

All build and test steps in this plan are safe to rerun. If a new target is missing after editing `meson.build`, rerun `meson setup --reconfigure build_debug` and repeat the compile command. If `Ctrl+C` still quits immediately with a selected text widget, inspect `src/display/event_dispatch.c` and `test/unit/test_display_event_dispatch.c` before changing the widget itself; the likely bug is dispatch order, not rendering or selection state. If clipboard copy works in the stream tests but not in an interactive terminal, keep the widget code and inspect the ANSI backend clipboard slot and immediate flush path before touching the widget implementation. Do not remove or revert the already-landed Wave 1 widgets while iterating on `text`.

## Artifacts and Notes

Observed targeted test transcript:

    $ meson test -C build_debug --print-errorlogs text display_event_dispatch
    1/2 unit - n00b:text OK
    2/2 unit - n00b:display_event_dispatch OK

Observed nearby regression transcript:

    $ meson test -C build_debug --print-errorlogs render_canvas scroll mouse
    1/3 unit - n00b:render_canvas OK
    2/3 unit - n00b:mouse OK
    3/3 unit - n00b:scroll OK

Observed non-interactive demo smoke:

    $ timeout 5s ./build_debug/widget_demo --widget text --backend stream
    Backend request 'stream' selected 'stream'
    Backend 'stream' has no input polling; using single-frame mode.

Expected stream-backend clipboard assertion inside `test_text.c`:

    selection == "Athens text\nwidget"
    clipboard == "Athens text\nwidget"

Expected manual TUI behavior after the final fix:

    - `Ctrl+C` with a selection leaves the demo running and copies text.
    - `Ctrl+C` with no selection exits the demo.

## Interfaces and Dependencies

In `include/display/render/backend.h`, extend `n00b_renderer_vtable_t` with:

    bool (*clipboard_copy)(void *ctx, const char *utf8, size_t len);

In `include/display/render/canvas.h`, define:

    extern bool n00b_canvas_clipboard_copy(n00b_canvas_t *canvas,
                                           n00b_string_t *text);

In `include/internal/display/backend_services.h`, define:

    extern bool n00b_display_backend_copy_text(n00b_canvas_t *canvas,
                                               const char    *utf8,
                                               size_t         len);

In `include/display/widgets/text.h`, define exactly the public types and functions listed in `Concrete Steps`. Do not add a public selection-callback API, explicit width setter, or read-only mode in this Wave 1 pass; the checked-in design dossier fixed the public surface already, and the remaining design work is in implementation details, not API expansion.

In `src/display/widgets/text.c`, define the private cache records:

    typedef struct n00b_text_line_t {
        n00b_string_t *text;
        uint32_t      *grapheme_boundaries;
        n00b_isize_t   grapheme_count;
        int32_t        display_cols;
        int32_t        indent_cols;
    } n00b_text_line_t;

    typedef struct n00b_text_impl_t {
        n00b_text_t      public_state;
        n00b_text_line_t *lines;
        n00b_isize_t      line_capacity;
        int32_t           cached_content_width_px;
        bool              dragging_selection;
    } n00b_text_impl_t;

The implementation depends on existing Athens modules only:

- `include/display/widget.h` and `src/display/widget.c` for attach, measure, event dispatch, and layout entry points.
- `include/display/render/plane.h` and `src/display/render/plane.c` for text drawing, line-height measurement, and content-size queries.
- `src/display/widgets/label.c` for the nearest wrapped-text rendering pattern.
- `src/display/widgets/scroll.c` for the production integration target and scroll smoke scenario.
- `src/display/mouse.c` for localized mouse coordinates and mouse capture.
- `include/internal/display/widget_primitives.h` for line-height and cell-width helpers.
- `include/text/unicode/linebreak.h`, `include/text/unicode/segmentation.h`, and `include/text/strings/string_ops.h` for wrapping, grapheme boundaries, and slicing.
- `include/text/strings/string_style.h` and `include/text/strings/theme.h` for selection overlay styling through palette roles.
- `src/display/render/backend_stream.c` and the ANSI-family backends for the first concrete clipboard implementations.

## Revision Notes

- 2026-03-17: Initial ExecPlan added for the Wave 1 production `text` reimplementation after confirming that `text` is the final missing Wave 1 widget in `docs/widgets.md`. The plan resolves the missing clipboard/runtime seam, the `Ctrl+C` dispatch order change, the stream-backend clipboard test strategy, and the width-sensitive measurement fallback that the design dossier did not fully pin down.
- 2026-03-17: Initial ExecPlan added for the Wave 1 production `text` reimplementation after confirming that `text` is the final missing Wave 1 widget in `docs/widgets.md`. The plan resolves the missing clipboard/runtime seam, the `Ctrl+C` dispatch order change, the stream-backend clipboard test strategy, and the width-sensitive measurement fallback that the design dossier did not fully pin down.
- 2026-03-17: Updated after implementation to mark all three execution milestones complete, record the style-preserving wrapped-line cache decision, and embed the exact reconfigure/build/test/demo commands and results from the finished run so the plan can be resumed or audited from the repository alone.
