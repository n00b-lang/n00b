#include <stdio.h>
#include <stdlib.h>

#include "parse/bnf.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
#include "parse/tree_visit.h"
#include "parse/tree_util.h"
#include "scanner/tokenizer.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

#define TYPES                                             \
    S("<type>  ::= <name> | <ptr>\n"                      \
      "<ptr>   ::= %\"ptr\" %\"[\" <type> %\"]\"\n"       \
      "<name>  ::= %IDENTIFIER\n")

#define LANG                                              \
    S("<decl>  ::= %\"local\" %IDENTIFIER %\":\" <type>\n" \
      "<decls> ::= <decl> | <decl> <decls>\n")

static int64_t nt_id_s(parse_grammar_t *g, core_string_t name)
{
    for (size_t i = 0; i < core_list_len(g->nt_list); i++) {
        if (g->nt_list.data[i]->name.data
            && core_string_eq(g->nt_list.data[i]->name, name)) {
            return g->nt_list.data[i]->id;
        }
    }

    return -1;
}

#define nt_id(g, name) nt_id_s((g), S(name))

static parse_token_info_t *tk_s(int64_t tid, core_string_t text, int i)
{
    parse_token_info_t *t = parse_token_new();

    parse_token_set_tid(t, (int32_t)tid);
    parse_token_set_index(t, i);
    parse_token_set_position(t, 1, 0, 0, 0);
    parse_token_set_value(t, core_option_set(core_string_t,
                                             core_string_dup(text)),
                          false);

    return t;
}

#define tk(tid, text, i) tk_s((tid), S(text), (i))

static bool parses_s(parse_grammar_t *g, core_string_t start,
                     parse_token_info_t **toks, int n)
{
    parse_token_stream_t *ts = scan_token_stream_from_array(toks, n);
    parse_pwz_parser_t   *p
        = parse_pwz_new_for_start(g, nt_id_s(g, start));
    bool                ok = parse_pwz_parse(p, ts);

    parse_pwz_free(p);
    scan_token_stream_free(ts);

    return ok;
}

#define parses(g, start, toks, n) parses_s((g), S(start), (toks), (n))

static parse_grammar_t *compose_s(core_string_t first, core_string_t second,
                                  core_string_t start)
{
    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    bool a = parse_bnf_load(first, start, g);
    bool b = parse_bnf_load(second, start, g);

    check("both parts load into one grammar", a && b);

    return g;
}

#define compose(first, second, start) compose_s((first), (second), S(start))

static void test_the_parts_load_in_either_order(void)
{
    const struct {
        core_string_t what;
        core_string_t first;
        core_string_t second;
    } orders[] = {
        {S("types first"),    TYPES, LANG },
        {S("the user first"), LANG,  TYPES},
    };

    for (size_t i = 0; i < sizeof(orders) / sizeof(orders[0]); i++) {
        parse_grammar_t *g = compose(orders[i].first, orders[i].second, "decls");

        if (!parse_grammar_report_undefined(g, stdout)) {
            printf("FAIL  %.*s leaves nonterminals undefined\n",
                   (int)orders[i].what.u8_bytes, orders[i].what.data);
            fails++;
        }

        parse_grammar_free(g);
    }
    parse_grammar_t *partial = parse_grammar_new();
    parse_grammar_set_error_recovery(partial, false);
    parse_bnf_load(LANG, CORE_STRING_STATIC("decls"), partial);
    check("a missing grammar part remains undefined",
          core_list_len(parse_get_nonterm(partial,
              nt_id(partial, "type"))->rule_ids) == 0);
    parse_grammar_free(partial);
}

