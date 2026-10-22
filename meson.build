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

add_test_setup('default', timeout_multiplier: 2, is_default: true,
    env: ['N00B_CACHE=' + (meson.current_build_dir() / 'n00b-cache')])

test('abi-static-graph', abi_static_graph_test)


n00bb_exe = executable(
    'n00b',
    ['compiler/main.c', 'compiler/compiler_driver.c', 'compiler/compiler_lower.c', 'compiler/compiler_buffer_borrow.c', 'compiler/compiler_layout.c', 'compiler/compiler_baked.c', 'compiler/compiler_abi_driver.c', 'compiler/compiler_cache.c', 'compiler/compiler_object_io.c', 'compiler/compiler_cli.c', 'compiler/compiler_tu.c', 'compiler/builtin_types.c',
     'compiler/compiler_tu_range.c', 'compiler/compiler_tu_builtins.c',
     'compiler/compiler_axioms.c',
     'compiler/compiler_int_ground.c',
     'compiler/compiler_tu_alias.c', 'compiler/compiler_tu_walk.c',
     'compiler/compiler_tu_interface.c',
     'compiler/compiler_link.c', 'compiler/compiler_link_calls.c',
     'compiler/compiler_link_traits.c',
     'compiler/compiler_link_packages.c', 'compiler/compiler_link_environment.c',
     'compiler/compiler_link_stages.c', 'compiler/driver.c',
     'compiler/enum_value.c',
     'compiler/preprocessor.c', 'abi/abi_string.c',
     'compiler/specialize.c', 'compiler/box_calls.c',
     'compiler/storage_placement.c',
     'compiler/interface_summaries.c',
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
     'compiler/llvm_backend.c', 'compiler/llvm_native_cache.c', 'compiler/cache_location.c', 'compiler/llvm_stack_maps.c',
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
    ['tests/llvm_backend.c', 'compiler/llvm_backend.c', 'compiler/llvm_native_cache.c', 'compiler/cache_location.c',
     'runtime/runtime_native_roots.c', 'compiler/compiler_native_roots.c',
     'compiler/compiler_axioms.c'],
    dependencies: [lantern, llvm],
)

test('llvm-backend', llvm_backend_test)

llvm_native_cache_test = executable(
    'llvm-native-cache-test',
    ['tests/llvm_native_cache.c', 'compiler/llvm_native_cache.c',
     'compiler/cache_location.c'],
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
    ['tests/llvm_frame_roots.c', 'compiler/llvm_backend.c', 'compiler/llvm_native_cache.c', 'compiler/cache_location.c',
     'compiler/llvm_stack_maps.c', 'compiler/native_frame_maps.c', 'runtime/runtime_native_roots.c',
     'compiler/compiler_native_roots.c', 'compiler/compiler_axioms.c'],
    dependencies: [lantern, llvm],
)
test('llvm-frame-roots', llvm_frame_roots_test)

test(
    'lto-n00b', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'containers' / 'readiness' / 'dict_basic.n'],
    suite: 'slow',
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
    suite: 'slow',
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
    'embed-size-rejected', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'features' / 'embed_size_rejected.n',
           'not a whole number of 4-byte u32 elements',
           'embed_size_rejected.n:1:25'],
    timeout: 120,
    suite: 'slow',
)

test(
    'generic-same-type-recursion', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'features' / 'generic_same_type_recursion.n'],
    suite: 'slow',
)

test(
    'generic-growing-recursion-rejected', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'features' / 'generic_growing_recursion_rejected.n',
           'N00BB-T0052',
           'generic_growing_recursion_rejected.n:6:12'],
    timeout: 120,
    suite: 'slow',
)

test(
    'generic-record-trait-dispatch', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'features' / 'generic_record_trait_dispatch.n'],
    suite: 'slow',
)

test(
    'name-resolution', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'name_resolution.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'name_resolution'],
    timeout: 300,
    suite: 'slow',
)

