#include "ir/analyses.h"
#include "ir/summary.h"
#include "ir/text.h"
#include "ir/vn.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

#define MOD(...) \
    S("module m {\n  profile bce\n  target t\n\n" __VA_ARGS__ "}\n")

static const ir_profile_t *const PROFILES[] = {&ir_profile_bce};

static void test_repeated_values_share_a_number(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *d = ir_diags_new(ctx, nullptr);
    ir_module_t *m = ir_module_parse(ctx, MOD(
        "  func @f(%a: array[i32], %i: u64) -> i32 {\n"
        "    local $x: array[i32]\n"
        "    block entry:\n"
        "      $x = copy %a\n"
        "      %two_small: u8 = const 2\n"
        "      %two: u64 = cast %two_small\n"
        "      %two_wide: u64 = const 2\n"
        "      %first: ref[array[i32]] = address.of $x\n"
        "      %second: ref[array[i32]] = address.of $x\n"
        "      %stored_at: ref[i32] = index %first, %two\n"
        "      %loaded_at: ref[i32] = index %second, %two_wide\n"
        "      %seven: i32 = const 7\n"
        "      store %stored_at, %seven\n"
        "      %element: i32 = load %loaded_at\n"
        "      %one: u64 = const 1\n"
        "      %next: u64 = add wrap %i, %one\n"
        "      %again: u64 = add wrap %one, %i\n"
        "      %other: u64 = add wrap %i, %two\n"
        "      return %element\n"
        "  }\n"), 0, PROFILES, 1, d);
    check("the module parses", m != nullptr);
    if (!m) return;
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED);
    ir_func_t *f = ir_func_at(m, 0);
    ir_value_numbers_t *numbers = ir_func_value_numbers(f);
    check("value numbers build", numbers != nullptr);
#define SAME(a, b) ir_value_numbers_same(numbers, ir_value_find(f, S(a)), \
                                         ir_value_find(f, S(b)))
    check("repeated address.of of one local is one value",
          SAME("first", "second"));
    check("equal constant indices are one value across widths",
          SAME("two", "two_wide"));
    check("indexes of the same element are one value",
          SAME("stored_at", "loaded_at"));
    check("an element read sees its own store", SAME("element", "seven"));
    check("repeated i + 1 is one value, in either operand order",
          SAME("next", "again"));
    check("different values keep different numbers", !SAME("next", "other"));
#undef SAME
    ir_ctx_free(ctx);
}

static uint32_t
links_of(ir_memssa_t *memssa, ir_func_t *f, const char *load)
{
    ir_value_t *value = ir_value_find(f, core_string_from_cstr(load));
    uint32_t access = value ? ir_memssa_access_of(memssa, value->def,
                                                  IR_ACCESS_LOAD)
                            : UINT32_MAX;
    return access == UINT32_MAX ? UINT32_MAX
        : ir_memssa_element_link_count(memssa, access);
}

static void test_array_ssa_links_element_reads(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *d = ir_diags_new(ctx, nullptr);
    ir_module_t *m = ir_module_parse(ctx, MOD(
        "  func @f(%a: array[i32], %c: bool, %k: u64) -> i32 {\n"
        "    local $x: array[i32]\n"
        "    block entry:\n"
        "      $x = copy %a\n"
        "      %zero: u64 = const 0\n"
        "      %one: u64 = const 1\n"
        "      %seven: i32 = const 7\n"
        "      %nine: i32 = const 9\n"
        "      %base: ref[array[i32]] = address.of $x\n"
        "      %slot0: ref[i32] = index %base, %zero\n"
        "      %slot1: ref[i32] = index %base, %one\n"
        "      %slotk: ref[i32] = index %base, %k\n"
        "      store %slot0, %seven\n"
        "      store %slot1, %nine\n"
        "      %past_other: i32 = load %slot0\n"
        "      store %slotk, %nine\n"
        "      %past_unknown: i32 = load %slot0\n"
        "      branch %c, left, right\n"
        "    block left:\n"
        "      store %slot0, %nine\n"
        "      jump join\n"
        "    block right:\n"
        "      store %slot0, %seven\n"
        "      jump join\n"
        "    block join:\n"
        "      %joined: i32 = load %slot0\n"
        "      return %joined\n"
        "  }\n"), 0, PROFILES, 1, d);
    check("the array module parses", m != nullptr);
    if (!m) return;
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED);
    ir_func_t *f = ir_func_at(m, 0);
    ir_memssa_t *memssa = ir_func_memssa(f);
    ir_value_numbers_t *numbers = ir_func_value_numbers(f);
    check("memory SSA with element links verifies", ir_memssa_verify(memssa));
    check("a store to another constant index does not reach a read",
          links_of(memssa, f, "past_other") == 1);
    check("so the read still sees its own store",
          ir_value_numbers_same(numbers, ir_value_find(f, S("past_other")),
                                ir_value_find(f, S("seven"))));
    check("a store to an unknown index reaches the read as well",
          links_of(memssa, f, "past_unknown") == 2);
    check("and the read no longer forwards a single store",
          !ir_value_numbers_same(numbers,
                                 ir_value_find(f, S("past_unknown")),
                                 ir_value_find(f, S("seven"))));
    check("a join reaches both arms' stores through an element phi",
          links_of(memssa, f, "joined") == 2);
    bool element_phi = false;
    for (uint32_t i = 0; i < ir_memssa_def_count(memssa); i++)
        element_phi = element_phi || ir_memssa_def(memssa, i)->kind
            == IR_MEMDEF_ELEMENT_PHI;
    check("the join has a per-element phi", element_phi);
    ir_ctx_free(ctx);
}

