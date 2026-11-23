TEST_PROGRAMS := writ-coexistence \
    builtin-os-test \
    builtin-abi-test \
    runtime-abi-io-test \
    runtime-abi-test \
    runtime-abi-scc-test \
    runtime-abi-trace-test \
    runtime-abi-option-test \
    storage-selection-test \
    abi-string-value-test \
    abi-layout-test \
    abi-record-layout-test \
    abi-directory-test \
    abi-container-test \
    abi-emit-test \
    abi-record-object-test \
    abi-register-object-test \
    abi-emit-strings-test \
    abi-type-signatures-test \
    llvm-native-cache-test \
    llvm-stack-maps-test

writ-coexistence_SOURCES := tests/writ_coexistence.c
writ-coexistence_LIBS := $(WRIT_LIB) $(LANTERN_LIB)

builtin-os-test_SOURCES := tests/builtin_os.c
builtin-os-test_LIBS := $(HOST_RUNTIME_LIB)

builtin-abi-test_SOURCES := tests/builtin_abi.c
builtin-abi-test_LIBS := $(HOST_RUNTIME_LIB)

runtime-abi-io-test_SOURCES := tests/runtime_abi_io.c
runtime-abi-io-test_LIBS := $(HOST_RUNTIME_LIB)

runtime-abi-test_SOURCES := tests/runtime_abi.c
runtime-abi-test_LIBS := $(HOST_RUNTIME_LIB)

runtime-abi-scc-test_SOURCES := tests/runtime_abi_scc.c
runtime-abi-scc-test_LIBS := $(HOST_RUNTIME_LIB)

runtime-abi-trace-test_SOURCES := tests/runtime_abi_trace.c
runtime-abi-trace-test_LIBS := $(HOST_RUNTIME_LIB)

runtime-abi-option-test_SOURCES := tests/runtime_abi_option.c
runtime-abi-option-test_LIBS := $(HOST_RUNTIME_LIB)

storage-selection-test_SOURCES := tests/storage_selection.c \
    compiler/storage_selection.c
storage-selection-test_LIBS := $(LANTERN_LIB)

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

abi-record-object-test_SOURCES := tests/abi_record_object.c \
    compiler/abi_container.c \
    abi/abi_directory.c
abi-record-object-test_LIBS := $(LANTERN_LIB)

abi-register-object-test_SOURCES := tests/abi_register_object.c
abi-register-object-test_LIBS := $(HOST_RUNTIME_LIB) $(LANTERN_LIB)

abi-emit-strings-test_SOURCES := tests/abi_emit_strings.c \
    compiler/abi_emit_strings.c \
    abi/abi_string.c
abi-emit-strings-test_LIBS := $(LANTERN_LIB)

abi-type-signatures-test_SOURCES := tests/abi_type_signatures.c \
    compiler/abi_type_signatures.c \
    compiler/abi_type_rows.c \
    compiler/abi_type_index.c \
    compiler/abi_emit_strings.c \
    abi/abi_string.c \
    compiler/abi_decl_index.c
abi-type-signatures-test_LIBS := $(LANTERN_LIB)

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
