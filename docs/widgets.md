# Widget Status And Roadmap

## Current Implementation Status

The widget runtime is active and integrated with the display pipeline (`plane + widget_vtable + widget_data`).

Implemented widgets:

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
- `box`
- `grid`
- `split`
- `scroll`
- `tabs`
- `zstack`

Representative widget tests live under `test/unit/test_{label,button,checkbox,input,switch,radio,list_widget,selectionlist,breadcrumb,link,grid,split,scroll,tabs,zstack}.c`.

## Runtime Integration Expectations

- Widgets render through planes attached to a canvas.
- Backend selection is runtime policy driven (`n00b_canvas_init(... .backend_name=...)`).
- Focus/mouse/key dispatch is shared across terminal and GUI-capable backends.
- `src/tools/widget_demo.c` is the practical end-to-end demo entrypoint.

## Usage Notes

- Preferred startup path for demos/apps: backend-name selection (`auto`, `tui`, `gui`, `stream`, and explicit backend names).
- Direct vtable startup remains useful for deterministic tests/harnesses.
- Demo debug logs are opt-in only (`--debug-log` or `N00B_WIDGET_DEMO_LOG`).
- `widget_demo --widget zstack` lays out the zstack scene beside a dedicated control column, so `bring_to_front` and `send_to_back` stay clickable while the centered overlay card is reordered.

## Roadmap Waves

These waves match `plans/notes/widget-port-priority.md` and cover the 38 missing
prototype widgets after treating `list` as represented by `list_widget`.

### Wave 1 - Foundation And Layout Unlock

- `text`

### Wave 2 - Form And Shell Primitives

- `header`
- `footer`
- `statusbar`
- `select`
- `maskedinput`
- `slider`
- `calendar`
- `timepicker`

### Wave 3 - Overlay And Command Interaction

- `modal`
- `tooltip`
- `toast`
- `collapsible`
- `accordion`
- `menu`
- `commandpalette`

### Wave 4 - Data Visualization And Decorative Status

- `sparkline`
- `chart`
- `plot`
- `digits`
- `gradient`
- `spinner`
- `log`

### Wave 5 - Backend-Heavy And Complex Integrations

- `tree`
- `filebrowser`
- `editor`
- `terminal`
- `image`
- `canvas`
- `asciiart`
- `asciify`
- `fontify`
- `colorpicker`

Notes:

- The prototype `stack` port ships in Athens as `zstack` because `stack` collides with an existing ADT macro.
- `select` is explicitly tracked as missing from Athens and included in Wave 2.
- `asciify` is explicitly tracked as missing from Athens and deferred to Wave 5.
