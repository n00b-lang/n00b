WRIT_TESTS_DIR := $(BUILD)/writ/tests
WRIT_GEN_DIR := $(BUILD)/writ/gen
SANITIZE := $(if $(HOST_WINDOWS),,-fsanitize=address,undefined -fno-omit-frame-pointer)
GENERATED_C := -std=c11 -Wall -Wextra -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
FUZZ_OBJ := $(BUILD)/writ/fuzz-obj
FUZZ_LIB := $(BUILD)/writ/libwrit_fuzz.a
FUZZ_OBJS := $(WRIT_SRCS:%.c=$(FUZZ_OBJ)/%.o)
HOST_TRIPLE := $(if $(HOST_WINDOWS),x86_64-w64-windows-gnu,$(shell uname -m | sed 's/arm64/aarch64/')-$(if $(filter Darwin,$(UNAME)),apple-darwin,linux-gnu))
RESHARP_ROOT ?= $(HOME)/resharp-writ

ifeq ($(UNAME),Darwin)
WRIT_SHARED := libwrit.0.dylib
WRIT_SHARED_FLAGS := -dynamiclib -install_name @rpath/$(WRIT_SHARED) \
    -Wl,-force_load,$(WRIT_RAW) \
    -Wl,-exported_symbols_list,$(SRC)/writ/exports/writ.exp
else ifdef HOST_WINDOWS
WRIT_SHARED := libwrit-0.dll
WRIT_SHARED_FLAGS := -shared \
    -Wl,--whole-archive $(WRIT_RAW) -Wl,--no-whole-archive
else
WRIT_SHARED := libwrit.so.0
WRIT_SHARED_FLAGS := -shared -Wl,-soname,$(WRIT_SHARED) \
    -Wl,--whole-archive $(WRIT_RAW) -Wl,--no-whole-archive \
    -Wl,--version-script=$(SRC)/writ/exports/writ.map
endif
export WRIT_SHARED HOST_TRIPLE RESHARP_ROOT

$(BUILD)/writ/$(WRIT_SHARED): $(WRIT_RAW) $(LANTERN_LIB)
	$(CC) -o $@ $(WRIT_SHARED_FLAGS) $(LANTERN_LIB) $(SYSTEM_LIBS)

$(FUZZ_OBJ)/%.o: %.c | $(WRIT_GENERATED)
	@mkdir -p $(dir $@)
	$(CC) $(WRIT_CFLAGS) -MMD -MP -fsanitize=fuzzer-no-link,address,undefined \
	    -fno-omit-frame-pointer -c $< -o $@

$(FUZZ_LIB): $(FUZZ_OBJS)
	@rm -f $@
	ar rcs $@ $^

WRIT_TEST_CFLAGS := $(BASE_CFLAGS) $(OPT) -I$(SRC)/writ/tests -I$(SRC)/writ/include -I$(SRC)/lantern/include

$(WRIT_TESTS_DIR)/api-session: writ/tests/api/session.c $(WRIT_LIB) $(WRIT_LINK_EXTRA)
	@mkdir -p $(dir $@)
	$(CC) $(WRIT_TEST_CFLAGS) $(SANITIZE) -o $@ $^ $(SYSTEM_LIBS)

$(WRIT_TESTS_DIR)/host-embedded: writ/tests/host/embedded.c $(WRIT_LIB) $(WRIT_LINK_EXTRA)
	@mkdir -p $(dir $@)
	$(CC) $(WRIT_TEST_CFLAGS) $(SANITIZE) -o $@ $^ $(SYSTEM_LIBS)

$(WRIT_TESTS_DIR)/core-internals: writ/tests/core/internals.c $(WRIT_LIB) $(WRIT_LINK_EXTRA)
	@mkdir -p $(dir $@)
	$(CC) $(WRIT_CFLAGS) -I$(SRC)/writ/tests -o $@ $^ $(SYSTEM_LIBS)

