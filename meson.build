project(
    'n00bb',
    'c', 'cpp',
    version: '0.1.0',
    meson_version: '>=1.1.0',
    default_options: [
        'c_std=c2x',
        'warning_level=2',
    ],
)

lantern = dependency('ir', method: 'pkg-config', required: true)
llvm = dependency('llvm', method: 'config-tool',
                  modules: ['core', 'bitwriter', 'bitreader'], required: true)
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
     'abi_layout.c', 'abi_record_layout.c', 'abi_directory.c',
     'abi_container.c', 'abi_index_patch.c', 'abi_emit.c',
     'abi_emit_strings.c',
     'abi_type_index.c', 'abi_type_rows.c', 'abi_type_layout_rows.c',
     'abi_type_signatures.c',
     'llvm_backend.c',
     'global_metadata.c',
     'storage_selection.c'],
    dependencies: [lantern, llvm],
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
    ['tests/abi_layout.c', 'abi_layout.c', 'abi_record_layout.c'],
    dependencies: lantern,
)

test('abi-layout', abi_layout_test)

abi_record_layout_test = executable(
    'abi-record-layout-test',
    ['tests/abi_record_layout.c', 'abi_record_layout.c'],
    dependencies: lantern,
)

test('abi-record-layout', abi_record_layout_test)

abi_directory_test = executable(
    'abi-directory-test',
    ['tests/abi_directory.c', 'abi_directory.c'],
    dependencies: lantern,
)

test('abi-directory', abi_directory_test)

abi_container_test = executable(
    'abi-container-test',
    ['tests/abi_container.c', 'abi_container.c', 'abi_directory.c'],
    dependencies: lantern,
)

test('abi-container', abi_container_test)

abi_index_patch_test = executable(
    'abi-index-patch-test',
    ['tests/abi_index_patch.c', 'abi_index_patch.c'],
    dependencies: lantern,
)

test('abi-index-patch', abi_index_patch_test)

abi_emit_test = executable(
    'abi-emit-test',
    ['tests/abi_emit.c', 'abi_emit.c', 'abi_emit_strings.c',
     'abi_type_index.c', 'abi_type_rows.c', 'abi_type_layout_rows.c',
     'abi_type_signatures.c',
     'abi_container.c',
     'abi_directory.c'],
    dependencies: lantern,
)

test('abi-emit', abi_emit_test)

abi_emit_unit_test = executable(
    'abi-emit-unit-test',
    ['tests/abi_emit_unit.c', 'abi_emit.c', 'abi_emit_strings.c',
     'abi_type_index.c', 'abi_type_rows.c', 'abi_type_layout_rows.c',
     'abi_type_signatures.c',
     'abi_container.c',
     'abi_directory.c', 'abi_string.c'],
    dependencies: lantern,
)

test('abi-emit-unit', abi_emit_unit_test)

abi_emit_strings_test = executable(
    'abi-emit-strings-test',
    ['tests/abi_emit_strings.c', 'abi_emit_strings.c', 'abi_string.c'],
    dependencies: lantern,
)

test('abi-emit-strings', abi_emit_strings_test)

abi_type_rows_test = executable(
    'abi-type-rows-test',
    ['tests/abi_type_rows.c', 'abi_type_rows.c', 'abi_type_index.c',
     'abi_emit_strings.c', 'abi_string.c'],
    dependencies: lantern,
)

test('abi-type-rows', abi_type_rows_test)

abi_type_layout_rows_test = executable(
    'abi-type-layout-rows-test',
    ['tests/abi_type_layout_rows.c', 'abi_type_layout_rows.c',
     'abi_type_rows.c', 'abi_type_index.c', 'abi_emit_strings.c'],
    dependencies: lantern,
)

test('abi-type-layout-rows', abi_type_layout_rows_test)

abi_type_signatures_test = executable(
    'abi-type-signatures-test',
    ['tests/abi_type_signatures.c', 'abi_type_signatures.c',
     'abi_type_rows.c', 'abi_type_index.c', 'abi_emit_strings.c',
     'abi_string.c'],
    dependencies: lantern,
)

test('abi-type-signatures', abi_type_signatures_test)

abi_type_index_test = executable(
    'abi-type-index-test',
    ['tests/abi_type_index.c', 'abi_type_index.c'],
    dependencies: lantern,
)

test('abi-type-index', abi_type_index_test)

llvm_backend_test = executable(
    'llvm-backend-test',
    ['tests/llvm_backend.c', 'llvm_backend.c'],
    dependencies: [lantern, llvm],
)

test('llvm-backend', llvm_backend_test)

test(
    'preprocessor',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'preprocessor.sh',
           n00bb_exe.full_path(), storage_interface_writer.full_path()],
    env: ['N00BB_STACK_OBJECT_LIMIT_BYTES='
        + get_option('stack_object_limit_bytes').to_string()],
    timeout: 120,
)
