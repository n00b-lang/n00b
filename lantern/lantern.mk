LANTERN_DIRS := th logic lib diag parse scanner ir unicode
LANTERN_SRCS := $(filter-out lantern/src/ir/main.c, \
    $(foreach d,$(LANTERN_DIRS),$(wildcard lantern/src/$(d)/*.c)))
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

$(BUILD)/lantern/ir: $(LANTERN_OBJ)/lantern/src/ir/main.o $(LANTERN_LIB)
	$(CC) -o $@ $^ $(SYSTEM_LIBS)

$(BUILD)/lantern/pp: $(LANTERN_OBJ)/lantern/src/pp/main.o $(LANTERN_LIB)
	$(CC) -o $@ $^ $(SYSTEM_LIBS)

$(LANTERN_OBJ)/lantern/src/pp/main.o: CC_EXTRA := -DPP_GRAMMAR_DIR="\"$(NATIVE_SRC)/lantern/grammars\""

LANTERN_TESTS := ir/core ir/lower ir/lifetime ir/storage_origin ir/exec_graph \
    ir/shape ir/span ir/rule ir/typing ir/ranges ir/vn ir/abcd ir/verify \
    ir/grammar ir/layout ir/promote ir/structure ir/cli ir/pdg ir/walk \
    ir/typemap ir/text ir/diag ir/typelang ir/grammar_accept ir/public \
    ir/scale \
    lib/macros lib/byteorder lib/cache diag/diagnostics lib/module \
    lib/snapshot_inventory lib/target lib/target_call lib/indexed_table \
    lib/link_unit lib/work_queue lib/arena lib/persist_arena \
    lib/object_archive lib/native_archive lib/object_emit lib/object_section \
    lib/variant lib/result \
    logic/clpfd_domain logic/clpfd_store logic/datalog logic/program \
    th/cycle th/nest_tmp th/infer_cycle th/constraint th/environment \
    parse/pwz parse/ambiguity parse/commander parse/compose parse/tree_print \
    parse/infer parse/symtab_binding parse/rewrite parse/generic_print \
    parse/bnf parse/annot parse/bnf_print parse/grammars parse/format \
    parse/ambiguity_cli parse/bnfcheck \
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
lantern: $(LANTERN_LIB) $(BUILD)/lantern/ir $(BUILD)/lantern/pp
lantern-tests: lantern $(LANTERN_TEST_PROGRAMS)
