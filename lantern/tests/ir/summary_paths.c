#include "ir/summary.h"
#include "ir/text.h"
#include "lib/buffer.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

#define MOD(...) \
    S("module m {\n  profile bce\n  target t\n\n" __VA_ARGS__ "}\n")

static const ir_profile_t *const PROFILES[] = {&ir_profile_bce};

typedef struct {
    ir_ctx_t       *ctx;
    ir_module_t    *m;
    ir_summaries_t *summaries;
} run_t;

static run_t
load(core_string_t text)
{
    run_t r = {.ctx = ir_ctx_new()};
    ir_diags_t *d = ir_diags_new(r.ctx, nullptr);
    r.m = ir_module_parse(r.ctx, text, 0, PROFILES, 1, d);
    check("the module parses", r.m != nullptr);
    if (!r.m) {
        core_string_t told = ir_diags_render_text(d, nullptr);
        fprintf(stderr, "%.*s\n", (int)told.u8_bytes, told.data);
        return r;
    }
    ir_module_establish(r.m, IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA);
    size_t count = ir_func_count(r.m);
    ir_func_t **functions = core_alloc_array(ir_func_t *, count);
    for (size_t i = 0; i < count; i++) functions[i] = ir_func_at(r.m, i);
    r.summaries = ir_summaries_compute(functions, count, nullptr);
    core_free(functions);
    return r;
}

static void
done(run_t *r)
{
    if (r->summaries) ir_summaries_free(r->summaries);
    ir_ctx_free(r->ctx);
}

static int
compare_lines(const void *left, const void *right)
{
    return core_string_cmp(*(const core_string_t *)left,
                           *(const core_string_t *)right);
}

static void
render_steps(core_buffer_t *out, const ir_spath_t *path)
{
    for (uint8_t s = 0; s < path->depth; s++) {
        if (!core_string_eq(path->steps[s], S("[]")))
            core_buffer_putc(out, '.');
        core_buffer_append_string(out, path->steps[s]);
    }
    if (path->flags & IR_SPATH_DEEP) core_buffer_puts(out, "...");
}

static void
render_place(core_buffer_t *out, const ir_func_t *f, const ir_spath_t *path)
{
    if (path->root == IR_SPLACE_HEAP)
        core_buffer_puts(out, "new");
    else if (path->root == IR_SPLACE_PARAM && path->param < f->n_params)
        core_buffer_append_string(out, f->params[path->param]->name);
    else if (path->global)
        core_buffer_append_string(out, path->global->name);
    else if (path->root == IR_SPLACE_GLOBAL)
        core_buffer_puts(out, "(globals)");
    else
        core_buffer_puts(out, "?");
    render_steps(out, path);
}

static core_string_t
render_access(const ir_func_t *f, const ir_spath_t *path)
{
    core_buffer_t *out = core_buffer_empty();
    uint8_t access = path->flags & (IR_SPATH_READ | IR_SPATH_WRITE);
    core_buffer_append_string(out, access == IR_SPATH_READ ? S("r ")
                              : access == IR_SPATH_WRITE ? S("w ")
                              : S("rw "));
    render_place(out, f, path);
    return core_buffer_take_string(out);
}

static core_string_t
render_result(const ir_func_t *f, const ir_sstore_t *result)
{
    core_buffer_t *out = core_buffer_empty();
    core_buffer_putc(out, '=');
    if (result->to.output)
        core_buffer_printf(out, "%u", (unsigned)result->to.output);
    render_steps(out, &result->to);
    core_buffer_putc(out, ' ');
    render_place(out, f, &result->from);
    return core_buffer_take_string(out);
}

static core_string_t
render_store(const ir_func_t *f, const ir_sstore_t *store)
{
    core_buffer_t *out = core_buffer_empty();
    core_buffer_puts(out, "s ");
    render_place(out, f, &store->to);
    core_buffer_puts(out, " <- ");
    render_place(out, f, &store->from);
    return core_buffer_take_string(out);
}

