#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "strings/string_ops.h"
#include "slay/grammar.h"
#include "internal/slay/grammar_internal.h"
#include "internal/slay/hashset.h"

// ============================================================================
// 1. Grammar construction
// ============================================================================

static void
test_grammar_new(void)
{
    n00b_grammar_t *g = n00b_grammar_new();
    assert(g != NULL);
    assert(!g->finalized);
    assert(g->error_rules);
    assert(g->max_penalty == N00B_DEFAULT_MAX_PENALTY);
    assert(g->nt_map != NULL);
    assert(g->terminal_map != NULL);
    n00b_grammar_free(g);
    printf("  [PASS] grammar_new\n");
}

// ============================================================================
// 2. Non-terminal creation
// ============================================================================

static void
test_nonterm_create(void)
{
    n00b_grammar_t *g  = n00b_grammar_new();
    n00b_nonterm_t *nt = n00b_nonterm(g, *r"expression");

    assert(nt != NULL);
    assert(n00b_unicode_str_eq(nt->name, *r"expression"));
    assert(nt->id >= 0);

    n00b_grammar_free(g);
    printf("  [PASS] nonterm_create\n");
}

// ============================================================================
// 3. Non-terminal duplicate returns same pointer
// ============================================================================

static void
test_nonterm_duplicate(void)
{
    n00b_grammar_t *g   = n00b_grammar_new();
    n00b_nonterm_t *nt1 = n00b_nonterm(g, *r"stmt");
    n00b_nonterm_t *nt2 = n00b_nonterm(g, *r"stmt");

    assert(nt1->id == nt2->id);
    assert(n00b_unicode_str_eq(nt1->name, nt2->name));

    n00b_grammar_free(g);
    printf("  [PASS] nonterm_duplicate\n");
}

// ============================================================================
// 4. Terminal registration (multi-char)
// ============================================================================

static void
test_terminal_register(void)
{
    n00b_grammar_t *g = n00b_grammar_new();

    int64_t id1 = n00b_register_terminal(g, *r"PLUS");
    int64_t id2 = n00b_register_terminal(g, *r"MINUS");
    int64_t id3 = n00b_register_terminal(g, *r"PLUS"); // duplicate

    assert(id1 != id2);
    assert(id1 == id3); // same name → same ID
    assert(n00b_token_id_is_fixed_text(id1));

    n00b_grammar_free(g);
    printf("  [PASS] terminal_register\n");
}

// ============================================================================
// 5. Single-char terminal → codepoint ID
// ============================================================================

static void
test_terminal_single_char(void)
{
    n00b_grammar_t *g = n00b_grammar_new();

    int64_t id = n00b_register_terminal(g, *r"+");
    // Single-char terminals now get hash-based IDs like all others.
    assert(n00b_token_id_is_fixed_text(id));

    int64_t id2 = n00b_register_terminal(g, *r"*");
    assert(n00b_token_id_is_fixed_text(id2));
    assert(id != id2);

    // Re-registering gives the same ID.
    int64_t id3 = n00b_register_terminal(g, *r"+");
    assert(id3 == id);

    n00b_grammar_free(g);
    printf("  [PASS] terminal_single_char\n");
}

// ============================================================================
// 6. Add rule
// ============================================================================

static void
test_add_rule(void)
{
    n00b_grammar_t *g    = n00b_grammar_new();
    n00b_nonterm_t *expr = n00b_nonterm(g, *r"expr");
    n00b_nonterm_t *term = n00b_nonterm(g, *r"term");
    int64_t plus_id      = n00b_register_terminal(g, *r"+");

    n00b_parse_rule_t *r = n00b_add_rule(g, expr,
                                          N00B_NT(term),
                                          N00B_TERMINAL(plus_id),
                                          N00B_NT(expr));
    assert(r != NULL);
    assert(r->nt_id == n00b_nonterm_id(expr));
    assert(r->contents.len == 3);
    assert(r->cost == 0);

    n00b_grammar_free(g);
    printf("  [PASS] add_rule\n");
}

// ============================================================================
// 7. Add rule with cost
// ============================================================================

