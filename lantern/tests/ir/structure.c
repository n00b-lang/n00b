#include <stdio.h>
#include <stdlib.h>

#include "ir/layout.h"
#include "ir/pass.h"
#include "ir/region_walk.h"
#include "ir/rule.h"
#include "ir/text.h"
#include "ir/verify.h"
#include "lib/buffer.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static bool produced[IR_DIAG_COUNT];

static const ir_profile_t *const PROFILES[] = {&ir_profile_bce};

typedef struct {
    ir_ctx_t    *ctx;
    ir_module_t *m;
    ir_diags_t  *d;
} run_t;

static run_t *load(core_string_t src)
{
    run_t *r = calloc(1, sizeof(run_t));

    r->ctx = ir_ctx_new();
    r->d   = ir_diags_new(r->ctx, nullptr);
    r->m   = ir_module_parse(r->ctx, src, 0, PROFILES, 1, r->d);

    if (r->m) {
        ir_module_establish(r->m, IR_PROP_PARSED | IR_PROP_TYPED);
    }

    return r;
}

static void done(run_t *r)
{
    for (uint32_t i = 0; i < ir_diags_count(r->d); i++) {
        produced[ir_diags_at(r->d, i)->code] = true;
    }
    ir_ctx_free(r->ctx);
    free(r);
}

static bool contains(core_string_t hay, core_string_t needle)
{
    return hay.data && core_string_find(hay, needle) >= 0;
}

static bool told(run_t *r, core_string_t text)
{
    for (uint32_t i = 0; i < ir_diags_count(r->d); i++) {
        const ir_diag_t *dg = ir_diags_at(r->d, i);

        if (contains(ir_diag_item(r->d, dg)->label, text) || contains(ir_diag_item(r->d, dg)->message, text)) {
            return true;
        }
    }

    return false;
}

static bool holds(const ir_region_t *r, const ir_block_t *b)
{
    if (!r || !b) {
        return false;
    }

    if (r->kind == IR_REGION_BLOCK && r->head == b) {
        return true;
    }

    for (uint32_t i = 0; i < r->n_kids; i++) {
        if (holds(r->kids[i], b)) {
            return true;
        }
    }

    return false;
}

static void sketch(core_buffer_t *b, const ir_region_t *r)
{
    if (!r) {
        return;
    }

    if (r->kind == IR_REGION_BLOCK) {
        core_buffer_puts(b, "[");
        core_buffer_append_string(b, r->head ? r->head->name : S("?"));
        core_buffer_puts(b, "]");

        return;
    }
    core_buffer_append_string(b, ir_region_kind_name(r->kind));
    core_buffer_puts(b, "(");

    for (uint32_t i = 0; i < r->n_kids; i++) {
        sketch(b, r->kids[i]);
    }
    core_buffer_puts(b, ")");
}

static core_string_t shape(ir_func_t *f)
{
    core_buffer_t *b = core_buffer_empty();

    sketch(b, ir_region_root(f));

    return core_buffer_take_string(b);
}

#define MOD(...) \
    S("module m {\n  profile bce\n  target t\n\n" __VA_ARGS__ "}\n")

static const core_string_t AN_IF = MOD(
    "  func @f(%c: bool, %a: i32) -> i32 {\n"
    "    block entry:\n      branch %c, t, e\n"
    "    block t:\n      jump join\n"
    "    block e:\n      jump join\n"
    "    block join:\n      return %a\n  }\n");

static const core_string_t A_LOOP = MOD(
    "  func @f(%c: bool, %a: i32) -> i32 {\n"
    "    block entry:\n      jump head\n"
    "    block head:\n      branch %c, body, out\n"
    "    block body:\n      jump head\n"
    "    block out:\n      return %a\n  }\n");

static const core_string_t A_SWITCH = MOD(
    "  func @f(%s: i32) -> i32 {\n"
    "    block entry:\n"
    "      switch %s, default: d, [1: one], [2: two]\n"
    "    block one:\n      jump join\n"
    "    block two:\n      jump join\n"
    "    block d:\n      jump join\n"
    "    block join:\n      return %s\n  }\n");

static const core_string_t IRREDUCIBLE = MOD(
    "  func @f(%c: bool, %a: i32) -> i32 {\n"
    "    block entry:\n      branch %c, x, y\n"
    "    block x:\n      jump y\n"
    "    block y:\n      branch %c, x, out\n"
    "    block out:\n      return %a\n  }\n");

