project(
    'n00bb',
    'c',
    version: '0.1.0',
    meson_version: '>=1.1.0',
    default_options: [
        'c_std=c2x',
        'warning_level=2',
    ],
)

lantern = dependency('ir', method: 'pkg-config', required: true)
grammar_path = meson.current_source_dir() / 'grammar' / 'n00b.bnf'

add_project_arguments(
    '-DN00BB_GRAMMAR_PATH="' + grammar_path + '"',
    '-DN00BB_STACK_OBJECT_LIMIT_BYTES='
        + get_option('stack_object_limit_bytes').to_string(),
    language: 'c',
)

install_data(
    'grammar/n00b.bnf',
    'grammar/n00b.lex',
    install_dir: get_option('datadir') / 'n00bb',
)

n00bb_exe = executable(
    'n00bb',
    ['main.c', 'driver.c', 'preprocessor.c', 'rich_string.c', 'abi_string.c',
     'abi_layout.c', 'abi_record_layout.c',
     'global_metadata.c',
     'storage_selection.c'],
    dependencies: lantern,
    install: true,
)

storage_interface_writer = executable(
    'storage-interface-writer',
    ['tests/storage_interface_writer.c', 'global_metadata.c'],
    dependencies: lantern,
)

storage_selection_test = executable(
    'storage-selection-test',
    ['tests/storage_selection.c', 'storage_selection.c'],
    dependencies: lantern,
)

test('storage-selection', storage_selection_test)

typehash_abi_test = executable(
    'typehash-abi-test',
    'tests/typehash_abi.c',
    dependencies: lantern,
)

test('typehash-abi', typehash_abi_test)

abi_string_value_test = executable(
    'abi-string-value-test',
    ['tests/abi_string_value.c', 'abi_string.c'],
)

test('abi-string-value', abi_string_value_test)

abi_layout_test = executable(
    'abi-layout-test',
    ['tests/abi_layout.c', 'abi_layout.c'],
    dependencies: lantern,
)

test('abi-layout', abi_layout_test)

abi_record_layout_test = executable(
    'abi-record-layout-test',
    ['tests/abi_record_layout.c', 'abi_record_layout.c'],
    dependencies: lantern,
)

test('abi-record-layout', abi_record_layout_test)

test(
    'preprocessor',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'preprocessor.sh',
           n00bb_exe.full_path(), storage_interface_writer.full_path()],
    env: ['N00BB_STACK_OBJECT_LIMIT_BYTES='
        + get_option('stack_object_limit_bytes').to_string()],
    timeout: 120,
)
