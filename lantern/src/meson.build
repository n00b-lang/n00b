ir_sources = files(
    'ir_analyze.c',
    'ir_annot.c',
    'ir_cli.c',
    'ir_ctx.c',
    'ir_diag.c',
    'ir_diag_render.c',
    'ir_func.c',
    'ir_ops.c',
    'ir_parse.c',
    'ir_pass.c',
    'ir_print.c',
    'ir_profile.c',
    'ir_rule.c',
    'ir_derive.c',
    'ir_layout.c',
    'ir_pdg.c',
    'ir_promote.c',
    'ir_rd.c',
    'ir_structure.c',
    'ir_verify.c',
    'ir_rules_default.c',
    'ir_sb.c',
    'ir_span.c',
)

ir_lib = library(
    'ir',
    ir_sources,
    dependencies: [typehash_dep, fe_dep],
    include_directories: ir_inc,
    install: true,
)

ir_dep = declare_dependency(
    link_with: ir_lib,
    dependencies: [typehash_dep, fe_dep],
    include_directories: ir_inc,
)

ir_exe = executable(
    'ir',
    'ir_main.c',
    dependencies: ir_dep,
    install: true,
)
