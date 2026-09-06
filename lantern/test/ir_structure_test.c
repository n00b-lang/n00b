#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/layout.h"
#include "ir/pass.h"
#include "ir/region_walk.h"
#include "ir/rule.h"
#include "ir/text.h"
#include "ir/verify.h"

static int  fails = 0;
static bool produced[IR_DIAG_COUNT];

static void check(const char *what, bool ok)
{
    if (!ok) {
        fails++;
        printf("FAIL  %s\n", what);
    }
}

static const ir_profile_t *const PROFILES[] = {&ir_profile_bce};

typedef struct {
    ir_ctx_t    *ctx;
    ir_module_t *m;
    ir_diags_t  *d;
} run_t;

static run_t *load(const char *src)
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

static bool told(run_t *r, const char *text)
{
    for (uint32_t i = 0; i < ir_diags_count(r->d); i++) {
        const ir_diag_t *dg = ir_diags_at(r->d, i);

        if ((dg->primary.label && strstr(dg->primary.label, text))
            || (dg->message && strstr(dg->message, text))) {
            return true;
        }
    }

    return false;
}

typedef struct {
    char  *p;
    size_t n;
    size_t cap;
} buf;

static void put(buf *b, const char *s)
{
    size_t n = strlen(s);

    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p   = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n + 1);
    b->n += n;
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

static void sketch(buf *b, const ir_region_t *r)
{
    if (!r) {
        return;
    }

    if (r->kind == IR_REGION_BLOCK) {
        put(b, "[");
        put(b, r->head ? r->head->name : "?");
        put(b, "]");

        return;
    }
    put(b, ir_region_kind_name(r->kind));
    put(b, "(");

    for (uint32_t i = 0; i < r->n_kids; i++) {
        sketch(b, r->kids[i]);
    }
    put(b, ")");
}

static char *shape(ir_func_t *f)
{
    buf b = {0};

    put(&b, "");
    sketch(&b, ir_region_root(f));

    return b.p;
}

#define MOD(...) "module m {\n  profile bce\n  target t\n\n" __VA_ARGS__ "}\n"

static const char *AN_IF = MOD(
    "  func @f(%c: bool, %a: i32) -> i32 {\n"
    "    block entry:\n      branch %c, t, e\n"
    "    block t:\n      jump join\n"
    "    block e:\n      jump join\n"
    "    block join:\n      return %a\n  }\n");

static const char *A_LOOP = MOD(
    "  func @f(%c: bool, %a: i32) -> i32 {\n"
    "    block entry:\n      jump head\n"
    "    block head:\n      branch %c, body, out\n"
    "    block body:\n      jump head\n"
    "    block out:\n      return %a\n  }\n");

static const char *A_SWITCH = MOD(
    "  func @f(%s: i32) -> i32 {\n"
    "    block entry:\n"
    "      switch %s, default: d, [1: one], [2: two]\n"
    "    block one:\n      jump join\n"
    "    block two:\n      jump join\n"
    "    block d:\n      jump join\n"
    "    block join:\n      return %s\n  }\n");

static const char *IRREDUCIBLE = MOD(
    "  func @f(%c: bool, %a: i32) -> i32 {\n"
    "    block entry:\n      branch %c, x, y\n"
    "    block x:\n      jump y\n"
    "    block y:\n      branch %c, x, out\n"
    "    block out:\n      return %a\n  }\n");

static void test_reducible_shapes(void)
{
    struct {
        const char *what;
        const char *src;
        const char *shape;
    } cases[] = {
        {"an if", AN_IF, "seq(if([entry][t][e])[join])"},
        {"a loop", A_LOOP, "seq([entry]loop([head][body])[out])"},
        {"a switch", A_SWITCH, "seq(switch([entry][d][one][two])[join])"},
        {"a sequence", MOD("  func @f(%a: i32) -> i32 {\n"
                           "    block entry:\n      jump second\n"
                           "    block second:\n      return %a\n  }\n"),
         "seq([entry][second])"},
    };

    for (size_t i = 0; i < 4; i++) {
        run_t *r = load(cases[i].src);

        ir_structure(r->m, r->d);

        char *got = shape(ir_func_at(r->m, 0));

        if (!got || strcmp(got, cases[i].shape)) {
            printf("FAIL  %s structures as %s, wanted %s\n", cases[i].what,
                   got ? got : "(nothing)", cases[i].shape);
            fails++;
        }
        free(got);
        done(r);
    }
}

static void test_the_overlay_is_metadata(void)
{
    run_t *r      = load(A_LOOP);
    char  *before = ir_module_print(r->m);

    check("structure runs", ir_structure(r->m, r->d));

    char *after = ir_module_print(r->m);

    check("and changes no instruction, block or anything else printable",
          before && after && !strcmp(before, after));
    free(before);
    free(after);
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
          ir_pass_run_checked(&ir_pass_structure, r->m, r->d, nullptr, r->d)
              == IR_PASS_OK);
    check("and establishes what it declared",
          (ir_module_props(r->m) & IR_PROP_STRUCTURED) != 0);
    done(r);
}

