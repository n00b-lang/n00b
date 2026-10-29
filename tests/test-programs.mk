TEST_PROGRAMS := abi-static-graph-test \
    writ-coexistence \
    builtin-os-test \
    builtin-abi-test \
    runtime-abi-io-test \
    runtime-abi-test \
    runtime-abi-scc-test \
    runtime-abi-trace-test \
    runtime-abi-option-test \
    storage-selection-test \
    typehash-abi-test \
    abi-string-value-test \
    abi-layout-test \
    abi-record-layout-test \
    abi-directory-test \
    abi-container-test \
    abi-index-patch-test \
    abi-emit-test \
    abi-emit-unit-test \
    abi-record-object-test \
    abi-link-input-test \
    abi-register-object-test \
    abi-emit-strings-test \
    abi-type-rows-test \
    abi-type-layout-rows-test \
    abi-type-signatures-test \
    abi-decl-index-test \
    abi-declarations-test \
    abi-type-index-test \
    llvm-backend-test \
    llvm-native-cache-test \
    llvm-stack-maps-test \
    llvm-frame-roots-test

abi-static-graph-test_SOURCES := tests/abi_static_graph.c \
    compiler/abi_static_graph.c
abi-static-graph-test_LIBS := $(LANTERN_LIB)

writ-coexistence_SOURCES := tests/writ_coexistence.c
writ-coexistence_LIBS := $(WRIT_LIB) $(LANTERN_LIB)

builtin-os-test_SOURCES := tests/builtin_os.c
builtin-os-test_LIBS := $(RUNTIME_LIB)

builtin-abi-test_SOURCES := tests/builtin_abi.c
builtin-abi-test_LIBS := $(RUNTIME_LIB)

runtime-abi-io-test_SOURCES := tests/runtime_abi_io.c
runtime-abi-io-test_LIBS := $(RUNTIME_LIB)

runtime-abi-test_SOURCES := tests/runtime_abi.c
runtime-abi-test_LIBS := $(RUNTIME_LIB)

runtime-abi-scc-test_SOURCES := tests/runtime_abi_scc.c
runtime-abi-scc-test_LIBS := $(RUNTIME_LIB)

runtime-abi-trace-test_SOURCES := tests/runtime_abi_trace.c
runtime-abi-trace-test_LIBS := $(RUNTIME_LIB)

runtime-abi-option-test_SOURCES := tests/runtime_abi_option.c
runtime-abi-option-test_LIBS := $(RUNTIME_LIB)

storage-selection-test_SOURCES := tests/storage_selection.c \
    compiler/storage_selection.c
storage-selection-test_LIBS := $(LANTERN_LIB)

typehash-abi-test_SOURCES := tests/typehash_abi.c
typehash-abi-test_LIBS := $(LANTERN_LIB)

abi-string-value-test_SOURCES := tests/abi_string_value.c \
    abi/abi_string.c

abi-layout-test_SOURCES := tests/abi_layout.c \
    compiler/abi_layout.c \
    compiler/abi_record_layout.c \
    compiler/builtin_types.c
abi-layout-test_LIBS := $(LANTERN_LIB)

abi-record-layout-test_SOURCES := tests/abi_record_layout.c \
    compiler/abi_record_layout.c
abi-record-layout-test_LIBS := $(LANTERN_LIB)

abi-directory-test_SOURCES := tests/abi_directory.c \
    abi/abi_directory.c
abi-directory-test_LIBS := $(LANTERN_LIB)

abi-container-test_SOURCES := tests/abi_container.c \
    compiler/abi_container.c \
    abi/abi_directory.c
abi-container-test_LIBS := $(LANTERN_LIB)

abi-index-patch-test_SOURCES := tests/abi_index_patch.c \
    compiler/abi_index_patch.c
abi-index-patch-test_LIBS := $(LANTERN_LIB)

abi-emit-test_SOURCES := tests/abi_emit.c \
    compiler/abi_emit.c \
    compiler/abi_emit_strings.c \
    abi/abi_string.c \
    compiler/abi_layout.c \
    compiler/abi_record_layout.c \
    compiler/abi_type_index.c \
    compiler/abi_type_rows.c \
    compiler/abi_type_layout_rows.c \
    compiler/abi_enum_rows.c \
    compiler/abi_function_rows.c \
    compiler/abi_constraint_rows.c \
    compiler/abi_storage.c \
    compiler/abi_trait_method_rows.c \
    compiler/abi_type_signatures.c \
    compiler/abi_decl_index.c \
    compiler/abi_declarations.c \
    compiler/abi_container.c \
    abi/abi_directory.c
abi-emit-test_LIBS := $(LANTERN_LIB)

abi-emit-unit-test_SOURCES := tests/abi_emit_unit.c \
    compiler/builtin_types.c \
    compiler/abi_emit.c \
    compiler/abi_emit_strings.c \
    abi/abi_string.c \
    compiler/abi_link.c \
    compiler/abi_link_finish.c \
    compiler/abi_link_strings.c \
    compiler/abi_index_patch.c \
    compiler/abi_layout.c \
    compiler/abi_record_layout.c \
    compiler/abi_type_index.c \
    compiler/abi_type_rows.c \
    compiler/abi_type_layout_rows.c \
    compiler/abi_enum_rows.c \
    compiler/abi_function_rows.c \
    compiler/abi_constraint_rows.c \
    compiler/abi_storage.c \
    compiler/abi_trait_method_rows.c \
    compiler/abi_type_signatures.c \
    compiler/abi_decl_index.c \
    compiler/abi_declarations.c \
    compiler/abi_container.c \
    abi/abi_directory.c
