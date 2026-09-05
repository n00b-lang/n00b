fe_inc = [include_directories('include'), tdesc_inc]

fe_sources = files(
    'src/lib/alloc.c',
    'src/lib/string.c',
    'src/parse/annot_walk.c',
    'src/parse/annotation.c',
    'src/parse/bnf.c',
    'src/parse/commander.c',
    'src/parse/grammar.c',
    'src/parse/parse_forest.c',
    'src/parse/parse_tree.c',
    'src/parse/pwz.c',
    'src/parse/symtab.c',
    'src/parse/token.c',
    'src/parse/tree_dump.c',
    'src/parse/tree_util.c',
    'src/parse/tree_visit.c',
    'src/parse/unicode_class.c',
    'src/scanner/scan_builtins.c',
    'src/scanner/scanner.c',
    'src/scanner/token_stream.c',
)

fe_lib = library(
    'irfe',
    fe_sources,
    include_directories: fe_inc,
    install: true,
)

fe_dep = declare_dependency(
    link_with: fe_lib,
    include_directories: fe_inc,
)

subdir('test')