static core_string_t
paths_of(run_t *r, core_string_t name)
{
    ir_func_t    *f       = r->m ? ir_func_find(r->m, name) : nullptr;
    ir_summary_t *summary = f && r->summaries
        ? ir_summary_of(r->summaries, f) : nullptr;
    if (!summary) return S("<no summary>");
    uint32_t paths   = ir_summary_path_count(summary);
    uint32_t results = ir_summary_result_count(summary);
    uint32_t stores  = ir_summary_store_count(summary);
    uint32_t count   = paths + results + stores;
    core_string_t *lines = core_alloc_array(core_string_t, count + 1);
    for (uint32_t i = 0; i < paths; i++)
        lines[i] = render_access(f, ir_summary_path_at(summary, i));
    for (uint32_t i = 0; i < results; i++)
        lines[paths + i] = render_result(f, ir_summary_result_at(summary, i));
    for (uint32_t i = 0; i < stores; i++)
        lines[paths + results + i] = render_store(
            f, ir_summary_store_at(summary, i));
    qsort(lines, count, sizeof(*lines), compare_lines);
    core_buffer_t *out = core_buffer_empty();
    for (uint32_t i = 0; i < count; i++) {
        if (i) core_buffer_puts(out, "; ");
        core_buffer_append_string(out, lines[i]);
    }
    core_free(lines);
    return core_buffer_take_string(out);
}

static void
expect(run_t *r, const char *what, core_string_t function,
       core_string_t expected)
{
    core_string_t actual = paths_of(r, function);
    bool same = core_string_eq(actual, expected);
    check(what, same);
    if (!same)
        fprintf(stderr, "    expected: %.*s\n    actual:   %.*s\n",
                (int)expected.u8_bytes, expected.data,
                (int)actual.u8_bytes, actual.data);
}

#define TYPES \
    "  type R = (x: i32, next: ref[R], items: array[ref[R]])\n" \
    "  type V = (a: ref[R], n: i32)\n" \
    "  global @g: ref[R]\n\n"

static void
test_joins_keep_every_arm(void)
{
    run_t r = load(MOD(TYPES
        "  func @f(%p: ref[R], %q: ref[R], %c: bool) -> nil {\n"
        "    block entry:\n"
        "      branch %c, left, right\n"
        "    block left:\n"
        "      jump join\n"
        "    block right:\n"
        "      jump join\n"
        "    block join:\n"
        "      %t: ref[R] = phi [left: %p], [right: %q]\n"
        "      %x: ref[i32] = field %t, \"x\"\n"
        "      %one: i32 = const 1\n"
        "      store %x, %one\n"
        "      %u: ref[R] = select %c, %p, %q\n"
        "      %y: ref[i32] = field %u, \"x\"\n"
        "      %v: i32 = load %y\n"
        "      %none: nil = nil\n"
        "      return %none\n"
        "  }\n"));
    expect(&r, "a phi and a select reach both of their arms", S("f"),
           S("rw p.x; rw q.x"));
    done(&r);
}

static void
test_slots_hold_what_was_stored(void)
{
    run_t r = load(MOD(TYPES
        "  func @f(%p: ref[R]) -> i32 {\n"
        "    block entry:\n"
        "      %slot: ref[ref[R]] = alloc ref[R]\n"
        "      store %slot, %p\n"
        "      %held: ref[R] = load %slot\n"
        "      %x: ref[i32] = field %held, \"x\"\n"
        "      %v: i32 = load %x\n"
        "      return %v\n"
        "  }\n"));
    expect(&r, "a pointer read back from a slot names its source", S("f"),
           S("r p.x"));
    done(&r);
}

static void
test_by_value_records(void)
{
    run_t r = load(MOD(TYPES
        "  func @f(%v: V) -> i32 {\n"
        "    block entry:\n"
        "      %a: ref[R] = extract %v, \"a\"\n"
        "      %x: ref[i32] = field %a, \"x\"\n"
        "      %read: i32 = load %x\n"
        "      %n: i32 = extract %v, \"n\"\n"
        "      %slot: ref[V] = alloc V\n"
        "      store %slot, %v\n"
        "      %count: ref[i32] = field %slot, \"n\"\n"
        "      store %count, %read\n"
        "      %sum: i32 = add trap %read, %n\n"
        "      return %sum\n"
        "  }\n"
        "  func @caller(%p: ref[V]) -> i32 {\n"
        "    block entry:\n"
        "      %v: V = load %p\n"
        "      %r: i32 = call @f, %v\n"
        "      return %r\n"
        "  }\n"));
    expect(&r, "a by-value record's own fields are not memory", S("f"),
           S("r v.a.x"));
    expect(&r, "and the caller sees the read through its copy",
           S("caller"), S("r p; r p.a.x"));
    done(&r);
}

