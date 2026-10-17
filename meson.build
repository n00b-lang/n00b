project(
    'n00bb',
    'c', 'cpp',
    version: '0.1.0',
    meson_version: '>=1.1.0',
    default_options: [
        'buildtype=debugoptimized',
        'c_std=c2x',
        'warning_level=2',
    ],
)

lantern = dependency('ir', method: 'pkg-config', required: true)
writ = dependency('writ', method: 'pkg-config', required: true)
llvm = dependency('llvm', method: 'config-tool',
                  modules: ['core', 'bitwriter', 'bitreader', 'linker'],
                  required: true)
writ_paths = [
    '-DN00BB_WRIT_PROFILE_DIR="' + writ.get_variable(pkgconfig: 'profiledir') + '"',
    '-DN00BB_LLVM_CLANG="' + llvm.get_variable(configtool: 'bindir') / 'clang' + '"',
]
cargo = find_program('cargo', required: get_option('rust-transports'))
if cargo.found() and not get_option('rust-transports').disabled()
    writ_paths += '-DN00BB_CARGO="' + cargo.full_path() + '"'
else
    writ_paths += '-DN00BB_CARGO=""'
endif
grammar_path = meson.current_source_dir() / 'grammar' / 'n00b.bnf'

add_project_arguments(
    '-DN00BB_GRAMMAR_PATH="' + grammar_path + '"',
    '-DN00BB_STACK_OBJECT_LIMIT_BYTES='
        + get_option('stack_object_limit_bytes').to_string(),
    '-I' + (meson.current_source_dir() / 'compiler'),
    '-I' + (meson.current_source_dir() / 'abi'),
    '-I' + (meson.current_source_dir() / 'runtime'),
    language: 'c',
)

install_data(
    'grammar/n00b.bnf',
    'grammar/n00b.lex',
    install_dir: get_option('datadir') / 'n00bb',
)

runtime_abi_headers = lantern.partial_dependency(
    compile_args: true, includes: true,
)

runtime_abi_sources = ['runtime/runtime_abi.c', 'runtime/runtime_allocate.c', 'runtime/runtime_abi_io.c',
     'runtime/runtime_panic.c', 'runtime/runtime_dwarf.c', 'runtime/runtime_ffi.c',
     'runtime/runtime_unwind.c',
     'runtime/runtime_abi_box.c',
     'runtime/runtime_abi_storage.c', 'runtime/native_frame_runtime.c', 'runtime/builtin_os.c',
     'runtime/builtin_os_stw.c',
     'runtime/builtin_abi.c', 'abi/abi_directory.c', 'abi/abi_string.c']

runtime_abi_support = static_library(
    'n00b-abi-runtime', runtime_abi_sources,
    dependencies: runtime_abi_headers,
    override_options: ['optimization=3'],
    install: true,
)

runtime_abi_lto = static_library(
    'n00b-abi-runtime-lto', runtime_abi_sources,
    dependencies: runtime_abi_headers,
    override_options: ['optimization=3', 'b_lto=true'],
)
runtime_abi_bitcode = custom_target(
    'n00b-abi-runtime-bitcode',
    input: runtime_abi_lto,
    output: 'libn00b-abi-runtime.bc',
    command: [find_program('tools/merge_runtime_bitcode.sh'), '@INPUT@',
              '@OUTPUT@', find_program('llvm-ar'), find_program('llvm-link')],
    build_by_default: true,
    install: true,
    install_dir: get_option('libdir'),
)

abi_static_graph_test = executable(
    'abi-static-graph-test',
    ['tests/abi_static_graph.c', 'compiler/abi_static_graph.c'],
    dependencies: lantern,
)

add_test_setup('default', timeout_multiplier: 2, is_default: true)

test('abi-static-graph', abi_static_graph_test)