static void
test_add_rule_with_cost(void)
{
    n00b_grammar_t *g    = n00b_grammar_new();
    n00b_nonterm_t *expr = n00b_nonterm(g, *r"expr");
    n00b_nonterm_t *term = n00b_nonterm(g, *r"term");

    n00b_parse_rule_t *r = n00b_add_rule_with_cost(g, expr, 5, N00B_NT(term));
    assert(r != NULL);
    assert(r->cost == 5);

    n00b_grammar_free(g);
    printf("  [PASS] add_rule_with_cost\n");
}

// ============================================================================
// 8. Duplicate rule dedup
// ============================================================================

static void
test_duplicate_rule(void)
{
    n00b_grammar_t *g    = n00b_grammar_new();
    n00b_nonterm_t *expr = n00b_nonterm(g, *r"expr");
    n00b_nonterm_t *term = n00b_nonterm(g, *r"term");

    n00b_parse_rule_t *r1 = n00b_add_rule(g, expr, N00B_NT(term));
    n00b_parse_rule_t *r2 = n00b_add_rule(g, expr, N00B_NT(term)); // duplicate

    // Second add should return the existing rule (dedup)
    assert(r1 != NULL);
    assert(r2 != NULL);
    // Both should reference the same storage position
    assert(r1 == r2);

    n00b_grammar_free(g);
    printf("  [PASS] duplicate_rule\n");
}

// ============================================================================
// 9. Group: optional (0..1)
// ============================================================================

static void
test_group_optional(void)
{
    n00b_grammar_t *g    = n00b_grammar_new();
    n00b_nonterm_t *item = n00b_nonterm(g, *r"item");

    n00b_match_t opt = n00b_optional(g, N00B_NT(item));
    assert(opt.kind == N00B_MATCH_GROUP);

    n00b_rule_group_t *grp = (n00b_rule_group_t *)opt.group;
    assert(grp->min == 0);
    assert(grp->max == 1);

    // Should have created a $$group_N non-terminal
    n00b_nonterm_t *group_nt = n00b_get_nonterm(g, grp->contents_id);
    assert(group_nt != NULL);
    assert(n00b_unicode_str_starts_with(group_nt->name, *r"$$group_"));

    n00b_grammar_free(g);
    printf("  [PASS] group_optional\n");
}

// ============================================================================
// 10. Group: star (0..∞)
// ============================================================================

static void
test_group_star(void)
{
    n00b_grammar_t *g    = n00b_grammar_new();
    n00b_nonterm_t *item = n00b_nonterm(g, *r"item");

    n00b_match_t star_m = n00b_star(g, N00B_NT(item));
    assert(star_m.kind == N00B_MATCH_GROUP);

    n00b_rule_group_t *grp = (n00b_rule_group_t *)star_m.group;
    assert(grp->min == 0);
    assert(grp->max == 0); // 0 = unlimited

    n00b_grammar_free(g);
    printf("  [PASS] group_star\n");
}

// ============================================================================
// 11. Group: plus (1..∞)
// ============================================================================

static void
test_group_plus(void)
{
    n00b_grammar_t *g    = n00b_grammar_new();
    n00b_nonterm_t *item = n00b_nonterm(g, *r"item");

    n00b_match_t plus_m = n00b_plus_group(g, N00B_NT(item));
    assert(plus_m.kind == N00B_MATCH_GROUP);

    n00b_rule_group_t *grp = (n00b_rule_group_t *)plus_m.group;
    assert(grp->min == 1);
    assert(grp->max == 0); // 0 = unlimited

    n00b_grammar_free(g);
    printf("  [PASS] group_plus\n");
}

// ============================================================================
// 12. Finalize
// ============================================================================

static void
test_finalize(void)
{
    n00b_grammar_t *g    = n00b_grammar_new();
    n00b_nonterm_t *expr = n00b_nonterm(g, *r"expr");
    n00b_nonterm_t *term = n00b_nonterm(g, *r"term");
    int64_t plus_id      = n00b_register_terminal(g, *r"+");
    int64_t num_id       = n00b_register_terminal(g, *r"NUM");

    // expr ::= term "+" expr | term
    n00b_add_rule(g, expr, N00B_NT(term), N00B_TERMINAL(plus_id), N00B_NT(expr));
    n00b_add_rule(g, expr, N00B_NT(term));

    // term ::= NUM
    n00b_add_rule(g, term, N00B_TERMINAL(num_id));

    n00b_grammar_set_start(g, expr);
    n00b_grammar_finalize(g);

    assert(g->finalized);

    // Re-resolve pointers after finalization (list may have reallocated)
    expr = n00b_get_nonterm(g, 0);
    term = n00b_get_nonterm(g, 1);

    // term is not nullable (only has terminal rules)
    assert(!term->nullable);
    // expr is not nullable (all rules start with term, which is not nullable)
    assert(!expr->nullable);

    n00b_grammar_free(g);
    printf("  [PASS] finalize\n");
}

