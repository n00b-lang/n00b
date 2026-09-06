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
    'ir_doms.c',
    'ir_verify.c',
    'ir_rules_default.c',
    'ir_fmt.c',
    'ir_span.c',
    'ir_typelang.c',
    'ir_grammar.c',
    'ir_build.c',
    'ir_walk.c',
    'ir_tree_pass.c',
    'ir_symcheck.c',
    'ir_type_diags.c',
)

ir_lib = library(
    'ir',
    typehash_sources + fe_sources + ir_sources
        + fe_generated + [typelang_bnf_h, ir_bnf_h],
    dependencies: th_deps,
    include_directories: [ir_inc, fe_inc] + th_inc,
    install: true,
)

ir_dep = declare_dependency(
    link_with: ir_lib,
    dependencies: th_deps,
    include_directories: [ir_inc, fe_inc] + th_inc,
)

ir_public_headers = files(
    '../include/ir/libir.h',
    '../include/ir/ir.h',
    '../include/ir/ir_ops.def',
    '../include/ir/ir_diags.def',
    '../include/ir/diag.h',
    '../include/ir/text.h',
    '../include/ir/verify.h',
    '../include/ir/pass.h',
    '../include/ir/tree_pass.h',
    '../include/ir/rule.h',
    '../include/ir/layout.h',
    '../include/ir/walk.h',
    '../include/ir/doms.h',
    '../include/ir/pdg.h',
    '../include/ir/region_walk.h',
    '../include/ir/analyze.h',
    '../include/ir/typelang.h',
    '../include/ir/grammar.h',
)

install_headers(ir_public_headers, subdir: 'ir')

pkg = import('pkgconfig')
pkg.generate(
    ir_lib,
    description: 'A typed intermediate representation with a grammar front end',
)

ir_public_dep = declare_dependency(
    link_with: ir_lib,
    include_directories: [ir_inc, fe_inc] + th_inc,
)

ir_exe = executable(
    'ir',
    'ir_main.c',
    dependencies: ir_dep,
    install: true,
)
