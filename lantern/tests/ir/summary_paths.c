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
render_result(const ir_func_t *f, const ir_spath_t *to, const ir_spath_t *from,
              char mark)
{
    core_buffer_t *out = core_buffer_empty();
    core_buffer_putc(out, mark);
    if (to->output) core_buffer_printf(out, "%u", (unsigned)to->output);
    render_steps(out, to);
    core_buffer_putc(out, ' ');
    render_place(out, f, from);
    return core_buffer_take_string(out);
}

static core_string_t
render_store(const ir_func_t *f, const ir_spath_t *to, const ir_spath_t *from,
             char mark)
{
    core_buffer_t *out = core_buffer_empty();
    core_buffer_puts(out, "s ");
    render_place(out, f, to);
    core_buffer_printf(out, " <%c ", mark);
    render_place(out, f, from);
    return core_buffer_take_string(out);
}

core_list_decl(core_string_t);

static void
render_flows(const ir_func_t *f, const ir_summary_t *summary,
             ir_sflow_table_t table, core_list_t(core_string_t) *lines)
{
    for (uint32_t d = 0; d < ir_summary_flow_count(summary, table); d++)
        for (uint32_t s = 0; s < ir_summary_flow_source_count(summary, table);
             s++)
            for (int kind = IR_SFLOW_DATA; kind <= IR_SFLOW_ALIAS; kind++) {
                if (!ir_summary_flow_has(summary, table, d, s, kind)) continue;
                const ir_spath_t *to   = ir_summary_flow_to(summary, table, d);
                const ir_spath_t *from = ir_summary_flow_source(summary, table,
                                                                s);
                char mark = kind == IR_SFLOW_DATA ? '~'
                          : kind == IR_SFLOW_CONTROL ? '^'
                          : table == IR_SFLOW_RESULTS ? '=' : '-';
                core_list_push(*lines, table == IR_SFLOW_RESULTS
                    ? render_result(f, to, from, mark)
                    : render_store(f, to, from, mark));
            }
}

static bool
only_store(const ir_summary_t *summary, const ir_spath_t **to,
           const ir_spath_t **from)
{
    uint32_t found = 0;
    for (uint32_t d = 0;
         summary && d < ir_summary_flow_count(summary, IR_SFLOW_STORES); d++)
        for (uint32_t s = 0;
             s < ir_summary_flow_source_count(summary, IR_SFLOW_STORES); s++)
            if (ir_summary_flow_has(summary, IR_SFLOW_STORES, d, s,
                                    IR_SFLOW_ALIAS)) {
                *to   = ir_summary_flow_to(summary, IR_SFLOW_STORES, d);
                *from = ir_summary_flow_source(summary, IR_SFLOW_STORES, s);
                found++;
            }
    return found == 1;
}

