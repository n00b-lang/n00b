#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "lib/alloc.h"
#include "parse/annot_walk.h"
#include "parse/annotation.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/symtab.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

#define SF(s) (int)(s).u8_bytes, (s).data

static parse_annot_result_t *
annotate_new(parse_grammar_t *g, parse_tree_t *tree)
{
    if (!g || !tree) return nullptr;
    parse_annot_result_t *result = parse_annot_result_new(nullptr);
    parse_tree_walk_t *walk = parse_tree_walk_new(g, result);
    parse_annot_register(walk, result);
    parse_tree_walk_run(walk, tree);
    parse_tree_walk_free(walk);
    return result;
}

typedef struct {
    parse_tree_t *target;
    parse_tree_t *parent;
    bool saw_parent;
} walk_parent_probe_t;

static bool
capture_walk_parent(parse_visit_t *visit)
{
    walk_parent_probe_t *probe = visit ? visit->thunk : nullptr;
    if (probe && visit->node == probe->target)
        probe->saw_parent = visit->parent == probe->parent;
    return true;
}

static void
test_tree_resolution_is_scope_or_symbol_without_replacing_lexical_scope(void)
{
    parse_scope_t lexical_scope = {0};
    parse_scope_t module_scope = {0};
    parse_sym_entry_t symbol = {0};
    parse_tree_t *node = core_tree_node(parse_nt_node_t,
        parse_token_info_ptr_t, ((parse_nt_node_t){0}));
    parse_tree_set_scope(node, &lexical_scope);
    parse_symtab_t *symtab = parse_symtab_new();
    bool scope_set = node
        && parse_tree_set_resolution_scope(node, &module_scope);
    bool lexical_scope_lookup = node && symtab
        && parse_symtab_scope_for_node(symtab, node) == &lexical_scope;
    parse_node_resolution_t scope_ref = parse_tree_resolution(node);
    bool scope_ok = scope_set
        && scope_ref.kind == PARSE_NODE_RESOLUTION_SCOPE
        && scope_ref.target.scope == &module_scope
        && parse_tree_scope(node) == &lexical_scope;
    bool symbol_set = node
        && parse_tree_set_resolution_symbol(node, &symbol);
    parse_node_resolution_t symbol_ref = parse_tree_resolution(node);
    bool symbol_ok = symbol_set
        && symbol_ref.kind == PARSE_NODE_RESOLUTION_SYMBOL
        && symbol_ref.target.symbol == &symbol
        && parse_tree_sym(node) == &symbol
        && parse_tree_scope(node) == &lexical_scope;
    check("tree resolution stores either a target scope or symbol independently of lexical scope",
          scope_ok && symbol_ok && lexical_scope_lookup);
    parse_symtab_free(symtab);
    parse_tree_free_node(node);
}

static bool
visit_type_namespace_scope(parse_scope_t *scope, void *context)
{
    size_t *count = context;
    if (!scope->ns
        || !core_string_eq(scope->ns->ns_name, S("type"))) return false;
    (*count)++;
    return true;
}

static bool
stop_namespace_scope_visit(parse_scope_t *scope, void *context)
{
    (void)scope;
    (*(size_t *)context)++;
    return false;
}

static bool
count_namespace_symbol(parse_sym_entry_t *symbol, void *context)
{
    (void)symbol;
    (*(size_t *)context)++;
    return true;
}

static void
test_symbol_namespace_scope_iteration(void)
{
    parse_symtab_t *st = parse_symtab_new();
    parse_symtab_push_scope_all(st, S("module"));
    parse_symtab_push_scope_all(st, S("function"));
    parse_symtab_pop_scope_all(st);

    parse_symtab_ns(st, S("type"));
    size_t count = 0;
    bool all_type_scopes = parse_symtab_visit_namespace_scopes(
        st, S("type"), visit_type_namespace_scope, &count);

    size_t stopped_count = 0;
    bool stopped = parse_symtab_visit_namespace_scopes(
        st, S("type"), stop_namespace_scope_visit, &stopped_count);

    check("namespace scope visitor covers only mirrored namespace scopes",
          all_type_scopes && count == 3);
    check("namespace scope visitor reports early termination",
          !stopped && stopped_count == 1);
    check("namespace scope visitor rejects an unknown namespace",
          !parse_symtab_visit_namespace_scopes(st, S("missing"),
              visit_type_namespace_scope, &count));
    parse_symtab_free(st);
}

static void
test_symbol_namespaces_share_lexical_scopes(void)
{
    parse_symtab_t *st = parse_symtab_new();
    core_string_t type_ns = S("type");
    parse_symtab_push_scope_all(st, S("module"));
    parse_scope_t *module_scope = parse_symtab_current_scope(st, S(""));
    parse_sym_entry_t *value = parse_symtab_add(
        st, S(""), S("Shared"), PARSE_SYM_VARIABLE, nullptr);
    parse_symtab_push_scope_all(st, S("function"));
    parse_scope_t *function_scope = parse_symtab_current_scope(st, S(""));
    parse_sym_entry_t *local_value = parse_symtab_add(
        st, S(""), S("Shared"), PARSE_SYM_VARIABLE, nullptr);
    parse_symtab_pop_scope_all(st);

    parse_sym_entry_t *type = parse_symtab_add(
        st, type_ns, S("Shared"), PARSE_SYM_TYPEDEF, nullptr);
    parse_scope_t *type_function_scope = parse_symtab_scope_in_namespace(
        st, function_scope, type_ns);
    parse_symtab_enter_scope(st, type_ns, type_function_scope);
    parse_sym_entry_t *local_type = parse_symtab_add(
        st, type_ns, S("Shared"), PARSE_SYM_TYPEDEF, nullptr);

    bool pass = value && type && local_type && local_value
        && type_function_scope
        && value->kind == PARSE_SYM_VARIABLE
        && type->kind == PARSE_SYM_TYPEDEF
        && parse_symtab_lookup_scoped(st, module_scope, type_ns, S("Shared"))
            == type
        && parse_symtab_lookup_scoped(st, function_scope, type_ns,
                                      S("Shared")) == local_type
        && parse_symtab_lookup_scoped(st, function_scope, S(""),
                                      S("Shared")) == local_value
        && parse_symtab_scope_in_namespace(st, module_scope, type_ns)
            == type->scope
        && type->scope != value->scope
        && type_function_scope->depth == function_scope->depth
        && local_type->scope == type_function_scope;
    size_t module_matches = 0;
    size_t function_matches = 0;
    size_t peer_scope_matches = 0;
    bool found_module_names = parse_symtab_visit_name_across_namespaces(
        st, value->scope, S("Shared"), count_namespace_symbol,
        &module_matches);
    bool found_function_names = parse_symtab_visit_name_across_namespaces(
        st, function_scope, S("Shared"), count_namespace_symbol,
        &function_matches);
    bool found_peer_scope_names = parse_symtab_visit_name_across_namespaces(
        st, type_function_scope, S("Shared"), count_namespace_symbol,
        &peer_scope_matches);
    pass = pass && found_module_names && module_matches == 2
        && found_function_names && function_matches == 2
        && found_peer_scope_names && peer_scope_matches == 2;
    check("late symbol namespaces mirror lexical scopes without merging tables",
          pass);
    parse_symtab_free(st);
}