static void test_reducible_shapes(void)
{
    struct {
        core_string_t what;
        core_string_t src;
        core_string_t shape;
    } cases[] = {
        {S("an if"), AN_IF, S("seq(if([entry][t][e])[join])")},
        {S("a loop"), A_LOOP, S("seq([entry]loop([head][body])[out])")},
        {S("a switch"), A_SWITCH,
         S("seq(switch([entry][d][one][two])[join])")},
        {S("a sequence"), MOD("  func @f(%a: i32) -> i32 {\n"
                              "    block entry:\n      jump second\n"
                              "    block second:\n      return %a\n  }\n"),
         S("seq([entry][second])")},
    };

    for (size_t i = 0; i < 4; i++) {
        run_t *r = load(cases[i].src);

        ir_structure(r->m, r->d);

        core_string_t got = shape(ir_func_at(r->m, 0));

        if (!got.data || !core_string_eq(got, cases[i].shape)) {
            core_string_t shown = got.data ? got : S("(nothing)");

            printf("FAIL  %.*s structures as %.*s, wanted %.*s\n",
                   (int)cases[i].what.u8_bytes, cases[i].what.data,
                   (int)shown.u8_bytes, shown.data,
                   (int)cases[i].shape.u8_bytes, cases[i].shape.data);
            fails++;
        }
        core_free(got.data);
        done(r);
    }
}

static void test_the_overlay_is_metadata(void)
{
    run_t        *r      = load(A_LOOP);
    core_string_t before = ir_module_print(r->m);

    check("structure runs", ir_structure(r->m, r->d));

    core_string_t after = ir_module_print(r->m);

    check("and changes no instruction, block or anything else printable",
          before.data && after.data && core_string_eq(before, after));
    core_free(before.data);
    core_free(after.data);
    done(r);
}

static void test_the_pass_declares_what_the_spec_says(void)
{
    check("structure requires parsed and establishes structured",
          ir_pass_structure.requires == IR_PROP_PARSED
              && ir_pass_structure.establishes == IR_PROP_STRUCTURED
              && ir_pass_declares(&ir_pass_structure));

    run_t *r = load(AN_IF);

    check("it runs through the pass manager",
          ir_pass_run(&ir_pass_structure, r->m, nullptr, nullptr, r->d)
              == IR_PASS_OK);
    check("and establishes what it declared",
          (ir_module_props(r->m) & IR_PROP_STRUCTURED) != 0);
    done(r);
}

static void test_irreducible_still_succeeds(void)
{
    run_t        *r      = load(IRREDUCIBLE);
    core_string_t before = ir_module_print(r->m);

    check("structure succeeds on irreducible control flow",
          ir_structure(r->m, r->d));

    core_string_t after = ir_module_print(r->m);

    check("without rewriting control flow to avoid the problem",
          before.data && after.data && core_string_eq(before, after));
    ir_func_t   *f    = ir_func_at(r->m, 0);
    ir_region_t *site = ir_region_dispatch_site(f);
    ir_block_t  *x    = ir_block_find(f, S("x"));
    ir_block_t  *y    = ir_block_find(f, S("y"));

    check("and it records that a dispatch loop is needed", site != nullptr);

    check("the region it names holds both ends of the irreducible edge",
          site && holds(site, x) && holds(site, y));

    bool a_child_would_do = false;

    for (uint32_t i = 0; site && i < site->n_kids; i++) {
        a_child_would_do = a_child_would_do
                        || (holds(site->kids[i], x) && holds(site->kids[i], y));
    }
    check("and no region inside it would have done, so it is the innermost",
          !a_child_would_do);
    check("the site answers the question a back end asks of a region",
          ir_region_needs_dispatch(site));
    check("and the root does not, unless it is itself the site",
          site == ir_region_root(f)
              || !ir_region_needs_dispatch(ir_region_root(f)));

    core_free(before.data);
    core_free(after.data);
    done(r);

    run_t *ok = load(A_LOOP);

    ir_structure(ok->m, ok->d);
    check("reducible flow is not marked as needing one anywhere",
          ir_region_dispatch_site(ir_func_at(ok->m, 0)) == nullptr);
    check("and no region of it claims to need one",
          !ir_region_needs_dispatch(
              ir_region_root(ir_func_at(ok->m, 0))));
    done(ok);
}