static void test_one_grammar_parses_from_several_start_symbols(void)
{
    parse_grammar_t *g = compose(TYPES, LANG, "decls");

    int64_t kw_local = parse_lookup_terminal(g, CORE_STRING_STATIC("local"));
    int64_t kw_ptr   = parse_lookup_terminal(g, CORE_STRING_STATIC("ptr"));

    parse_token_info_t *bare[] = {tk(PARSE_TOK_IDENTIFIER, "int", 0)};

    check("a type parses on its own", parses(g, "type", bare, 1));

    parse_token_info_t *decl[] = {
        tk(kw_local, "local", 0),
        tk(PARSE_TOK_IDENTIFIER, "x", 1),
        tk(':', ":", 2),
        tk(PARSE_TOK_IDENTIFIER, "int", 3),
    };

    check("and the same grammar parses what contains it",
          parses(g, "decl", decl, 4));

    parse_token_info_t *nested[] = {
        tk(kw_local, "local", 0),
        tk(PARSE_TOK_IDENTIFIER, "p", 1),
        tk(':', ":", 2),
        tk(kw_ptr, "ptr", 3),
        tk('[', "[", 4),
        tk(PARSE_TOK_IDENTIFIER, "int", 5),
        tk(']', "]", 6),
    };

    check("including a structured type from the other file",
          parses(g, "decl", nested, 7));

    check("and going back to the smaller start still works",
          parses(g, "type", bare, 1));

    parse_grammar_free(g);
}

static void test_a_failed_load_changes_nothing(void)
{
    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);
    check("the transaction baseline grammar loads",
          parse_bnf_load(CORE_STRING_STATIC("<ok> ::= 'x'\n"),
                         CORE_STRING_STATIC("ok"), g));

    size_t nonterms = core_list_len(g->nt_list);
    size_t rules = core_list_len(g->rules);
    size_t terminals = core_list_len(g->named_terms);
    size_t annotations = core_list_len(g->annotations);

    check("a grammar with an undeclared token is rejected",
          !parse_bnf_load(CORE_STRING_STATIC(
                              "@tokenizer(\"json\")\n"
                              "<bad> ::= %MISSING\n"),
                          CORE_STRING_STATIC("bad"), g));
    check("a rejected load adds no nonterminals",
          core_list_len(g->nt_list) == nonterms
              && parse_find_nonterm(g, CORE_STRING_STATIC("bad")) == nullptr);
    check("a rejected load adds no rules, terminals, or annotations",
          core_list_len(g->rules) == rules
              && core_list_len(g->named_terms) == terminals
              && core_list_len(g->annotations) == annotations);
    check("a rejected load does not change the tokenizer",
          core_string_eq(scan_grammar_tokenizer(g), S("text")));

    check("an empty literal is rejected before installing its production",
          !parse_bnf_load(CORE_STRING_STATIC("<empty> ::= \"\"\n"),
                          CORE_STRING_STATIC("empty"), g)
              && parse_find_nonterm(g, CORE_STRING_STATIC("empty")) == nullptr
              && core_list_len(g->rules) == rules);

    parse_grammar_free(g);
}

static void *r_int(parse_reduce_t *r)
{
    (void)r;

    return (void *)(uintptr_t)1;
}

static void *r_sum(parse_reduce_t *r)
{
    if (r->n_kids != 2) {
        printf("FAIL  <list> saw %zu children, not the 2 its production has\n",
               r->n_kids);
        fails++;

        return (void *)(uintptr_t)0;
    }

    uintptr_t total = (uintptr_t)r->kids[0];

    parse_reduce_list_t *rest = parse_reduce_as_list(r->kids[1]);

    if (!rest) {
        printf("FAIL  the repetition did not come through as a list\n");
        fails++;

        return (void *)total;
    }

    for (size_t i = 0; i < rest->n; i++) {
        total += (uintptr_t)rest->v[i];
    }

    return (void *)total;
}