n00bb_exe = executable(
    'n00bb',
    ['compiler/main.c', 'compiler/compiler_driver.c', 'compiler/compiler_lower.c', 'compiler/compiler_buffer_borrow.c', 'compiler/compiler_layout.c', 'compiler/compiler_baked.c', 'compiler/compiler_abi_driver.c', 'compiler/compiler_object_io.c', 'compiler/compiler_cli.c', 'compiler/compiler_tu.c', 'compiler/builtin_types.c',
     'compiler/compiler_tu_range.c', 'compiler/compiler_tu_builtins.c',
     'compiler/compiler_tu_alias.c', 'compiler/compiler_tu_walk.c',
     'compiler/compiler_tu_interface.c',
     'compiler/compiler_link.c', 'compiler/compiler_link_calls.c', 'compiler/compiler_link_effects.c',
     'compiler/compiler_link_initializers.c',
     'compiler/compiler_link_lifetime.c',
     'compiler/compiler_link_traits.c',
     'compiler/compiler_link_packages.c', 'compiler/compiler_link_environment.c',
     'compiler/compiler_link_stages.c', 'compiler/driver.c',
     'compiler/enum_value.c',
     'compiler/preprocessor.c', 'compiler/rich_string.c', 'abi/abi_string.c',
     'compiler/specialize.c',
     'runtime/runtime_native_roots.c', 'compiler/compiler_native_roots.c',
     'compiler/abi_layout.c', 'compiler/abi_record_layout.c', 'compiler/abi_static_graph.c',
     'abi/abi_directory.c',
     'compiler/abi_container.c', 'compiler/abi_index_patch.c', 'compiler/abi_emit.c', 'compiler/abi_link.c',
     'compiler/abi_link_finish.c', 'compiler/abi_link_strings.c', 'compiler/abi_semantics.c',
     'compiler/abi_emit_strings.c',
     'compiler/abi_type_index.c', 'compiler/abi_type_rows.c', 'compiler/abi_type_layout_rows.c',
     'compiler/abi_enum_rows.c',
     'compiler/abi_function_rows.c', 'compiler/abi_constraint_rows.c', 'compiler/abi_storage.c',
     'compiler/abi_trait_method_rows.c',
     'compiler/abi_type_signatures.c', 'compiler/abi_decl_index.c', 'compiler/abi_declarations.c',
     'compiler/llvm_backend.c', 'compiler/llvm_native_cache.c', 'compiler/llvm_stack_maps.c',
     'compiler/native_frame_maps.c',
     'compiler/global_metadata.c',
     'compiler/storage_selection.c',
     'compiler/compiler_ffi.c'],
    dependencies: [lantern, writ, llvm],
    c_args: writ_paths,
    link_with: runtime_abi_support,
    install: true,
)

test(
    'writ-coexistence',
    executable('writ-coexistence', 'tests/writ_coexistence.c', dependencies: [lantern, writ]),
)

install_headers('abi/n00b-abi-v1.h', 'runtime/builtin_os.h', 'runtime/builtin_abi.h')

builtin_os_test = executable(
    'builtin-os-test',
    'tests/builtin_os.c',
    link_with: runtime_abi_support,
    dependencies: dependency('threads'),
)

test('builtin-os', builtin_os_test)

builtin_abi_test = executable(
    'builtin-abi-test',
    'tests/builtin_abi.c',
    link_with: runtime_abi_support,
)

test('builtin-abi', builtin_abi_test)

runtime_abi_io_test = executable(
    'runtime-abi-io-test',
    'tests/runtime_abi_io.c',
    link_with: runtime_abi_support,
)

test('runtime-abi-io', runtime_abi_io_test)

runtime_abi_test = executable(
    'runtime-abi-test',
    'tests/runtime_abi.c',
    link_with: runtime_abi_support,
    dependencies: runtime_abi_headers,
)

test('runtime-abi', runtime_abi_test)
test('runtime-abi-partial-dispatch', runtime_abi_test,
    args: ['--partial-dispatch'])
test('runtime-abi-function-parameters', runtime_abi_test,
    args: ['--function-parameters'])
test('runtime-abi-call-shape', runtime_abi_test,
    args: ['--call-shape'])

runtime_abi_scc_test = executable(
    'runtime-abi-scc-test',
    'tests/runtime_abi_scc.c',
    link_with: runtime_abi_support,
    dependencies: runtime_abi_headers,
)

test('runtime-abi-scc', runtime_abi_scc_test)
test('runtime-abi-scc-acyclic', runtime_abi_scc_test, args: ['--acyclic'])

runtime_abi_trace_test = executable(
    'runtime-abi-trace-test',
    'tests/runtime_abi_trace.c',
    link_with: runtime_abi_support,
    dependencies: runtime_abi_headers,
)

test('runtime-abi-trace', runtime_abi_trace_test)

runtime_abi_option_test = executable(
    'runtime-abi-option-test',
    'tests/runtime_abi_option.c',
    link_with: runtime_abi_support,
    dependencies: runtime_abi_headers,
)

test('runtime-abi-option', runtime_abi_option_test)

storage_selection_test = executable(
    'storage-selection-test',
    ['tests/storage_selection.c', 'compiler/storage_selection.c'],
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
    ['tests/abi_string_value.c', 'abi/abi_string.c'],
)

test('abi-string-value', abi_string_value_test)