abi-emit-unit-test_LIBS := $(LANTERN_LIB)

abi-record-object-test_SOURCES := tests/abi_record_object.c \
    compiler/abi_container.c \
    abi/abi_directory.c
abi-record-object-test_LIBS := $(LANTERN_LIB)

abi-link-input-test_SOURCES := tests/abi_link_input.c \
    compiler/abi_link.c \
    abi/abi_string.c \
    compiler/abi_index_patch.c \
    compiler/abi_link_finish.c \
    compiler/abi_link_strings.c \
    compiler/abi_emit.c \
    compiler/abi_emit_strings.c \
    compiler/abi_layout.c \
    compiler/abi_record_layout.c \
    compiler/abi_type_index.c \
    compiler/abi_type_rows.c \
    compiler/abi_type_layout_rows.c \
    compiler/abi_enum_rows.c \
    compiler/abi_function_rows.c \
    compiler/abi_constraint_rows.c \
    compiler/abi_storage.c \
    compiler/abi_trait_method_rows.c \
    compiler/abi_type_signatures.c \
    compiler/abi_decl_index.c \
    compiler/abi_declarations.c \
    compiler/abi_container.c \
    abi/abi_directory.c
abi-link-input-test_LIBS := $(LANTERN_LIB)

abi-register-object-test_SOURCES := tests/abi_register_object.c
abi-register-object-test_LIBS := $(RUNTIME_LIB) $(LANTERN_LIB)

abi-emit-strings-test_SOURCES := tests/abi_emit_strings.c \
    compiler/abi_emit_strings.c \
    abi/abi_string.c
abi-emit-strings-test_LIBS := $(LANTERN_LIB)

abi-type-rows-test_SOURCES := tests/abi_type_rows.c \
    compiler/builtin_types.c \
    compiler/abi_type_rows.c \
    compiler/abi_type_index.c \
    compiler/abi_emit_strings.c \
    abi/abi_string.c \
    compiler/abi_decl_index.c
abi-type-rows-test_LIBS := $(LANTERN_LIB)

abi-type-layout-rows-test_SOURCES := tests/abi_type_layout_rows.c \
    compiler/abi_type_layout_rows.c \
    compiler/abi_type_rows.c \
    compiler/abi_type_index.c \
    compiler/abi_emit_strings.c \
    compiler/abi_decl_index.c
abi-type-layout-rows-test_LIBS := $(LANTERN_LIB)

abi-type-signatures-test_SOURCES := tests/abi_type_signatures.c \
    compiler/abi_type_signatures.c \
    compiler/abi_type_rows.c \
    compiler/abi_type_index.c \
    compiler/abi_emit_strings.c \
    abi/abi_string.c \
    compiler/abi_decl_index.c
abi-type-signatures-test_LIBS := $(LANTERN_LIB)

abi-decl-index-test_SOURCES := tests/abi_decl_index.c \
    compiler/abi_decl_index.c
abi-decl-index-test_LIBS := $(LANTERN_LIB)

abi-declarations-test_SOURCES := tests/abi_declarations.c \
    compiler/abi_declarations.c \
    compiler/abi_decl_index.c \
    compiler/abi_type_rows.c \
    compiler/abi_type_index.c \
    compiler/abi_emit_strings.c
abi-declarations-test_LIBS := $(LANTERN_LIB)

abi-type-index-test_SOURCES := tests/abi_type_index.c \
    compiler/abi_type_index.c \
    compiler/abi_decl_index.c
abi-type-index-test_LIBS := $(LANTERN_LIB)

llvm-backend-test_SOURCES := tests/llvm_backend.c \
    compiler/llvm_backend.c \
    compiler/llvm_diagnostics.c \
    compiler/catalog.c \
    compiler/llvm_native_cache.c \
    compiler/cache_location.c \
    runtime/runtime_native_roots.c \
    compiler/compiler_native_roots.c \
    compiler/compiler_axioms.c \
    compiler/compiler_system.c
llvm-backend-test_LIBS = $(VERSION_OBJECT) $(LANTERN_LIB)
llvm-backend-test_LLVM := yes

llvm-native-cache-test_SOURCES := tests/llvm_native_cache.c \
    compiler/llvm_native_cache.c \
    compiler/cache_location.c
llvm-native-cache-test_LIBS := $(LANTERN_LIB)
llvm-native-cache-test_LLVM := yes

llvm-stack-maps-test_SOURCES := tests/llvm_stack_maps.c \
    compiler/llvm_stack_maps.c \
    compiler/native_frame_maps.c \
    compiler/native_frame_saves.c
llvm-stack-maps-test_LIBS := $(LANTERN_LIB)
llvm-stack-maps-test_LLVM := yes

llvm-frame-roots-test_SOURCES := tests/llvm_frame_roots.c \
    compiler/llvm_backend.c \
    compiler/llvm_diagnostics.c \
    compiler/catalog.c \
    compiler/llvm_native_cache.c \
    compiler/cache_location.c \
    compiler/llvm_stack_maps.c \
    compiler/native_frame_maps.c \
    compiler/native_frame_saves.c \
    runtime/runtime_native_roots.c \
    compiler/compiler_native_roots.c \
    compiler/compiler_axioms.c \
    compiler/compiler_system.c
llvm-frame-roots-test_LIBS = $(VERSION_OBJECT) $(LANTERN_LIB)
llvm-frame-roots-test_LLVM := yes