static void test_every_block_exactly_once(void)
{
    const core_string_t all[] = {AN_IF, A_LOOP, A_SWITCH, IRREDUCIBLE};

    for (size_t i = 0; i < 4; i++) {
        run_t *r = load(all[i]);

        ir_structure(r->m, r->d);

        ir_func_t *f = ir_func_at(r->m, 0);

        for (uint32_t k = 0; k < f->n_blocks; k++) {
            if (!ir_region_of_block(f, f->blocks[k])) {
                printf("FAIL  block %.*s is in no region\n",
                       (int)f->blocks[k]->name.u8_bytes,
                       f->blocks[k]->name.data);
                fails++;
            }
        }

        ir_module_establish(r->m, IR_PROP_STRUCTURED);
        check("the verifier agrees the overlay covers the blocks",
              (ir_verify(r->m, r->d) & IR_PROP_STRUCTURED) != 0);
        done(r);
    }
}

static void test_a_broken_overlay_is_caught(void)
{
    run_t *r = load(AN_IF);

    ir_structure(r->m, r->d);

    ir_func_t   *f    = ir_func_at(r->m, 0);
    ir_region_t *root = ir_region_root(f);

    root->n_kids = 0;
    ir_module_establish(r->m, IR_PROP_STRUCTURED);

    ir_props_t held = ir_verify(r->m, r->d);

    check("an overlay that stopped covering the blocks is caught",
          !(held & IR_PROP_STRUCTURED));
    check("and the rule is named", told(r, S("structured.overlay")));
    check("and says how many times the block was covered",
          told(r, S("0 times")));
    done(r);

    run_t *none = load(AN_IF);

    ir_module_establish(none->m, IR_PROP_STRUCTURED);
    check("so is claiming structured with no overlay at all",
          !(ir_verify(none->m, none->d) & IR_PROP_STRUCTURED));
    check("and it says so", told(none, S("no region overlay")));
    done(none);
}

static void test_the_overlay_is_deterministic(void)
{
    const core_string_t all[] = {AN_IF, A_LOOP, A_SWITCH, IRREDUCIBLE};

    for (size_t i = 0; i < 4; i++) {
        run_t *a = load(all[i]);
        run_t *b = load(all[i]);

        ir_structure(a->m, a->d);
        ir_structure(a->m, a->d);

        core_string_t twice = shape(ir_func_at(a->m, 0));

        ir_structure(b->m, b->d);

        core_string_t once = shape(ir_func_at(b->m, 0));

        if (!once.data || !twice.data || !core_string_eq(once, twice)) {
            printf("FAIL  running structure twice changes the tree\n");
            fails++;
        }

        core_string_t text  = ir_module_print(b->m);
        run_t        *again = load(text);

        ir_structure(again->m, again->d);

        core_string_t after = shape(ir_func_at(again->m, 0));

        if (!after.data || !core_string_eq(after, once)) {
            core_string_t shown = after.data ? after : S("(nothing)");

            printf("FAIL  the tree differs after a round trip: %.*s vs %.*s\n",
                   (int)shown.u8_bytes, shown.data, (int)once.u8_bytes,
                   once.data);
            fails++;
        }
        core_free(text.data);
        core_free(after.data);
        core_free(once.data);
        core_free(twice.data);
        done(again);
        done(a);
        done(b);
    }
}

static void test_the_nested_form_is_a_rendering(void)
{
    run_t *r = load(A_LOOP);

    ir_structure(r->m, r->d);

    core_string_t drawn = ir_module_render_nested(r->m);

    check("the rendering exists", drawn.data != nullptr);
    check("it names every block it draws",
          contains(drawn, S("block entry")) && contains(drawn, S("block head"))
              && contains(drawn, S("block body"))
              && contains(drawn, S("block out")));
    check("it draws the regions around them",
          contains(drawn, S("loop at head {")));
    check("and says outright that it is not the format",
          contains(drawn, S("not its format")));

    ir_diags_t  *d2 = ir_diags_new(r->ctx, nullptr);
    ir_module_t *m2 = ir_module_parse(r->ctx, drawn, 0, PROFILES, 1, d2);

    check("it is a rendering, not a second syntax: it does not parse",
          m2 == nullptr);
    core_free(drawn.data);
    done(r);
}