static void
test_fresh_interiors_stay_local(void)
{
    run_t r = load(MOD(TYPES
        "  func @quiet() -> ref[R] {\n"
        "    block entry:\n"
        "      %o: ref[R] = alloc R !ir.fresh(true)\n"
        "      %x: ref[i32] = field %o, \"x\"\n"
        "      %one: i32 = const 1\n"
        "      store %x, %one\n"
        "      %n: ref[ref[R]] = field %o, \"next\"\n"
        "      %next: ref[R] = load %n\n"
        "      return %o\n"
        "  }\n"
        "  func @holds(%p: ref[R]) -> i32 {\n"
        "    block entry:\n"
        "      %o: ref[R] = alloc R !ir.fresh(true)\n"
        "      %n: ref[ref[R]] = field %o, \"next\"\n"
        "      store %n, %p\n"
        "      %back: ref[R] = load %n\n"
        "      %x: ref[i32] = field %back, \"x\"\n"
        "      %v: i32 = load %x\n"
        "      return %v\n"
        "  }\n"));
    expect(&r, "a fresh object's own fields are not reported", S("quiet"),
           S("= new"));
    expect(&r, "what was stored into a fresh object is followed",
           S("holds"), S("r p.x"));
    done(&r);
}

static void
test_returned_parameters_alias(void)
{
    run_t r = load(MOD(TYPES
        "  func @same(%a: ref[R]) -> ref[R] {\n"
        "    block entry:\n"
        "      return %a\n"
        "  }\n"
        "  func @f(%p: ref[R]) -> nil {\n"
        "    block entry:\n"
        "      %r: ref[R] = call @same, %p\n"
        "      %x: ref[i32] = field %r, \"x\"\n"
        "      %one: i32 = const 1\n"
        "      store %x, %one\n"
        "      %none: nil = nil\n"
        "      return %none\n"
        "  }\n"
        "  func @wrap(%p: ref[R]) -> ref[R] {\n"
        "    block entry:\n"
        "      %r: ref[R] = call @same, %p\n"
        "      return %r\n"
        "  }\n"));
    expect(&r, "a call that returns its argument aliases it", S("f"),
           S("w p.x"));
    expect(&r, "and its result names the argument, not a fresh object",
           S("wrap"), S("= p"));
    done(&r);
}

static void
test_callees_write_through_locals(void)
{
    run_t r = load(MOD(TYPES
        "  func @set(%r: ref[R]) -> nil {\n"
        "    block entry:\n"
        "      %x: ref[i32] = field %r, \"x\"\n"
        "      %one: i32 = const 1\n"
        "      store %x, %one\n"
        "      %none: nil = nil\n"
        "      return %none\n"
        "  }\n"
        "  func @local() -> nil {\n"
        "    block entry:\n"
        "      %slot: ref[R] = alloc R\n"
        "      %r: nil = call @set, %slot\n"
        "      return %r\n"
        "  }\n"
        "  func @through(%p: ref[R]) -> nil {\n"
        "    block entry:\n"
        "      %slot: ref[ref[R]] = alloc ref[R]\n"
        "      store %slot, %p\n"
        "      %held: ref[R] = load %slot\n"
        "      %r: nil = call @set, %held\n"
        "      return %r\n"
        "  }\n"));
    expect(&r, "a callee writing a local slot is not reported", S("local"),
           S(""));
    expect(&r, "a callee writing through a held pointer is", S("through"),
           S("w p.x"));
    done(&r);
}

static void
test_cycles_widen(void)
{
    run_t r = load(MOD(TYPES
        "  func @walk(%p: ref[R]) -> nil {\n"
        "    block entry:\n"
        "      jump loop\n"
        "    block loop:\n"
        "      %at: ref[R] = phi [entry: %p], [loop: %next]\n"
        "      %n: ref[ref[R]] = field %at, \"next\"\n"
        "      %next: ref[R] = load %n\n"
        "      %go: bool = const 1\n"
        "      branch %go, loop, out\n"
        "    block out:\n"
        "      %none: nil = nil\n"
        "      return %none\n"
        "  }\n"));
    expect(&r, "a loop through a link widens to a deep path", S("walk"),
           S("r p.next..."));
    done(&r);
}

static void
test_globals_and_elements(void)
{
    run_t r = load(MOD(TYPES
        "  func @f(%i: u64) -> i32 {\n"
        "    block entry:\n"
        "      %root: ref[R] = load @g\n"
        "      %field: ref[array[ref[R]]] = field %root, \"items\"\n"
        "      %items: array[ref[R]] = load %field\n"
        "      %item: ref[R] = sequence.get %items, %i\n"
        "      %x: ref[i32] = field %item, \"x\"\n"
        "      %v: i32 = load %x\n"
        "      return %v\n"
        "  }\n"));
    expect(&r, "global roots and element steps are named", S("f"),
           S("r g; r g.items; r g.items[]; r g.items[].x"));
    done(&r);
}

