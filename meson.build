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

runtime_abi_headers = lantern.partial_dependency(
    compile_args: true, includes: true,
)

runtime_abi_support = static_library(
    'n00b-abi-runtime',
    ['runtime_abi.c', 'runtime_allocate.c', 'runtime_abi_io.c',
     'runtime_abi_storage.c', 'native_frame_runtime.c', 'builtin_os.c',
     'builtin_os_stw.c',
     'builtin_abi.c', 'abi_directory.c', 'abi_string.c'],
    dependencies: runtime_abi_headers,
    override_options: ['optimization=1'],
    install: true,
)

comptime_allocate_support = static_library(
    'n00b-comptime-allocate', 'comptime_allocate.c',
    dependencies: runtime_abi_headers,
)

n00bb_exe = executable(
    'n00bb',
    ['main.c', 'driver.c', 'preprocessor.c', 'rich_string.c', 'abi_string.c',
     'runtime_native_roots.c',
     'abi_layout.c', 'abi_record_layout.c', 'abi_directory.c',
     'abi_container.c', 'abi_index_patch.c', 'abi_emit.c', 'abi_link.c',
     'abi_link_finish.c', 'abi_link_strings.c', 'abi_semantics.c',
     'abi_emit_strings.c',
     'abi_type_index.c', 'abi_type_rows.c', 'abi_type_layout_rows.c',
     'abi_enum_rows.c',
     'abi_function_rows.c', 'abi_constraint_rows.c', 'abi_storage.c',
     'abi_trait_method_rows.c',
     'abi_type_signatures.c', 'abi_decl_index.c', 'abi_declarations.c',
     'llvm_backend.c', 'llvm_stack_maps.c', 'native_frame_maps.c',
     'global_metadata.c',
     'storage_selection.c'],
    dependencies: [lantern, llvm],
    link_with: runtime_abi_support,
    install: true,
)

install_headers('n00b-abi-v1.h', 'builtin_os.h', 'builtin_abi.h')

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

test(
    'unsigned-literal-context',
    n00bb_exe,
    args: ['lower', meson.current_source_dir() / 'tests' / 'programs'
        / 'unsigned_literal_context.n'],
)

test(
    'unsafe-ref-cast',
    n00bb_exe,
    args: ['lower', meson.current_source_dir() / 'tests' / 'programs'
        / 'unsafe_ref_cast.n'],
)

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
    ['tests/abi_emit.c', 'abi_emit.c', 'abi_emit_strings.c', 'abi_string.c',
     'abi_layout.c', 'abi_record_layout.c',
     'abi_type_index.c', 'abi_type_rows.c', 'abi_type_layout_rows.c',
     'abi_enum_rows.c',
     'abi_function_rows.c', 'abi_constraint_rows.c', 'abi_storage.c',
     'abi_trait_method_rows.c',
     'abi_type_signatures.c', 'abi_decl_index.c',
     'abi_declarations.c',
     'abi_container.c',
     'abi_directory.c'],
    dependencies: lantern,
)

test('abi-emit', abi_emit_test)

abi_emit_unit_test = executable(
    'abi-emit-unit-test',
    ['tests/abi_emit_unit.c', 'abi_emit.c', 'abi_emit_strings.c', 'abi_string.c',
     'abi_link.c', 'abi_link_finish.c', 'abi_link_strings.c',
     'abi_index_patch.c',
     'abi_layout.c', 'abi_record_layout.c',
     'abi_type_index.c', 'abi_type_rows.c', 'abi_type_layout_rows.c',
     'abi_enum_rows.c',
     'abi_function_rows.c', 'abi_constraint_rows.c', 'abi_storage.c',
     'abi_trait_method_rows.c',
     'abi_type_signatures.c', 'abi_decl_index.c',
     'abi_declarations.c',
     'abi_container.c',
     'abi_directory.c', 'abi_string.c'],
    dependencies: lantern,
)

test('abi-emit-unit', abi_emit_unit_test)

abi_record_object_test = executable(
    'abi-record-object-test',
    ['tests/abi_record_object.c', 'abi_container.c', 'abi_directory.c'],
    dependencies: lantern,
)

abi_link_input_test = executable(
    'abi-link-input-test',
    ['tests/abi_link_input.c', 'abi_link.c', 'abi_string.c',
     'abi_index_patch.c',
     'abi_link_finish.c', 'abi_link_strings.c', 'abi_emit.c',
     'abi_emit_strings.c',
     'abi_layout.c', 'abi_record_layout.c',
     'abi_type_index.c', 'abi_type_rows.c', 'abi_type_layout_rows.c',
     'abi_enum_rows.c', 'abi_function_rows.c', 'abi_constraint_rows.c',
     'abi_storage.c', 'abi_trait_method_rows.c',
     'abi_type_signatures.c', 'abi_decl_index.c', 'abi_declarations.c',
     'abi_container.c', 'abi_directory.c'],
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
    ['tests/abi_emit_strings.c', 'abi_emit_strings.c', 'abi_string.c'],
    dependencies: lantern,
)