test(
    'imports-bring', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'imports_bring.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'imports_bring'],
    timeout: 300,
    suite: 'slow',
)

test(
    'overload-resolution', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'overload_resolution.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'features' / 'overload_specificity.n',
           meson.current_source_dir() / 'tests' / 'programs'
               / 'features' / 'overload_ambiguous_generic.n'],
    timeout: 120,
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
           'N00BB-T0002',
           'option_type_spelling_rejected.n:6:9'],
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
           'N00BB-L0014',
           'initializer_self_reference.n:2:18'],
    timeout: 120,
    suite: 'slow',
)

test(
    'try-without-failure-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'features'
               / 'try_without_failure.n',
           'N00BB-T0048',
           'try_without_failure.n:2:21'],
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
           'IR-E0037',
           'let_array_reassign.n:3:5'],
    timeout: 120,
    suite: 'slow',
)

test(
    'const-array-write-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'features'
               / 'const_array_write_rejected.n',
           'IR-E0037',
           'const_array_write_rejected.n:22:5',
           'const_array_write_rejected.n:18:5',
           'const_array_write_rejected.n:14:5',
           'const_array_write_rejected.n:10:5'],
    timeout: 120,
    suite: 'slow',
)

foreach declared_root : ['shape', 'maker']
    test(
        'import-cycle-declared-' + declared_root + '-n00b',
        find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'compile_module.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs'
                   / 'import_cycles' / 'declared',
               declared_root],
        timeout: 120,
        suite: 'slow',
    )
endforeach

test(
    'compile-fallible-index-read-n00b', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'compile_module.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'compile_mode',
           'fallible_index_read'],
    timeout: 120,
    suite: 'slow',
)

foreach static_case : ['static_enum', 'static_option', 'static_nested_import',
                       'static_literals']
    test(
        static_case.replace('_', '-') + '-n00b', find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs'
                   / 'static_encoding' / static_case + '.n'],
        env: ['N00B_PATH=' + (meson.current_source_dir() / 'tests'
            / 'programs' / 'static_encoding')],
        timeout: 120,
        suite: 'slow',
    )
    test(
        static_case.replace('_', '-') + '-baked-n00b', find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'no_build_time_run.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs'
                   / 'static_encoding' / static_case + '.n'],
        env: ['N00B_PATH=' + (meson.current_source_dir() / 'tests'
            / 'programs' / 'static_encoding')],
        timeout: 120,
        suite: 'slow',
    )
endforeach

test(
    'hello-no-build-time-run-n00b', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'no_build_time_run.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'static_encoding' / 'hello.n'],
    timeout: 120,
    suite: 'slow',
)

test(
    'untyped-consts-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'untyped_consts' / 'untyped_consts.n'],
    env: ['N00B_PATH=' + (meson.current_source_dir() / 'tests' / 'programs'
        / 'untyped_consts')],
    timeout: 120,
    suite: 'slow',
)

test(
    'untyped-const-range-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'untyped_consts' / 'untyped_const_range.n',
           'N00BB-T0041',
           'untyped_const_range.n:4:21'],
    timeout: 120,
    suite: 'slow',
)

test(
    'folded-consts-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'folded_consts' / 'folded_consts.n'],
    timeout: 120,
    suite: 'slow',
)

foreach rejected : [
    ['folded-const-range-n00b', 'folded_const_range.n',
     'N00BB-T0041'],
    ['folded-bitwise-local-n00b', 'folded_bitwise_local.n',
     'N00BB-T0042'],
    ['folded-bitwise-argument-n00b', 'folded_bitwise_argument.n',
     'N00BB-T0002'],
    ['folded-bitwise-return-n00b', 'folded_bitwise_return.n',
     'N00BB-T0044'],
    ['folded-bitwise-assign-n00b', 'folded_bitwise_assign.n',
     'N00BB-T0042'],
    ['folded-bitwise-const-n00b', 'folded_bitwise_const.n',
     'N00BB-T0042'],
]
    test(
        rejected[0],
        find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs'
                   / 'folded_consts' / rejected[1],
               rejected[2]],
        timeout: 120,
        suite: 'slow',
    )
