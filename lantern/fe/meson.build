annot_bnf_h = custom_target(
    'annot_bnf.h',
    input: 'annot.bnf',
    output: 'annot_bnf.h',
    command: [
        find_program('python3'),
        meson.project_source_root() / 'tools' / 'embed_text.py',
        '@INPUT@',
        '@OUTPUT@',
        'ANNOT_BNF',
    ],
)

infer_bnf_h = custom_target(
    'infer_bnf.h',
    input: 'infer.bnf',
    output: 'infer_bnf.h',
    command: [
        find_program('python3'),
        meson.project_source_root() / 'tools' / 'embed_text.py',
        '@INPUT@',
        '@OUTPUT@',
        'INFER_BNF',
    ],
)

fe_sources = files(
    'src/lib/hash.c',
    'src/lib/string.c',
    'src/parse/annot_walk.c',
    'src/parse/annotation.c',
    'src/parse/bnf.c',
    'src/parse/bnf_print.c',
    'src/parse/commander.c',
    'src/parse/annot_args.c',
    'src/parse/annot_grammar.c',
    'src/parse/grammar.c',
    'src/parse/infer.c',
    'src/parse/infer_eval.c',
    'src/parse/parse_forest.c',
    'src/parse/parse_tree.c',
    'src/parse/pwz.c',
    'src/parse/rewrite.c',
    'src/parse/symtab.c',
    'src/parse/token.c',
    'src/parse/tree_dump.c',
    'src/parse/tree_print.c',
    'src/parse/tree_util.c',
    'src/parse/tree_visit.c',
    'src/parse/unicode_class.c',
    'src/scanner/scan_builtins.c',
    'src/scanner/scanner.c',
    'src/scanner/token_stream.c',
    'src/scanner/tokenizer.c',
    'src/scanner/tokenizers.c',
)

fe_generated = [annot_bnf_h, infer_bnf_h]

install_headers('include/ncc.h')

foreach d : ['lib', 'parse', 'scanner', 'unicode']
    install_subdir('include' / d, install_dir: get_option('includedir'))
endforeach