test('abi-emit-strings', abi_emit_strings_test)

abi_type_rows_test = executable(
    'abi-type-rows-test',
    ['tests/abi_type_rows.c', 'abi_type_rows.c', 'abi_type_index.c',
     'abi_emit_strings.c', 'abi_string.c', 'abi_decl_index.c'],
    dependencies: lantern,
)

test('abi-type-rows', abi_type_rows_test)

abi_type_layout_rows_test = executable(
    'abi-type-layout-rows-test',
    ['tests/abi_type_layout_rows.c', 'abi_type_layout_rows.c',
     'abi_type_rows.c', 'abi_type_index.c', 'abi_emit_strings.c',
     'abi_decl_index.c'],
    dependencies: lantern,
)

test('abi-type-layout-rows', abi_type_layout_rows_test)

abi_type_signatures_test = executable(
    'abi-type-signatures-test',
    ['tests/abi_type_signatures.c', 'abi_type_signatures.c',
     'abi_type_rows.c', 'abi_type_index.c', 'abi_emit_strings.c',
     'abi_string.c', 'abi_decl_index.c'],
    dependencies: lantern,
)

test('abi-type-signatures', abi_type_signatures_test)

abi_decl_index_test = executable(
    'abi-decl-index-test',
    ['tests/abi_decl_index.c', 'abi_decl_index.c'],
    dependencies: lantern,
)

test('abi-decl-index', abi_decl_index_test)

abi_declarations_test = executable(
    'abi-declarations-test',
    ['tests/abi_declarations.c', 'abi_declarations.c',
     'abi_decl_index.c', 'abi_type_rows.c', 'abi_type_index.c',
     'abi_emit_strings.c'],
    dependencies: lantern,
)

test('abi-declarations', abi_declarations_test)

abi_type_index_test = executable(
    'abi-type-index-test',
    ['tests/abi_type_index.c', 'abi_type_index.c'],
    dependencies: lantern,
)

test('abi-type-index', abi_type_index_test)

llvm_backend_test = executable(
    'llvm-backend-test',
    ['tests/llvm_backend.c', 'llvm_backend.c', 'runtime_native_roots.c'],
    dependencies: [lantern, llvm],
)

test('llvm-backend', llvm_backend_test)

llvm_stack_maps_test = executable(
    'llvm-stack-maps-test',
    ['tests/llvm_stack_maps.c', 'llvm_stack_maps.c',
     'native_frame_maps.c'],
    dependencies: [lantern, llvm],
)
test('llvm-stack-maps', llvm_stack_maps_test)

llvm_frame_roots_test = executable(
    'llvm-frame-roots-test',
    ['tests/llvm_frame_roots.c', 'llvm_backend.c',
     'llvm_stack_maps.c', 'native_frame_maps.c', 'runtime_native_roots.c'],
    dependencies: [lantern, llvm],
)
test('llvm-frame-roots', llvm_frame_roots_test)

test(
    'builtin-os-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_builtin_os.n'],
)

test(
    'thread-global-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_thread_global.n'],
)

test(
    'thread-global-pair-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_thread_global_pair.n'],
)

test(
    'atomic-global-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_atomic_global.n'],
)

test(
    'comptime-c-record-deferred-array-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_comptime_c_record_deferred_array.n'],
)

test(
    'gc-byte-offset-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_byte_offset.n'],
)

test(
    'gc-memcpy-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_memcpy.n'],
)

test(
    'gc-block-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_block.n'],
)

test(
    'gc-sweep-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_sweep.n'],
)

test(
    'gc-handshake-sweep-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_handshake_sweep.n'],
)

test(
    'gc-epoch-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_epoch.n'],
)

test(
    'gc-los-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_los.n'],
)

test(
    'gc-los-allocate-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_los_allocate.n'],
)

test(
    'gc-los-pacing-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_los_pacing.n'],
)

test(
    'gc-small-allocate-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_small_allocate.n'],
)

test(
    'gc-mutator-lifecycle-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_mutator_lifecycle.n'],
)

test(
    'option-get-field-arith-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_option_get_field_arith.n'],
)

test(
    'gc-mark-pool-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_mark_pool.n'],
)

test(
    'gc-finalize-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_finalize.n'],
)

test(
    'gc-array-backing-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_array_backing.n'],
)

test(
    'gc-finalizer-cycle-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_finalizer_cycle.n'],
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
)

test(
    'gc-empty-array-backing-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_empty_array_backing.n'],
)

test(
    'typeof-storage-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_typeof_storage.n'],
)

test(
    'typeof-storage-facts',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'typeof_storage.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_typeof_storage.n'],
)

test(
    'gc-heap-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_heap.n'],
)

test(
    'gc-roots-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_roots.n'],
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
)

test(
    'gc-new-record-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_new_record.n'],
)

