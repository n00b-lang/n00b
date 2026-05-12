# Regex engine port (resharp-c → n00b). See ~/dd/n00b_regex_port_plan.md.
#
# Each leaf subdirectory declares its own `n00b_text_regex_<sub>_src` files()
# list; this file aggregates them plus the single public.c into
# `n00b_text_regex_src`, which the parent src/text/meson.build folds into
# `n00b_text_src`.

subdir('common')
subdir('algebra')
subdir('parser')
subdir('engine')
subdir('stream')

n00b_text_regex_src = (
    n00b_text_regex_common_src
    + n00b_text_regex_algebra_src
    + n00b_text_regex_parser_src
    + n00b_text_regex_engine_src
    + n00b_text_regex_stream_src
    + files('public.c')
)
