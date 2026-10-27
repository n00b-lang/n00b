RUNTIME_SRCS := runtime/runtime_abi.c runtime/runtime_allocate.c \
    runtime/runtime_abi_io.c runtime/runtime_panic.c runtime/runtime_dwarf.c \
    runtime/runtime_ffi.c runtime/runtime_unwind.c runtime/runtime_abi_storage.c \
    runtime/native_frame_runtime.c runtime/builtin_os.c runtime/builtin_os_stw.c \
    runtime/builtin_os_blocking.c \
    runtime/builtin_abi.c abi/abi_directory.c abi/abi_string.c
N00B_SRCS := $(wildcard compiler/*.c) abi/abi_directory.c abi/abi_string.c \
    runtime/runtime_native_roots.c

N00B_OBJ := $(BUILD)/obj
RUNTIME_OBJ := $(BUILD)/runtime
RUNTIME_LTO_OBJ := $(BUILD)/runtime-lto
N00B_OBJS := $(N00B_SRCS:%.c=$(N00B_OBJ)/%.o)
RUNTIME_OBJS := $(RUNTIME_SRCS:%.c=$(RUNTIME_OBJ)/%.o)
RUNTIME_LTO_OBJS := $(RUNTIME_SRCS:%.c=$(RUNTIME_LTO_OBJ)/%.o)
VERSION_HEADER := $(BUILD)/n00bb_version.h
VERSION_SOURCE := $(BUILD)/n00bb_version.c
VERSION_OBJECT := $(N00B_OBJ)/n00bb_version.o
IDENTITY_SOURCE := $(BUILD)/n00bb_identity.c
IDENTITY_OBJECT := $(N00B_OBJ)/n00bb_identity.o

N00B_INC := -I$(SRC) -I$(SRC)/compiler -I$(SRC)/abi -I$(SRC)/runtime -I$(BUILD) \
    $(LANTERN_INC)
N00B_DEFINES := \
    -DN00BB_SOURCE_ROOT="\"$(NATIVE_SRC)\"" \
    -DN00BB_BUILD_ROOT="\"$(NATIVE_BUILD)\"" \
    -DN00BB_STACK_OBJECT_LIMIT_BYTES=4096 \
    -DN00BB_WRIT_PROFILE_DIR="\"$(NATIVE_SRC)/writ/profiles\"" \
    -DN00BB_WRIT_CONTRACT_DIR="\"$(NATIVE_SRC)/writ/contracts\"" \
    -DN00BB_LLVM_CLANG="\"$(NATIVE_LLVM_BINDIR)/clang$(EXE)\"" \
    -DN00BB_CARGO="\"$(CARGO)\"" \
    -DN00BB_LAYOUT_MARKER="\"n00b-toolchain\"" \
    -DN00BB_LAYOUT_STDLIB="\"stdlib\"" \
    -DN00BB_LAYOUT_RUNTIME="\"lib\"" \
    -DN00BB_LAYOUT_GRAMMAR="\"share/grammar/n00b.bnf\"" \
    -DN00BB_LAYOUT_WRIT_PROFILES="\"share/writ/profiles\"" \
    -DN00BB_LAYOUT_WRIT_CONTRACTS="\"share/writ/contracts\"" \
    -DN00BB_RUNTIME_ARCHIVE="\"$(notdir $(RUNTIME_LIB))\"" \
    -DN00BB_RUNTIME_BITCODE="\"$(notdir $(RUNTIME_BITCODE))\""
N00B_CFLAGS := $(BASE_CFLAGS) $(OPT) $(N00B_INC) -I$(SRC)/writ/include \
    $(LLVM_CFLAGS) $(N00B_DEFINES)
RUNTIME_CFLAGS := $(BASE_CFLAGS) -O3 -g -fno-omit-frame-pointer $(N00B_INC) \
    -DN00BB_SOURCE_ROOT="\"$(NATIVE_SRC)\"" -DN00BB_STACK_OBJECT_LIMIT_BYTES=4096

$(VERSION_HEADER): FORCE
	@mkdir -p $(dir $@)
	@sh tools/version.sh $(SRC) $@

$(VERSION_SOURCE): $(VERSION_HEADER)

$(VERSION_OBJECT): $(VERSION_SOURCE) $(call flags_file,n00b)
	@mkdir -p $(dir $@)
	$(CC) $(N00B_CFLAGS) -c $< -o $@

$(IDENTITY_SOURCE): $(N00B_OBJS) $(RUNTIME_LIB) $(LANTERN_LIB) $(WRIT_LIB) $(call flags_file,link)
	@printf '#include <stdint.h>\nconst uint64_t n00bb_compiler_identity = 0x%sULL;\n' \
	    "$$(cat $^ | shasum -a 256 | cut -c1-16)" >$@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(IDENTITY_OBJECT): $(IDENTITY_SOURCE) $(call flags_file,n00b)
	@mkdir -p $(dir $@)
	$(CC) $(N00B_CFLAGS) -c $< -o $@

$(eval $(call record_flags,n00b,$(CC) $(N00B_CFLAGS)))
$(eval $(call record_flags,runtime,$(CC) $(RUNTIME_CFLAGS)))

$(N00B_OBJ)/%.o: %.c $(call flags_file,n00b) | $(VERSION_HEADER)
	@mkdir -p $(dir $@)
	$(CC) $(N00B_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(RUNTIME_OBJ)/%.o: %.c $(call flags_file,runtime)
	@mkdir -p $(dir $@)
	$(CC) $(RUNTIME_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(RUNTIME_LTO_OBJ)/%.o: %.c $(call flags_file,runtime)
	@mkdir -p $(dir $@)
	$(CC) $(RUNTIME_CFLAGS) -flto -MMD -MP -c $< -o $@

$(RUNTIME_LIB): $(RUNTIME_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(RUNTIME_BITCODE): $(RUNTIME_LTO_OBJS)
	$(LLVM_LINK) $^ -o $@

cross_triple_x86_64-windows := x86_64-w64-windows-gnu

define cross_runtime_rules
$(1)_RUNTIME_DIR := $(BUILD)/$(1)
$(1)_RUNTIME_OBJS := $$(RUNTIME_SRCS:%.c=$$($(1)_RUNTIME_DIR)/obj/%.o)
$(1)_RUNTIME_LTO_OBJS := $$(RUNTIME_SRCS:%.c=$$($(1)_RUNTIME_DIR)/lto/%.o)
$(1)_RUNTIME_CFLAGS := $$(RUNTIME_CFLAGS) --target=$$(cross_triple_$(1))
$$(eval $$(call record_flags,runtime-$(1),$$(CC) $$($(1)_RUNTIME_CFLAGS)))

$$($(1)_RUNTIME_DIR)/obj/%.o: %.c $$(call flags_file,runtime-$(1))
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(1)_RUNTIME_CFLAGS) -MMD -MP -c $$< -o $$@

$$($(1)_RUNTIME_DIR)/lto/%.o: %.c $$(call flags_file,runtime-$(1))
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(1)_RUNTIME_CFLAGS) -flto -MMD -MP -c $$< -o $$@

$$($(1)_RUNTIME_DIR)/$$(notdir $$(RUNTIME_LIB)): $$($(1)_RUNTIME_OBJS)
	@rm -f $$@
	$$(LLVM_AR) rcs $$@ $$^

$$($(1)_RUNTIME_DIR)/$$(notdir $$(RUNTIME_BITCODE)): $$($(1)_RUNTIME_LTO_OBJS)
	$$(LLVM_LINK) $$^ -o $$@


CROSS_RUNTIMES += $$($(1)_RUNTIME_DIR)/$$(notdir $$(RUNTIME_LIB)) \
    $$($(1)_RUNTIME_DIR)/$$(notdir $$(RUNTIME_BITCODE))
endef

CROSS_RUNTIMES :=
$(foreach t,$(CROSS_TARGETS),$(eval $(call cross_runtime_rules,$(t))))

$(eval $(call record_flags,link,$(LLVM_LIBS) $(LLVM_TEST_LIBS) $(SYSTEM_LIBS)))

$(N00B): $(N00B_OBJS) $(VERSION_OBJECT) $(IDENTITY_OBJECT) $(RUNTIME_LIB) $(LANTERN_LIB) $(WRIT_LIB) $(call flags_file,link)
	$(CC) -o $@ $(N00B_OBJS) $(VERSION_OBJECT) $(IDENTITY_OBJECT) \
	    $(RUNTIME_LIB) $(WRIT_LIB) $(LANTERN_LIB) $(LLVM_LIBS) $(SYSTEM_LIBS)

define test_program_rule
$(BUILD)/$(1): $$(patsubst %.c,$(N00B_OBJ)/%.o,$$($(1)_SOURCES)) $$($(1)_LIBS) $(call flags_file,link)
	$$(CC) -o $$@ $$(filter-out %.flags,$$^) $$(if $$($(1)_LLVM),$$(LLVM_TEST_LIBS)) $$(SYSTEM_LIBS)
endef

$(foreach p,$(TEST_PROGRAMS),$(eval $(call test_program_rule,$(p))))

TEST_PROGRAM_FILES := $(TEST_PROGRAMS:%=$(BUILD)/%)
ifeq ($(UNAME),Darwin)
TEST_PROGRAM_FILES += $(BUILD)/gc-worker-thread.o
endif

$(BUILD)/gc-worker-thread.o: tests/gc_worker_thread.c
	$(CC) -std=c11 -Wall -Wextra -Werror -c $< -o $@

.PHONY: n00b-test-programs
n00b-test-programs: $(N00B) $(RUNTIME_BITCODE) $(TEST_PROGRAM_FILES)