// ============================================================================
// 13. FIRST set
// ============================================================================

static void
test_first_set(void)
{
    n00b_grammar_t *g    = n00b_grammar_new();
    n00b_nonterm_t *expr = n00b_nonterm(g, *r"expr");
    n00b_nonterm_t *term = n00b_nonterm(g, *r"term");
    int64_t num_id       = n00b_register_terminal(g, *r"NUM");
    int64_t plus_id      = n00b_register_terminal(g, *r"+");

    // expr ::= term "+" expr | term
    n00b_add_rule(g, expr, N00B_NT(term), N00B_TERMINAL(plus_id), N00B_NT(expr));
    n00b_add_rule(g, expr, N00B_NT(term));

    // term ::= NUM
    n00b_add_rule(g, term, N00B_TERMINAL(num_id));

    n00b_grammar_set_start(g, expr);
    n00b_grammar_finalize(g);

    // Re-resolve after finalization
    term = n00b_get_nonterm(g, 1);

    // term's FIRST set should contain NUM
    assert(term->first_set != NULL);
    // FIRST sets encode terminal IDs as (void *)(uintptr_t)((uint64_t)id + 0x100)
    void *encoded = (void *)(uintptr_t)((uint64_t)num_id + 0x100);
    assert(n00b_hashset_contains(term->first_set, encoded));

    n00b_grammar_free(g);
    printf("  [PASS] first_set\n");
}

// ============================================================================
// 14. Walk action
// ============================================================================

static void *
dummy_action(n00b_nt_node_t *node, void *children, void *thunk)
{
    (void)node;
    (void)children;
    (void)thunk;
    return NULL;
}

static void
test_walk_action(void)
{
    n00b_grammar_t *g  = n00b_grammar_new();
    n00b_nonterm_t *nt = n00b_nonterm(g, *r"stmt");

    n00b_nonterm_set_action(nt, dummy_action);

    // Re-resolve after potential realloc
    nt = n00b_get_nonterm(g, nt->id);
    assert(nt->action == dummy_action);

    n00b_grammar_free(g);
    printf("  [PASS] walk_action\n");
}

// ============================================================================
// 15. User data
// ============================================================================

static void
test_user_data(void)
{
    n00b_grammar_t *g  = n00b_grammar_new();
    n00b_nonterm_t *nt = n00b_nonterm(g, *r"stmt");

    int tag = 42;
    n00b_nonterm_set_user_data(nt, &tag);

    // Re-resolve after potential realloc
    nt = n00b_get_nonterm(g, nt->id);
    assert(n00b_nonterm_get_user_data(nt) == &tag);

    n00b_grammar_free(g);
    printf("  [PASS] user_data\n");
}

// ============================================================================
// 16. Annotation attachment
// ============================================================================

static void
test_annotation_attach(void)
{
    n00b_grammar_t *g  = n00b_grammar_new();
    n00b_nonterm_t *nt = n00b_nonterm(g, *r"block");

    assert(!nt->pending_annotations.data);

    // Stage an annotation on the NT.
    n00b_nt_scope_open(nt, *r"block_scope", N00B_CHILD_IX(0));

    nt = n00b_get_nonterm(g, nt->id);
    assert(nt->pending_annotations.data);
    assert(n00b_list_len(nt->pending_annotations) == 1);

    // Add a rule so finalize can flush the staged annotations.
    n00b_match_t empty = {.kind = N00B_MATCH_EMPTY};
    n00b_grammar_set_start(g, nt);
    n00b_parse_rule_t *rule = n00b_add_rule_v(g, nt->id, 1, &empty);
    assert(rule != NULL);

    // Re-resolve nt (list may have reallocated).
    nt = n00b_get_nonterm(g, nt->id);

    // Finalize distributes pending annotations to all rules.
    n00b_grammar_finalize(g);

    // Now the rule should carry the annotation.
    rule = n00b_get_rule(g, n00b_list_get(nt->rule_ids, 0));
    assert(rule->annotations.data);
    assert(n00b_list_len(rule->annotations) == 1);

    n00b_annotation_t *a = n00b_list_get(rule->annotations, 0);
    assert(a->kind == N00B_ANNOT_SCOPE_OPEN);
    assert(n00b_unicode_str_eq(a->scope_tag, *r"block_scope"));

    n00b_grammar_free(g);
    printf("  [PASS] annotation_attach\n");
}