endforeach

test(
    'wide-integer-literal-range-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'features'
               / 'wide_integer_literal_range.n',
           'N00BB-T0041',
           'wide_integer_literal_range.n:4:9'],
    timeout: 120,
    suite: 'slow',
)

foreach rejected : [
    ['free', [
        'N00BB-T0002',
        'keyword_argument_free_rejected.n:5:15',
        'N00BB-T0037',
        'keyword_argument_free_rejected.n:6:11',
        'N00BB-T0038',
        'keyword_argument_free_rejected.n:7:14']],
    ['method', [
        'N00BB-T0002',
        'keyword_argument_method_rejected.n:14:22']],
    ['generic', [
        'N00BB-T0002',
        'keyword_argument_generic_rejected.n:14:28']],
    ['constructor', [
        'N00BB-T0020',
        'keyword_argument_constructor_rejected.n:9:17']],
    ['context', [
        'N00BB-T0002',
        'keyword_argument_context_rejected.n:9:16',
        'N00BB-T0002',
        'keyword_argument_context_rejected.n:12:16']],
]
    test(
        'keyword-argument-' + rejected[0] + '-rejected',
        find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs'
                   / 'features'
                   / 'keyword_argument_' + rejected[0] + '_rejected.n']
            + rejected[1],
        timeout: 120,
        suite: 'slow',
    )
endforeach

foreach rejected : [
    ['free', [
        'N00BB-T0039',
        'call_arity_free_rejected.n:5:17',
        'N00BB-T0040',
        'call_arity_free_rejected.n:6:9']],
    ['method', [
        'N00BB-T0039',
        'call_arity_method_rejected.n:14:20',
        'N00BB-T0040',
        'call_arity_method_rejected.n:15:9']],
    ['generic', [
        'N00BB-T0039',
        'call_arity_generic_rejected.n:14:26',
        'N00BB-T0040',
        'call_arity_generic_rejected.n:18:12']],
    ['constructor', [
        'N00BB-T0039',
        'call_arity_constructor_rejected.n:9:35']],
    ['constructor_missing', [
        'N00BB-T0040',
        'call_arity_constructor_missing_rejected.n:9:19']],
    ['context', [
        'N00BB-T0039',
        'call_arity_context_rejected.n:8:17',
        'N00BB-T0040',
        'call_arity_context_rejected.n:10:6']],
]
    test(
        'call-arity-' + rejected[0].replace('_', '-') + '-rejected',
        find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs'
                   / 'features'
                   / 'call_arity_' + rejected[0] + '_rejected.n']
            + rejected[1],
        timeout: 120,
        suite: 'slow',
    )
endforeach

foreach feature : ['fallible_without_result', 'try_error_binding',
                   'builtin_type_record_literal', 'record_literal_nil_option',
                   'option_literal_argument', 'record_literal_return',
                   'constructor_option_argument', 'option_literal_context',
                   'packed_record_fields', 'hash_xxh3', 'data_file',
                   'embed_tables',
                   'os_env_term', 'digit_separators',
                   'array_element_compound', 'array_read_store_index',
                   'package_named_fields', 'array_declared_bounds',
                   'initializer_shadowing', 'array_element_shift',
                   'array_element_field_update', 'let_array_elements',
                   'constructor_integer_literal', 'wide_integer_literal',
                   'quotes_litmod_raw', 'import_prelude_name',
                   'atomic_auto_load', 'value_branch_literal_context',
                   'one_bit_enum_array', 'same_module_overloads',
                   'range_literal_start', 'record_literal_argument',
                   'keyword_argument_context']
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
    'program-args', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'program_args_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'features'
               / 'program_args.n',
           'alpha', 'two words'],
    timeout: 120,
    suite: 'slow',
)

test(
    'buffer-probe', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'buffer'
               / 'buffer_probe.n'],
    timeout: 180,
    suite: 'slow',
)