static void
test_returned_fresh_objects(void)
{
    run_t r = load(MOD(TYPES
        "  func @keeps(%p: ref[R]) -> ref[R] {\n"
        "    block entry:\n"
        "      %o: ref[R] = alloc R !ir.fresh(true)\n"
        "      %n: ref[ref[R]] = field %o, \"next\"\n"
        "      store %n, %p\n"
        "      return %o\n"
        "  }\n"
        "  func @f(%p: ref[R]) -> nil {\n"
        "    block entry:\n"
        "      %o: ref[R] = call @keeps, %p\n"
        "      %n: ref[ref[R]] = field %o, \"next\"\n"
        "      %next: ref[R] = load %n\n"
        "      %x: ref[i32] = field %next, \"x\"\n"
        "      %one: i32 = const 1\n"
        "      store %x, %one\n"
        "      %none: nil = nil\n"
        "      return %none\n"
        "  }\n"));
    expect(&r, "a fresh object holding an argument says what it holds",
           S("keeps"), S("= new; s new.next <- p"));
    expect(&r, "and what it holds is followed by the caller", S("f"),
           S("w p.x"));
    done(&r);
}

static void
test_published_objects(void)
{
    run_t r = load(MOD(TYPES
        "  func @f() -> nil {\n"
        "    block entry:\n"
        "      %o: ref[R] = alloc R !ir.fresh(true)\n"
        "      store @g, %o\n"
        "      %x: ref[i32] = field %o, \"x\"\n"
        "      %one: i32 = const 1\n"
        "      store %x, %one\n"
        "      %none: nil = nil\n"
        "      return %none\n"
        "  }\n"
        "  func @forged(%i: u64) -> i32 {\n"
        "    block entry:\n"
        "      %q: ref[R] = cast %i\n"
        "      %x: ref[i32] = field %q, \"x\"\n"
        "      %v: i32 = load %x\n"
        "      return %v\n"
        "  }\n"));
    expect(&r, "an object stored in a global is written through its name",
           S("f"), S("s g <- new; w g; w g.x"));
    expect(&r, "a pointer made from an integer reaches anything",
           S("forged"), S("r ?..."));
    done(&r);
}

static void
test_writes_before_publication(void)
{
    run_t r = load(MOD(TYPES
        "  func @f(%p: ref[R]) -> nil {\n"
        "    block entry:\n"
        "      %o: ref[R] = alloc R !ir.fresh(true)\n"
        "      %x: ref[i32] = field %o, \"x\"\n"
        "      %one: i32 = const 1\n"
        "      store %x, %one\n"
        "      %n: ref[ref[R]] = field %p, \"next\"\n"
        "      store %n, %o\n"
        "      %items: ref[array[ref[R]]] = field %o, \"items\"\n"
        "      %none: array[ref[R]] = undef\n"
        "      store %items, %none\n"
        "      %nothing: nil = nil\n"
        "      return %nothing\n"
        "  }\n"));
    expect(&r, "only writes after an object is published are seen",
           S("f"), S("s p.next <- new; w p.next; w p.next.items"));
    done(&r);
}

static void
test_callee_stores(void)
{
    run_t r = load(MOD(TYPES
        "  func @link(%a: ref[R], %b: ref[R]) -> nil {\n"
        "    block entry:\n"
        "      %n: ref[ref[R]] = field %a, \"next\"\n"
        "      store %n, %b\n"
        "      %none: nil = nil\n"
        "      return %none\n"
        "  }\n"
        "  func @f(%p: ref[R]) -> i32 {\n"
        "    block entry:\n"
        "      %slot: ref[R] = alloc R\n"
        "      %r: nil = call @link, %slot, %p\n"
        "      %n: ref[ref[R]] = field %slot, \"next\"\n"
        "      %q: ref[R] = load %n\n"
        "      %x: ref[i32] = field %q, \"x\"\n"
        "      %v: i32 = load %x\n"
        "      return %v\n"
        "  }\n"
        "  func @g(%p: ref[R], %q: ref[R]) -> nil {\n"
        "    block entry:\n"
        "      %r: nil = call @link, %p, %q\n"
        "      return %r\n"
        "  }\n"));
    ir_func_t *link = ir_func_find(r.m, S("link"));
    ir_summary_t *summary = link ? ir_summary_of(r.summaries, link) : nullptr;
    const ir_sstore_t *store = summary && ir_summary_store_count(summary) == 1
        ? ir_summary_store_at(summary, 0) : nullptr;
    check("a store summary names where an argument was put",
          store && store->to.root == IR_SPLACE_PARAM && store->to.param == 0
              && store->to.depth == 1
              && core_string_eq(store->to.steps[0], S("next"))
              && store->from.root == IR_SPLACE_PARAM
              && store->from.param == 1 && !store->from.depth);
    expect(&r, "what a callee stores into a local is followed", S("f"),
           S("r p.x"));
    ir_func_t *g = ir_func_find(r.m, S("g"));
    summary = g ? ir_summary_of(r.summaries, g) : nullptr;
    store = summary && ir_summary_store_count(summary) == 1
        ? ir_summary_store_at(summary, 0) : nullptr;
    check("and a callee's store is restated in the caller's terms",
          store && store->to.param == 0 && store->from.param == 1);
    done(&r);
}