static core_string_t
paths_of(run_t *r, core_string_t name)
{
    ir_func_t    *f       = r->m ? ir_func_find(r->m, name) : nullptr;
    ir_summary_t *summary = f && r->summaries
        ? ir_summary_of(r->summaries, f) : nullptr;
    if (!summary) return S("<no summary>");
    core_list_t(core_string_t) lines = core_list_new(core_string_t);
    for (uint32_t i = 0; i < ir_summary_path_count(summary); i++)
        core_list_push(lines, render_access(f, ir_summary_path_at(summary, i)));
    render_flows(f, summary, IR_SFLOW_RESULTS, &lines);
    render_flows(f, summary, IR_SFLOW_STORES, &lines);
    qsort(lines.data, lines.len, sizeof(*lines.data), compare_lines);
    core_buffer_t *out = core_buffer_empty();
    for (size_t i = 0; i < lines.len; i++) {
        if (i) core_buffer_puts(out, "; ");
        core_buffer_append_string(out, lines.data[i]);
    }
    core_list_free(lines);
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
           S("r p.x; ~ p.x"));
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
           S("r v.a.x; ~ v.a.x; ~ v.n"));
    expect(&r, "and the caller sees the read through its copy",
           S("caller"), S("r p; r p.a.x; ~ p.a.x; ~ p.n"));
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
           S("holds"), S("r p.x; ~ p.x"));
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
test_overwritten_locals(void)
{
    run_t r = load(MOD(TYPES
        "  func @f(%p: ref[R], %q: ref[R]) -> nil {\n"
        "    local $s: ref[R]\n"
        "    block entry:\n"
        "      $s = copy %p\n"
        "      $s = copy %q\n"
        "      %t: ref[R] = copy $s\n"
        "      %x: ref[i32] = field %t, \"x\"\n"
        "      %one: i32 = const 1\n"
        "      store %x, %one\n"
        "      %none: nil = nil\n"
        "      return %none\n"
        "  }\n"
        "  func @g(%p: ref[R], %q: ref[R], %c: bool) -> nil {\n"
        "    local $s: ref[R]\n"
        "    block entry:\n"
        "      $s = copy %p\n"
        "      branch %c, left, join\n"
        "    block left:\n"
        "      $s = copy %q\n"
        "      jump join\n"
        "    block join:\n"
        "      %t: ref[R] = copy $s\n"
        "      %x: ref[i32] = field %t, \"x\"\n"
        "      %one: i32 = const 1\n"
        "      store %x, %one\n"
        "      %none: nil = nil\n"
        "      return %none\n"
        "  }\n"
        "  func @h(%p: ref[R], %q: ref[R], %n: i32) -> nil {\n"
        "    local $v: V\n"
        "    block entry:\n"
        "      %init: V = aggregate %p, %n\n"
        "      $v = copy %init\n"
        "      %at: ref[V] = address.of $v\n"
        "      %a: ref[ref[R]] = field %at, \"a\"\n"
        "      store %a, %q\n"
        "      %w: V = copy $v\n"
        "      %held: ref[R] = extract %w, \"a\"\n"
        "      %x: ref[i32] = field %held, \"x\"\n"
        "      %one: i32 = const 1\n"
        "      store %x, %one\n"
        "      %none: nil = nil\n"
        "      return %none\n"
        "  }\n"));
    expect(&r, "a local overwritten before its read holds only the new value",
           S("f"), S("w q.x"));
    expect(&r, "an overwrite on one arm keeps both values at the join",
           S("g"), S("w p.x; w q.x"));
    expect(&r, "an overwritten field of a record local holds the new value",
           S("h"), S("w q.x"));
    done(&r);
}

static void
test_overwritten_fields_of_copies(void)
{
    run_t r = load(MOD(TYPES
        "  func @k(%v: V, %m: i32) -> V {\n"
        "    local $s: V\n"
        "    block entry:\n"
        "      $s = copy %v\n"
        "      %at: ref[V] = address.of $s\n"
        "      %n: ref[i32] = field %at, \"n\"\n"
        "      store %n, %m\n"
        "      %w: V = copy $s\n"
        "      return %w\n"
        "  }\n"
        "  type P = (a: i32, b: i32)\n"
        "  func @scalars(%v: P, %m: i32) -> P {\n"
        "    local $s: P\n"
        "    block entry:\n"
        "      $s = copy %v\n"
        "      %at: ref[P] = address.of $s\n"
        "      %b: ref[i32] = field %at, \"b\"\n"
        "      store %b, %m\n"
        "      %w: P = copy $s\n"
        "      return %w\n"
        "  }\n"
        "  func @modify(inout %v: P, %m: i32) -> nil {\n"
        "    local $s: P\n"
        "    block entry:\n"
        "      $s = copy %v\n"
        "      %early: P = copy $s\n"
        "      %at: ref[P] = address.of $s\n"
        "      %b: ref[i32] = field %at, \"b\"\n"
        "      store %b, %m\n"
        "      %w: P = copy $s\n"
        "      return %w\n"
        "  }\n"));
    expect(&r, "a field written into a copy replaces only that field",
           S("k"), S("= v; ~.n m"));
    expect(&r, "and the same holds for a record of scalars",
           S("scalars"), S("~.a v.a; ~.b m"));
    expect(&r, "and for a modified parameter's copy-out",
           S("modify"), S("~.a v.a; ~.b m"));
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
           S("r g; r g.items; r g.items[]; r g.items[].x; ~ g.items[].x"));
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
           S("forged"), S("r ?...; ~ ?..."));
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
    const ir_spath_t *to = nullptr, *from = nullptr;
    check("a store summary names where an argument was put",
          only_store(summary, &to, &from) && to->root == IR_SPLACE_PARAM
              && to->param == 0 && to->depth == 1
              && core_string_eq(to->steps[0], S("next"))
              && from->root == IR_SPLACE_PARAM && from->param == 1
              && !from->depth);
    expect(&r, "what a callee stores into a local is followed", S("f"),
           S("r p.x; ~ p.x"));
    ir_func_t *g = ir_func_find(r.m, S("g"));
    summary = g ? ir_summary_of(r.summaries, g) : nullptr;
    check("and a callee's store is restated in the caller's terms",
          only_store(summary, &to, &from) && to->param == 0
              && from->param == 1);
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
           S("make"), S("r p.x; ~ p.x"));
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
           S("bump"), S("rw count; s count <~ count; ~ count"));
    expect(&r, "a scalar global returned by value is read", S("peek"),
           S("r count; ~ count"));
    expect(&r, "and so is one loaded through its address", S("loaded"),
           S("r count; ~ count..."));
    done(&r);
}

