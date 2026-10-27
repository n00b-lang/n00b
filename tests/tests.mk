TEST_RESULTS := $(BUILD)/test-results
export TEST_RESULTS
export N00B_DIAGNOSTICS := terse
export XDG_CONFIG_HOME := $(BUILD)/test-config
export N00B_RUST
export CC

N00B_MANIFEST := tests/n00b.tests
LANTERN_MANIFEST := lantern/tests/lantern.tests
LANTERN_BENCH_MANIFEST := lantern/tests/lantern.bench
WRIT_MANIFEST := writ/tests/writ.tests

select = $(shell awk -F'\t' '$(2) { print $$1 }' $(1))
dev = $$2 ~ /(^|,)dev(,|$$)/
pr = $$2 ~ /(^|,)pr(,|$$)/
serial = $$2 ~ /(^|,)serial(,|$$)/

N00B_DEV := $(call select,$(N00B_MANIFEST),$(dev))
N00B_PR := $(call select,$(N00B_MANIFEST),$(pr) && !($(serial)))
N00B_SERIAL := $(call select,$(N00B_MANIFEST),$(pr) && $(serial))
N00B_ALL := $(call select,$(N00B_MANIFEST),1)
LANTERN_DEV := $(call select,$(LANTERN_MANIFEST),$(dev))
LANTERN_PR := $(call select,$(LANTERN_MANIFEST),$(pr))
LANTERN_ALL := $(call select,$(LANTERN_MANIFEST),1)
LANTERN_BENCH := $(call select,$(LANTERN_BENCH_MANIFEST),1)
WRIT_DEV := $(call select,$(WRIT_MANIFEST),$(dev))
WRIT_PR := $(call select,$(WRIT_MANIFEST),$(pr))
WRIT_ALL := $(call select,$(WRIT_MANIFEST),1)
DEV_TESTS := $(N00B_DEV:%=test/n00b/%) $(LANTERN_DEV:%=test/lantern/%) \
    $(WRIT_DEV:%=test/writ/%)
PR_TESTS := $(N00B_PR:%=test/n00b/%) $(N00B_SERIAL:%=serial/n00b/%) \
    $(LANTERN_PR:%=test/lantern/%) $(WRIT_PR:%=test/writ/%)

RUN_TEST := python3 tools/run_test.py

.PHONY: test test-pr test-all test-lantern test-writ bench test-reset test-summary

test-reset:
	@rm -rf $(TEST_RESULTS)
	@mkdir -p $(TEST_RESULTS)
	@python3 -c 'import time; print(f"{time.time():.2f}")' > $(TEST_RESULTS)/started

test/n00b/%: n00b-test-programs | test-reset
	@N00B_CACHE=$(BUILD)/n00b-cache $(RUN_TEST) $(N00B_MANIFEST) n00b $(SRC) $(BUILD) $*

test/lantern/%: lantern-tests | test-reset
	@$(RUN_TEST) $(LANTERN_MANIFEST) lantern $(SRC)/lantern $(BUILD)/lantern $*

bench/lantern/%: lantern-tests | test-reset
	@$(RUN_TEST) $(LANTERN_BENCH_MANIFEST) lantern-bench $(SRC)/lantern $(BUILD)/lantern $*

test/writ/%: writ-tests | test-reset
	@LSAN_OPTIONS=suppressions=$(SRC)/writ/tests/lsan.supp:print_suppressions=0 \
	    $(RUN_TEST) $(WRIT_MANIFEST) writ $(SRC)/writ $(BUILD)/writ $*

serial/n00b/%: $(N00B_PR:%=test/n00b/%)
	@N00B_CACHE=$(BUILD)/n00b-cache $(RUN_TEST) $(N00B_MANIFEST) n00b $(SRC) $(BUILD) $*

ifdef T
SELECTED := $(addprefix test/n00b/,$(filter $(T),$(N00B_ALL))) \
    $(addprefix test/lantern/,$(filter $(T),$(LANTERN_ALL))) \
    $(addprefix test/writ/,$(filter $(T),$(WRIT_ALL)))
UNKNOWN := $(filter-out $(N00B_ALL) $(LANTERN_ALL) $(WRIT_ALL),$(T))
ifneq ($(UNKNOWN),)
$(error no test named $(UNKNOWN); test names are the first column of $(N00B_MANIFEST), $(LANTERN_MANIFEST) and $(WRIT_MANIFEST))
endif
test: $(SELECTED)
	@tools/test_summary.sh
else
test: $(DEV_TESTS)
	@TEST_TIER=dev tools/test_summary.sh
endif

test-pr: $(PR_TESTS)
	@TEST_TIER=pr tools/test_summary.sh

test-all: $(DEV_TESTS) $(PR_TESTS)
	@tools/test_summary.sh

test-lantern: $(LANTERN_ALL:%=test/lantern/%)
	@tools/test_summary.sh

test-writ: $(WRIT_ALL:%=test/writ/%)
	@tools/test_summary.sh

bench: $(LANTERN_BENCH:%=bench/lantern/%)
	@tools/test_summary.sh
