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
    ['main.c', 'driver.c', 'preprocessor.c', 'rich_string.c', 'global_metadata.c',
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

test(
    'preprocessor',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'preprocessor.sh',
           n00bb_exe.full_path(), storage_interface_writer.full_path()],
    env: ['N00BB_STACK_OBJECT_LIMIT_BYTES='
        + get_option('stack_object_limit_bytes').to_string()],
    timeout: 60,
)
