B ?= build
BUILD := $(abspath $(B))
SRC := $(abspath .)
CONFIG := $(BUILD)/config.mk

.DEFAULT_GOAL := all

$(CONFIG): tools/configure.sh
	@mkdir -p $(BUILD)
	@printf '*\n' > $(BUILD)/.gitignore
	@tools/configure.sh $@

include $(CONFIG)

UNAME := $(shell uname -s)
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
HOST_WINDOWS := $(if $(filter MINGW% MSYS% CYGWIN%,$(UNAME)),1)
EXE := $(if $(HOST_WINDOWS),.exe)
native_path = $(if $(HOST_WINDOWS),$(shell cygpath -m '$(1)'),$(1))
NATIVE_SRC := $(call native_path,$(SRC))
NATIVE_BUILD := $(call native_path,$(BUILD))
NATIVE_LLVM_BINDIR := $(call native_path,$(LLVM_BINDIR))

ifeq ($(UNAME),Darwin)
export SDKROOT
SYSTEM_LIBS := -lpthread -lm
else ifdef HOST_WINDOWS
SYSTEM_LIBS := -lpthread -lm -lsynchronization
else
SYSTEM_LIBS := -lpthread -lm -ldl
endif

ifeq ($(BUILD_TYPE),release)
OPT := -O3 -g
else ifeq ($(BUILD_TYPE),debug)
OPT := -O0 -g
else ifeq ($(BUILD_TYPE),sanitize)
SANITIZE_FLAGS := -fsanitize=address,undefined
OPT := -O1 -g -fno-omit-frame-pointer $(SANITIZE_FLAGS)
SYSTEM_LIBS += $(SANITIZE_FLAGS)
export N00B_SANITIZE_FLAGS := $(SANITIZE_FLAGS)
else
OPT := -O2 -g
endif

FLAGS_DIR := $(BUILD)/flags
flags_file = $(FLAGS_DIR)/$(1).flags
define record_flags
$(FLAGS_DIR)/$(1).flags: FORCE
	@mkdir -p $(FLAGS_DIR)
	@printf '%s\n' '$(subst ','"'"',$(2))' > $$@.tmp
	@if cmp -s $$@.tmp $$@; then rm -f $$@.tmp; else mv $$@.tmp $$@; fi
endef

WARN := -Wall -Wextra
DEPFLAGS = -MMD -MP -MJ $@.json
BASE_CFLAGS := -std=c2x $(WARN)

LANTERN_INC := -I$(SRC)/lantern/include -I$(SRC)/lantern/src \
    -isystem $(SRC)/lantern/include/vendor
LANTERN_LIB := $(BUILD)/lantern/libir.a
WRIT_LIB := $(BUILD)/writ/libwrit.a
RUNTIME_LIB := $(BUILD)/libn00b-abi-runtime.a
RUNTIME_BITCODE := $(BUILD)/libn00b-abi-runtime.bc
N00B := $(BUILD)/n00b

include lantern/lantern.mk
include writ/writ.mk
include writ/writ-tests.mk
include tests/test-programs.mk
include compiler/n00b.mk
include tests/tests.mk

.PHONY: all clean compdb FORCE
FORCE:

all: $(N00B) $(RUNTIME_BITCODE) $(CROSS_RUNTIMES) compdb

compdb: $(BUILD)/compile_commands.json

$(BUILD)/compile_commands.json: $(N00B)
	@{ printf '[\n'; find $(BUILD) -name '*.o.json' -exec cat {} + | sed '$$ s/,$$//'; printf ']\n'; } > $@.tmp && mv $@.tmp $@

clean:
	rm -rf $(BUILD)

.PHONY: install uninstall regen-unicode
install: $(N00B) $(RUNTIME_BITCODE) $(CROSS_RUNTIMES)
	@tools/install.sh --build $(BUILD) $(if $(PREFIX),--prefix $(PREFIX)) $(INSTALL_FLAGS)

uninstall:
	@tools/install.sh --uninstall $(if $(PREFIX),--prefix $(PREFIX))

regen-unicode:
	python3 tools/gen_unicode_tables.py --cache-dir $(HOME)/.cache/n00b/ucd \
	    --ucd-version 18.0.0 --out-dir lib/unicode/generated \
	    --conformance-dir tests/data/unicode \
	    --conformance-program tests/programs/unicode/gen_conformance.n \
	    --test-data-dir $(BUILD)/unicode-test-data --allow-downloads --no-strict
	python3 tools/gen_lantern_unicode.py --allow-downloads

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
