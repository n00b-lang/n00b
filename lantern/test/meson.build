ir_core_test = executable(
    'ir_core_test',
    'ir_core_test.c',
    dependencies: ir_dep,
)

test('ir-core', ir_core_test)

ir_span_test = executable(
    'ir_span_test',
    'ir_span_test.c',
    dependencies: ir_dep,
)

test('ir-spans', ir_span_test)

ir_text_test = executable(
    'ir_text_test',
    'ir_text_test.c',
    dependencies: ir_dep,
)

test(
    'ir-text',
    ir_text_test,
    args: [meson.current_source_dir() / 'golden'],
)

ir_rule_test = executable(
    'ir_rule_test',
    'ir_rule_test.c',
    dependencies: ir_dep,
)

test('ir-rules', ir_rule_test)

ir_typing_test = executable(
    'ir_typing_test',
    'ir_typing_test.c',
    dependencies: ir_dep,
)

test('ir-typing', ir_typing_test)

ir_verify_test = executable(
    'ir_verify_test',
    'ir_verify_test.c',
    dependencies: ir_dep,
)

test('ir-verify', ir_verify_test)

ir_grammar_test = executable(
    'ir_grammar_test',
    'ir_grammar_test.c',
    dependencies: ir_dep,
)

test('ir-grammar', ir_grammar_test)

ir_layout_test = executable(
    'ir_layout_test',
    'ir_layout_test.c',
    dependencies: ir_dep,
)

test('ir-layout', ir_layout_test)

ir_promote_test = executable(
    'ir_promote_test',
    'ir_promote_test.c',
    dependencies: ir_dep,
)

test('ir-promote', ir_promote_test)

ir_structure_test = executable(
    'ir_structure_test',
    'ir_structure_test.c',
    dependencies: ir_dep,
)

test('ir-structure', ir_structure_test)

ir_cli_test = executable(
    'ir_cli_test',
    'ir_cli_test.c',
    dependencies: ir_dep,
)

test('ir-cli', ir_cli_test)

ir_pdg_test = executable(
    'ir_pdg_test',
    'ir_pdg_test.c',
    dependencies: ir_dep,
)

test('ir-pdg', ir_pdg_test)

ir_typelang_test = executable(
    'ir_typelang_test',
    'ir_typelang_test.c',
    dependencies: ir_dep,
)

test(
    'ir-typelang',
    ir_typelang_test,
    args: [meson.project_source_root() / 'th' / 'vectors.tsv'],
)

ir_scale_test = executable(
    'ir_scale_test',
    'ir_scale_test.c',
    dependencies: ir_dep,
)

test('ir-scale', ir_scale_test)

ir_diag_test = executable(
    'ir_diag_test',
    'ir_diag_test.c',
    dependencies: ir_dep,
)

test(
    'ir-diagnostics',
    ir_diag_test,
    args: [meson.current_source_dir() / 'golden'],
)

test(
    'ir-gates',
    find_program('sh'),
    args: [
        files('gates.sh'),
        meson.project_source_root(),
        ir_diag_test,
        ir_text_test,
        ir_typing_test,
        ir_verify_test,
        ir_layout_test,
        ir_promote_test,
        ir_structure_test,
        ir_pdg_test,
    ],
    depends: [
        ir_diag_test,
        ir_text_test,
        ir_typing_test,
        ir_verify_test,
        ir_layout_test,
        ir_promote_test,
        ir_structure_test,
        ir_pdg_test,
    ],
)
