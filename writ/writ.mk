WRIT_SRCS := $(wildcard writ/src/core/*.c writ/src/core/*/*.c \
    writ/src/gen/*.c writ/src/toolchain/*.c)
WRIT_CLANG_SRCS := $(wildcard writ/src/clang/*.c)
WRIT_OBJ := $(BUILD)/writ/obj
WRIT_OBJS := $(WRIT_SRCS:%.c=$(WRIT_OBJ)/%.o)
WRIT_CLANG_OBJS := $(WRIT_CLANG_SRCS:%.c=$(WRIT_OBJ)/%.o)
WRIT_RAW := $(BUILD)/writ/libwrit_raw.a
WRIT_CLANG_LIB := $(BUILD)/writ/libwrit_clang.a
WRIT := $(BUILD)/writ/writ
WRIT_DEFS := $(BUILD)/writ/defs
WRIT_MODEL_DEFS := $(WRIT_DEFS)/model_names.h $(WRIT_DEFS)/model_json.h
WRIT_GENERATED := $(WRIT_MODEL_DEFS) $(WRIT_DEFS)/writ_keywords.h

WRIT_CFLAGS := $(BASE_CFLAGS) $(OPT) $(LANTERN_INC) -I$(SRC)/writ/include \
    -I$(WRIT_DEFS) -I$(SRC)/writ/defs -I$(SRC)/writ/src/core \
    -DWRIT_CONTRACT_INSTALL_DIR="\"/usr/local/share/writ/contracts\""
ifneq ($(UNAME),Darwin)
WRIT_CFLAGS += -D_GNU_SOURCE
endif
ifeq ($(UNAME),Linux)
WRIT_CFLAGS += -fPIC
endif
LIBCLANG := -L$(LLVM_LIBDIR) -lclang $(if $(HOST_WINDOWS),,-Wl,-rpath,$(LLVM_LIBDIR))
WRIT_LINK_EXTRA := $(if $(HOST_WINDOWS),$(LANTERN_LIB))

$(eval $(call record_flags,writ,$(CC) $(WRIT_CFLAGS)))

$(WRIT_DEFS)/model.stamp: writ/defs/model_fields.def writ/tools/gen_model.py
	@mkdir -p $(WRIT_DEFS)
	python3 writ/tools/gen_model.py $< --internal $(WRIT_MODEL_DEFS)
	@touch $@

$(WRIT_MODEL_DEFS): $(WRIT_DEFS)/model.stamp
	@test -f $@ || python3 writ/tools/gen_model.py writ/defs/model_fields.def --internal $(WRIT_MODEL_DEFS)

$(WRIT_DEFS)/keywords.stamp: writ/grammar/writ.bnf writ/tools/soft_keywords.py
	@mkdir -p $(WRIT_DEFS)
	python3 writ/tools/soft_keywords.py $< $(WRIT_DEFS)/writ_keywords.h
	@touch $@

$(WRIT_DEFS)/writ_keywords.h: $(WRIT_DEFS)/keywords.stamp
	@test -f $@ || python3 writ/tools/soft_keywords.py writ/grammar/writ.bnf $@

$(WRIT_OBJ)/%.o: %.c $(call flags_file,writ) | $(WRIT_GENERATED)
	@mkdir -p $(dir $@)
	$(CC) $(WRIT_CFLAGS) $(CC_EXTRA) $(DEPFLAGS) -c $< -o $@

$(WRIT_CLANG_OBJS): CC_EXTRA := -I$(LLVM_INCLUDEDIR)

$(WRIT_RAW): $(WRIT_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(WRIT_LIB): $(WRIT_RAW) $(LANTERN_LIB) writ/tools/isolate.sh writ/exports/writ.exp
	writ/tools/isolate.sh $(CC) $@ $(WRIT_RAW) $(LANTERN_LIB) $(SRC)/writ/exports/writ.exp

$(WRIT_CLANG_LIB): $(WRIT_CLANG_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(WRIT_OBJ)/writ/src/cli/main.o: CC_EXTRA := \
    -DWRIT_DEFAULT_LLVM_PREFIX="\"$(call native_path,$(LLVM_PREFIX))\"" \
    -DWRIT_PROFILE_INSTALL_DIR="\"$(NATIVE_SRC)/writ/profiles\""

$(WRIT): $(WRIT_OBJ)/writ/src/cli/main.o $(WRIT_CLANG_LIB) $(WRIT_LIB) $(WRIT_LINK_EXTRA)
	$(CC) -o $@ $^ $(LIBCLANG) $(SYSTEM_LIBS)

.PHONY: writ writ-tests
writ: $(WRIT_LIB) $(WRIT)