// ============================================================================
// 17. Token list
// ============================================================================

static void
test_token_list(void)
{
    n00b_list_t(n00b_token_info_t) tl = n00b_list_new_cap(n00b_token_info_t, 8);

    n00b_token_info_t t1 = {0};
    t1.value = n00b_option_set(n00b_string_t, *r"foo");
    t1.tid   = 1;
    t1.line  = 1;

    n00b_token_info_t t2 = {0};
    t2.value = n00b_option_set(n00b_string_t, *r"bar");
    t2.tid   = 2;
    t2.line  = 1;

    n00b_list_push(tl, t1);
    n00b_list_push(tl, t2);

    assert(n00b_list_len(tl) == 2);

    n00b_token_info_t g0 = n00b_list_get(tl, 0);
    n00b_token_info_t g1 = n00b_list_get(tl, 1);

    assert(n00b_option_is_set(g0.value));
    assert(n00b_option_is_set(g1.value));
    assert(n00b_unicode_str_eq(n00b_option_get(g0.value), *r"foo"));
    assert(n00b_unicode_str_eq(n00b_option_get(g1.value), *r"bar"));
    assert(g0.tid == 1);
    assert(g1.tid == 2);

    // Build pointer view
    n00b_token_info_ptr_t *ptrs;
    int32_t n = n00b_token_list_build_ptrs(&tl, &ptrs);
    assert(n == 2);
    assert(ptrs[0] == &tl.data[0]);
    assert(ptrs[1] == &tl.data[1]);

    n00b_list_free(tl);
    printf("  [PASS] token_list\n");
}

// ============================================================================
// 18. Hashset basic operations
// ============================================================================

static void
test_hashset(void)
{
    n00b_hashset_t *s = n00b_hashset_new(16);
    assert(s != NULL);
    assert(s->len == 0);

    // Add items (encoded pointers to avoid NULL/tombstone)
    void *a = (void *)(uintptr_t)0x100;
    void *b = (void *)(uintptr_t)0x200;
    void *c = (void *)(uintptr_t)0x300;

    assert(n00b_hashset_add(s, a) == true);   // new
    assert(n00b_hashset_add(s, a) == false);  // duplicate
    assert(n00b_hashset_add(s, b) == true);
    assert(s->len == 2);

    assert(n00b_hashset_contains(s, a));
    assert(n00b_hashset_contains(s, b));
    assert(!n00b_hashset_contains(s, c));

    // Copy
    n00b_hashset_t *copy = n00b_hashset_copy(s);
    assert(copy->len == 2);
    assert(n00b_hashset_contains(copy, a));
    assert(n00b_hashset_contains(copy, b));

    // Union
    n00b_hashset_t *s2 = n00b_hashset_new(16);
    n00b_hashset_add(s2, c);
    n00b_hashset_t *u = n00b_hashset_union(s, s2);
    assert(u->len == 3);
    assert(n00b_hashset_contains(u, a));
    assert(n00b_hashset_contains(u, b));
    assert(n00b_hashset_contains(u, c));

    n00b_hashset_free(s);
    n00b_hashset_free(copy);
    n00b_hashset_free(s2);
    n00b_hashset_free(u);
    printf("  [PASS] hashset\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running grammar tests...\n");

    test_grammar_new();
    test_nonterm_create();
    test_nonterm_duplicate();
    test_terminal_register();
    test_terminal_single_char();
    test_add_rule();
    test_add_rule_with_cost();
    test_duplicate_rule();
    test_group_optional();
    test_group_star();
    test_group_plus();
    test_finalize();
    test_first_set();
    test_walk_action();
    test_user_data();
    test_annotation_attach();
    test_token_list();
    test_hashset();

    printf("All grammar tests passed.\n");
    n00b_shutdown();
    return 0;
}