abi_layout_test = executable(
    'abi-layout-test',
    ['tests/abi_layout.c', 'compiler/abi_layout.c', 'compiler/abi_record_layout.c',
     'compiler/builtin_types.c'],
    dependencies: lantern,
)

test('abi-layout', abi_layout_test)

test(
    'unsigned-literal-context',
    n00bb_exe,
    args: ['lower', meson.current_source_dir() / 'tests' / 'programs'
        / 'unsigned_literal_context.n'],
    suite: 'slow',
)

test(
    'unsafe-ref-cast',
    n00bb_exe,
    args: ['lower', meson.current_source_dir() / 'tests' / 'programs'
        / 'unsafe_ref_cast.n'],
    suite: 'slow',
)

abi_record_layout_test = executable(
    'abi-record-layout-test',
    ['tests/abi_record_layout.c', 'compiler/abi_record_layout.c'],
    dependencies: lantern,
)

test('abi-record-layout', abi_record_layout_test)

abi_directory_test = executable(
    'abi-directory-test',
    ['tests/abi_directory.c', 'abi/abi_directory.c'],
    dependencies: lantern,
)

test('abi-directory', abi_directory_test)

abi_container_test = executable(
    'abi-container-test',
    ['tests/abi_container.c', 'compiler/abi_container.c', 'abi/abi_directory.c'],
    dependencies: lantern,
)

test('abi-container', abi_container_test)

abi_index_patch_test = executable(
    'abi-index-patch-test',
    ['tests/abi_index_patch.c', 'compiler/abi_index_patch.c'],
    dependencies: lantern,
)

test('abi-index-patch', abi_index_patch_test)

abi_emit_test = executable(
    'abi-emit-test',
    ['tests/abi_emit.c', 'compiler/abi_emit.c', 'compiler/abi_emit_strings.c', 'abi/abi_string.c',
     'compiler/abi_layout.c', 'compiler/abi_record_layout.c',
     'compiler/abi_type_index.c', 'compiler/abi_type_rows.c', 'compiler/abi_type_layout_rows.c',
     'compiler/abi_enum_rows.c',
     'compiler/abi_function_rows.c', 'compiler/abi_constraint_rows.c', 'compiler/abi_storage.c',
     'compiler/abi_trait_method_rows.c',
     'compiler/abi_type_signatures.c', 'compiler/abi_decl_index.c',
     'compiler/abi_declarations.c',
     'compiler/abi_container.c',
     'abi/abi_directory.c'],
    dependencies: lantern,
)

test('abi-emit', abi_emit_test)

abi_emit_unit_test = executable(
    'abi-emit-unit-test',
    ['tests/abi_emit_unit.c', 'compiler/builtin_types.c',
     'compiler/abi_emit.c', 'compiler/abi_emit_strings.c', 'abi/abi_string.c',
     'compiler/abi_link.c', 'compiler/abi_link_finish.c', 'compiler/abi_link_strings.c',
     'compiler/abi_index_patch.c',
     'compiler/abi_layout.c', 'compiler/abi_record_layout.c',
     'compiler/abi_type_index.c', 'compiler/abi_type_rows.c', 'compiler/abi_type_layout_rows.c',
     'compiler/abi_enum_rows.c',
     'compiler/abi_function_rows.c', 'compiler/abi_constraint_rows.c', 'compiler/abi_storage.c',
     'compiler/abi_trait_method_rows.c',
     'compiler/abi_type_signatures.c', 'compiler/abi_decl_index.c',
     'compiler/abi_declarations.c',
     'compiler/abi_container.c',
     'abi/abi_directory.c', 'abi/abi_string.c'],
    dependencies: lantern,
)

test('abi-emit-unit', abi_emit_unit_test)

abi_record_object_test = executable(
    'abi-record-object-test',
    ['tests/abi_record_object.c', 'compiler/abi_container.c', 'abi/abi_directory.c'],
    dependencies: lantern,
)

abi_link_input_test = executable(
    'abi-link-input-test',
    ['tests/abi_link_input.c', 'compiler/abi_link.c', 'abi/abi_string.c',
     'compiler/abi_index_patch.c',
     'compiler/abi_link_finish.c', 'compiler/abi_link_strings.c', 'compiler/abi_emit.c',
     'compiler/abi_emit_strings.c',
     'compiler/abi_layout.c', 'compiler/abi_record_layout.c',
     'compiler/abi_type_index.c', 'compiler/abi_type_rows.c', 'compiler/abi_type_layout_rows.c',
     'compiler/abi_enum_rows.c', 'compiler/abi_function_rows.c', 'compiler/abi_constraint_rows.c',
     'compiler/abi_storage.c', 'compiler/abi_trait_method_rows.c',
     'compiler/abi_type_signatures.c', 'compiler/abi_decl_index.c', 'compiler/abi_declarations.c',
     'compiler/abi_container.c', 'abi/abi_directory.c'],
    dependencies: lantern,
)

