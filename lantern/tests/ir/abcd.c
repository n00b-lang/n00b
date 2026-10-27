#include "ir/abcd.h"
#include "ir/analyses.h"
#include "ir/summary.h"
#include "ir/text.h"
#include "ir/typelang.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

#define MOD(...) \
    S("module m {\n  profile bce\n  target t\n\n" __VA_ARGS__ "}\n")

static const ir_profile_t *const PROFILES[] = {&ir_profile_bce};

static void test_last_element_under_positive_length(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *d = ir_diags_new(ctx, nullptr);
    ir_module_t *m = ir_module_parse(ctx, MOD(
        "  func @f(%a: array[i32]) -> i32 {\n"
        "    block entry:\n"
        "      %n: u64 = sequence.length %a\n"
        "      %zero: u64 = const 0\n"
        "      %positive: bool = gt %n, %zero\n"
        "      branch %positive, nonempty, empty\n"
        "    block nonempty:\n"
        "      %one: u64 = const 1\n"
        "      %last: u64 = sub trap %n, %one\n"
        "      %value: i32 = sequence.get %a, %last\n"
        "      %past: i32 = sequence.get %a, %n\n"
        "      %sum: i32 = add trap %value, %past\n"
        "      return %sum\n"
        "    block empty:\n"
        "      %nothing: i32 = const 0\n"
        "      return %nothing\n"
        "  }\n"), 0, PROFILES, 1, d);
    check("the module parses", m != nullptr);
    if (!m) return;
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);
    ir_func_t *f = ir_func_at(m, 0);
    ir_abcd_t *abcd = ir_func_abcd(f);
    ir_value_t *value = ir_value_find(f, S("value"));
    ir_value_t *past = ir_value_find(f, S("past"));
    check("a[n - 1] under n > 0 with n = len(a) is proven",
          value && ir_abcd_check_redundant(abcd, value->def));
    check("a[n] keeps its check", past
          && !ir_abcd_check_redundant(abcd, past->def));
    ir_ctx_free(ctx);
}

static void test_array_ssa_feeds_abcd(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *d = ir_diags_new(ctx, nullptr);
    ir_module_t *m = ir_module_parse(ctx, MOD(
        "  func @f(%a: array[i32], %picks: ref[array[u64]], %c: bool,"
        " %k: u64) -> i32 {\n"
        "    block entry:\n"
        "      %n: u64 = sequence.length %a\n"
        "      %zero: u64 = const 0\n"
        "      %one: u64 = const 1\n"
        "      %slot: ref[u64] = index %picks, %zero\n"
        "      %positive: bool = gt %n, %zero\n"
        "      branch %positive, nonempty, empty\n"
        "    block nonempty:\n"
        "      %last: u64 = sub trap %n, %one\n"
        "      branch %c, left, right\n"
        "    block left:\n"
        "      store %slot, %last\n"
        "      jump join\n"
        "    block right:\n"
        "      store %slot, %zero\n"
        "      jump join\n"
        "    block join:\n"
        "      %picked: u64 = load %slot\n"
        "      %value: i32 = sequence.get %a, %picked\n"
        "      %first: i32 = sequence.get %a, %k\n"
        "      %again: i32 = sequence.get %a, %k\n"
        "      %sum: i32 = add trap %value, %first\n"
        "      %total: i32 = add trap %sum, %again\n"
        "      return %total\n"
        "    block empty:\n"
        "      %nothing: i32 = const 0\n"
        "      return %nothing\n"
        "  }\n"), 0, PROFILES, 1, d);
    check("the array module parses", m != nullptr);
    if (!m) return;
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);
    ir_func_t *f = ir_func_at(m, 0);
    ir_abcd_t *abcd = ir_func_abcd(f);
    ir_value_t *value = ir_value_find(f, S("value"));
    ir_value_t *first = ir_value_find(f, S("first"));
    ir_value_t *again = ir_value_find(f, S("again"));
    check("a loaded element carries the bounds of every store reaching it",
          value && ir_abcd_check_redundant(abcd, value->def));
    check("the first access to an unknown index keeps its check",
          first && !ir_abcd_check_redundant(abcd, first->def));
    check("a dominating access to the same index of an unchanged array "
          "proves the repeat",
          again && ir_abcd_check_redundant(abcd, again->def));
    ir_ctx_free(ctx);
}