test(
    'buffer-finalizer-borrow', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'buffer'
               / 'buffer_finalizer_borrow.n'],
    timeout: 180,
    suite: 'slow',
)

test(
    'buffer-finalizer-borrow-release', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'buffer'
               / 'buffer_finalizer_borrow.n'],
    timeout: 180,
    suite: 'slow',
)

test(
    'coercion-borrow', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'coercion_borrow.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'buffer'
               / 'coercion_borrow.n'],
    timeout: 180,
    suite: 'slow',
)

test(
    'coercion-reject', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'coercion_reject.sh',
           n00bb_exe.full_path()],
    timeout: 300,
    suite: 'slow',
)

test(
    'visibility-n00b', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'visibility_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'type_visibility'],
    timeout: 120,
    suite: 'slow',
)

test(
    'package-imports', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'package_imports.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'packages'],
    timeout: 120,
    suite: 'slow',
)

foreach import_cycle : ['two', 'three', 'types']
    test(
        'import-cycle-' + import_cycle + '-n00b', find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'lto_n00b.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs'
                   / 'import_cycles' / import_cycle
                   / import_cycle + '_cycle.n'],
        env: ['N00B_PATH=' + (meson.current_source_dir() / 'tests'
            / 'programs' / 'import_cycles' / import_cycle)],
        timeout: 180,
        suite: 'slow',
    )
endforeach

test(
    'import-cycle-initializer-n00b', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'import_cycles' / 'initializer' / 'initializer_cycle.n',
           'N00BB-L0040',
           'beta.n:3:21'],
    env: ['N00B_PATH=' + (meson.current_source_dir() / 'tests'
        / 'programs' / 'import_cycles' / 'initializer')],
    timeout: 120,
    suite: 'slow',
)

test(
    'import-name-clash-n00b', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'import_names' / 'import_name_clash.n',
           'N00BB-M0012',
           'import_name_clash.n:8:1',
           'import_name_clash.n:12:1',
           'import_name_clash.n:16:7',
           'import_name_clash.n:18:5'],
    env: ['N00B_PATH=' + (meson.current_source_dir() / 'tests'
        / 'programs' / 'import_names')],
    timeout: 120,
    suite: 'slow',
)

test(
    'implicit-package-name-n00b', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'import_names' / 'implicit_package_name.n',
           'N00BB-M0012',
           'implicit_package_name.n:1:1'],
    timeout: 120,
    suite: 'slow',
)

test(
    'import-name-clash-module-n00b', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'import_names' / 'import_name_nested.n',
           'N00BB-M0012',
           'wrapper.n:3:1'],
    env: ['N00B_PATH=' + (meson.current_source_dir() / 'tests'
        / 'programs' / 'import_names')],
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
    'ffi-bake', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'ffi_bake.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'ffi'
               / 'bake'],
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

test(
    'ffi-load-dynamic', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'ffi_load_dynamic.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'ffi_load_dynamic',
           writ.get_variable(pkgconfig: 'contractdir')],
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
    suite: 'slow',
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
                        'encoding_probe', 'span_gc_probe', 'raw_offset_probe',
                        'rich_literal_probe']
    test(
        'string-' + string_probe.replace('_', '-'),
        find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs' / 'string'
                   / string_probe + '.n'],
        timeout: 180,
        suite: 'slow',
    )
endforeach

foreach conformance_suite : ['grapheme', 'word', 'sentence', 'line',
                             'normalization', 'bidi', 'collation', 'idna']
    test(
        conformance_suite,
        find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'program_args_n00b.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs' / 'unicode'
                   / 'conformance.n',
               conformance_suite],
        workdir: meson.current_source_dir() / 'tests' / 'programs'
            / 'unicode',
        timeout: 600,
        suite: ['conformance', 'slow'],
    )
endforeach