abi_register_object_test = executable(
    'abi-register-object-test',
    'tests/abi_register_object.c',
    link_with: runtime_abi_support,
    dependencies: lantern,
)

abi_emit_strings_test = executable(
    'abi-emit-strings-test',
    ['tests/abi_emit_strings.c', 'compiler/abi_emit_strings.c', 'abi/abi_string.c'],
    dependencies: lantern,
)

test('abi-emit-strings', abi_emit_strings_test)

abi_type_rows_test = executable(
    'abi-type-rows-test',
    ['tests/abi_type_rows.c', 'compiler/builtin_types.c', 'compiler/abi_type_rows.c', 'compiler/abi_type_index.c',
     'compiler/abi_emit_strings.c', 'abi/abi_string.c', 'compiler/abi_decl_index.c'],
    dependencies: lantern,
)

test('abi-type-rows', abi_type_rows_test)

abi_type_layout_rows_test = executable(
    'abi-type-layout-rows-test',
    ['tests/abi_type_layout_rows.c', 'compiler/abi_type_layout_rows.c',
     'compiler/abi_type_rows.c', 'compiler/abi_type_index.c', 'compiler/abi_emit_strings.c',
     'compiler/abi_decl_index.c'],
    dependencies: lantern,
)

test('abi-type-layout-rows', abi_type_layout_rows_test)

abi_type_signatures_test = executable(
    'abi-type-signatures-test',
    ['tests/abi_type_signatures.c', 'compiler/abi_type_signatures.c',
     'compiler/abi_type_rows.c', 'compiler/abi_type_index.c', 'compiler/abi_emit_strings.c',
     'abi/abi_string.c', 'compiler/abi_decl_index.c'],
    dependencies: lantern,
)

test('abi-type-signatures', abi_type_signatures_test)

abi_decl_index_test = executable(
    'abi-decl-index-test',
    ['tests/abi_decl_index.c', 'compiler/abi_decl_index.c'],
    dependencies: lantern,
)

test('abi-decl-index', abi_decl_index_test)

abi_declarations_test = executable(
    'abi-declarations-test',
    ['tests/abi_declarations.c', 'compiler/abi_declarations.c',
     'compiler/abi_decl_index.c', 'compiler/abi_type_rows.c', 'compiler/abi_type_index.c',
     'compiler/abi_emit_strings.c'],
    dependencies: lantern,
)

test('abi-declarations', abi_declarations_test)

abi_type_index_test = executable(
    'abi-type-index-test',
    ['tests/abi_type_index.c', 'compiler/abi_type_index.c',
     'compiler/abi_decl_index.c'],
    dependencies: lantern,
)

test('abi-type-index', abi_type_index_test)

llvm_backend_test = executable(
    'llvm-backend-test',
    ['tests/llvm_backend.c', 'compiler/llvm_backend.c', 'compiler/llvm_native_cache.c',
     'runtime/runtime_native_roots.c', 'compiler/compiler_native_roots.c'],
    dependencies: [lantern, llvm],
)

test('llvm-backend', llvm_backend_test)

llvm_native_cache_test = executable(
    'llvm-native-cache-test',
    ['tests/llvm_native_cache.c', 'compiler/llvm_native_cache.c'],
    dependencies: [lantern, llvm],
)
test('llvm-native-cache', llvm_native_cache_test)

llvm_stack_maps_test = executable(
    'llvm-stack-maps-test',
    ['tests/llvm_stack_maps.c', 'compiler/llvm_stack_maps.c',
     'compiler/native_frame_maps.c'],
    dependencies: [lantern, llvm],
)
test('llvm-stack-maps', llvm_stack_maps_test)

llvm_frame_roots_test = executable(
    'llvm-frame-roots-test',
    ['tests/llvm_frame_roots.c', 'compiler/llvm_backend.c', 'compiler/llvm_native_cache.c',
     'compiler/llvm_stack_maps.c', 'compiler/native_frame_maps.c', 'runtime/runtime_native_roots.c',
     'compiler/compiler_native_roots.c'],
    dependencies: [lantern, llvm],
)
test('llvm-frame-roots', llvm_frame_roots_test)

test(
    'lto-n00b', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'containers' / 'readiness' / 'dict_basic.n'],
)