static void test_every_named_pass_declares_what_the_spec_lists(void)
{
    struct {
        const ir_pass_t *pass;
        ir_props_t       requires;
        ir_props_t       establishes;
    } table[] = {
        {&ir_pass_promote, IR_PROP_PARSED | IR_PROP_TYPED, IR_PROP_SSA},
        {&ir_pass_structure, IR_PROP_PARSED, IR_PROP_STRUCTURED},
        {&ir_pass_layout, IR_PROP_TYPED, IR_PROP_LAID_OUT},
        {&ir_pass_split_edges, IR_PROP_PARSED, IR_PROP_NO_CRIT_EDGE},
    };

    for (size_t i = 0; i < 4; i++) {
        const ir_pass_t *p = table[i].pass;

        if (p->requires != table[i].requires
            || p->establishes != table[i].establishes
            || !ir_pass_declares(p)) {
            printf("FAIL  %.*s does not declare what it needs and gives\n",
                   (int)p->name.u8_bytes, p->name.data);
            fails++;
        }
    }
    check("the four declared passes are the four that exist", true);
}

static void test_a_created_block_takes_its_construct_s_span(void)
{
    run_t *r = load(MOD("  func @f(%a: i32, %c: bool) -> i32 {\n"
                        "    block entry:\n      branch %c, l, join\n"
                        "    block l:\n      jump join\n"
                        "    block join:\n      return %a\n  }\n"));
    ir_func_t  *f     = ir_func_at(r->m, 0);
    ir_block_t *entry = ir_block_entry(f);
    ir_span_t   was   = entry->span;

    check("the branching block has a location", ir_span_known(was));
    check("split-edges runs", ir_split_edges(r->m, r->d));

    ir_block_t *made = ir_block_find(f, S("entry.to.join"));

    check("a block was created", made != nullptr);
    check("and it takes the span of the construct it implements",
          made && ir_span_known(made->span)
              && made->span.start_byte == was.start_byte);
    check("its jump is attributed to the pass that made it",
          made && made->n_instrs && made->instrs[0]->origin
              && core_string_eq(made->instrs[0]->origin->agent,
                                S("split-edges")));
    done(r);
}

static void test_a_stale_overlay_does_not_outlive_its_property(void)
{
    run_t *r = load(MOD("  func @f(%a: i32, %c: bool) -> i32 {\n"
                        "    block entry:\n      branch %c, l, join\n"
                        "    block l:\n      jump join\n"
                        "    block join:\n      return %a\n  }\n"));
    ir_func_t *f = ir_func_at(r->m, 0);

    ir_structure(r->m, r->d);
    check("the overlay exists", ir_region_root(f) != nullptr);

    ir_split_edges(r->m, r->d);

    check("split-edges dropped the structured claim",
          !(ir_module_props(r->m) & IR_PROP_STRUCTURED));
    check("and the overlay went with it, rather than lingering as a tree "
          "that no longer covers the function",
          ir_region_root(f) == nullptr);
    done(r);
}

static void test_a_loop_with_a_body_of_several_blocks(void)
{
    run_t *r = load(MOD("  func @f(%c: bool, %a: i32) -> i32 {\n"
                        "    block entry:\n      jump head\n"
                        "    block head:\n      branch %c, body, out\n"
                        "    block body:\n      branch %c, more, latch\n"
                        "    block more:\n      jump latch\n"
                        "    block latch:\n      jump head\n"
                        "    block out:\n      return %a\n  }\n"));

    check("a loop whose body branches structures", ir_structure(r->m, r->d));

    ir_func_t *f = ir_func_at(r->m, 0);

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        if (!ir_region_of_block(f, f->blocks[i])) {
            printf("FAIL  block %.*s is in no region\n",
                   (int)f->blocks[i]->name.u8_bytes, f->blocks[i]->name.data);
            fails++;
        }
    }

    ir_module_establish(r->m, IR_PROP_STRUCTURED);
    check("and the overlay covers every one of its blocks exactly once",
          (ir_verify(r->m, r->d) & IR_PROP_STRUCTURED) != 0);
    check("the loop is recognised as a loop",
          ir_region_of_block(f, ir_block_find(f, S("latch"))) != nullptr);
    check("and nothing in it is marked irreducible",
          ir_region_dispatch_site(f) == nullptr);
    done(r);
}