$(WRIT_TESTS_DIR)/grammar-audit: writ/tests/core/grammar_audit.c $(WRIT_LIB) $(WRIT_LINK_EXTRA)
	@mkdir -p $(dir $@)
	$(CC) $(WRIT_CFLAGS) -I$(SRC)/writ/tests -o $@ $^ $(SYSTEM_LIBS)

$(WRIT_TESTS_DIR)/fuzz-parse: writ/tests/fuzz/parse.c $(FUZZ_LIB) $(LANTERN_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(WRIT_TEST_CFLAGS) -fsanitize=fuzzer,address,undefined -o $@ $^ \
	    $(SYSTEM_LIBS)

$(WRIT_TESTS_DIR)/keywords.writ: writ/grammar/writ.bnf writ/tools/keyword_corpus.py
	@mkdir -p $(dir $@)
	python3 writ/tools/keyword_corpus.py $< $@

$(BUILD)/writ/libcallbacks.a: writ/tests/callbacks/library.c
	@mkdir -p $(BUILD)/writ/callbacks
	$(CC) -std=c11 $(WARN) $(OPT) -c $< -o $(BUILD)/writ/callbacks/library.o
	@rm -f $@
	ar rcs $@ $(BUILD)/writ/callbacks/library.o

BINDINGS := libc:libc/libc zlib:zlib/zlib callbacks:callbacks/callbacks \
    keywords:keywords/keywords policy:policy/policy unions:unions/unions \
    spans:spans/spans
ROUNDTRIPS := libc zlib callbacks
binding_name = $(word 1,$(subst :, ,$(1)))
binding_input = writ/tests/$(word 2,$(subst :, ,$(1))).writ
binding_stem = $(notdir $(word 2,$(subst :, ,$(1))))
ROUNDTRIP_LIBS_zlib := -lz
ROUNDTRIP_DEPS_callbacks := $(BUILD)/writ/libcallbacks.a

define binding_rules
$(WRIT_GEN_DIR)/$(call binding_name,$(1))/.stamp: $(call binding_input,$(1)) $(WRIT)
	@mkdir -p $$(dir $$@)
	$(WRIT) gen c $$< -o $$(dir $$@) --conformance
	@touch $$@

$(WRIT_TESTS_DIR)/$(call binding_name,$(1))-conformance: $(WRIT_GEN_DIR)/$(call binding_name,$(1))/.stamp
	@mkdir -p $$(dir $$@)
	$$(CC) $(GENERATED_C) $(SANITIZE) -o $$@ \
	    $(addprefix $(WRIT_GEN_DIR)/$(call binding_name,$(1))/$(call binding_stem,$(1)),_writ.c _writ_mock.c _writ_conformance.c)
endef

define roundtrip_rule
$(WRIT_TESTS_DIR)/$(1)-roundtrip: writ/tests/$(1)/roundtrip.c $(WRIT_GEN_DIR)/$(1)/.stamp $(ROUNDTRIP_DEPS_$(1))
	@mkdir -p $$(dir $$@)
	$$(CC) $(GENERATED_C) $(SANITIZE) -I$(SRC)/writ/tests -I$(WRIT_GEN_DIR)/$(1) -o $$@ \
	    writ/tests/$(1)/roundtrip.c $(WRIT_GEN_DIR)/$(1)/$(1)_writ.c \
	    $(ROUNDTRIP_DEPS_$(1)) $(ROUNDTRIP_LIBS_$(1))
endef

$(foreach b,$(BINDINGS),$(eval $(call binding_rules,$(b))))
$(foreach r,$(ROUNDTRIPS),$(eval $(call roundtrip_rule,$(r))))

WRIT_TEST_FILES := $(BUILD)/writ/$(WRIT_SHARED) \
    $(addprefix $(WRIT_TESTS_DIR)/,api-session host-embedded core-internals \
        grammar-audit $(if $(HOST_WINDOWS),,fuzz-parse) keywords.writ) \
    $(foreach b,$(BINDINGS),$(WRIT_TESTS_DIR)/$(call binding_name,$(b))-conformance) \
    $(ROUNDTRIPS:%=$(WRIT_TESTS_DIR)/%-roundtrip)

writ-tests: $(WRIT_TEST_FILES)