static void test_a_reduce_folds_children_into_a_value(void)
{
    core_string_t G = S("<list> ::= <item> (%\",\" <item>)*\n"
                        "<item> ::= %IDENTIFIER\n");

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);
    parse_bnf_load(G, CORE_STRING_STATIC("list"), g);

    parse_token_info_t *toks[] = {
        tk(PARSE_TOK_IDENTIFIER, "a", 0),
        tk(',', ",", 1),
        tk(PARSE_TOK_IDENTIFIER, "b", 2),
        tk(',', ",", 3),
        tk(PARSE_TOK_IDENTIFIER, "c", 4),
    };

    parse_token_stream_t *ts = scan_token_stream_from_array(toks, 5);
    parse_pwz_parser_t   *p  = parse_pwz_new_for_start(
        g, nt_id(g, "list"));

    check("the list parses", parse_pwz_parse(p, ts));

    parse_tree_walk_t *w = parse_tree_walk_new(g, nullptr);

    parse_walk_reduce_nt(w, CORE_STRING_STATIC("item"), r_int, nullptr);
    parse_walk_reduce_nt(w, CORE_STRING_STATIC("list"), r_sum, nullptr);

    uintptr_t got = (uintptr_t)parse_tree_walk_reduce(w, parse_pwz_get_tree(p));

    if (got != 3) {
        printf("FAIL  a reduce over a quantified group saw %llu items, not 3\n",
               (unsigned long long)got);
        fails++;
    }

    parse_tree_walk_free(w);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static int reduced;

static void *r_count(parse_reduce_t *r)
{
    (void)r;
    reduced++;

    return nullptr;
}

static bool stop_at_item(parse_visit_t *v)
{
    parse_tree_walk_stop(v->walk);

    return true;
}

static void test_a_visitor_can_stop_a_reduce(void)
{
    core_string_t G = S("<list> ::= <item> (%\",\" <item>)*\n"
                        "<item> ::= %IDENTIFIER\n");

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);
    parse_bnf_load(G, CORE_STRING_STATIC("list"), g);

    parse_token_info_t *toks[] = {
        tk(PARSE_TOK_IDENTIFIER, "a", 0),
        tk(',', ",", 1),
        tk(PARSE_TOK_IDENTIFIER, "b", 2),
        tk(',', ",", 3),
        tk(PARSE_TOK_IDENTIFIER, "c", 4),
    };

    parse_token_stream_t *ts = scan_token_stream_from_array(toks, 5);
    parse_pwz_parser_t   *p  = parse_pwz_new_for_start(
        g, nt_id(g, "list"));

    check("the list parses for the stop test", parse_pwz_parse(p, ts));

    parse_tree_walk_t *w = parse_tree_walk_new(g, nullptr);

    reduced = 0;
    parse_walk_reduce_default(w, r_count, nullptr);
    parse_tree_walk_reduce(w, parse_pwz_get_tree(p));
    check("the whole tree reduces when nothing stops it", reduced > 1);
    parse_tree_walk_free(w);

    int whole = reduced;

    w = parse_tree_walk_new(g, nullptr);

    reduced = 0;
    parse_walk_on_nt(w, CORE_STRING_STATIC("item"), stop_at_item, nullptr,
                   nullptr);
    parse_walk_reduce_default(w, r_count, nullptr);

    void *out = parse_tree_walk_reduce(w, parse_pwz_get_tree(p));

    check("stopping in a visitor halts the reduce", reduced < whole);
    check("and the reduce reports that it stopped",
          parse_tree_walk_stopped(w));
    check("and yields nothing rather than a half-built result",
          out == nullptr);
    parse_tree_walk_free(w);

    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

typedef struct {
    parse_tree_t *first;
    parse_tree_t *second;
    parse_tree_t *third;
    parse_tree_t *replacement;
    parse_visit_t removed_visit;
    bool ancestry_ok;
    char visits[8];
    size_t count;
} edit_walk_t;

static parse_visit_outcome_t edit_children(parse_visit_t *v)
{
    edit_walk_t *edit = v->user;
    if (v->node == edit->first) {
        edit->visits[edit->count++] = 'a';
        edit->removed_visit = *v;
        edit->ancestry_ok &= parse_visit_ancestor(v, 0) == v->node
            && parse_visit_ancestor(v, 1) == v->parent
            && parse_visit_ancestor(v, 2) == nullptr;
        v->remove = true;
    }
    else if (v->node == edit->second) {
        edit->visits[edit->count++] = 'b';
        v->replace = edit->replacement;
        v->revisit_replacement = true;
    }
    else if (v->node == edit->replacement)
    {
        edit->visits[edit->count++] = 'r';
        edit->ancestry_ok &= parse_visit_ancestor(v, 0) == v->node
            && parse_visit_ancestor(v, 1) == v->parent;
    }
    else if (v->node == edit->third)
        edit->visits[edit->count++] = 'c';
    edit->visits[edit->count] = '\0';
    return PARSE_VISIT_CONTINUE;
}