test(
    'gc-known-finalizer-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_known_finalizer.n'],
)

test(
    'gc-escaped-record-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_escaped_record.n'],
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
)

test(
    'gc-trace-record-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_trace_record.n'],
)

test(
    'uninitialized-local-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_uninitialized_local.n',
           'is read on a path that does not assign it'],
)

test(
    'finalize-signature-rejected-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_finalize_signature_rejected.n',
           'N00BB-T0031', 'N00BB-T0032', 'N00BB-T0033'],
)

test(
    'finalize-ref-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_finalize_ref.n'],
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
)

test(
    'uninitialized-global-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'diagnostic_failure.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_uninitialized_global.n',
           'N00BB-L0044'],
)

test(
    'indirect-global-init-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'indirect_global_init.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'],
)

test(
    'gc-new-pruned-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_new_pruned.n'],
)

test(
    'gc-handshake-stack-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_handshake_stack.n'],
)

test(
    'gc-worker-shutdown-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_worker_shutdown.n'],
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
               'builtin.gc.collect.worker'],
    )
endif

test(
    'option-guard-flow-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_option_guard_flow.n'],
)

test(
    'option-nil-global-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_option_nil_global.n'],
)

test(
    'array-index-write-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_array_index_write.n'],
)

test(
    'array-value-index-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_array_value_index.n'],
)

test(
    'shift-count-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'shift_count_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'],
)

test(
    'gc-los-registry-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_los_registry.n'],
)

test(
    'gc-block-registry-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_block_registry.n'],
)

test(
    'gc-grow-batch-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_grow_batch.n'],
)

test(
    'gc-reclaim-caches-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_reclaim_caches.n'],
)

test(
    'gc-pacing-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_pacing.n'],
)

test(
    'reference-read-escape-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'reference_read_escape.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'reference_read_escape.n'],
)

test(
    'integer-complement-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_integer_complement.n'],
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
)

test(
    'nominal-casts-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_nominal_cast.n'],
)

test(
    'bit-count-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_bit_count.n'],
)

test(
    'wrapping-mul-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_wrapping_mul.n'],
)

test(
    'gc-hash-lower',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'gc_hash_lower.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_hash.n'],
)

test(
    'gc-registry-lower',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'gc_hash_lower.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_registry.n'],
)

test(
    'gc-block-lower',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'gc_hash_lower.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_block.n'],
)

test(
    'logical-continuation-lower',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'gc_hash_lower.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_logical_continuation.n'],
)

test(
    'option-reference-field-lower',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'gc_hash_lower.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_option_get_field.n'],
)

test(
    'plain-ref-cast-rejected',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'plain_ref_cast_rejected.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_plain_ref_cast_rejected.n'],
)

test(
    'inline-array-layout-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'inline_array_layout.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'inline_array_record_layout.n'],
)

test(
    'runtime-array-bound-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'runtime_array_bound.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs'
               / 'array_runtime_sized_declaration.n'],
)

test(
    'gc-atomic-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_atomic.n'],
)

test(
    'atomic-rmw-u32-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_atomic_rmw_u32.n'],
)

test(
    'gc-recycle-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_gc_recycle.n'],
)

test(
    'copyout-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_copyout.n'],
)

test(
    'os-mapped-write-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_os_mapped_write.n'],
)

test(
    'os-mapped-wrapper-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_os_mapped_write_wrapper.n'],
)

test(
    'builtin-abi-n00b',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_abi_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_builtin_abi.n'],
)

test(
    'builtin-debug',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'builtin_debug.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
               / 'bitcode_builtin_debug.n00b'],
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
)

test(
    'bitcode-native',
    find_program('sh'),
    args: [meson.current_source_dir() / 'tests' / 'bitcode.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'],
)

test(
    'inline-n00b',
    find_program('sh'),
    suite: 'regression',
    args: [meson.current_source_dir() / 'tests' / 'inline_n00b.sh',
           n00bb_exe.full_path(),
           meson.current_source_dir() / 'tests' / 'programs' / 'inline'],
)

test(
    'preprocessor',
    find_program('sh'),
    suite: 'regression',
    args: [meson.current_source_dir() / 'tests' / 'preprocessor.sh',
           n00bb_exe.full_path(), storage_interface_writer.full_path(),
           abi_record_object_test.full_path(),
           abi_link_input_test.full_path(),
           abi_register_object_test.full_path()],
    env: ['N00BB_STACK_OBJECT_LIMIT_BYTES='
        + get_option('stack_object_limit_bytes').to_string()],
    timeout: 120,
)

foreach fixture : [
    'bitcode_any_box.n',
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
        suite: 'regression',
        args: [meson.current_source_dir() / 'tests' / 'builtin_os_n00b.sh',
               n00bb_exe.full_path(),
               meson.current_source_dir() / 'tests' / 'programs' / 'bitcode'
                   / fixture],
    )
endforeach
