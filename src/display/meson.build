# Display module — render engine, widgets, table. Each subdir has its
# own meson.build; this file aggregates them plus the top-level files
# (layout + hexdump + widget core + event loop).

subdir('render')
subdir('widgets')
subdir('table')

n00b_display_src = files(
    # Top-level display sources (not inside render/, widgets/, or table/)
    'layout.c',
    'hexdump.c',
    'widget.c',
    'focus.c',
    'event_loop.c',
    'mouse.c',
) + (
    n00b_display_render_src +
    n00b_display_widgets_src +
    n00b_display_table_src
)