static void test_tree_editing_keeps_shifted_siblings(void)
{
    edit_walk_t edit = {.ancestry_ok = true};
    parse_tree_t *root = core_tree_node(parse_nt_node_t,
        parse_token_info_ptr_t, ((parse_nt_node_t){.id = PARSE_EMPTY_STRING}));
    edit.first = core_tree_leaf(parse_nt_node_t, parse_token_info_ptr_t,
                                tk(PARSE_TOK_IDENTIFIER, "a", 0));
    edit.second = core_tree_leaf(parse_nt_node_t, parse_token_info_ptr_t,
                                 tk(PARSE_TOK_IDENTIFIER, "b", 1));
    edit.third = core_tree_leaf(parse_nt_node_t, parse_token_info_ptr_t,
                                tk(PARSE_TOK_IDENTIFIER, "c", 2));
    edit.replacement = core_tree_leaf(parse_nt_node_t, parse_token_info_ptr_t,
                                      tk(PARSE_TOK_IDENTIFIER, "r", 3));
    core_tree_add_child(root, edit.first);
    core_tree_add_child(root, edit.second);
    core_tree_add_child(root, edit.third);
    check("leaf lookup starts at the first child",
          core_tree_first_leaf_node(root) == edit.first);
    parse_tree_walk_t *walk = parse_tree_walk_new(nullptr, &edit);
    parse_walk_on_stage(walk, PARSE_WALK_PREPARE, edit_children, nullptr);
    parse_tree_walk_run(walk, root);
    check("removal revisits the shifted sibling and replacement is prepared",
          core_string_eq(core_string_borrow(edit.visits, edit.count),
                         S("abrc"))
              && !parse_tree_walk_failed(walk));
    check("active ancestry follows leaf replacement and not detached nodes",
          edit.ancestry_ok
              && parse_visit_ancestor(&edit.removed_visit, 0) == nullptr);
    check("only the retained children remain attached in order",
          core_tree_num_children(root) == 2
              && core_tree_child(root, 0) == edit.replacement
              && core_tree_child(root, 1) == edit.third);
    check("leaf lookup follows edited source order",
          core_tree_first_leaf_node(root) == edit.replacement
              && parse_tree_scan_leaves(root, true, nullptr,
                                        nullptr) == edit.third);
    parse_tree_t *second = parse_tree_walk_take_detached(walk);
    parse_tree_t *first = parse_tree_walk_take_detached(walk);
    check("removed and replaced nodes transfer back to the caller",
          second == edit.second && first == edit.first
              && !parse_tree_walk_take_detached(walk));
    parse_tree_walk_free(walk);
    parse_tree_free(first);
    parse_tree_free(second);
    parse_tree_free(root);
}

typedef struct {
    parse_tree_t *root;
    parse_tree_t *left;
    parse_tree_t *right;
    parse_tree_t *shared;
    size_t left_visits;
    size_t right_visits;
    bool ancestry_ok;
} shared_walk_t;

static parse_visit_outcome_t check_shared_ancestry(parse_visit_t *v)
{
    shared_walk_t *state = v->user;
    if (v->node != state->shared) return PARSE_VISIT_CONTINUE;
    state->ancestry_ok &= parse_visit_ancestor(v, 0) == state->shared
        && parse_visit_ancestor(v, 1) == v->parent
        && parse_visit_ancestor(v, 2) == state->root
        && parse_visit_ancestor(v, 3) == nullptr;
    if (v->parent == state->left) state->left_visits++;
    if (v->parent == state->right) state->right_visits++;
    return PARSE_VISIT_CONTINUE;
}