test(
    'public-literal-handler', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'features' / 'public_literal_handler.n'],
    suite: 'slow',
)

test(
    'compound-order-n00b', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'operator_compound_evaluation_order.n00b'],
    env: ['N00B_PATH=' + (meson.current_source_dir() / 'tests' / 'programs')],
    suite: 'slow',
)

test(
    'tuple-pairs-n00b', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'containers' / 'readiness' / 'dict_tuple_pairs.n'],
    suite: 'slow',
)

test(
    'tree-n00b', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'containers' / 'readiness' / 'tree_basic.n'],
)

test(
    'trait-typecase-generic-pair', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'containers' / 'readiness'
               / 'trait_typecase_generic_pair.n'],
    suite: 'slow',
)

test(
    'trait-operator-generic-result', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'containers' / 'readiness'
               / 'trait_operator_generic_result.n'],
    suite: 'slow',
)

test(
    'ref-field-compound-n00b', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'bitcode' / 'bitcode_ref_field_compound.n'],
    suite: 'slow',
)

test(
    'let-integer-n00b', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'features' / 'let_integer_runtime.n'],
    suite: 'slow',
)

test(
    'generic-trait-unbound-method', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'features' / 'generic_trait_unbound_method.n'],
    suite: 'slow',
)

test(
    'generic-union-parameter', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'features' / 'generic_union_parameter.n'],
    suite: 'slow',
)

test(
    'typecase-array-index', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'features' / 'typecase_array_index.n'],
    suite: 'slow',
)

test(
    'default-integer-keywords', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'features' / 'default_integer_keywords.n'],
    suite: 'slow',
)

test(
    'byte-literal-trait-arguments', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'features' / 'byte_literal_trait_arguments.n'],
    suite: 'slow',
)

test(
    'option-type-spelling-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'features'
               / 'option_type_spelling_rejected.n',
           '(string vs u32?)'],
    timeout: 120,
    suite: 'slow',
)

test(
    'initializer-self-reference-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'features'
               / 'initializer_self_reference.n',
           'unresolved name \'y\''],
    timeout: 120,
    suite: 'slow',
)

test(
    'let-array-reassign-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'features'
               / 'let_array_reassign.n',
           'has multiple initialization sites'],
    timeout: 120,
    suite: 'slow',
)

foreach feature : ['fallible_without_result', 'try_error_binding',
                   'builtin_type_record_literal', 'record_literal_nil_option',
                   'option_literal_argument', 'record_literal_return',
                   'constructor_option_argument', 'option_literal_context',
                   'packed_record_fields', 'hash_xxh3', 'data_file',
                   'os_env_term', 'digit_separators',
                   'array_element_compound', 'array_read_store_index',
                   'package_named_fields', 'array_declared_bounds',
                   'initializer_shadowing', 'array_element_shift',
                   'array_element_field_update', 'let_array_elements']
    test(
        feature.replace('_', '-'), find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs'
                   / 'features' / feature + '.n'],
        timeout: 120,
        suite: 'slow',
    )
endforeach

test(
    'visibility-n00b', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'visibility_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'type_visibility'],
    timeout: 120,
)

test(
    'package-imports', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'package_imports.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'packages'],
    timeout: 120,
    suite: 'slow',
)

test(
    'litmod-duplicates', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'litmod_duplicates.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'litmod_duplicates'],
    timeout: 300,
    suite: 'slow',
)

test(
    'root-modules', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'root_modules.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'root_modules'],
    timeout: 120,
    suite: 'slow',
)

test(
    'ffi', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'ffi.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'ffi'],
    timeout: 300,
    suite: 'slow',
)

test(
    'ffi-zlib', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'ffi_zlib.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'ffi',
           writ.get_variable(pkgconfig: 'contractdir'),
           abi_register_object_test.full_path()],
    timeout: 300,
    suite: 'slow',
)

if cargo.found() and not get_option('rust-transports').disabled()
    test(
        'ffi-resharp', find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'ffi_resharp.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs' / 'ffi',
               writ.get_variable(pkgconfig: 'contractdir')],
        timeout: 600,
        suite: 'slow',
    )
endif

test(
    'builtin-os-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_builtin_os.n'],
    suite: 'slow',
)

test(
    'thread-global-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_thread_global.n'],
    suite: 'slow',
)

test(
    'panic-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'panic_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_panic.n',
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_require_trace.n'],
    timeout: 120,
)

test(
    'f32-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_f32.n'],
    suite: 'slow',
)

test(
    'thread-global-pair-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_thread_global_pair.n'],
    suite: 'slow',
)