static const ir_value_t *
value_named(const ir_func_t *f, core_string_t name)
{
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = f->blocks[b]->instrs[i];
            if (in->dest && core_string_eq(in->dest->name, name))
                return in->dest;
        }
    return nullptr;
}

static const ir_instr_t *
instr_of(const ir_func_t *f, ir_op_t op)
{
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++)
            if (f->blocks[b]->instrs[i]->op == op)
                return f->blocks[b]->instrs[i];
    return nullptr;
}

static bool
names_param(const ir_pto_list_t *objects, uint32_t param)
{
    for (size_t i = 0; i < objects->len; i++)
        if ((objects->data[i].kind == IR_PTO_PARAM
             || objects->data[i].kind == IR_PTO_CONTENTS)
            && objects->data[i].param == param)
            return true;
    return false;
}

static ir_pt_fresh_t
copies_are_fresh(void *context, const ir_instr_t *in)
{
    (void)context;
    return in->op == IR_OP_COPY ? IR_PT_FRESH_HOLDING : IR_PT_FRESH_NONE;
}

static void
test_points_to_view(void)
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
        "  func @through(%p: ref[R], %q: ref[R]) -> nil {\n"
        "    block entry:\n"
        "      %slot: ref[ref[R]] = alloc ref[R]\n"
        "      store %slot, %p\n"
        "      %held: ref[R] = load %slot\n"
        "      %wrapped: ref[R] = copy %q\n"
        "      %r: nil = call @set, %held\n"
        "      return %r\n"
        "  }\n"
        "  func @publish(%out: ref[ref[R]]) -> nil {\n"
        "    block entry:\n"
        "      %fresh: ref[R] = alloc R\n"
        "      store %out, %fresh\n"
        "      %none: nil = nil\n"
        "      return %none\n"
        "  }\n"));
    ir_func_t *through = r.m ? ir_func_find(r.m, S("through")) : nullptr;
    ir_func_t *publish = r.m ? ir_func_find(r.m, S("publish")) : nullptr;
    check("the view fixtures load", through && publish);
    if (!through || !publish) {
        done(&r);
        return;
    }
    const ir_points_to_hooks_t hooks = {.fresh = copies_are_fresh};
    ir_points_to_t *view = ir_points_to_new(r.summaries, through, &hooks);
    check("the view solves", view != nullptr);
    ir_pto_list_t objects = core_list_new(ir_pto_t);
    ir_points_to_value(view, nullptr, value_named(through, S("held")),
                       &objects);
    check("a value loaded from a slot names the stored parameter",
          names_param(&objects, 0) && !names_param(&objects, 1));
    objects.len = 0;
    ir_points_to_loaded(view, nullptr, value_named(through, S("slot")),
                        &objects);
    check("loading through the slot reaches the same parameter",
          names_param(&objects, 0));
    objects.len = 0;
    const ir_value_t *wrapped = value_named(through, S("wrapped"));
    ir_points_to_value(view, nullptr, wrapped, &objects);
    check("a fresh copy is its own object",
          objects.len == 1 && objects.data[0].kind == IR_PTO_STORAGE
              && objects.data[0].instr == wrapped->def);
    ir_points_to_reach(view, &objects, false);
    check("which holds what it copied", names_param(&objects, 1));
    objects.len = 0;
    check("a known callee's writes map into the caller",
          ir_points_to_call_writes(view, instr_of(through, IR_OP_CALL), 0,
                                   &objects)
              && names_param(&objects, 0) && !names_param(&objects, 1));
    objects.len = 0;
    check("a trusted argument's writes are left out",
          ir_points_to_call_writes(view, instr_of(through, IR_OP_CALL), 1,
                                   &objects)
              && !names_param(&objects, 0));
    ir_points_to_free(view);
    view = ir_points_to_new(r.summaries, publish, nullptr);
    ir_pto_t fresh = {.kind  = IR_PTO_STORAGE,
                      .instr = instr_of(publish, IR_OP_ALLOC)};
    const ir_instr_t *where = nullptr;
    check("a stored object escapes at the store",
          view && ir_points_to_escapes(view, &fresh, &where)
              && where == instr_of(publish, IR_OP_STORE));
    ir_points_to_free(view);
    core_list_free(objects);
    done(&r);
}