static void test_irreducible_still_succeeds(void)
{
    run_t *r      = load(IRREDUCIBLE);
    char  *before = ir_module_print(r->m);

    check("structure succeeds on irreducible control flow",
          ir_structure(r->m, r->d));

    char *after = ir_module_print(r->m);

    check("without rewriting control flow to avoid the problem",
          before && after && !strcmp(before, after));
    ir_func_t   *f    = ir_func_at(r->m, 0);
    ir_region_t *site = ir_region_dispatch_site(f);
    ir_block_t  *x    = ir_block_find(f, "x");
    ir_block_t  *y    = ir_block_find(f, "y");

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

    free(before);
    free(after);
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
    const char *all[] = {AN_IF, A_LOOP, A_SWITCH, IRREDUCIBLE};

    for (size_t i = 0; i < 4; i++) {
        run_t *r = load(all[i]);

        ir_structure(r->m, r->d);

        ir_func_t *f = ir_func_at(r->m, 0);

        for (uint32_t k = 0; k < f->n_blocks; k++) {
            if (!ir_region_of_block(f, f->blocks[k])) {
                printf("FAIL  block %s is in no region\n", f->blocks[k]->name);
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
    check("and the rule is named", told(r, "structured.overlay"));
    check("and says how many times the block was covered",
          told(r, "0 times"));
    done(r);

    run_t *none = load(AN_IF);

    ir_module_establish(none->m, IR_PROP_STRUCTURED);
    check("so is claiming structured with no overlay at all",
          !(ir_verify(none->m, none->d) & IR_PROP_STRUCTURED));
    check("and it says so", told(none, "no region overlay"));
    done(none);
}

static void test_the_overlay_is_deterministic(void)
{
    const char *all[] = {AN_IF, A_LOOP, A_SWITCH, IRREDUCIBLE};

    for (size_t i = 0; i < 4; i++) {
        run_t *a = load(all[i]);
        run_t *b = load(all[i]);

        ir_structure(a->m, a->d);
        ir_structure(a->m, a->d);

        char *twice = shape(ir_func_at(a->m, 0));

        ir_structure(b->m, b->d);

        char *once = shape(ir_func_at(b->m, 0));

        if (!once || !twice || strcmp(once, twice)) {
            printf("FAIL  running structure twice changes the tree\n");
            fails++;
        }

        char  *text  = ir_module_print(b->m);
        run_t *again = load(text);

        ir_structure(again->m, again->d);

        char *after = shape(ir_func_at(again->m, 0));

        if (!after || strcmp(after, once)) {
            printf("FAIL  the tree differs after a round trip: %s vs %s\n",
                   after ? after : "(nothing)", once);
            fails++;
        }
        free(text);
        free(after);
        free(once);
        free(twice);
        done(again);
        done(a);
        done(b);
    }
}

static void test_the_nested_form_is_a_rendering(void)
{
    run_t *r = load(A_LOOP);

    ir_structure(r->m, r->d);

    char *drawn = ir_module_render_nested(r->m);

    check("the rendering exists", drawn != nullptr);
    check("it names every block it draws",
          drawn && strstr(drawn, "block entry") && strstr(drawn, "block head")
              && strstr(drawn, "block body") && strstr(drawn, "block out"));
    check("it draws the regions around them",
          drawn && strstr(drawn, "loop at head {"));
    check("and says outright that it is not the format",
          drawn && strstr(drawn, "not its format"));

    ir_diags_t  *d2 = ir_diags_new(r->ctx, nullptr);
    ir_module_t *m2 = ir_module_parse(r->ctx, drawn, 0, PROFILES, 1, d2);

    check("it is a rendering, not a second syntax: it does not parse",
          m2 == nullptr);
    free(drawn);
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
            printf("FAIL  %s declares what §3 lists for it\n", p->name);
            fails++;
        }
    }
    check("the four §3 passes are the four that exist", true);
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

    ir_block_t *made = ir_block_find(f, "entry.to.join");

    check("a block was created", made != nullptr);
    check("and it takes the span of the construct it implements",
          made && ir_span_known(made->span)
              && made->span.start_byte == was.start_byte);
    check("its jump is attributed to the pass that made it",
          made && made->n_instrs && made->instrs[0]->origin
              && !strcmp(made->instrs[0]->origin->agent, "split-edges"));
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
            printf("FAIL  block %s is in no region\n", f->blocks[i]->name);
            fails++;
        }
    }

    ir_module_establish(r->m, IR_PROP_STRUCTURED);
    check("and the overlay covers every one of its blocks exactly once",
          (ir_verify(r->m, r->d) & IR_PROP_STRUCTURED) != 0);
    check("the loop is recognised as a loop",
          ir_region_of_block(f, ir_block_find(f, "latch")) != nullptr);
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

static void write_produced(const char *dir)
{
    size_t n    = strlen(dir) + 32;
    char  *path = malloc(n);

    snprintf(path, n, "%s/produced-codes-structure.txt", dir);

    FILE *f = fopen(path, "w");

    if (f) {
        for (uint16_t i = 0; i < IR_DIAG_COUNT; i++) {
            if (produced[i]) {
                fprintf(f, "%s\n", ir_diag_code((ir_diagcode_t)i));
            }
        }
        fclose(f);
    }
    free(path);
}

int main(int argc, char **argv)
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

    if (argc > 1) {
        write_produced(argv[1]);
    }

    if (!fails) {
        printf("ir structure: all checks passed\n");
    }

    return fails ? 1 : 0;
}