test(
    'atomic-global-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_atomic_global.n'],
    suite: 'slow',
)

test(
    'compound-in-place-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_compound_in_place.n'],
    suite: 'slow',
)

foreach string_probe : ['core_probe', 'rstr_probe', 'raw_literal_probe',
                        'encoding_probe', 'span_gc_probe']
    test(
        'string-' + string_probe.replace('_', '-'),
        find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs' / 'string'
                   / string_probe + '.n'],
        suite: 'slow',
    )
endforeach

test(
    'comptime-c-record-deferred-array-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_comptime_c_record_deferred_array.n'],
    suite: 'slow',
)

test(
    'gc-byte-offset-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_byte_offset.n'],
    suite: 'slow',
)

test(
    'gc-memcpy-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_memcpy.n'],
    suite: 'slow',
)

test(
    'gc-block-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_block.n'],
    suite: 'slow',
)

test(
    'gc-sweep-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_sweep.n'],
    suite: 'slow',
)

test(
    'gc-handshake-sweep-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_handshake_sweep.n'],
    suite: 'slow',
)

test(
    'gc-epoch-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_epoch.n'],
    suite: 'slow',
)

test(
    'gc-los-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_los.n'],
    suite: 'slow',
)

test(
    'gc-los-allocate-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_los_allocate.n'],
    suite: 'slow',
)

test(
    'gc-los-pacing-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_los_pacing.n'],
    suite: 'slow',
)

test(
    'gc-small-allocate-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_small_allocate.n'],
    suite: 'slow',
)

test(
    'gc-mutator-lifecycle-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_mutator_lifecycle.n'],
    suite: 'slow',
)

test(
    'option-get-field-arith-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_option_get_field_arith.n'],
    suite: 'slow',
)

test(
    'gc-mark-pool-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_mark_pool.n'],
    suite: 'slow',
)

test(
    'gc-finalize-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_finalize.n'],
    suite: 'slow',
)

test(
    'gc-array-backing-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_array_backing.n'],
    suite: 'slow',
)

test(
    'gc-finalizer-cycle-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_finalizer_cycle.n'],
    suite: 'slow',
)

test(
    'operator-newline-parse',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'operator_newline.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'operator_newline_continuation.n',
           meson.current_source_dir() / 'tests' / 'programs'
               / 'operator_double_newline_rejected.n'],
)

test(
    'operator-newline-bitcode',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_operator_newlines.n'],
    suite: 'slow',
)

test(
    'gc-empty-array-backing-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_empty_array_backing.n'],
    suite: 'slow',
)

test(
    'typeof-storage-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_typeof_storage.n'],
    suite: 'slow',
)

test(
    'typeof-storage-facts',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'typeof_storage.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_typeof_storage.n'],
    suite: 'slow',
)

test(
    'gc-heap-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_heap.n'],
    suite: 'slow',
)

test(
    'gc-roots-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_roots.n'],
    suite: 'slow',
)

test(
    'gc-global-mark-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_global_mark.n'],
    env: ['N00B_PATH=' + (meson.current_source_dir() / 'tests'
        / 'programs' / 'bitcode')],
    suite: 'slow',
)

test(
    'gc-collect-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_collect.n'],
)

test(
    'gc-allocate-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_allocate.n'],
    suite: 'slow',
)

test(
    'gc-new-record-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_new_record.n'],
    suite: 'slow',
)

test(
    'gc-known-finalizer-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_known_finalizer.n'],
    suite: 'slow',
)

test(
    'gc-escaped-record-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_escaped_record.n'],
    suite: 'slow',
)

test(
    'storage-analysis-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'storage_analysis_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_nominal_scalar_storage.n',
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_escaped_record.n'],
    env: ['N00B_PATH=' + meson.current_source_dir() / 'lib'],
    suite: 'slow',
)

test(
    'gc-trace-record-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_trace_record.n'],
    suite: 'slow',
)

test(
    'uninitialized-local-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_uninitialized_local.n',
           'is read on a path that does not assign it'],
    suite: 'slow',
)

test(
    'comptime-statement-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_comptime_statement.n',
           'write \'if comptime() { } else { }\''],
)

test(
    'script-comptime-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_script_comptime.n',
           'N00BB-B0028'],
    suite: 'slow',
)

test(
    'script-const-reads-var-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_script_const_reads_var.n',
           'N00BB-B0029'],
    suite: 'slow',
)

test(
    'start-signature-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_start_signature.n',
           'N00BB-B0030'],
    suite: 'slow',
)