static void
test_symtab_storage_facts(void)
{
    parse_symtab_t *st = parse_symtab_new();
    core_string_t ns = CORE_STRING_STATIC("storage-test");
    parse_sym_entry_t *value = parse_symtab_add(
        st, ns, CORE_STRING_STATIC("value"), PARSE_SYM_VARIABLE, nullptr);
    parse_sym_entry_t *unknown_view = parse_symtab_add(
        st, ns, CORE_STRING_STATIC("unknown_view"), PARSE_SYM_VARIABLE,
        nullptr);

    parse_sym_set_storage_class(value, CORE_STRING_STATIC("gc"), true,
                                CORE_STRING_STATIC("gc"));
    parse_sym_set_storage_duration(value, PARSE_SYM_STORAGE_DYNAMIC);

    check("storage request and compiler selection are retained",
          value->storage_class_required
              && core_string_eq(value->requested_storage_class,
                                CORE_STRING_STATIC("gc"))
              && core_string_eq(value->selected_storage_class,
                                CORE_STRING_STATIC("gc")));
    parse_sym_set_storage_class(value, value->requested_storage_class, true,
                                value->requested_storage_class);
    check("storage class setter preserves aliased input strings",
          value->storage_class_required
              && core_string_eq(value->requested_storage_class,
                                CORE_STRING_STATIC("gc"))
              && core_string_eq(value->selected_storage_class,
                                CORE_STRING_STATIC("gc")));
    parse_sym_set_storage_class(value, (core_string_t){0}, true,
                                (core_string_t){0});
    check("required storage remains required when its class is missing",
          value->storage_class_required
              && !value->requested_storage_class.u8_bytes
              && !value->selected_storage_class.u8_bytes);
    parse_sym_set_storage_class(value, CORE_STRING_STATIC("gc"), true,
                                CORE_STRING_STATIC("gc"));
    check("storage duration is tracked separately from storage class",
          value->storage_duration == PARSE_SYM_STORAGE_DYNAMIC
              && core_string_eq(parse_sym_storage_duration_name(
                    value->storage_duration), S("dynamic"))
              && unknown_view->storage_duration
                    == PARSE_SYM_STORAGE_UNSPECIFIED);

    parse_symtab_free(st);
}

typedef struct {
    int               decls_seen;
    parse_tree_t *block_node;
    parse_tree_t *cond_node;
    parse_scope_t      *module_scope;
    parse_scope_t      *block_scope;
} probe_t;

static bool
saw_decl(parse_visit_t *v)
{
    ((probe_t *)v->thunk)->decls_seen++;

    return true;
}

static bool
saw_block(parse_visit_t *v)
{
    probe_t *p = (probe_t *)v->thunk;

    p->block_node  = v->node;
    p->block_scope = parse_tree_scope(v->node);

    return true;
}

static bool
saw_prog(parse_visit_t *v)
{
    ((probe_t *)v->thunk)->module_scope = parse_tree_scope(v->node);

    return true;
}

static bool
saw_cond(parse_visit_t *v)
{
    ((probe_t *)v->thunk)->cond_node = v->node;

    return true;
}

typedef struct {
    size_t enters;
    size_t exits;
} node_probe_t;

static void
count_annot_nodes(parse_visit_t *v, bool entering, void *context)
{
    node_probe_t *probe = context;
    (void)v;

    if (entering) {
        probe->enters++;
    }
    else {
        probe->exits++;
    }
}

static parse_grammar_t *
load_toy(void)
{
    core_string_t text = slurp("toy.bnf");

    if (!text.data) {
        return nullptr;
    }

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    if (!parse_bnf_load(text, S("prog"), g)) {
        check("toy.bnf loads", false);
        parse_grammar_free(g);

        return nullptr;
    }

    return g;
}

static parse_tree_t *
parse_toy_s(parse_grammar_t *g, core_string_t start, core_string_t src,
            parse_pwz_parser_t **p_out,
            parse_token_stream_t **ts_out)
{
    parse_token_stream_t *ts = scan_token_stream_from_codepoints(src);
    parse_pwz_parser_t   *p = parse_pwz_new_for_start(
        g, parse_nonterm_id(parse_find_nonterm(g, start)));

    if (!parse_pwz_parse(p, ts)) {
        return nullptr;
    }

    *p_out  = p;
    *ts_out = ts;

    return parse_pwz_get_tree(p);
}

#define parse_toy(g, start, src, p_out, ts_out) \
    parse_toy_s((g), S(start), S(src), (p_out), (ts_out))

static parse_tree_t *
find_node_s(parse_grammar_t *g, parse_tree_t *node, core_string_t name)
{
    if (!node || core_tree_is_leaf(node)) {
        return nullptr;
    }

    if (core_string_eq(parse_tree_nt_name(g, node), name)) {
        return node;
    }

    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *found = find_node_s(g, core_tree_child(node, i), name);

        if (found) {
            return found;
        }
    }

    return nullptr;
}

#define find_node(g, node, name) find_node_s((g), (node), S(name))

static parse_annotation_t *
find_unique_rule_annot(parse_grammar_t *g, const parse_nonterm_t *nt,
                       parse_annot_kind_t kind)
{
    parse_annotation_t *found = nullptr;

    for (size_t i = 0; g && nt && i < core_list_len(nt->rule_ids); i++) {
        parse_annotation_t *candidate = parse_rule_find_annot(
            g, parse_get_rule(g, nt->rule_ids.data[i]), kind);
        if (!candidate) continue;
        if (found) return nullptr;
        found = candidate;
    }

    return found;
}

static void
test_annotations_are_retained_on_the_grammar(parse_grammar_t *g)
{
    parse_annotation_t *tok = parse_grammar_find_annot(g, PARSE_ANNOT_TOKENIZER);

    check("a file-level annotation is retained on the grammar", tok != nullptr);

    if (tok) {
        check("and keeps its argument", core_string_eq(tok->scope_tag, S("toy")));
    }

    parse_nonterm_t *prog = nullptr;

    for (size_t i = 0; i < core_list_len(g->nt_list); i++) {
        if (core_string_eq(g->nt_list.data[i]->name, S("prog"))) {
            prog = g->nt_list.data[i];
        }
    }

    check("the start non-terminal exists", prog != nullptr);

    if (prog) {
        check("and carries its @scope annotation",
              find_unique_rule_annot(g, prog, PARSE_ANNOT_SCOPE_OPEN) != nullptr);
    }

    parse_annotation_t *a
        = find_unique_rule_annot(g, prog, PARSE_ANNOT_SCOPE_OPEN);

    if (a) {
        check("with the tag it was written with",
              core_string_eq(a->scope_tag, S("module")));
    }
}

