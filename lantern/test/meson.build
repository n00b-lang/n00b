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

test(
    'ir-gates',
    find_program('sh'),
    args: [files('gates.sh'), meson.project_source_root()],
)
