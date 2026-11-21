LANTERN_DIRS := th logic lib diag parse scanner ir unicode types
LANTERN_SRCS := $(foreach d,$(LANTERN_DIRS),$(wildcard lantern/src/$(d)/*.c))
LANTERN_OBJ := $(BUILD)/lantern/obj
LANTERN_OBJS := $(LANTERN_SRCS:%.c=$(LANTERN_OBJ)/%.o)
LANTERN_CFLAGS := $(BASE_CFLAGS) $(OPT) $(LANTERN_INC)
ifeq ($(UNAME),Linux)
LANTERN_CFLAGS += -fPIC
endif
LANTERN_TEST_CFLAGS := $(LANTERN_CFLAGS) -I$(SRC)/lantern/tests

$(eval $(call record_flags,lantern,$(CC) $(LANTERN_TEST_CFLAGS)))

$(LANTERN_OBJ)/lantern/tests/%.o: lantern/tests/%.c $(call flags_file,lantern)
	@mkdir -p $(dir $@)
	$(CC) $(LANTERN_TEST_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(LANTERN_OBJ)/%.o: %.c $(call flags_file,lantern)
	@mkdir -p $(dir $@)
	$(CC) $(LANTERN_CFLAGS) $(CC_EXTRA) $(DEPFLAGS) -c $< -o $@

$(LANTERN_LIB): $(LANTERN_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(BUILD)/lantern/pp: $(LANTERN_OBJ)/lantern/src/pp/main.o $(LANTERN_LIB)
	$(CC) -o $@ $^ $(SYSTEM_LIBS)

$(LANTERN_OBJ)/lantern/src/pp/main.o: CC_EXTRA := -DPP_GRAMMAR_DIR="\"$(NATIVE_SRC)/lantern/grammars\""

LANTERN_TESTS := ir/span \
    lib/macros lib/byteorder lib/cache diag/diagnostics lib/module \
    lib/snapshot_inventory lib/target lib/target_call lib/indexed_table \
    lib/link_unit lib/work_queue lib/arena lib/persist_arena \
    lib/object_archive lib/native_archive lib/object_emit lib/object_section \
    lib/variant lib/result \
    logic/datalog \
    th/nest_tmp th/constraint th/match \
    parse/pwz parse/ambiguity parse/commander parse/compose parse/tree_print \
    parse/symtab_binding parse/rewrite parse/generic_print \
    parse/bnf parse/annot parse/grammars parse/format \
    parse/bnfcheck \
    scanner/tokenizer scanner/lex scanner/lex_perf

LANTERN_TEST_PROGRAMS := $(foreach t,$(LANTERN_TESTS), \
    $(BUILD)/lantern/tests/$(subst /,_,$(t)))

define lantern_test_rule
$(BUILD)/lantern/tests/$(subst /,_,$(1)): $(LANTERN_OBJ)/lantern/tests/$(1).o $(LANTERN_LIB)
	@mkdir -p $$(dir $$@)
	$$(CC) -o $$@ $$^ $(SYSTEM_LIBS)
endef

$(foreach t,$(LANTERN_TESTS),$(eval $(call lantern_test_rule,$(t))))

.PHONY: lantern lantern-tests
lantern: $(LANTERN_LIB) $(BUILD)/lantern/pp
lantern-tests: lantern $(LANTERN_TEST_PROGRAMS)