typedef struct {
    parse_grammar_t *grammar;
    parse_rule_t    *rule;
    bool             ok;
} annot_lookup_run_t;

static void *
lookup_annotations_concurrently(void *arg)
{
    annot_lookup_run_t *run = arg;
    run->ok = true;

    for (int i = 0; i < 2000; i++) {
        parse_annotation_t *file
            = parse_grammar_find_annot(run->grammar, PARSE_ANNOT_TOKENIZER);
        parse_annotation_t *scope
            = parse_rule_find_annot(run->grammar, run->rule,
                                    PARSE_ANNOT_SCOPE_OPEN);

        if (!file || !scope) {
            run->ok = false;
            break;
        }
    }

    return nullptr;
}

static void
test_annotation_cache_is_thread_safe(parse_grammar_t *g)
{
    parse_nonterm_t *prog = parse_find_nonterm(g, S("prog"));
    enum { N_THREADS = 8 };
    pthread_t threads[N_THREADS];
    annot_lookup_run_t runs[N_THREADS];
    bool created[N_THREADS] = {0};

    parse_grammar_finalize(g);

    parse_rule_t *prog_rule
        = prog && core_list_len(prog->rule_ids)
              ? parse_get_rule(g, prog->rule_ids.data[0]) : nullptr;

    for (int i = 0; i < N_THREADS; i++) {
        runs[i] = (annot_lookup_run_t){.grammar = g, .rule = prog_rule};
        created[i] = pthread_create(&threads[i], nullptr,
                                    lookup_annotations_concurrently,
                                    &runs[i]) == 0;
    }

    bool ok = prog != nullptr;
    for (int i = 0; i < N_THREADS; i++) {
        if (!created[i]) {
            ok = false;
            continue;
        }

        pthread_join(threads[i], nullptr);
        ok &= runs[i].ok;
    }

    check("annotation cache lookups are safe across threads", ok);
}

static size_t
symbols_in_scope(parse_scope_t *scope)
{
    size_t count = 0;
    for (parse_sym_entry_t *e = scope ? scope->first_in_scope : nullptr; e;
         e = e->next_in_scope)
        count++;
    return count;
}

static void
test_a_declaration_walk_is_reentered_by_the_full_walk(parse_grammar_t *g)
{
    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "prog", "vaT;{vbU;}?xyztcV;", &p, &ts);
    if (!tree) {
        check("the toy program parses for the two-pass walk", false);
        return;
    }
    parse_annot_result_t *r = parse_annot_result_new(nullptr);

    probe_t declared = {0};
    parse_tree_walk_t *w = parse_tree_walk_new(g, r);
    parse_annot_register_declarations(w, r);
    parse_walk_on_nt(w, S("block"), nullptr, saw_block, &declared);
    parse_walk_on_nt(w, S("prog"), nullptr, saw_prog, &declared);
    parse_tree_walk_run(w, tree);
    parse_tree_walk_free(w);

    parse_sym_entry_t *a_declared = parse_symtab_lookup_scoped(r->symtab,
        declared.module_scope, core_string_empty(), S("a"));
    parse_sym_entry_t *c_declared = parse_symtab_lookup_scoped(r->symtab,
        declared.module_scope, core_string_empty(), S("c"));
    size_t module_symbols = symbols_in_scope(declared.module_scope);
    check("the declaration walk builds the module scope and its symbols",
          declared.module_scope && a_declared && module_symbols > 0);

    probe_t full = {0};
    w = parse_tree_walk_new(g, r);
    parse_annot_register(w, r);
    parse_walk_on_nt(w, S("block"), nullptr, saw_block, &full);
    parse_walk_on_nt(w, S("prog"), nullptr, saw_prog, &full);
    parse_tree_walk_run(w, tree);
    parse_tree_walk_free(w);

    parse_sym_entry_t *a_full = parse_symtab_lookup_scoped(r->symtab,
        full.module_scope, core_string_empty(), S("a"));
    check("the full walk re-enters the declared scopes",
          full.module_scope == declared.module_scope
              && full.block_scope == declared.block_scope);
    check("the full walk reuses the declared symbols",
          a_full == a_declared
              && symbols_in_scope(full.module_scope) == module_symbols);
    parse_sym_entry_t *c_full = parse_symtab_lookup_scoped(r->symtab,
        full.module_scope, core_string_empty(), S("c"));
    check("the full walk reuses every declared symbol",
          c_declared && c_full == c_declared);

    parse_annot_result_free(r);
}