static void
test_fresh_construction(void)
{
    run_t r = load(MOD(TYPES
        "  func @init(inout %self: V, %a: ref[R]) -> void {\n"
        "    block entry:\n"
        "      %made: V = insert %self, \"a\", %a\n"
        "      return %made\n"
        "  }\n"
        "  func @make(%p: ref[R]) -> i32 {\n"
        "    block entry:\n"
        "      %storage: ref[V] = alloc V\n"
        "      %done: void = call @init, fresh %storage, %p\n"
        "      %field: ref[ref[R]] = field %storage, \"a\"\n"
        "      %q: ref[R] = load %field\n"
        "      %x: ref[i32] = field %q, \"x\"\n"
        "      %v: i32 = load %x\n"
        "      return %v\n"
        "  }\n"));
    expect(&r, "a constructor's copy-out names what it holds", S("init"),
           S("=1 self; =1.a a"));
    expect(&r, "and the caller reads it from the constructed storage",
           S("make"), S("r p.x"));
    done(&r);
}

static void
test_nested_construction(void)
{
    run_t r = load(MOD(
        "  type R = (x: i32, next: ref[R], items: array[ref[R]])\n"
        "  type W = (items: array[ref[R]], n: i32)\n"
        "  type C = (inner: W, at: i32)\n\n"
        "  func @winit(inout %self: W, %items: array[ref[R]]) -> void {\n"
        "    block entry:\n"
        "      %made: W = insert %self, \"items\", %items\n"
        "      return %made\n"
        "  }\n"
        "  func @cinit(inout %self: C, %view: W) -> void {\n"
        "    block entry:\n"
        "      %made: C = insert %self, \"inner\", %view\n"
        "      return %made\n"
        "  }\n"
        "  func @slice(%a: array[ref[R]]) -> W {\n"
        "    block entry:\n"
        "      %slot: ref[W] = alloc W\n"
        "      %done: void = call @winit, fresh %slot, %a\n"
        "      %w: W = load %slot\n"
        "      return %w\n"
        "  }\n"
        "  func @iter(%a: array[ref[R]]) -> C {\n"
        "    block entry:\n"
        "      %v: W = call @slice, %a\n"
        "      %slot: ref[C] = alloc C\n"
        "      %done: void = call @cinit, fresh %slot, %v\n"
        "      %c: C = load %slot\n"
        "      return %c\n"
        "  }\n"));
    expect(&r, "a constructed record names its argument at its field",
           S("slice"), S("=.items a"));
    expect(&r, "and a record built around it names it one level down",
           S("iter"), S("=.inner.items a"));
    done(&r);
}

static void
test_scalar_globals(void)
{
    run_t r = load(MOD(
        "  global @count: i32\n\n"
        "  func @bump() -> i32 {\n"
        "    block entry:\n"
        "      %one: i32 = const 1\n"
        "      %next: i32 = add @count, %one\n"
        "      global.store @count, %next\n"
        "      return %next\n"
        "  }\n"
        "  func @peek() -> i32 {\n"
        "    block entry:\n"
        "      return @count\n"
        "  }\n"
        "  func @loaded() -> i32 {\n"
        "    block entry:\n"
        "      %v: i32 = load @count\n"
        "      return %v\n"
        "  }\n"));
    expect(&r, "a scalar global used as a value is read, and stored is written",
           S("bump"), S("rw count"));
    expect(&r, "a scalar global returned by value is read", S("peek"),
           S("r count"));
    expect(&r, "and so is one loaded through its address", S("loaded"),
           S("r count"));
    done(&r);
}

int
main(void)
{
    test_scalar_globals();
    test_nested_construction();
    test_fresh_construction();
    test_writes_before_publication();
    test_callee_stores();
    test_published_objects();
    test_joins_keep_every_arm();
    test_slots_hold_what_was_stored();
    test_by_value_records();
    test_fresh_interiors_stay_local();
    test_returned_parameters_alias();
    test_callees_write_through_locals();
    test_cycles_widen();
    test_globals_and_elements();
    test_returned_fresh_objects();
    return report("IR summary paths");
}
