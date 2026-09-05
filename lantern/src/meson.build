ir_sources = files(
    'ir_annot.c',
    'ir_ctx.c',
    'ir_diag.c',
    'ir_diag_render.c',
    'ir_func.c',
    'ir_ops.c',
    'ir_profile.c',
    'ir_span.c',
)

ir_lib = library(
    'ir',
    ir_sources,
    dependencies: typehash_dep,
    include_directories: ir_inc,
    install: true,
)

ir_dep = declare_dependency(
    link_with: ir_lib,
    dependencies: typehash_dep,
    include_directories: ir_inc,
)
