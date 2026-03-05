# n00b Render Library

## Overview

The display runtime is a backend-neutral scene graph:

- `n00b_plane_t` stores draw commands and widget attachments.
- `n00b_canvas_t` owns top-level planes plus one selected renderer backend.
- `n00b_canvas_render()` flattens, composites, and dispatches to backend render hooks.

The same scene can target terminal or GUI backends without changing widget code.

## Runtime Startup And Backend Selection

`n00b_init()` initializes the renderer registry (`n00b_renderer_registry_init()`), then canvas startup selects a backend at runtime.

Canonical startup API:

```c
n00b_canvas_t *canvas = n00b_alloc(n00b_canvas_t);
n00b_canvas_init(canvas,
                 .backend_name               = r"auto",
                 .backend_allow_fallback     = true,
                 .backend_allow_dynamic_load = true,
                 .backend_allow_env_override = true,
                 .output                     = stdout_topic);
```

Selection policy is implemented in `render/backend_registry.h`:

- Alias normalization:
- `tui -> ansi`
- `nc -> notcurses`
- `nullptr` / empty / `auto` -> deterministic auto list
- Auto candidate order:
- `ansi`, `gui`, `notcurses`, `stream`, `dumb`
- Optional environment override:
- `N00B_RENDERER_BACKEND` prepends the first candidate when enabled
- Explicit request behavior:
- with fallback disabled: only the requested candidate is tried
- with fallback enabled: auto candidates are appended after the explicit request

`gui` is a portable alias to the built native window backend:

- macOS: `cocoa`
- Linux/Unix: `x11` (when built)

## Canvas Lifecycle And Ownership

Current lifecycle API (`include/display/render/canvas.h`):

```c
void n00b_canvas_init(n00b_canvas_t *c, ...);
void n00b_canvas_deinit(n00b_canvas_t *c);
void n00b_canvas_destroy(n00b_canvas_t *c);
```

Ownership contract:

- `n00b_canvas_init` initializes a pre-allocated canvas.
- `n00b_canvas_deinit` tears down backend/list resources without freeing `c`.
- `n00b_canvas_destroy` is heap convenience (`deinit` + `n00b_free(c)`).

Use `deinit` for stack/embedded canvases and `destroy` for heap canvases.

## Render Pipeline

`n00b_canvas_render(c)` runs:

1. refresh backend size and pixel metrics (unless fixed by `n00b_canvas_resize`)
2. rerender dirty widget planes
3. flatten scene hierarchy into composite entries
4. run backend plane renderer
5. flush backend output

Helpers:

- `n00b_canvas_add_plane`, `n00b_canvas_remove_plane`
- `n00b_canvas_invalidate`
- `n00b_canvas_flush`
- `n00b_canvas_alt_screen_enter`, `n00b_canvas_alt_screen_leave`

## Direct Vtable Path (Harness/Tests)

Direct vtable startup remains supported for deterministic tests:

```c
n00b_canvas_t canvas = {0};
n00b_canvas_init(&canvas, .vtable = &n00b_renderer_stream);
/* ... */
n00b_canvas_deinit(&canvas);
```

For user-facing tools and app startup, backend-name policy (`.backend_name`) is preferred.

## Backends In Current Tree

Built-in always available:

- `stream`
- `ansi`
- `dumb`

Optional by build/platform:

- `notcurses`
- `x11`
- `cocoa`

All backends implement `n00b_renderer_vtable_t` from `render/backend.h`.

## Diagnostics Expectations

Default display startup paths should not create hardcoded debug files. Tools expose explicit opt-in diagnostics flags/environment variables when logs are needed.