static void
test_the_walk_builds_a_symbol_table(parse_grammar_t *g)
{
    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t   *tree
        = parse_toy(g, "prog", "vaT;{vbU;}?xyztcV;", &p, &ts);

    if (!tree) {
        check("the toy program parses", false);

        return;
    }

    probe_t             probe = {0};
    parse_annot_result_t *r     = core_alloc(parse_annot_result_t);

    memset(r, 0, sizeof(*r));
    r->symtab    = parse_symtab_new();
    r->cf_labels = core_alloc(core_dict_t);
    core_dict_init(r->cf_labels, core_hash_word, nullptr);

    parse_tree_walk_t *w = parse_tree_walk_new(g, r);

    parse_annot_register(w, r);

    check("a visitor can be registered by non-terminal name",
          parse_walk_on_nt(w, S("decl"), saw_decl, nullptr, &probe));
    check("registering an unknown non-terminal fails",
          !parse_walk_on_nt(w, S("no-such-rule"), saw_decl, nullptr, &probe));

    parse_walk_on_nt(w, S("block"), nullptr, saw_block, &probe);
    parse_walk_on_nt(w, S("prog"), nullptr, saw_prog, &probe);
    parse_walk_on_nt(w, S("cond"), nullptr, saw_cond, &probe);

    parse_tree_walk_run(w, tree);
    parse_tree_walk_free(w);

    parse_tree_t *subtree = nullptr;
    int32_t subtree_index = -1;
    for (size_t i = 0; i < core_tree_num_children(tree); i++) {
        parse_tree_t *child = core_tree_child(tree, i);
        if (core_tree_is_leaf(child)) continue;
        subtree = child;
        subtree_index = (int32_t)i;
        break;
    }
    walk_parent_probe_t parent_probe = {
        .target = subtree,
        .parent = tree,
    };
    parse_tree_walk_t *subtree_walk = parse_tree_walk_new(g, nullptr);
    if (subtree && subtree_walk) {
        parse_walk_on_node(subtree_walk, capture_walk_parent, nullptr,
                           &parent_probe);
        parse_tree_walk_run_with_parent(subtree_walk, subtree, tree, 1,
                                        subtree_index);
    }
    check("a subtree walk preserves its structural parent",
          subtree && subtree_walk && parent_probe.saw_parent
              && parse_tree_parent(subtree) == tree);
    parse_tree_walk_free(subtree_walk);

    check("a caller's visitor fires once per matching node",
          probe.decls_seen == 2);
    check("the module scope was opened", probe.module_scope != nullptr);
    check("the block scope was opened", probe.block_scope != nullptr);

    if (probe.module_scope && probe.block_scope) {
        check("the block scope nests inside the module scope",
              probe.block_scope->parent == probe.module_scope);
        check("and the module scope is tagged as the annotation wrote it",
              core_string_eq(probe.module_scope->scope_tag, S("module")));
        check("scope depth counts from the file scope",
              probe.module_scope->depth == 1 && probe.block_scope->depth == 2);
    }

    parse_symtab_t *st = r->symtab;

    parse_sym_entry_t *a = parse_symtab_lookup_scoped(st, probe.module_scope,
                                                  core_string_empty(), S("a"));
    parse_sym_entry_t *b_from_module
        = parse_symtab_lookup_scoped(st, probe.module_scope, core_string_empty(),
                                   S("b"));
    parse_sym_entry_t *b_from_block
        = parse_symtab_lookup_scoped(st, probe.block_scope, core_string_empty(),
                                   S("b"));
    parse_sym_entry_t *a_from_block
        = parse_symtab_lookup_scoped(st, probe.block_scope, core_string_empty(),
                                   S("a"));
    parse_sym_entry_t *c = parse_symtab_lookup_scoped(st, probe.module_scope,
                                                  core_string_empty(), S("c"));

    check("a symbol declared at module scope is found there", a != nullptr);
    check("a symbol declared in a block is not visible outside it",
          b_from_module == nullptr);
    check("but is visible from inside the block", b_from_block != nullptr);
    check("and the enclosing scope is still reachable from the block",
          a_from_block == a);
    parse_tree_t *decl = find_node(g, tree, "decl");
    parse_tree_t *name_node = decl ? find_node(g, decl, "name") : nullptr;
    parse_tree_set_scope(name_node, probe.module_scope);
    check("a reference test has a nonterminal name node",
          name_node && !core_tree_is_leaf(name_node));
    parse_tree_set_unresolved_name(name_node, S("a"));
    check("a reference test uses the declared name",
          name_node && core_string_eq(
              parse_tree_leaf_text(core_tree_first_leaf_node(name_node)), S("a")));
    check("a reference node resolves and retains its symbol",
          name_node && parse_symtab_resolve_node(
              st, name_node, core_string_empty()) == a
              && parse_tree_sym(name_node) == a);
    bool deferred_scope_resolves = name_node
        && parse_tree_set_resolution_scope(name_node, probe.block_scope)
        && parse_symtab_resolve_node(st, name_node,
                                     core_string_empty()) == a;
    parse_node_resolution_t resolved_scope = name_node
        ? parse_tree_resolution(name_node) : (parse_node_resolution_t){0};
    check("a deferred scope target resolves through that scope and binds the symbol",
          deferred_scope_resolves
          && resolved_scope.kind == PARSE_NODE_RESOLUTION_SYMBOL
          && resolved_scope.target.symbol == a
          && parse_tree_scope(name_node) == probe.module_scope);
    check("a scope is still searchable after the walk popped it",
          b_from_block != nullptr && b_from_block->scope == probe.block_scope);
    check("every declaration was recorded", c != nullptr);
    check("a declaration annotation leaves its symbol untyped",
          a && !a->type && c && !c->type);

    check("the @branch node was found", probe.cond_node != nullptr);

    parse_cf_label_t *cf = parse_cf_label_lookup(r, probe.cond_node);

    check("@branch produces a control flow label", cf != nullptr);

    if (cf) {
        check("of the right kind", cf->kind == PARSE_CF_BRANCH);
        check("with all three arms resolved",
              cf->cond && cf->then_body && cf->else_body
                  && cf->cond != cf->then_body
                  && cf->then_body != cf->else_body);
        check("and each arm is the child the annotation named",
              core_string_eq(parse_tree_leaf_text(core_tree_first_leaf_node(cf->cond)), S("x"))
                  && core_string_eq(
                      parse_tree_leaf_text(core_tree_first_leaf_node(cf->then_body)), S("y"))
                  && core_string_eq(
                      parse_tree_leaf_text(core_tree_first_leaf_node(cf->else_body)),
                      S("z")));
    }

    parse_annot_result_free(r);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
}

static void
test_a_deep_tree_does_not_overflow_the_stack(parse_grammar_t *g)
{
    const char *env  = getenv("IR_DEEP");
    int64_t     deep = 600;

    if (env) {
        core_string_t want     = core_string_from_cstr(env);
        size_t        consumed = 0;

        if (!core_string_parse_i64(want, 10, &deep, &consumed)) {
            deep = 0;
        }
        core_free(want.data);
    }

    const int N = (int)deep;

    char *src = core_alloc_size(1, (size_t)N * 4 + 1);

    for (int i = 0; i < N; i++) {
        memcpy(src + i * 4, "vaT;", 4);
    }

    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t   *tree = parse_toy_s(g, S("prog"),
                                       core_string_borrow(src, (size_t)N * 4),
                                       &p, &ts);

    if (!tree) {
        check("a deeply nested program parses", false);
        core_free(src);

        return;
    }

    probe_t          probe = {0};
    parse_tree_walk_t *w     = parse_tree_walk_new(g, nullptr);

    parse_walk_on_nt(w, S("decl"), saw_decl, nullptr, &probe);
    parse_tree_walk_run(w, tree);
    parse_tree_walk_free(w);

    check("a walk over a deeply nested tree visits every node",
          probe.decls_seen == N);

    parse_pwz_free(p);
    scan_token_stream_free(ts);
    core_free(src);
}

static parse_nonterm_t *
nt_named_s(parse_grammar_t *g, core_string_t name)
{
    for (size_t i = 0; i < core_list_len(g->nt_list); i++) {
        if (core_string_eq(g->nt_list.data[i]->name, name)) {
            return g->nt_list.data[i];
        }
    }

    return nullptr;
}

#define nt_named(g, name) nt_named_s((g), S(name))