static void test_constraints_decide_compares_and_overflow(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *d = ir_diags_new(ctx, nullptr);
    ir_module_t *m = ir_module_parse(ctx, MOD(
        "  func @g(%x: u64, %n: u64) -> u64 {\n"
        "    block entry:\n"
        "      %one: u64 = const 1\n"
        "      %unguarded: u64 = add trap %x, %one\n"
        "      %outer: bool = lt %x, %n\n"
        "      branch %outer, inside, out\n"
        "    block inside:\n"
        "      %inner: bool = lt %x, %n\n"
        "      %reversed: bool = ge %x, %n\n"
        "      %next: u64 = add trap %x, %one\n"
        "      branch %inner, yes, out\n"
        "    block yes:\n"
        "      return %next\n"
        "    block out:\n"
        "      return %unguarded\n"
        "  }\n"), 0, PROFILES, 1, d);
    check("the compare module parses", m != nullptr);
    if (!m) return;
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);
    ir_func_t *f = ir_func_at(m, 0);
    ir_abcd_t *abcd = ir_func_abcd(f);
    ir_value_t *inner = ir_value_find(f, S("inner"));
    ir_value_t *reversed = ir_value_find(f, S("reversed"));
    ir_value_t *outer = ir_value_find(f, S("outer"));
    ir_value_t *next = ir_value_find(f, S("next"));
    ir_value_t *unguarded = ir_value_find(f, S("unguarded"));
    check("x < n inside x < n is always true",
          inner && ir_abcd_compare(abcd, inner->def) == 1);
    check("x >= n inside x < n is always false",
          reversed && ir_abcd_compare(abcd, reversed->def) == 0);
    check("the outer test itself is unknown",
          outer && ir_abcd_compare(abcd, outer->def) == -1);
    check("i + 1 under i < len cannot overflow",
          next && ir_abcd_overflow_safe(abcd, next->def));
    check("i + 1 with no constraint keeps its overflow check",
          unguarded && !ir_abcd_overflow_safe(abcd, unguarded->def));
    ir_block_t *inside = inner ? inner->def->block : nullptr;
    check("folding decides both inner compares",
          ir_abcd_fold(f) == 2);
    check("the decided compare becomes a constant",
          inner && inner->def->op == IR_OP_CONST
          && reversed && reversed->def->op == IR_OP_CONST);
    check("the branch on it becomes a jump",
          inside && ir_block_terminator(inside)->op == IR_OP_JUMP);
    check("the undecided compare is kept",
          outer && outer->def->op == IR_OP_LT);
    ir_ctx_free(ctx);
}

static void test_monotone_loop_checks_move_before_the_loop(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *d = ir_diags_new(ctx, nullptr);
    ir_module_t *m = ir_module_parse(ctx, MOD(
        "  func @f(%a: array[i32], %n: u64, %c: bool) -> i32 {\n"
        "    block entry:\n"
        "      %zero: u64 = const 0\n"
        "      %one: u64 = const 1\n"
        "      %none: i32 = const 0\n"
        "      jump head\n"
        "    block head:\n"
        "      %i: u64 = phi [entry: %zero], [latch: %next]\n"
        "      %sum: i32 = phi [entry: %none], [latch: %total]\n"
        "      %more: bool = lt %i, %n\n"
        "      branch %more, body, done\n"
        "    block body:\n"
        "      %value: i32 = sequence.get %a, %i\n"
        "      branch %c, extra, latch\n"
        "    block extra:\n"
        "      %ahead: u64 = add trap %i, %one\n"
        "      %sometimes: i32 = sequence.get %a, %ahead\n"
        "      jump latch\n"
        "    block latch:\n"
        "      %total: i32 = add trap %sum, %value\n"
        "      %next: u64 = add trap %i, %one\n"
        "      jump head\n"
        "    block done:\n"
        "      return %sum\n"
        "  }\n"), 0, PROFILES, 1, d);
    check("the loop module parses", m != nullptr);
    if (!m) return;
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);
    ir_func_t *f = ir_func_at(m, 0);
    ir_value_t *value = ir_value_find(f, S("value"));
    ir_value_t *sometimes = ir_value_find(f, S("sometimes"));
    check("the every-iteration access starts with a check",
          value && !ir_abcd_check_redundant(ir_func_abcd(f), value->def));
    check("one access is hoisted", ir_abcd_hoist(f) == 1);
    ir_abcd_t *abcd = ir_func_abcd(f);
    check("the every-iteration access is covered by the loop check",
          value && ir_abcd_check_redundant(abcd, value->def));
    check("a conditional access keeps its check",
          sometimes && !ir_abcd_check_redundant(abcd, sometimes->def));
    ir_block_t *entry = ir_block_entry(f);
    check("the loop entry tests whether the loop runs",
          entry && ir_block_terminator(entry)->op == IR_OP_BRANCH);
    ir_block_t *checks = ir_block_find(f, S("__abcd_loop_checks_0"));
    bool probed = false;
    for (uint32_t i = 0; checks && i < checks->n_instrs; i++)
        probed |= checks->instrs[i]->op == IR_OP_SEQUENCE_GET;
    check("the check block probes the last index", probed);
    ir_ctx_free(ctx);
}

