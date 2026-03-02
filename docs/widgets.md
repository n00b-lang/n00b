# Widget Implementation Plan

## Overview

The n00b widget system builds on the plane/canvas/backend infrastructure.
Each widget is a `(vtable, data)` pair attached to an `n00b_plane_t`.
Currently only the **label** widget is implemented.  This document lists
every planned widget (drawn from slop's ctui library) in implementation
order, grouped into waves by dependency and complexity.

## Architecture Recap

- **Widget = plane + vtable + opaque data** — no separate object hierarchy
- **Vtable**: `destroy`, `render`, `measure` callbacks
- **Styling**: flows through `n00b_string_t` rich markup; state-based
  overrides via `box->state_styles[]`
- **Box model**: borders + padding separate from content grid
- **Multi-backend**: ANSI, Cocoa, notcurses (pixel + cell), dumb, stream

## Implementation Waves

### Wave 1 — Foundation

Simple widgets that exercise core rendering patterns and serve as
building blocks for everything else.

| # | Widget       | Description                                       | Key Patterns          |
|---|-------------|---------------------------------------------------|-----------------------|
| 1 | **divider**  | Horizontal/vertical separator with optional label | Border/line drawing   |
| 2 | **spacer**   | Flexible empty space for layouts                  | Measure/layout only   |
| 3 | **button**   | Clickable button with shortcut key detection      | State-based styling   |
| 4 | **checkbox** | Toggle checkbox with label                        | Boolean state + label |
| 5 | **input**    | Single-line text input with cursor                | Text editing, cursor  |
| 6 | **progress** | Horizontal/vertical progress bar                  | Partial-cell fill     |

**Status**: done (label, divider, spacer, button, checkbox, input, progress)

### Wave 2 — Selection & Navigation

| # | Widget           | Description                                |
|---|------------------|--------------------------------------------|
| 7 | **radio**        | Radio button group (single selection)      |
| 8 | **switch**       | Toggle switch (on/off) with label          |
| 9 | **list**         | Scrollable list with keyboard navigation   |
| 10 | **selectionlist** | List with checkboxes for multi-select     |
| 11 | **breadcrumb**   | Navigation path with clickable segments   |
| 12 | **link**         | Clickable hyperlink (OSC 8)               |

**Status**: done (switch, radio, link, list, selectionlist, breadcrumb)

### Wave 3 — Layout Containers

| # | Widget    | Description                                    |
|---|----------|------------------------------------------------|
| 13 | **box**   | Flex container with border/background          |
| 14 | **grid**  | 2D layout with auto-flow and flexible columns  |
| 15 | **split** | Resizable split panes with draggable divider   |
| 16 | **scroll**| Scrollable container with scrollbar            |
| 17 | **stack** | Z-order stacking container                     |
| 18 | **tabs**  | Tabbed interface with content switching        |

### Wave 4 — Text & Content

| # | Widget       | Description                                  |
|---|-------------|----------------------------------------------|
| 19 | **text**     | Wrapped text display with optional selection |
| 20 | **editor**   | Multi-line text editor with scrolling        |
| 21 | **header**   | App title/subtitle bar                       |
| 22 | **footer**   | Key bindings display bar                     |
| 23 | **statusbar**| Status bar with L/C/R sections               |

### Wave 5 — Overlays & Modals

| # | Widget           | Description                              |
|---|-----------------|------------------------------------------|
| 24 | **modal**        | Modal dialog overlay with backdrop      |
| 25 | **tooltip**      | Floating help text on hover             |
| 26 | **toast**        | Temporary notification with auto-dismiss|
| 27 | **collapsible**  | Expandable/collapsible section          |
| 28 | **accordion**    | Multiple collapsible panels             |
| 29 | **menu**         | Dropdown/popup menu with hierarchy      |
| 30 | **commandpalette** | Fuzzy-search command launcher         |

### Wave 6 — Data Input

| # | Widget         | Description                              |
|---|---------------|------------------------------------------|
| 31 | **slider**     | Numeric value slider (h/v)              |
| 32 | **maskedinput**| Template-based input (phone, date, etc.)|
| 33 | **timepicker** | Time picker with AM/PM                  |
| 34 | **calendar**   | Month-view date picker                  |
| 35 | **colorpicker**| HSV color selection                     |

### Wave 7 — Data Visualization

| # | Widget      | Description                                |
|---|------------|--------------------------------------------|
| 36 | **sparkline**| Compact inline data viz (block chars)     |
| 37 | **chart**   | Static charts (line, bar, scatter, area)   |
| 38 | **plot**    | Real-time streaming data visualization     |
| 39 | **digits**  | Large 7-segment digit display              |
| 40 | **gradient**| Color gradient background                  |

### Wave 8 — Rich Content

| # | Widget      | Description                                  |
|---|------------|----------------------------------------------|
| 41 | **image**   | Image display via ncvisual                   |
| 42 | **canvas**  | High-res drawing surface (braille/pixel)     |
| 43 | **asciiart**| Image-to-ASCII conversion                    |
| 44 | **fontify** | FreeType-rendered text banners               |
| 45 | **spinner** | Animated loading indicator                   |
| 46 | **log**     | Real-time log display from fd/buffer         |

### Wave 9 — Complex Widgets

| # | Widget         | Description                             |
|---|---------------|-----------------------------------------|
| 47 | **tree**       | Hierarchical data browser              |
| 48 | **filebrowser**| Directory tree + breadcrumb navigation |
| 49 | **terminal**   | Embedded terminal emulator (forkpty)   |

## Reference

- **Slop widgets**: `~/slop/src/ctui/widgets/` (20 implemented),
  `~/slop/include/ctui/widgets/` (31 header-only)
- **Slop widget infra**: `~/slop/src/ctui/widget/` — `base.c` (lifecycle,
  visual children), `focus.c`, `keybind.c`, `graphical_text.c`
- **n00b widget infra**: `include/display/widget.h`,
  `include/display/render/plane.h`, `include/display/render/types.h`
- **n00b label** (reference implementation): `include/display/widgets/label.h`,
  `src/display/widgets/label.c`