test(
    'finalize-signature-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_finalize_signature_rejected.n',
           'N00BB-T0031', 'N00BB-T0032', 'N00BB-T0033'],
    suite: 'slow',
)

test(
    'finalize-ref-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_finalize_ref.n'],
    suite: 'slow',
)

test(
    'finalize-value-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_finalize_value_rejected.n',
           'N00BB-T0031'],
)

test(
    'finalize-nullable-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_finalize_nullable_rejected.n',
           'N00BB-T0031'],
)

test(
    'private-consume-trait-method-n00b',
    n00bb_exe,
    args: ['lower', meson.current_source_dir() / 'tests' / 'programs'
        / 'bitcode' / 'trait_consume_private.n'],
    suite: 'slow',
)

test(
    'uninitialized-global-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_uninitialized_global.n',
           'N00BB-L0044'],
    suite: 'slow',
)

test(
    'indirect-global-init-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'indirect_global_init.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'],
    timeout: 120,
    suite: 'slow',
)

test(
    'gc-new-pruned-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_new_pruned.n'],
    suite: 'slow',
)

test(
    'gc-handshake-stack-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_handshake_stack.n'],
    suite: 'slow',
)

test(
    'gc-worker-shutdown-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_worker_shutdown.n'],
    suite: 'slow',
)

if host_machine.system() == 'darwin'
    gc_worker_thread_object = custom_target(
        'gc-worker-thread-object',
        input: 'tests/gc_worker_thread.c',
        output: 'gc-worker-thread.o',
        command: [find_program('cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                  '-c', '@INPUT@', '-o', '@OUTPUT@'],
        build_by_default: true,
    )
    test(
        'gc-worker-live-n00b',
        find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
                   / 'bitcode_gc_worker_live.n',
               gc_worker_thread_object.full_path(),
               'sys.internal.gc.collect.worker'],
        suite: 'slow',
    )
endif

test(
    'option-guard-flow-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_option_guard_flow.n'],
    suite: 'slow',
)

test(
    'option-nil-global-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_option_nil_global.n'],
    suite: 'slow',
)

test(
    'array-index-write-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_array_index_write.n'],
    suite: 'slow',
)

test(
    'array-uint-index-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'array_uint_index.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'],
    timeout: 120,
    suite: 'slow',
)

test(
    'array-value-index-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_array_value_index.n'],
    suite: 'slow',
)

test(
    'variant-array-literal-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_variant_array_literal.n'],
    suite: 'slow',
)

test(
    'index-read-variant-override-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_index_read_variant_override.n'],
    suite: 'slow',
)

test(
    'shift-count-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'shift_count_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'],
    timeout: 120,
    suite: 'slow',
)

test(
    'field-constraint-diagnostic',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests'
               / 'field_constraint_diagnostic.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'option_not_through_index.n'],
    suite: 'slow',
)

test(
    'gc-los-registry-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_los_registry.n'],
    suite: 'slow',
)

test(
    'gc-block-registry-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_block_registry.n'],
    suite: 'slow',
)

test(
    'gc-grow-batch-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_grow_batch.n'],
    suite: 'slow',
)

test(
    'gc-reclaim-caches-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_reclaim_caches.n'],
    suite: 'slow',
)

test(
    'gc-pacing-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_pacing.n'],
    suite: 'slow',
)

test(
    'reference-read-escape-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'reference_read_escape.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'reference_read_escape.n'],
    suite: 'slow',
)

test(
    'integer-complement-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_integer_complement.n'],
    suite: 'slow',
)

test(
    'numeric-casts-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'numeric_casts.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_numeric_casts.n',
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_numeric_cast_trap.n'],
    timeout: 120,
)

test(
    'nominal-casts-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_nominal_cast.n'],
    suite: 'slow',
)

test(
    'bit-count-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_bit_count.n'],
    suite: 'slow',
)

test(
    'string-runtime-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_string_runtime.n'],
    timeout: 120,
)

test(
    'overloads-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_overloads.n'],
    suite: 'slow',
)

test(
    'option-record-fields-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_option_record_fields.n'],
    suite: 'slow',
)

test(
    'option-field-narrowing-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_option_field_narrowing.n'],
    suite: 'slow',
)

test(
    'option-narrowing-rejections',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'option_narrowing.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'option_narrowing'],
)

test(
    'named-reference-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_named_reference.n'],
    suite: 'slow',
)

test(
    'builtin-operand-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_builtin_operand.n'],
    suite: 'slow',
)

test(
    'native-frame-scan-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_native_frame_scan.n'],
    timeout: 120,
    suite: 'slow',
)

