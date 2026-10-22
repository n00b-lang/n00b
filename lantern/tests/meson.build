test_dep = declare_dependency(include_directories: include_directories('.'))
tdep     = [ir_dep, test_dep]

d_ir    = meson.current_source_dir() / 'ir'
d_parse = meson.current_source_dir() / 'parse'
d_gram  = meson.project_source_root() / 'grammars'
d_samp  = meson.current_source_dir() / 'samples'
d_fmt   = meson.current_source_dir() / 'format'
golden  = d_ir / 'golden'

suites = [
    ['ir-core',             'ir/core.c',             {}],
    ['ir-lower',            'ir/lower.c',            {}],
    ['ir-lifetime',         'ir/lifetime.c',         {}],
    ['ir-storage-origin',   'ir/storage_origin.c',   {}],
    ['ir-exec-graph',       'ir/exec_graph.c',       {}],
    ['ir-shape',            'ir/shape.c',            {}],
    ['ir-spans',            'ir/span.c',             {}],
    ['ir-rules',            'ir/rule.c',             {}],
    ['ir-typing',           'ir/typing.c',           {}],
    ['ir-ranges',           'ir/ranges.c',           {}],
    ['ir-value-numbers',    'ir/vn.c',               {}],
    ['ir-abcd',             'ir/abcd.c',             {}],
    ['ir-verify',           'ir/verify.c',           {}],
    ['ir-grammar',          'ir/grammar.c',          {}],
    ['ir-layout',           'ir/layout.c',           {}],
    ['ir-promote',          'ir/promote.c',          {}],
    ['ir-structure',        'ir/structure.c',        {}],
    ['ir-cli',              'ir/cli.c',              {}],
    ['ir-pdg',              'ir/pdg.c',              {}],
    ['ir-walk',             'ir/walk.c',             {}],
    ['ir-typemap',          'ir/typemap.c',          {'args': [d_ir]}],
    ['ir-text',             'ir/text.c',      {'args': [golden, '25']}],
    ['ir-diagnostics',      'ir/diag.c',             {'args': [golden]}],
    ['ir-typelang',         'ir/typelang.c',
     {'args': [d_ir / 'vectors.tsv']}],
    ['ir-grammar-accept',   'ir/grammar_accept.c',
     {'args': [golden / 'canonical.ir']}],

    ['lib-macros',          'lib/macros.c',          {}],
    ['lib-byteorder',       'lib/byteorder.c',       {}],
    ['lib-cache',           'lib/cache.c',           {}],
    ['lib-diagnostics',     'lib/diagnostic.c',      {}],
    ['lib-module',          'lib/module.c',          {}],
    ['lib-snapshot-inventory', 'lib/snapshot_inventory.c', {}],
    ['lib-target',          'lib/target.c',          {}],
    ['lib-target-call',     'lib/target_call.c',     {}],
    ['lib-indexed-table',   'lib/indexed_table.c',   {}],
    ['lib-link-unit',       'lib/link_unit.c',       {}],
    ['lib-work-queue',      'lib/work_queue.c',      {}],
    ['lib-arena',           'lib/arena.c',           {}],
    ['lib-persist-arena',   'lib/persist_arena.c',   {}],
    ['lib-object-archive',  'lib/object_archive.c',  {}],
    ['lib-native-archive',  'lib/native_archive.c',
     {'args': [ir_lib.full_path()]}],
    ['lib-object-emit',     'lib/object_emit.c',     {}],
    ['lib-object-section',  'lib/object_section.c',  {}],
    ['lib-variant',         'lib/variant.c',         {}],
    ['lib-result',          'lib/result.c',          {}],
    ['logic-csp-domain',    'logic/clpfd_domain.c',  {}],
    ['logic-csp-store',     'logic/clpfd_store.c',   {}],
    ['logic-datalog',       'logic/datalog.c',       {}],
    ['logic-program',       'logic/program.c',       {}],

    ['th-cycles',           'th/cycle.c',            {}],
    ['th-nest-tmp',         'th/nest_tmp.c',         {}],
    ['th-inference-cycles', 'th/infer_cycle.c',      {}],
    ['th-constraints',      'th/constraint.c',       {}],
    ['th-environment',      'th/environment.c',     {}],

    ['fe-pwz',              'parse/pwz.c',           {}],
    ['fe-ambiguity',        'parse/ambiguity.c',     {}],
    ['fe-commander',        'parse/commander.c',     {}],
    ['fe-compose',          'parse/compose.c',       {}],
    ['fe-tree-print',       'parse/tree_print.c',    {}],
    ['fe-infer',            'parse/infer.c',         {}],
    ['fe-symtab-binding',   'parse/symtab_binding.c', {}],
    ['fe-rewrite',          'parse/rewrite.c',       {}],
    ['fe-generic-print',    'parse/generic_print.c', {}],
    ['fe-tokenizer',        'scanner/tokenizer.c',   {}],
    ['fe-lex',              'scanner/lex.c',         {}],
    ['fe-bnf',              'parse/bnf.c',        {'workdir': d_parse}],
    ['fe-annot',            'parse/annot.c',      {'workdir': d_parse}],
    ['fe-bnf-print',        'parse/bnf_print.c',  {'workdir': d_parse}],
    ['fe-grammars',         'parse/grammars.c',
     {'args': [d_gram, d_samp], 'timeout': 120}],
    ['fe-format',           'parse/format.c',
     {'args': [d_gram, d_fmt], 'timeout': 120}],
]

built = {}

foreach s : suites
    exe = executable(
        s[1].replace('/', '_').replace('.c', ''),
        s[1],
        dependencies: tdep,
    )

    test(s[0], exe, kwargs: s[2])
    built += {s[0]: exe}
endforeach

ambiguity_cli = executable(
    'ambiguity-audit',
    'parse/ambiguity_cli.c',
    dependencies: tdep,
)

ir_public_test = executable(
    'ir_public',
    'ir/public.c',
    dependencies: [ir_public_dep, test_dep],
)

test('ir-public', ir_public_test)

ir_scale_test = executable('ir_scale', 'ir/scale.c', dependencies: tdep)
lexer_perf_test = executable('scanner_lex_perf', 'scanner/lex_perf.c',
                             dependencies: tdep)

benchmark('ir-scale', ir_scale_test, timeout: 180)
benchmark('ir-text-soak', built['ir-text'], args: [golden])
benchmark('lexer-throughput', lexer_perf_test, timeout: 180)

bnfcheck = executable('bnfcheck', 'parse/bnfcheck.c', dependencies: ir_dep)

gate_bins = [
    built['ir-diagnostics'],
    built['ir-text'],
    built['ir-typing'],
    built['ir-verify'],
    built['ir-layout'],
    built['ir-promote'],
    built['ir-structure'],
    built['ir-pdg'],
    built['ir-lower'],
]

benchmark(
    'ir-gates',
    find_program('sh'),
    args: [files('gates.sh'), meson.project_source_root()] + gate_bins,
    depends: gate_bins,
    timeout: 180,
)