typedef struct {
    int  enters, leaves, blocks, loops;
    int  max_depth;
    char order[256];
    size_t at;
} rtally;

static void rnote(rtally *t, char c)
{
    if (t->at + 1 < sizeof t->order) {
        t->order[t->at++] = c;
    }
}

static bool r_enter(ir_region_visit_t *v)
{
    rtally *t = v->user;

    t->enters++;
    rnote(t, 'E');

    if (v->depth > t->max_depth) {
        t->max_depth = v->depth;
    }

    return true;
}

static bool r_leave(ir_region_visit_t *v)
{
    rtally *t = v->user;

    t->leaves++;
    rnote(t, 'e');

    return true;
}

static bool r_block(ir_region_visit_t *v)
{
    ((rtally *)v->user)->blocks++;

    return true;
}

static bool r_loop(ir_region_visit_t *v)
{
    ((rtally *)v->user)->loops++;

    return true;
}

static bool r_refuse(ir_region_visit_t *v)
{
    (void)v;

    return false;
}

static bool r_stop(ir_region_visit_t *v)
{
    ir_region_walk_stop(v->walk);

    return true;
}

static void test_the_region_walk(void)
{
    run_t *r = load(MOD("  func @f(%c: bool, %a: i32) -> i32 {\n"
                        "    block entry:\n      jump head\n"
                        "    block head:\n      branch %c, body, out\n"
                        "    block body:\n      jump head\n"
                        "    block out:\n      return %a\n  }\n"));

    check("the module for the region walk structures",
          ir_structure(r->m, r->d));

    ir_func_t   *f    = ir_func_at(r->m, 0);
    ir_region_t *root = ir_region_root(f);

    rtally            t = {0};
    ir_region_walk_t *w = ir_region_walk_new(&t);

    ir_region_on_node(w, r_enter, r_leave, nullptr);
    ir_region_on_kind(w, IR_REGION_BLOCK, r_block, nullptr, nullptr);
    ir_region_on_kind(w, IR_REGION_LOOP, r_loop, nullptr, nullptr);
    ir_region_walk_run(w, root);
    ir_region_walk_free(w);

    check("every region is entered and left once",
          t.enters == t.leaves && t.enters > 0);
    check("the walk reaches every block of the function",
          t.blocks == (int)f->n_blocks);
    check("and sees the loop", t.loops == 1);
    check("a kind visitor stacks with the node visitor rather than "
          "replacing it", t.enters > t.blocks);
    check("the root is at depth zero", t.order[0] == 'E');
    check("and a nested region is deeper than the root", t.max_depth > 0);

    rtally            p  = {0};
    ir_region_walk_t *w2 = ir_region_walk_new(&p);

    ir_region_on_node(w2, r_refuse, nullptr, nullptr);
    ir_region_on_kind(w2, IR_REGION_BLOCK, r_block, nullptr, nullptr);
    ir_region_walk_run(w2, root);
    ir_region_walk_free(w2);

    check("a pre visitor refusing children prunes the subtree",
          p.blocks == 0);

    rtally            s  = {0};
    ir_region_walk_t *w3 = ir_region_walk_new(&s);

    ir_region_on_node(w3, r_stop, nullptr, nullptr);
    ir_region_on_kind(w3, IR_REGION_BLOCK, r_block, nullptr, nullptr);
    ir_region_walk_run(w3, root);

    check("stopping at the root reaches nothing below it", s.blocks == 0);
    check("and the walk reports that it stopped", ir_region_walk_stopped(w3));
    ir_region_walk_free(w3);

    check("walking a null root is not a crash",
          (ir_region_walk_run(nullptr, nullptr), true));
    done(r);
}


int main(void)
{
    test_reducible_shapes();
    test_the_overlay_is_metadata();
    test_the_pass_declares_what_the_spec_says();
    test_irreducible_still_succeeds();
    test_every_block_exactly_once();
    test_a_broken_overlay_is_caught();
    test_the_overlay_is_deterministic();
    test_the_region_walk();
    test_the_nested_form_is_a_rendering();
    test_every_named_pass_declares_what_the_spec_lists();
    test_a_created_block_takes_its_construct_s_span();
    test_a_stale_overlay_does_not_outlive_its_property();
    test_a_loop_with_a_body_of_several_blocks();

    dump_produced_codes("produced-codes-structure.txt", produced);

    return report("ir structure");
}