test(
    'ffi-borrow-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_ffi_borrow.n'],
    suite: 'slow',
)

test(
    'unsafe-arithmetic-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_unsafe_arithmetic.n'],
    suite: 'slow',
)

test(
    'gc-hash-lower',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'gc_hash_lower.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_hash.n'],
    suite: 'slow',
)

test(
    'gc-registry-lower',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'gc_hash_lower.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_registry.n'],
    suite: 'slow',
)

test(
    'gc-block-lower',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'gc_hash_lower.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_block.n'],
    suite: 'slow',
)

test(
    'logical-continuation-lower',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'gc_hash_lower.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_logical_continuation.n'],
    suite: 'slow',
)

test(
    'option-reference-field-lower',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'gc_hash_lower.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_option_get_field.n'],
    suite: 'slow',
)

test(
    'plain-ref-cast-rejected',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'plain_ref_cast_rejected.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_plain_ref_cast_rejected.n'],
    suite: 'slow',
)

test(
    'inline-array-layout-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'inline_array_layout.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'inline_array_record_layout.n'],
    suite: 'slow',
)

test(
    'runtime-array-bound-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'runtime_array_bound.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'array_runtime_sized_declaration.n'],
    suite: 'slow',
)

test(
    'gc-atomic-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_atomic.n'],
    suite: 'slow',
)

test(
    'atomic-rmw-u32-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_atomic_rmw_u32.n'],
    suite: 'slow',
)

test(
    'gc-recycle-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_recycle.n'],
    suite: 'slow',
)

test(
    'copyout-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_copyout.n'],
    suite: 'slow',
)

test(
    'os-mapped-write-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_os_mapped_write.n'],
    suite: 'slow',
)

test(
    'os-mapped-wrapper-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_os_mapped_write_wrapper.n'],
    suite: 'slow',
)

test(
    'builtin-abi-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_abi_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_builtin_abi.n'],
    suite: 'slow',
)

test(
    'builtin-debug',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_debug.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_builtin_debug.n00b'],
    suite: 'slow',
)

test(
    'raw-repr',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'raw_repr.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_raw_repr_dynamic.n00b',
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_raw_repr_out_of_range.n00b',
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_raw_repr_defaults.n00b',
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_raw_repr_array_override.n00b',
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_raw_repr_reference.n00b',
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_raw_repr_element_override.n00b',
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_raw_repr_nested_override.n00b'],
    timeout: 120,
    suite: 'slow',
)

test(
    'bitcode-native',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'bitcode.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'],
    timeout: 120,
    suite: 'slow',
)

test(
    'inline-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'inline_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'inline'],
    timeout: 120,
    suite: 'slow',
)

test(
    'abi-link-fixtures',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'abi_link_fixtures.sh',
           n00bb_exe.full_path(), abi_record_object_test.full_path(),
           abi_link_input_test.full_path(),
           abi_register_object_test.full_path()],
    timeout: 120,
    suite: 'slow',
)

test(
    'formatter',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'formatter.sh',
           n00bb_exe.full_path()],
    timeout: 30,
    suite: 'slow',
)

foreach fixture : [
    'bitcode_any_box.n',
    'bitcode_any_record_field.n',
    'bitcode_callback.n',
    'bitcode_callback_state.n',
    'bitcode_callback_state_ref.n',
    'bitcode_callback_setup.n',
    'bitcode_callback_failure.n',
    'bitcode_callback_setup_failure.n',
    'bitcode_callback_handler_ref.n',
]
    test(
        fixture,
        find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
                   / fixture],
        suite: 'slow',
    )
endforeach

run_target(
    'regen-unicode',
    command: [
        find_program('python3'),
        meson.current_source_dir() / 'tools' / 'gen_unicode_tables.py',
        '--cache-dir', meson.current_build_dir() / 'unicode-cache',
        '--ucd-version', '18.0.0',
        '--out-dir', meson.current_source_dir() / 'lib' / 'unicode' / 'generated',
        '--conformance-dir', meson.current_source_dir() / 'tests' / 'data' / 'unicode',
        '--conformance-program', meson.current_source_dir() / 'tests' / 'programs'
            / 'unicode' / 'gen_conformance.n',
        '--test-data-dir', meson.current_build_dir() / 'unicode-test-data',
        '--allow-downloads',
        '--no-strict',
    ],
)

test(
    'unicode-tables-match',
    find_program('python3'),
    args: [meson.current_source_dir() / 'tools' / 'check_unicode_tables.py',
           '--root', meson.current_source_dir()],
    timeout: 120,
)
