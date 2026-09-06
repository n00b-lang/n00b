typelang_bnf_h = custom_target(
    'typelang_bnf.h',
    input: '../th/typelang.bnf',
    output: 'typelang_bnf.h',
    command: [
        find_program('python3'),
        meson.project_source_root() / 'tools' / 'embed_text.py',
        '@INPUT@',
        '@OUTPUT@',
        'TYPELANG_BNF',
    ],
)

ir_bnf_h = custom_target(
    'ir_bnf.h',
    input: 'ir.bnf',
    output: 'ir_bnf.h',
    command: [
        find_program('python3'),
        meson.project_source_root() / 'tools' / 'embed_text.py',
        '@INPUT@',
        '@OUTPUT@',
        'IR_BNF',
    ],
)

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
    'ir_typelang.c',
    'ir_grammar.c',
    'ir_walk.c',
)

ir_lib = library(
    'ir',
    ir_sources + [typelang_bnf_h, ir_bnf_h],
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