static void
test_annotation_arguments_only_need_balanced_parens(void)
{
    core_string_t src
        = S("<top> @declares($1,\"var\") @disambig(pred(a, b, c()), 4) "
            "::= <n> <t>\n"
            "<n> ::= __LOWER\n"
            "<t> ::= __UPPER\n");

    parse_grammar_t *g = parse_grammar_new();

    check("an annotation argument may nest balanced parentheses",
          parse_bnf_load(src, S("top"), g));

    parse_nonterm_t *top = nt_named(g, "top");

    check("the annotated non-terminal is there", top != nullptr);

    if (top) {
        parse_annotation_t *d
            = find_unique_rule_annot(g, top, PARSE_ANNOT_DECLARES);

        check("@declares reads its name reference and kind",
              d != nullptr && d->name_ref.kind == PARSE_ROLE_BY_INDEX
                  && d->name_ref.index == 1
                  && core_string_eq(d->sym_kind, S("var")));

        check("the annotation that nests parentheses survives the load",
              find_unique_rule_annot(g, top, PARSE_ANNOT_DISAMBIG) != nullptr);
    }

    parse_grammar_free(g);

    core_string_t by_name
        = S("<top> @assigns($1, $t) ::= <n> <t>\n"
            "<n> ::= __LOWER\n"
            "<t> ::= __UPPER\n");

    parse_grammar_t *g2 = parse_grammar_new();

    check("a reference may name a non-terminal instead of an index",
          parse_bnf_load(by_name, S("top"), g2));

    parse_nonterm_t    *t2 = nt_named(g2, "top");
    parse_annotation_t *l  = t2 ? find_unique_rule_annot(g2, t2, PARSE_ANNOT_ASSIGNS)
                              : nullptr;

    check("and $name carries the name, not an index",
          l != nullptr && l->name_ref.kind == PARSE_ROLE_BY_INDEX
              && l->name_ref.index == 1
              && l->value_ref.kind == PARSE_ROLE_BY_NAME
              && core_string_eq(l->value_ref.name, S("t")));

    parse_annotation_t *copy = l ? parse_annot_clone(l) : nullptr;

    check("a clone owns its own copy of a named reference",
          copy && copy->value_ref.name.data != l->value_ref.name.data
              && core_string_eq(copy->value_ref.name, S("t")));

    parse_annot_free(copy);

    parse_grammar_free(g2);

    core_string_t unbalanced
        = S("<top> @disambig(pred(a) ::= <n>\n"
            "<n> ::= __LOWER\n");

    parse_grammar_t *bad = parse_grammar_new();

    check("an unbalanced argument region is not a grammar",
          !parse_bnf_load(unbalanced, S("top"), bad));

    parse_grammar_free(bad);
}

static void
test_annotation_arguments_carry_integers(void)
{
    core_string_t src
        = S("<top> ::= <mid>\n"
            "<mid> @disambig(pred, 3) ::= <n>\n"
            "<n> ::= __LOWER\n");

    parse_grammar_t *g = parse_grammar_new();

    check("a grammar with integer annotation arguments loads",
          parse_bnf_load(src, S("top"), g));

    parse_nonterm_t *mid = nt_named(g, "mid");

    parse_annotation_t *d = mid ? find_unique_rule_annot(g, mid, PARSE_ANNOT_DISAMBIG)
                              : nullptr;

    check("@disambig reads its predicate and its cost",
          d != nullptr && core_string_eq(d->disambig_predicate, S("pred"))
              && d->disambig_cost == 3);

    parse_grammar_free(g);

    static const core_string_t BAD[] = {
        S("<top> @disambig(pred, a) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @disambig(pred, 1, 2) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @capture(\"c\") ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @penalty(1) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @branch($1) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @assigns($1) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @scope($1) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @jump($1) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @adt(\"enum\", $1) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @type($1, int) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @type($1, \"int\") ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @declares($1, $1) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @declares($1, $1, \"var\") ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @field($1, $1) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @nospace($1, $2) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @declares(\"v\", $1) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @assigns($1, t) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @nospace(n) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @varref($) ::= <n>\n<n> ::= __LOWER\n"),
    };

    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        parse_grammar_t *bad = parse_grammar_new();

        if (parse_bnf_load(BAD[i], S("top"), bad)) {
            printf("FAIL  this should not have loaded: %.*s", SF(BAD[i]));
            fails++;
        }

        parse_grammar_free(bad);
    }
}

static void
test_annotation_argument_lists_are_growable(void)
{
    core_buffer_t src = {0};
    static const char prefix[] = "<top> @statement_end(\"end\"";
    core_buffer_append(&src, prefix, sizeof(prefix) - 1);
    for (size_t i = 1; i < 40; i++)
        core_buffer_append(&src, ", \"end\"", 7);
    static const char suffix[] = ") ::= <n>\n<n> ::= __LOWER\n";
    core_buffer_append(&src, suffix, sizeof(suffix) - 1);

    parse_grammar_t *g = parse_grammar_new();
    bool loaded = parse_bnf_load(
        core_string_borrow(src.data, src.byte_len), S("top"), g);
    parse_nonterm_t *top = loaded ? nt_named(g, "top") : nullptr;
    parse_annotation_t *annotation = top
        ? find_unique_rule_annot(g, top, PARSE_ANNOT_STATEMENT_END) : nullptr;

    check("annotation argument lists accept more than 32 entries",
          annotation && core_list_len(annotation->tags) == 40);

    parse_grammar_free(g);
    core_buffer_release(&src);
}

static void
test_direct_nonterminal_annotation_lookup(void)
{
    core_string_t G
        = S("<top> ::= <choice>\n"
            "<choice> @varref($1) ::= <name>\n"
            "<name> ::= __LOWER\n");
    parse_grammar_t *g = parse_grammar_new();
    bool loaded = parse_bnf_load(G, S("top"), g);
    parse_nonterm_t *choice = loaded ? nt_named(g, "choice") : nullptr;
    parse_rule_t *choice_rule = choice
        ? parse_get_rule(g, choice->rule_ids.data[0]) : nullptr;
    check("an exact production query finds its annotation",
          choice_rule
              && parse_rule_find_annot(g, choice_rule, PARSE_ANNOT_VARREF));
    check("a direct nonterminal query does not inherit a production annotation",
          choice && !parse_nt_find_direct_annot(choice, PARSE_ANNOT_VARREF));
    parse_grammar_free(g);
}