foreach unicode_probe : ['transcode_probe', 'encoding_probe']
    test(
        'unicode-' + unicode_probe.replace('_', '-'),
        find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs' / 'unicode'
                   / unicode_probe + '.n'],
        timeout: 180,
        suite: 'slow',
    )
endforeach

foreach text_probe : ['color_probe', 'capability_probe', 'theme_probe',
                      'border_probe', 'rich_static_probe', 'rich_build_probe',
                      'cascade_probe', 'layout_probe', 'text_string_probe',
                      'style_probe', 'style_fields_probe',
                      'rich_literal_probe', 'rich_repr_probe', 'table_probe']
    test(
        'text-' + text_probe.replace('_', '-'),
        find_program('sh'),
        args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs' / 'text'
                   / text_probe + '.n'],
        timeout: 180,
        suite: 'slow',
    )
endforeach

test(
    'text-theme-builtin-immutable',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'panic_message_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'text'
               / 'theme_builtin_immutable.n',
           'builtin themes are immutable; copy the theme first'],
    timeout: 180,
    suite: 'slow',
)

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
    'gc-destroy-blocked-finalizer-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_destroy_blocked_finalizer.n'],
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
    'pass-statement',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'pass_statement.n'],
    suite: 'slow',
)

test(
    'pass-not-alone', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'pass_not_alone.n',
           'N00BB-PP0001',
           'pass_not_alone.n:2:5'],
    timeout: 120,
)

test(
    'bidi-control-rejected', find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'bidi_control_rejected.n',
           'N00BB-L0001',
           'bidi_control_rejected.n:2:22'],
    timeout: 120,
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
    'shape-literal-inference',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'features'
               / 'shape_literal_inference.n'],
)

test(
    'integer-operand-widening',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'features'
               / 'integer_operand_widening.n'],
)

test(
    'ref-integer-widening',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'features'
               / 'ref_integer_widening.n'],
)

test(
    'sys-locks',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'features'
               / 'sys_locks.n'],
)

test(
    'thread-var-storage',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'features'
               / 'thread_var_storage.n'],
)

test(
    'verify-analyses',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'verify_analyses.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_shared_ranges.n'],
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
    'incremental-cache',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'incremental_cache.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'incremental'],
    timeout: 900,
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
           'IR-W0027',
           'bitcode_uninitialized_local.n:3:12'],
    suite: 'slow',
)

test(
    'comptime-statement-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_comptime_statement.n',
           'N00BB-P0001',
           'bitcode_comptime_statement.n:1:1',
           '`comptime` is a function, not a block keyword'],
)

test(
    'type-parameter-fixed-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_type_parameter_fixed.n',
           'N00BB-T0046',
           'bitcode_type_parameter_fixed.n:13:1'],
    suite: 'slow',
)

test(
    'type-parameter-assigned-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_type_parameter_assigned.n',
           'N00BB-T0046',
           'bitcode_type_parameter_assigned.n:2:5'],
)

test(
    'script-comptime-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_script_comptime.n',
           'N00BB-B0028',
           'bitcode_script_comptime.n:1:6'],
    suite: 'slow',
)

test(
    'script-const-reads-var-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_script_const_reads_var.n',
           'N00BB-B0029',
           'bitcode_script_const_reads_var.n:2:1'],
    suite: 'slow',
)

test(
    'start-signature-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_start_signature.n',
           'N00BB-B0030',
           'bitcode_start_signature.n:1:1'],
    suite: 'slow',
)

test(
    'finalize-signature-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_finalize_signature_rejected.n',
           'N00BB-T0031',
           'N00BB-T0032',
           'N00BB-T0033',
           'bitcode_finalize_signature_rejected.n:1:1'],
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
           'N00BB-T0031',
           'bitcode_finalize_value_rejected.n:5:1'],
)

test(
    'finalize-nullable-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_finalize_nullable_rejected.n',
           'N00BB-T0031',
           'bitcode_finalize_nullable_rejected.n:5:1'],
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
           'N00BB-L0044',
           'bitcode_uninitialized_global.n:3:46'],
    suite: 'slow',
)