typedef struct {
    uint32_t calls;
    uint32_t stores;
} clobber_count_t;

static bool
count_clobber(void *context, const ir_memdef_t *def)
{
    clobber_count_t *count = context;
    count->calls += def->kind == IR_MEMDEF_CALL;
    count->stores += def->kind == IR_MEMDEF_STORE;
    return true;
}

static clobber_count_t
clobbers_of(ir_func_t *f, const ir_value_t *loaded)
{
    clobber_count_t count = {0};
    ir_places_t *places = ir_func_places(f);
    for (uint32_t a = 0; loaded && a < ir_places_access_count(places); a++)
        if (ir_places_access_at(places, a)->instr == loaded->def
            && ir_places_access_at(places, a)->kind == IR_ACCESS_LOAD)
            ir_memssa_visit_clobbers(ir_func_memssa(f), a, count_clobber,
                                     &count);
    return count;
}

static void test_call_clobbers_come_from_callee_summaries(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_diags_t *d = ir_diags_new(ctx, nullptr);
    ir_module_t *m = ir_module_parse(ctx, MOD(
        "  global @g: i32\n"
        "  global @h: i32\n"
        "  func @touch_h() -> void {\n"
        "    block entry:\n"
        "      %one: i32 = const 1\n"
        "      global.store @h, %one\n"
        "      return\n"
        "  }\n"
        "  func @touch_g() -> void {\n"
        "    block entry:\n"
        "      %two: i32 = const 2\n"
        "      global.store @g, %two\n"
        "      return\n"
        "  }\n"
        "  func @f() -> i32 {\n"
        "    block entry:\n"
        "      %zero: i32 = const 0\n"
        "      global.store @g, %zero\n"
        "      %quiet: void = call @touch_h\n"
        "      %first: i32 = load @g\n"
        "      %loud: void = call @touch_g\n"
        "      %second: i32 = load @g\n"
        "      %sum: i32 = add trap %first, %second\n"
        "      return %sum\n"
        "  }\n"), 0, PROFILES, 1, d);
    check("the clobber module parses", m != nullptr);
    if (!m) return;
    ir_module_establish(m, IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);
    ir_func_t *functions[3];
    for (uint32_t i = 0; i < 3; i++) functions[i] = ir_func_at(m, i);
    ir_func_t *f = ir_func_find(m, S("f"));
    ir_summaries_t *summaries = ir_summaries_compute(functions, 3, nullptr);
    ir_summaries_t *previous = ir_analysis_bind_summaries(summaries);
    clobber_count_t first = clobbers_of(f, ir_value_find(f, S("first")));
    clobber_count_t second = clobbers_of(f, ir_value_find(f, S("second")));
    check("a call whose callee writes another global does not clobber",
          first.calls == 0 && first.stores == 1);
    check("a call whose callee writes the loaded global clobbers",
          second.calls == 1);
    ir_analysis_bind_summaries(previous);
    ir_summaries_free(summaries);
    ir_ctx_free(ctx);
}

int main(void)
{
    test_repeated_values_share_a_number();
    test_array_ssa_links_element_reads();
    test_call_clobbers_come_from_callee_summaries();
    return report("IR value numbers");
}
