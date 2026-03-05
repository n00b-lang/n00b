# Widget Status And Roadmap

## Current Implementation Status

The widget runtime is active and integrated with the display pipeline (`plane + widget_vtable + widget_data`).

Implemented and covered by unit/integration tests:

- `label`
- `divider`
- `spacer`
- `button`
- `checkbox`
- `input`
- `progress`
- `switch`
- `radio`
- `link`
- `list_widget`
- `selectionlist`
- `breadcrumb`

Representative tests live under `test/unit/test_{label,button,checkbox,input,switch,radio,list_widget,selectionlist,breadcrumb,link}.c`.

## Runtime Integration Expectations

- Widgets render through planes attached to a canvas.
- Backend selection is runtime policy driven (`n00b_canvas_init(... .backend_name=...)`).
- Focus/mouse/key dispatch is shared across terminal and GUI-capable backends.
- `src/tools/widget_demo.c` is the practical end-to-end demo entrypoint.

## Usage Notes

- Preferred startup path for demos/apps: backend-name selection (`auto`, `tui`, `gui`, `stream`, and explicit backend names).
- Direct vtable startup remains useful for deterministic tests/harnesses.
- Demo debug logs are opt-in only (`--debug-log` or `N00B_WIDGET_DEMO_LOG`).

## Roadmap Waves

### Wave 3 - Layout Containers

- `box`
- `grid`
- `split`
- `scroll`
- `stack`
- `tabs`

### Wave 4 - Text And Content

- `text`
- `editor`
- `header`
- `footer`
- `statusbar`

### Wave 5 - Overlays And Modals

- `modal`
- `tooltip`
- `toast`
- `collapsible`
- `accordion`
- `menu`
- `commandpalette`

### Wave 6 - Data Input

- `slider`
- `maskedinput`
- `timepicker`
- `calendar`
- `colorpicker`

### Wave 7 - Data Visualization

- `sparkline`
- `chart`
- `plot`
- `digits`
- `gradient`

### Wave 8 - Rich Content

- `image`
- `canvas`
- `asciiart`
- `fontify`
- `spinner`
- `log`

### Wave 9 - Complex Widgets

- `tree`
- `filebrowser`
- `terminal`