static core_string_t
flows_of(run_t *r, core_string_t name)
{
    ir_func_t    *f       = r->m ? ir_func_find(r->m, name) : nullptr;
    ir_summary_t *summary = f && r->summaries
        ? ir_summary_of(r->summaries, f) : nullptr;
    if (!summary) return S("<no summary>");
    core_list_t(core_string_t) lines = core_list_new(core_string_t);
    render_flows(f, summary, IR_SFLOW_RESULTS, &lines);
    render_flows(f, summary, IR_SFLOW_STORES, &lines);
    qsort(lines.data, lines.len, sizeof(*lines.data), compare_lines);
    core_buffer_t *out = core_buffer_empty();
    for (size_t i = 0; i < lines.len; i++) {
        if (i) core_buffer_puts(out, "; ");
        core_buffer_append_string(out, lines.data[i]);
    }
    core_list_free(lines);
    return core_buffer_take_string(out);
}

static void
expect_flows(run_t *r, const char *what, core_string_t function,
             core_string_t expected)
{
    core_string_t actual = flows_of(r, function);
    bool same = core_string_eq(actual, expected);
    check(what, same);
    if (!same)
        fprintf(stderr, "    expected: %.*s\n    actual:   %.*s\n",
                (int)expected.u8_bytes, expected.data,
                (int)actual.u8_bytes, actual.data);
}

static void
test_value_dependences(void)
{
    run_t r = load(MOD(TYPES
        "  func @sum(%a: i32, %b: i32) -> i32 {\n"
        "    block entry:\n"
        "      %s: i32 = add trap %a, %b\n"
        "      return %s\n"
        "  }\n"
        "  func @deref(%p: ref[R]) -> i32 {\n"
        "    block entry:\n"
        "      %x: ref[i32] = field %p, \"x\"\n"
        "      %v: i32 = load %x\n"
        "      return %v\n"
        "  }\n"
        "  func @choose(%c: bool, %a: i32) -> i32 {\n"
        "    block entry:\n"
        "      branch %c, left, right\n"
        "    block left:\n"
        "      %one: i32 = const 1\n"
        "      jump join\n"
        "    block right:\n"
        "      jump join\n"
        "    block join:\n"
        "      %v: i32 = phi [left: %one], [right: %a]\n"
        "      return %v\n"
        "  }\n"
        "  func @set(%p: ref[R], %a: i32) -> nil {\n"
        "    block entry:\n"
        "      %x: ref[i32] = field %p, \"x\"\n"
        "      store %x, %a\n"
        "      %none: nil = nil\n"
        "      return %none\n"
        "  }\n"
        "  func @through_callee(%a: i32) -> i32 {\n"
        "    block entry:\n"
        "      %o: ref[R] = alloc R\n"
        "      %none: nil = call @set, %o, %a\n"
        "      %x: ref[i32] = field %o, \"x\"\n"
        "      %v: i32 = load %x\n"
        "      return %v\n"
        "  }\n"
        "  func @through_local(%a: i32) -> i32 {\n"
        "    local $s: V\n"
        "    block entry:\n"
        "      %addr: ref[V] = address.of $s\n"
        "      %n: ref[i32] = field %addr, \"n\"\n"
        "      store %n, %a\n"
        "      %c: V = copy $s\n"
        "      %v: i32 = extract %c, \"n\"\n"
        "      return %v\n"
        "  }\n"));
    expect_flows(&r, "a result depends on the parameters it combines",
                 S("sum"), S("~ a; ~ b"));
    expect_flows(&r, "a loaded result depends on the field it reads",
                 S("deref"), S("~ p.x"));
    expect_flows(&r, "a phi depends on the branch that chose it (D6)",
                 S("choose"), S("^ c; ~ a"));
    expect_flows(&r, "a written field depends on the stored value",
                 S("set"), S("s p.x <~ a"));
    expect_flows(&r, "a value written by a callee reaches the result",
                 S("through_callee"), S("~ a"));
    expect_flows(&r, "a value stored into a local's field reaches the "
                 "result (D1)", S("through_local"), S("~ a"));
    done(&r);
}

int
main(void)
{
    test_value_dependences();
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
    test_overwritten_locals();
    test_overwritten_fields_of_copies();
    test_callees_write_through_locals();
    test_cycles_widen();
    test_globals_and_elements();
    test_returned_fresh_objects();
    test_points_to_view();
    return report("IR summary paths");
}