test(
    'partial-field-global-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_partial_field_global.n',
           'N00BB-L0044',
           'bitcode_partial_field_global.n:10:15'],
    suite: 'slow',
)

test(
    'assert-ref-write-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_assert_ref_write.n',
           'assert expression has a side effect: it calls \'fill\'; \'fill\' writes through its argument 1',
           'bitcode_assert_ref_write.n:7:1'],
    suite: 'slow',
)

test(
    'unbounded-recursion-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'warning_expected.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_unbounded_recursion.n',
           'function \'spin\' recurses with no path that returns',
           'function \'ping\' recurses with no path that returns',
           'function \'pong\' recurses with no path that returns'],
    suite: 'slow',
)

test(
    'range-agreement-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'range_agreement.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_shared_ranges.n',
           'bitcode_shared_ranges'],
)

test(
    'range-agreement-programs',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'range_agreement.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'range_agreement' / 'agreement.n',
           'parameters', 'guards', 'loops', 'widths'],
)

test(
    'overflow-warning-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'warning_expected.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_overflow_warning.n',
           'bitcode_overflow_warning.n:2:12'],
    suite: 'slow',
)

test(
    'overflow-always-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_overflow_always.n',
           'N00BB-L0050', 'bitcode_overflow_always.n:3:12'],
    suite: 'slow',
)

test(
    'abi-summaries-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_abi_summaries.n'],
    suite: 'slow',
)

test(
    'thread-races-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'thread_races.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_thread_races.n'],
    suite: 'slow',
)

test(
    'unused-fields-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'closed_world_fields.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_unused_fields.n'],
    suite: 'slow',
)

test(
    'function-effects-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_function_effects.n'],
    suite: 'slow',
)

test(
    'closed-world-callback-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'closed_world_callback.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_closed_world_callback.n'],
    suite: 'slow',
)

test(
    'range-warnings-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'warning_expected.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_range_warnings.n',
           'N00BB-W0013', 'bitcode_range_warnings.n:2:8',
           'the divisor may be invalid', 'bitcode_range_warnings.n:9:16',
           'the shift amount may be invalid: [0, 39]',
           'bitcode_range_warnings.n:15:16'],
    suite: 'slow',
)

test(
    'divide-always-zero-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_divide_always_zero.n',
           'N00BB-L0051', 'bitcode_divide_always_zero.n:3:12'],
    suite: 'slow',
)

test(
    'trap-summaries-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'summary_expected.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_trap_summaries.n',
           'summary @bitcode_trap_summaries.both may-trap=>  traps explicit bounds arithmetic',
           'summary @bitcode_trap_summaries.square may-trap=>  traps arithmetic',
           'summary @bitcode_trap_summaries.checked may-trap=>  traps explicit'],
    suite: 'slow',
)

test(
    'ignored-results-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'warning_expected.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_ignored_results.n',
           'bitcode_ignored_results.n:13:1'],
    suite: 'slow',
)

test(
    'dead-stores-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'warning_expected.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_dead_stores.n',
           '\'never\' is assigned here and never read afterward',
           'bitcode_dead_stores.n:8:5'],
    suite: 'slow',
)

test(
    'dead-code-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'warning_expected.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_dead_code.n',
           'bitcode_dead_code.n:7:9', 'bitcode_dead_code.n:13:12',
           'bitcode_dead_code.n:18:9', 'bitcode_dead_code.n:23:33'],
    suite: 'slow',
)

test(
    'diverging-paths-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_diverging_paths.n'],
    suite: 'slow',
)

test(
    'initializer-read-after-exit-arm-n00b',
    n00bb_exe,
    args: ['lower', meson.current_source_dir() / 'tests' / 'programs'
        / 'bitcode' / 'bitcode_initializer_read_after_exit_arm.n'],
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
    suite: 'slow',
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
    suite: 'slow',
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
    'bitcode_generic_trait_calls.n',
    'bitcode_bake_foreign.n',
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
