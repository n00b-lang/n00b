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

test('ir-text', ir_text_test)

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
    ],
    depends: [ir_diag_test, ir_text_test],
)