static void
test_the_binding_annotations(void)
{
    core_string_t G
        = S("<top> @scope(\"module\") ::= <d> <p>\n"
            "<d> @declares($1,\"var\") "
            "@namespace(\"tag\") ::= \"v\" <n> \";\"\n"
            "<p> @declares($1,\"param\") @direction($1,\"out\") "
            "@pass_by(\"ref\") ::= \"p\" <n> \";\"\n"
            "<n> ::= __LOWER\n");

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    check("a grammar using the binding annotations loads",
          parse_bnf_load(G, S("top"), g));

    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t   *tree = parse_toy(g, "top", "va;pq;", &p, &ts);

    if (!tree) {
        check("the program parses", false);
        parse_grammar_free(g);

        return;
    }

    parse_annot_result_t *r = annotate_new(g, tree);

    parse_sym_entry_t *a = nullptr;
    parse_sym_entry_t *q = nullptr;

    for (parse_scope_t *s = r->symtab->all_scopes; s; s = s->all_next) {
        for (parse_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (e->name.data && core_string_eq(e->name, S("a"))) {
                a = e;
            }

            if (e->name.data && core_string_eq(e->name, S("q"))) {
                q = e;
            }
        }
    }

    check("@direction records the parameter's direction as a th_mode",
          q && q->has_mode && q->mode == TH_OUT);
    check("@direction preserves the source-level mode spelling",
          q && core_string_eq(q->direction, S("out")));
    check("@pass_by records a separate parameter passing strategy",
          q && core_string_eq(q->pass_by, S("ref")));
    check("@namespace put the declaration in the namespace it named",
          a && a->scope && a->scope->ns
              && a->scope->ns->ns_name.data
              && core_string_eq(a->scope->ns->ns_name, S("tag")));

    parse_annot_result_free(r);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_initial_symbol_walk_callback(parse_grammar_t *g)
{
    parse_pwz_parser_t *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "prog", "vaT;{vbU;}?xyztcV;",
                                   &p, &ts);
    check("initial-symbol sample parses", tree != nullptr);
    if (!tree) return;

    parse_annot_result_t *result = parse_annot_result_new(nullptr);
    node_probe_t probe = {0};
    parse_annot_result_set_node_callback(result, count_annot_nodes, &probe);
    parse_tree_walk_t *walk = parse_tree_walk_new(g, result);
    parse_annot_register(walk, result);
    parse_tree_walk_run(walk, tree);

    check("annotation walk visits each nonterminal on entry and exit",
          probe.enters > 0 && probe.enters == probe.exits);
    check("annotation walk creates its module scope",
          parse_tree_scope(tree) != nullptr);
    parse_tree_t *decl = find_node(g, tree, "decl");
    check("ordinary nodes retain their enclosing lexical scope",
          decl && parse_tree_scope(decl) == parse_tree_scope(tree));
    check("annotation walk records the declared name",
          parse_symtab_lookup_scoped(result->symtab,
              parse_tree_scope(tree), S(""), S("a")) != nullptr);
    parse_sym_entry_t *inferred = parse_symtab_lookup_scoped(
        result->symtab, parse_tree_scope(tree), S(""), S("a"));
    parse_sym_mark_type_inferred(inferred);
    check("inferred type state is stored on its symbol",
          inferred && inferred->type_is_inferred);
    check("symbol retains its declaration node",
          inferred && inferred->decl_node != nullptr);
    check("symbol accepts a definition site", parse_sym_add_definition(inferred, tree));
    check("duplicate definition sites are ignored", parse_sym_add_definition(inferred, tree));
    check("symbol accepts a use site", parse_sym_add_use(inferred, tree));
    check("duplicate use sites are ignored", parse_sym_add_use(inferred, tree));
    check("symbol definition and use sites are stored without duplicates",
          inferred && inferred->definition_count == 1
              && inferred->definition_nodes[0] == tree
              && inferred->use_count == 1 && inferred->use_nodes[0] == tree);
    check("a classified write can be removed from use sites",
          parse_sym_remove_use(inferred, tree)
              && inferred->use_count == 0 && inferred->uses == 0);

    parse_tree_walk_free(walk);
    parse_annot_result_free(result);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
}

static void
test_selected_annotations_per_node(parse_grammar_t *g)
{
    parse_pwz_parser_t *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "prog", "tbI;", &p, &ts);
    check("per-node annotation sample parses", tree != nullptr);
    if (!tree) return;

    parse_annot_result_t *result = parse_annot_result_new(nullptr);
    parse_tree_walk_t *walk = parse_tree_walk_new(g, result);
    parse_annot_register(walk, result);
    const parse_annot_kind_t kinds[] = {
        PARSE_ANNOT_SCOPE_OPEN,
        PARSE_ANNOT_DECLARES,
    };
    check("annotation subset is accepted",
          parse_walk_set_annotation_order(walk, kinds,
                                          sizeof(kinds) / sizeof(kinds[0])));
    parse_tree_walk_run(walk, tree);

    parse_scope_t *scope = parse_tree_scope(tree);
    parse_sym_entry_t *symbol = parse_symtab_lookup_scoped(
        result->symtab, scope, S(""), S("b"));
    check("selected declaration annotations bind the symbol",
          symbol != nullptr);
    check("per-node entry and exit preserve the module scope tree",
          scope && core_string_eq(scope->scope_tag, S("module"))
              && parse_symtab_current_scope(result->symtab, S(""))
                     == scope->parent);

    parse_tree_walk_free(walk);
    parse_annot_result_free(result);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
}

static void
test_symbol_records_declaration_definition_and_use_nodes(void)
{
    core_string_t G
        = S("@tokenizer(\"toy\")\n"
            "<top> @scope(\"module\") ::= <decl> <read>\n"
            "<decl> @declares($1, \"var\") @assigns($1, $3) ::= "
            "\"v\" <name> \"=\" <name> \";\"\n"
            "<read> ::= \"r\" <ref> \";\"\n"
            "<ref> @varref($0) ::= <name>\n"
            "<name> ::= __LOWER\n");
    parse_grammar_t *grammar = parse_grammar_new();
    parse_grammar_set_error_recovery(grammar, false);
    bool loaded = parse_bnf_load(G, S("top"), grammar);
    check("symbol site grammar loads", loaded);
    if (!loaded) {
        parse_grammar_free(grammar);
        return;
    }

    parse_pwz_parser_t *parser;
    parse_token_stream_t *tokens;
    parse_tree_t *tree = parse_toy(grammar, "top", "va=b;ra;",
                                   &parser, &tokens);
    check("symbol site sample parses", tree != nullptr);
    if (tree) {
        parse_annot_result_t *result = annotate_new(grammar, tree);
        parse_scope_t *scope = parse_tree_scope(tree);
        parse_sym_entry_t *symbol = parse_symtab_lookup_scoped(
            result->symtab, scope, S(""), S("a"));
        parse_tree_t *declaration = find_node(grammar, tree, "decl");
        parse_tree_t *use = find_node(grammar, tree, "ref");
        check("symbol retains its declaration node",
              symbol && symbol->decl_node == declaration);
        check("assignment annotation records the definition node",
              symbol && symbol->definition_count == 1
                  && symbol->definition_nodes[0] == declaration);
        check("variable-reference annotation records the use node",
              symbol && symbol->use_count == 1
                  && symbol->use_nodes[0] == use && symbol->uses == 1);
        parse_annot_result_free(result);
        parse_pwz_free(parser);
        scan_token_stream_free(tokens);
    }
    parse_grammar_free(grammar);
}