static void test_shared_node_ancestry_uses_active_path(void)
{
    shared_walk_t state = {.ancestry_ok = true};
    state.root = core_tree_node(parse_nt_node_t, parse_token_info_ptr_t,
        ((parse_nt_node_t){.id = PARSE_EMPTY_STRING}));
    state.left = core_tree_node(parse_nt_node_t, parse_token_info_ptr_t,
        ((parse_nt_node_t){.id = PARSE_EMPTY_STRING}));
    state.right = core_tree_node(parse_nt_node_t, parse_token_info_ptr_t,
        ((parse_nt_node_t){.id = PARSE_EMPTY_STRING}));
    state.shared = core_tree_leaf(parse_nt_node_t, parse_token_info_ptr_t,
                                  tk(PARSE_TOK_IDENTIFIER, "shared", 0));
    core_tree_add_child(state.root, state.left);
    core_tree_add_child(state.root, state.right);
    core_tree_add_child(state.left, state.shared);
    core_tree_add_child(state.right, state.shared);
    parse_tree_walk_t *walk = parse_tree_walk_new(nullptr, &state);
    parse_walk_on_stage(walk, PARSE_WALK_PREPARE,
                        check_shared_ancestry, nullptr);
    parse_tree_walk_run(walk, state.root);
    check("shared nodes report their active parent",
          state.ancestry_ok && state.left_visits == 1
              && state.right_visits == 1 && !parse_tree_walk_failed(walk));
    parse_tree_walk_free(walk);
    core_tree_remove_child(state.right, 0);
    parse_tree_free(state.root);
}

static void test_named_child_lookup_distinguishes_group_wrappers(void)
{
    parse_grammar_t *grammar = compose(TYPES, LANG, "decls");
    parse_tree_t *root = core_tree_node(parse_nt_node_t,
        parse_token_info_ptr_t, ((parse_nt_node_t){.id = nt_id(grammar,
                                                               "decls")}));
    parse_tree_t *outer = core_tree_node(parse_nt_node_t,
        parse_token_info_ptr_t, ((parse_nt_node_t){.group_top = true}));
    parse_tree_t *inner = core_tree_node(parse_nt_node_t,
        parse_token_info_ptr_t, ((parse_nt_node_t){.group_top = true}));
    parse_tree_t *type = core_tree_node(parse_nt_node_t,
        parse_token_info_ptr_t, ((parse_nt_node_t){.id = nt_id(grammar,
                                                               "type")}));
    parse_tree_t *name = core_tree_node(parse_nt_node_t,
        parse_token_info_ptr_t, ((parse_nt_node_t){.id = nt_id(grammar,
                                                               "name")}));
    parse_tree_t *decl = core_tree_node(parse_nt_node_t,
        parse_token_info_ptr_t, ((parse_nt_node_t){.id = nt_id(grammar,
                                                               "decl")}));
    core_tree_add_child(root, outer);
    core_tree_add_child(outer, type);
    core_tree_add_child(outer, inner);
    core_tree_add_child(inner, name);
    core_tree_add_child(root, decl);
    check("direct child search does not cross group wrappers",
          parse_tree_find_direct_child_by_nt_name(grammar, root,
              CORE_STRING_STATIC("name")) == nullptr
              && parse_tree_find_direct_child_by_nt_name(grammar, root,
                  CORE_STRING_STATIC("decl")) == decl);
    check("flat child search crosses nested group wrappers only",
          parse_tree_find_flat_child_by_nt_name(grammar, root,
              CORE_STRING_STATIC("name")) == name
              && parse_tree_find_flat_child_by_nt_name(grammar, root,
                  CORE_STRING_STATIC("decl")) == decl);
    check("nth child uses the same flattened order",
          parse_tree_get_nth_nt_child(root, 0) == type
              && parse_tree_get_nth_nt_child(root, 1) == name
              && parse_tree_get_nth_nt_child(root, 2) == decl
              && parse_tree_get_nth_nt_child(root, 3) == nullptr);
    parse_tree_free(root);
    parse_grammar_free(grammar);
}

int main(void)
{
    test_the_parts_load_in_either_order();
    test_one_grammar_parses_from_several_start_symbols();
    test_a_failed_load_changes_nothing();
    test_a_reduce_folds_children_into_a_value();
    test_a_visitor_can_stop_a_reduce();
    test_tree_editing_keeps_shifted_siblings();
    test_shared_node_ancestry_uses_active_path();
    test_named_child_lookup_distinguishes_group_wrappers();

    return report("fe compose");
}
