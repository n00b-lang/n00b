embed = find_program('python3')
embed_script = meson.project_source_root() / 'tools' / 'embed_text.py'

annot_bnf_h = custom_target(
    'annot_bnf.h',
    input: 'parse/annot.bnf',
    output: 'annot_bnf.h',
    command: [embed, embed_script, '@INPUT@', '@OUTPUT@', 'ANNOT_BNF'],
)

infer_bnf_h = custom_target(
    'infer_bnf.h',
    input: 'parse/infer.bnf',
    output: 'infer_bnf.h',
    command: [embed, embed_script, '@INPUT@', '@OUTPUT@', 'INFER_BNF'],
)

typelang_bnf_h = custom_target(
    'typelang_bnf.h',
    input: 'th/typelang.bnf',
    output: 'typelang_bnf.h',
    command: [embed, embed_script, '@INPUT@', '@OUTPUT@', 'TYPELANG_BNF'],
)

ir_bnf_h = custom_target(
    'ir_bnf.h',
    input: 'ir/ir.bnf',
    output: 'ir_bnf.h',
    command: [embed, embed_script, '@INPUT@', '@OUTPUT@', 'IR_BNF'],
)

ir_generated = [annot_bnf_h, infer_bnf_h, typelang_bnf_h, ir_bnf_h]

th_sources = files(
    'th/typehash.c',
    'th/access.c',
    'th/error.c',
    'th/infer.c',
    'th/print.c',
)

if have_nfc
    th_sources += files('th/nfc_utf8proc.c')
endif

lib_sources = files(
    'lib/cache.c',
    'lib/hash.c',
    'lib/string.c',
)

parse_sources = files(
    'parse/annot_args.c',
    'parse/annot_grammar.c',
    'parse/annot_walk.c',
    'parse/annotation.c',
    'parse/bnf.c',
    'parse/bnf_print.c',
    'parse/commander.c',
    'parse/grammar.c',
    'parse/infer.c',
    'parse/infer_eval.c',
    'parse/forest.c',
    'parse/tree.c',
    'parse/pwz.c',
    'parse/rewrite.c',
    'parse/symtab.c',
    'parse/token.c',
    'parse/tree_dump.c',
    'parse/tree_print.c',
    'parse/tree_util.c',
    'parse/tree_visit.c',
    'parse/unicode_class.c',
)

scanner_sources = files(
    'scanner/lex.c',
    'scanner/lex_build.c',
    'scanner/scan_builtins.c',
    'scanner/scanner.c',
    'scanner/token_stream.c',
    'scanner/tokenizer.c',
    'scanner/tokenizers.c',
)

ir_sources = files(
    'ir/analyze.c',
    'ir/annot.c',
    'ir/build.c',
    'ir/cli.c',
    'ir/ctx.c',
    'ir/derive.c',
    'ir/diag.c',
    'ir/diag_render.c',
    'ir/doms.c',
    'ir/fmt.c',
    'ir/func.c',
    'ir/grammar.c',
    'ir/layout.c',
    'ir/ops.c',
    'ir/pass.c',
    'ir/pdg.c',
    'ir/print.c',
    'ir/profile.c',
    'ir/promote.c',
    'ir/rd.c',
    'ir/rule.c',
    'ir/rules_default.c',
    'ir/span.c',
    'ir/structure.c',
    'ir/symcheck.c',
    'ir/tree_pass.c',
    'ir/type_diags.c',
    'ir/typelang.c',
    'ir/verify.c',
    'ir/walk.c',
)

ir_lib = library(
    'ir',
    th_sources + lib_sources + parse_sources + scanner_sources + ir_sources
        + ir_generated,
    dependencies: lantern_deps,
    include_directories: lantern_inc,
    install: true,
)

ir_dep = declare_dependency(
    link_with: ir_lib,
    dependencies: lantern_deps,
    include_directories: lantern_inc,
)

ir_public_dep = declare_dependency(
    link_with: ir_lib,
    include_directories: lantern_inc,
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
install_headers('../include/core.h')
install_headers('../include/th/typehash.h', subdir: 'th')

foreach d : ['lib', 'parse', 'scanner', 'unicode']
    install_subdir('../include' / d, install_dir: get_option('includedir'))
endforeach

pkg = import('pkgconfig')
pkg.generate(
    ir_lib,
    description: 'A typed intermediate representation with a grammar front end',
)

ir_exe = executable(
    'ir',
    'ir/main.c',
    dependencies: ir_dep,
    install: true,
)

pp_grammar_dir = get_option('prefix') / get_option('datadir') / 'ir/grammars'

install_subdir(
    '../grammars',
    install_dir: get_option('datadir') / 'ir',
    strip_directory: false,
)

pp_exe = executable(
    'pp',
    'pp/main.c',
    dependencies: ir_dep,
    c_args: ['-DPP_GRAMMAR_DIR="' + pp_grammar_dir + '"'],
    install: true,
)