static parse_tree_t *
last_node_s(parse_grammar_t *g, parse_tree_t *node, core_string_t name)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    for (size_t i = core_tree_num_children(node); i > 0; i--) {
        parse_tree_t *found = last_node_s(g, core_tree_child(node, i - 1),
                                          name);
        if (found) return found;
    }
    return parse_tree_nt_name(g, node).data
        && core_string_eq(parse_tree_nt_name(g, node), name)
        ? node : nullptr;
}

#define last_node(g, node, name) last_node_s((g), (node), S(name))

typedef enum {
    INITIALIZER_TARGET_NONE,
    INITIALIZER_TARGET_SELF,
    INITIALIZER_TARGET_EARLIER,
} initializer_target_t;

static initializer_target_t
initializer_reference_target(core_string_t point)
{
    core_buffer_t text = {0};
    core_buffer_printf(&text,
        "@tokenizer(\"toy\")\n%.*s"
        "<top> @scope(\"module\") ::= <decl> <decl>\n"
        "<decl> @declares($1, \"var\") @assigns($1, $3) ::= "
        "\"v\" <name> \"=\" <ref> \";\"\n"
        "<ref> @varref($0) ::= <name>\n"
        "<name> ::= __LOWER\n", SF(point));
    parse_grammar_t *grammar = parse_grammar_new();
    parse_grammar_set_error_recovery(grammar, false);
    bool loaded = parse_bnf_load(core_string_borrow(text.data,
                                                    text.byte_len),
                                 S("top"), grammar);
    core_buffer_release(&text);
    parse_pwz_parser_t *parser = nullptr;
    parse_token_stream_t *tokens = nullptr;
    parse_tree_t *tree = loaded
        ? parse_toy(grammar, "top", "va=b;va=a;", &parser, &tokens)
        : nullptr;
    parse_annot_result_t *result = tree
        ? annotate_new(grammar, tree) : nullptr;
    parse_tree_t *declaration = tree ? last_node(grammar, tree, "decl")
                                     : nullptr;
    parse_tree_t *use = tree ? last_node(grammar, tree, "ref") : nullptr;
    parse_node_resolution_t resolution = use ? parse_tree_resolution(use)
        : (parse_node_resolution_t){0};
    parse_sym_entry_t *target = resolution.kind
            == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol : nullptr;
    bool self = target && target->decl_node == declaration;
    bool earlier = target && target->decl_node != declaration
        && target->decl_node;
    if (result) parse_annot_result_free(result);
    if (parser) parse_pwz_free(parser);
    if (tokens) scan_token_stream_free(tokens);
    parse_grammar_free(grammar);
    return self ? INITIALIZER_TARGET_SELF
        : earlier ? INITIALIZER_TARGET_EARLIER : INITIALIZER_TARGET_NONE;
}

static void
test_declaration_point_hides_a_binding_in_its_initializer(void)
{
    check("by default a binding is visible in its own initializer",
          initializer_reference_target(S("")) == INITIALIZER_TARGET_SELF);
    check("a declarator declaration point keeps the default",
          initializer_reference_target(
              S("@declaration_point(\"declarator\")\n"))
              == INITIALIZER_TARGET_SELF);
    check("after_initializer resolves the initializer to the earlier binding",
          initializer_reference_target(
              S("@declaration_point(\"after_initializer\")\n"))
              == INITIALIZER_TARGET_EARLIER);
}

static void
test_function_visibility_does_not_leak_to_parameters(void)
{
    core_string_t G =
        S("@tokenizer(\"toy\")\n"
          "<top> @scope(\"module\") ::= <func>\n"
          "<func> @scope(\"function\", $2) @declares($2, \"function\") ::= "
          "<func-prelude> <name> <param> \";\"\n"
          "<func-prelude> @visibility(\"public\") ::= \"P\"\n"
          "<param> @declares($1, \"param\") ::= <name>\n"
          "<name> ::= __LOWER\n");
    parse_grammar_t *grammar = parse_grammar_new();
    parse_grammar_set_error_recovery(grammar, false);
    bool loaded = parse_bnf_load(G, S("top"), grammar);
    parse_pwz_parser_t *parser = nullptr;
    parse_token_stream_t *tokens = nullptr;
    parse_tree_t *tree = loaded
        ? parse_toy(grammar, "top", "Pfx;", &parser, &tokens)
        : nullptr;
    parse_annot_result_t *result = tree
        ? annotate_new(grammar, tree) : nullptr;
    parse_tree_t *func_node = result ? find_node(grammar, tree, "func") : nullptr;
    parse_sym_entry_t *function = func_node
        ? parse_tree_sym(func_node) : nullptr;
    check("function-prelude visibility binds to the function declaration",
          loaded && tree && function
              && core_string_eq(function->visibility, S("public")));
    parse_annot_result_free(result);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(grammar);
}

static void
test_visibility_prefix_binds_to_non_function_declaration(void)
{
    core_string_t G =
        S("@tokenizer(\"toy\")\n"
          "<top> @scope(\"module\") ::= <trait>\n"
          "<trait> @scope(\"trait\", $2) @declares($2) ::= "
          "<visibility> <name> \";\"\n"
          "<visibility> @visibility(\"public\") ::= \"P\"\n"
          "<name> ::= __LOWER\n");
    parse_grammar_t *grammar = parse_grammar_new();
    parse_grammar_set_error_recovery(grammar, false);
    bool loaded = parse_bnf_load(G, S("top"), grammar);
    parse_pwz_parser_t *parser = nullptr;
    parse_token_stream_t *tokens = nullptr;
    parse_tree_t *tree = loaded
        ? parse_toy(grammar, "top", "Pf;", &parser, &tokens) : nullptr;
    parse_annot_result_t *result = tree
        ? annotate_new(grammar, tree) : nullptr;
    parse_tree_t *trait_node = tree ? find_node(grammar, tree, "trait") : nullptr;
    parse_sym_entry_t *trait = trait_node
        ? parse_tree_sym(trait_node) : nullptr;
    check("non-function visibility grammar loads", loaded);
    check("non-function visibility sample parses", tree != nullptr);
    check("non-function declaration symbol is attached", trait != nullptr);
    check("visibility prefix binds to a non-function declaration",
          trait && core_string_eq(trait->visibility, S("public")));
    parse_annot_result_free(result);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(grammar);
}