static void test_trap_summaries_leave_out_proven_checks(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *d = ir_diags_new(ctx, nullptr);
    ir_module_t *m = ir_module_parse(ctx, MOD(
        "  func @f(%a: array[i32], %i: u64, %j: u64) -> i32 {\n"
        "    block entry:\n"
        "      %n: u64 = sequence.length %a\n"
        "      %inside: bool = lt %i, %n\n"
        "      branch %inside, yes, no\n"
        "    block yes:\n"
        "      %guarded: i32 = sequence.get %a, %i\n"
        "      %open: i32 = sequence.get %a, %j\n"
        "      %sum: i32 = add trap %guarded, %open\n"
        "      return %sum\n"
        "    block no:\n"
        "      %zero: i32 = const 0\n"
        "      return %zero\n"
        "  }\n"), 0, PROFILES, 1, d);
    check("the summary module parses", m != nullptr);
    if (!m) return;
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);
    ir_func_t *f = ir_func_at(m, 0);
    ir_summaries_t *summaries = ir_summaries_compute(&f, 1, nullptr);
    ir_summary_t *summary = summaries ? ir_summary_of(summaries, f) : nullptr;
    uint32_t bounds = 0, arithmetic = 0;
    for (uint32_t s = 0; s < ir_summary_trap_site_count(summary); s++) {
        const ir_strap_site_t *site = ir_summary_trap_site_at(summary, s);
        bounds += site->kind == IR_STRAP_BOUNDS;
        arithmetic += site->kind == IR_STRAP_ARITHMETIC;
    }
    check("only the unguarded access is a bounds trap site", bounds == 1);
    check("the unproven add is an arithmetic trap site", arithmetic == 1);
    check("the summary may trap",
          ir_summary_flags(summary) & IR_SUMMARY_MAY_TRAP);
    ir_summaries_free(summaries);
    ir_ctx_free(ctx);
}

typedef struct {
    core_string_t texts[8];
    uint32_t count;
} text_table_t;

static uint32_t
intern_text(void *context, core_string_t text)
{
    text_table_t *table = context;
    for (uint32_t i = 0; i < table->count; i++)
        if (core_string_eq(table->texts[i], text)) return i + 1;
    if (table->count == 8) return 0;
    table->texts[table->count++] = text;
    return table->count;
}

static core_string_t
lookup_text(void *context, uint32_t reference)
{
    text_table_t *table = context;
    return reference && reference <= table->count
        ? table->texts[reference - 1] : (core_string_t){0};
}

static void test_decoded_summaries_keep_heap_types(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *d = ir_diags_new(ctx, nullptr);
    ir_module_t *m = ir_module_parse(ctx, MOD(
        "  func @f(%pp: ref[ref[i32]]) -> void {\n"
        "    block entry:\n"
        "      %p: ref[i32] = load %pp\n"
        "      %one: i32 = const 1\n"
        "      store %p, %one\n"
        "      return\n"
        "  }\n"), 0, PROFILES, 1, d);
    check("the heap module parses", m != nullptr);
    if (!m) return;
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);
    ir_func_t *f = ir_func_at(m, 0);
    ir_summaries_t *summaries = ir_summaries_compute(&f, 1, nullptr);
    ir_summary_t *summary = ir_summary_of(summaries, f);
    core_buffer_t encoded = {0};
    check("the summary encodes", ir_summary_encode(summary, &encoded));
    ir_summary_t *decoded = ir_summary_decode(summaries, f,
        (const uint8_t *)encoded.data, encoded.byte_len, nullptr, nullptr);
    th_type *heap = nullptr;
    th_type *kept = nullptr;
    for (uint32_t a = 0; a < ir_summary_access_count(summary); a++)
        if (ir_summary_access_at(summary, a)->place.kind == IR_SPLACE_HEAP)
            heap = ir_summary_access_at(summary, a)->place.type;
    for (uint32_t a = 0; a < ir_summary_access_count(decoded); a++)
        if (ir_summary_access_at(decoded, a)->place.kind == IR_SPLACE_HEAP)
            kept = ir_summary_access_at(decoded, a)->place.type;
    th_ctx *types = ir_module_types(m);
    check("the summary writes a typed heap place", heap != nullptr);
    check("the decoded heap place keeps its type",
          kept && th_hash(types, kept) == th_hash(types, heap));
    text_table_t table = {0};
    ir_summary_texts_t texts = {
        .intern = intern_text, .lookup = lookup_text, .context = &table,
    };
    core_buffer_t referenced = {0};
    check("the summary encodes with a text table",
          ir_summary_encode_with(summary, &referenced, &texts)
          && table.count > 0);
    ir_summary_t *resolved = ir_summary_decode_with(summaries, f,
        (const uint8_t *)referenced.data, referenced.byte_len, nullptr,
        nullptr, &texts);
    th_type *named = nullptr;
    for (uint32_t a = 0; resolved && a < ir_summary_access_count(resolved);
         a++)
        if (ir_summary_access_at(resolved, a)->place.kind == IR_SPLACE_HEAP)
            named = ir_summary_access_at(resolved, a)->place.type;
    check("a table-encoded heap place keeps its type",
          named && th_hash(types, named) == th_hash(types, heap));
    check("table encodings need a table to decode",
          !ir_summary_decode(summaries, f, (const uint8_t *)referenced.data,
                             referenced.byte_len, nullptr, nullptr));
    core_buffer_release(&referenced);
    core_buffer_release(&encoded);
    ir_summaries_free(summaries);
    ir_ctx_free(ctx);
}

int main(void)
{
    test_last_element_under_positive_length();
    test_array_ssa_feeds_abcd();
    test_constraints_decide_compares_and_overflow();
    test_monotone_loop_checks_move_before_the_loop();
    test_trap_summaries_leave_out_proven_checks();
    test_decoded_summaries_keep_heap_types();
    return report("IR ABCD");
}
