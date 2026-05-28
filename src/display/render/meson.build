# Render engine — cell / plane / box / canvas plus the various
# backends (ANSI, stream, dumb) and the Cocoa init bridge.

n00b_display_render_src = files(
    'cell.c',
    'border_themes.c',
    'plane.c',
    'box.c',
    'composite.c',
    'canvas.c',
    'backend_services.c',
    'backend_stream.c',
    'backend_ansi.c',
    'backend_ansi_inline.c',
    'ansi_sgr.c',
    'terminal_input.c',
    'cocoa_input.c',
    'cocoa_bridge_contracts.c',
    'backend_dumb.c',
    'draw_cmd.c',
    'font_metrics.c',
    'backend_registry.c',
    'cocoa_init_bridge.c',
)

if conf_x11
    n00b_display_render_src += files(
        'x11_backend_contracts.c',
        'backend_x11.c',
    )
endif