static void
test_field_visibility_in_non_type_tagged_scope(void)
{
    core_string_t G =
        S("@tokenizer(\"toy\")\n"
          "<top> @scope(\"module\") ::= <record>\n"
          "<record> @namespace(\"type\") @scope(\"record\", $1) "
          "@type($1) @exposes ::= "
          "<public> <name> \"{\" <field> \"}\"\n"
          "<field> @field($1) ::= <field-mod> <name>\n"
          "<field-mod> ::= <private>\n"
          "<public> @visibility(\"public\") ::= \"P\"\n"
          "<private> @visibility(\"private\") ::= \"p\"\n"
          "<name> ::= __LOWER\n");
    parse_grammar_t *grammar = parse_grammar_new();
    parse_grammar_set_error_recovery(grammar, false);
    bool loaded = parse_bnf_load(G, S("top"), grammar);
    parse_pwz_parser_t *parser = nullptr;
    parse_token_stream_t *tokens = nullptr;
    parse_tree_t *tree = loaded
        ? parse_toy(grammar, "top", "Pr{pf}", &parser, &tokens) : nullptr;
    parse_annot_result_t *result = tree
        ? annotate_new(grammar, tree) : nullptr;
    parse_scope_t *module_scope = tree
        ? parse_tree_scope(tree) : nullptr;
    parse_sym_entry_t *record = result && module_scope
        ? parse_symtab_lookup_scoped(result->symtab, module_scope, S("type"),
                                     S("r")) : nullptr;
    parse_sym_entry_t *field = record && record->member_scope
        ? parse_symtab_lookup_in(record->member_scope, S("f")) : nullptr;
    check("non-type-tagged member visibility grammar loads", loaded);
    check("non-type-tagged member visibility sample parses", tree != nullptr);
    check("record keeps its own visibility",
          record && core_string_eq(record->visibility, S("public")));
    check("field receives its own visibility",
          field && core_string_eq(field->visibility, S("private")));
    parse_annot_result_free(result);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(grammar);
}

static void
test_scoped_declaration_is_bound_outside_its_body(void)
{
    core_string_t G
        = S("@tokenizer(\"toy\")\n"
            "<top> @scope(\"module\") ::= <func>\n"
            "<func> @scope(\"function\", $1) @declares($1, \"function\") "
            "::= \"f\" <name>\n"
            "<name> ::= __LOWER\n");
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    check("scoped-declaration grammar loads", parse_bnf_load(G, S("top"), g));

    parse_pwz_parser_t *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "top", "fa", &p, &ts);
    check("scoped-declaration sample parses", tree != nullptr);
    if (!tree) {
        parse_grammar_free(g);
        return;
    }

    parse_annot_result_t *result = parse_annot_result_new(nullptr);
    parse_tree_walk_t *walk = parse_tree_walk_new(g, result);
    parse_annot_register(walk, result);
    parse_tree_walk_run(walk, tree);
    parse_tree_t *func = find_node(g, tree, "func");
    parse_sym_entry_t *symbol = func
        ? parse_tree_sym(func) : nullptr;

    check("function symbol is provisionally stored in its enclosing module",
          symbol && symbol->kind == PARSE_SYM_FUNCTION
              && symbol->scope == parse_tree_scope(tree));
    check("function body still has its own scope",
          func && parse_tree_scope(func)
              && parse_tree_scope(func) != parse_tree_scope(tree));

    parse_tree_walk_free(walk);
    parse_annot_result_free(result);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_part_two_rejections(void)
{
    static const core_string_t BAD[] = {
        S("<top> @namespace($1) ::= <n>\n<n> ::= __LOWER\n"),
        S("<top> @direction(\"out\") ::= <n>\n<n> ::= __LOWER\n"),
        S("@declaration_point(\"later\")\n<top> ::= <n>\n<n> ::= __LOWER\n"),
    };

    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        parse_grammar_t *bad = parse_grammar_new();

        if (parse_bnf_load(BAD[i], S("top"), bad)) {
            printf("FAIL  this should not have loaded: %.*s", SF(BAD[i]));
            fails++;
        }

        parse_grammar_free(bad);
    }
}

static void
test_pass_by_modes(void)
{
    static const core_string_t MODES[] = {
        S("in"), S("out"), S("inout"), S("ref"), S("consume")
    };
    bool all_load = true;
    for (size_t i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) {
        core_buffer_t grammar_text = {0};
        core_buffer_printf(&grammar_text,
            "<top> @tokenizer(\"toy\") @scope(\"module\") ::= <p>\n"
            "<p> @declares($1,\"param\") @pass_by(\"%.*s\") ::= \"v\" <n> \";\"\n"
            "<n> ::= __LOWER\n", SF(MODES[i]));
        parse_grammar_t *g = parse_grammar_new();
        parse_grammar_set_error_recovery(g, false);
        all_load = all_load && parse_bnf_load(
            core_string_borrow(grammar_text.data, grammar_text.byte_len),
            S("top"), g);
        parse_grammar_free(g);
        core_buffer_release(&grammar_text);
    }
    check("pass_by accepts all parameter passing modes", all_load);
}

int
main(void)
{
    parse_grammar_t *g = load_toy();

    if (!g) {
        return 1;
    }

    test_annotation_arguments_only_need_balanced_parens();
    test_annotation_arguments_carry_integers();
    test_direct_nonterminal_annotation_lookup();
    test_annotation_argument_lists_are_growable();
    test_the_binding_annotations();
    test_initial_symbol_walk_callback(g);
    test_symbol_records_declaration_definition_and_use_nodes();
    test_declaration_point_hides_a_binding_in_its_initializer();
    test_selected_annotations_per_node(g);
    test_function_visibility_does_not_leak_to_parameters();
    test_visibility_prefix_binds_to_non_function_declaration();
    test_field_visibility_in_non_type_tagged_scope();
    test_scoped_declaration_is_bound_outside_its_body();
    test_part_two_rejections();
    test_pass_by_modes();
    test_annotations_are_retained_on_the_grammar(g);
    test_annotation_cache_is_thread_safe(g);
    test_the_walk_builds_a_symbol_table(g);
    test_a_declaration_walk_is_reentered_by_the_full_walk(g);
    test_symtab_storage_facts();
    test_symbol_namespaces_share_lexical_scopes();
    test_symbol_namespace_scope_iteration();
    test_tree_resolution_is_scope_or_symbol_without_replacing_lexical_scope();
    test_a_deep_tree_does_not_overflow_the_stack(g);

    parse_grammar_free(g);

    return report("fe annot");
}
