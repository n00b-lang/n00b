#include <stdlib.h>
#include <assert.h>

#include "core.h"
#include "lib/alloc.h"
#include "lib/array.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "lib/option.h"
#include "lib/tree.h"
#include "parse/grammar.h"
#include "harness.h"

core_array_decl(int);
core_tree_decl(int, int);

typedef int *intp_t;

core_option_decl(intp_t);

typedef struct {
    int  a;
    char b[4];
} flex_t;

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static void test_the_list_macros_all_expand_and_work(void)
{
    core_list_t(int) l = core_list_new(int);

    check("a fresh list is empty", core_list_is_empty(l));

    core_list_push(l, 30);
    core_list_push(l, 10);
    core_list_insert(l, 1, 20);

    check("insert lands at the index", core_list_get(l, 1) == 20);
    check("and grows the length", core_list_len(l) == 3);
    check("capacity is at least the length", core_list_cap(l) >= 3);

    core_list_sort(l, cmp_int);

    check("sort orders the whole list",
          core_list_get(l, 0) == 10 && core_list_get(l, 1) == 20
              && core_list_get(l, 2) == 30);

    core_option_t(size_t) at = core_list_find(l, 20);

    check("find reports a hit", core_option_is_set(at));
    check("at the right index", core_option_get(at) == 1);
    check("and a miss for what is absent",
          !core_option_is_set(core_list_find(l, 99)));
    check("delete returns what it removed", core_list_delete(l, 1) == 20);
    check("and shortens the list", core_list_len(l) == 2);

    core_option_t(int) front = core_list_pop_front(int, l);

    check("pop_front takes the first", core_option_is_set(front)
                                          && core_option_get(front) == 10);

    core_list_t(int) other = core_list_new(int);

    core_list_push(other, 40);
    core_list_push(other, 50);
    core_list_insert_list(l, 0, other);

    check("insert_list splices every element", core_list_len(l) == 3);
    check("in order", core_list_get(l, 0) == 40 && core_list_get(l, 1) == 50);

    core_list_delete_range(l, 0, 2);

    check("delete_range removes the span", core_list_len(l) == 1);

    core_list_push(l, 30);
    core_list_remove_all(l, 30);

    check("remove_all removes every match", core_list_is_empty(l));

    core_list_push(l, 7);

    core_list_t(int) cloned = core_list_clone(l);

    check("a clone has the same contents",
          core_list_len(cloned) == 1 && core_list_get(cloned, 0) == 7);

    core_list_push(cloned, 8);

    check("and is independent of the original", core_list_len(l) == 1);

    core_list_t(int) joined = core_list_concat(l, cloned);

    check("concat is both lengths", core_list_len(joined) == 3);

    core_list_free(joined);
    core_list_free(cloned);
    core_list_free(other);
    core_list_free(l);
}

static void test_the_array_macros_all_expand_and_work(void)
{
    core_list_t(int) l = core_list_new(int);

    core_list_push(l, 11);
    core_list_push(l, 22);

    core_array_t(int) a = core_list_to_array(int, l);

    check("to_array keeps the length", core_array_len(a) == 2);
    check("and the elements", core_array_get(a, 0) == 11);
    check("capacity is at least the length", core_array_cap(a) >= 2);

    int sum = 0;

    core_array_foreach(a, it) {
        sum += *it;
    }

    check("foreach visits every element", sum == 33);
    core_list_free(l);
}

static void test_the_option_macros_all_expand_and_work(void)
{
    core_option_t(int) some = core_option_set(int, 7);
    core_option_t(int) none = core_option_none(int);

    check("match takes the set branch", core_option_match(some, 1, 2) == 1);
    check("and the none branch", core_option_match(none, 1, 2) == 2);
    check("get_or_else yields the value", core_option_get_or_else(some, 5) == 7);
    check("and the fallback when unset",
          core_option_get_or_else(none, 5) == 5);

    int v = 3;

    check("from_nullable is set for a pointer",
          core_option_is_set(core_option_from_nullable(intp_t, &v)));
    check("and unset for null",
          !core_option_is_set(core_option_from_nullable(intp_t, nullptr)));
}

static void test_the_tree_macros_all_expand_and_work(void)
{
    core_tree_t(int, int) *root = core_tree_node(int, int, 1);

    check("a node is a node", core_tree_is_node(root));

    core_tree_t(int, int) *kid = core_tree_add_node(root, int, int, 2);

    core_tree_add_leaf(root, int, int, 3);

    check("children accumulate", core_tree_num_children(root) == 2);

    int seen = 0;

    core_tree_foreach_child(root, ch) {
        seen += core_tree_is_node(ch) ? 1 : 0;
    }

    check("foreach_child visits each one", seen == 1);

    core_tree_t(int, int) *spare = core_tree_node(int, int, 4);

    check("insert_child_at places a child",
          core_tree_insert_child_at(root, 0, spare));
    check("and counts it", core_tree_num_children(root) == 3);
    check("remove_child drops one", core_tree_remove_child(root, 0));
    check("and counts down", core_tree_num_children(root) == 2);

    core_tree_replace_children(root, nullptr, 0);

    check("replace_children can empty a node",
          core_tree_num_children(root) == 0);

    (void)kid;
}

static void test_the_grammar_macros_all_expand(void)
{
    parse_grammar_t *g  = parse_grammar_new();
    parse_nonterm_t *s  = parse_nonterm(g, CORE_STRING_STATIC("s"));
    parse_match_t    ch = PARSE_CHAR('x');

    parse_match_t opt = parse_optional(g, ch);
    parse_match_t st  = parse_star(g, ch);
    parse_match_t rp  = parse_repeat(g, 1, 3, ch);

    check("optional, star and repeat all build a group",
          opt.kind == PARSE_MATCH_GROUP && st.kind == PARSE_MATCH_GROUP
              && rp.kind == PARSE_MATCH_GROUP);
    check("a rule can be added with an explicit cost",
          parse_add_rule_with_cost(g, s, 5, ch) != nullptr);

    parse_grammar_free(g);
}

static void test_the_remaining_macros_expand(void)
{
    check("min picks the smaller", core_min(1, 2) == 1);
    check("likely and unlikely pass the value through",
          core_likely(1) && !core_unlikely(0));

    flex_t *fx = core_alloc_flex(flex_t, char, 8);

    check("alloc_flex returns storage", fx != nullptr);

    core_dict_t d;

    core_dict_init(&d, nullptr, nullptr);

    check("replace on an absent key reports nothing to replace",
          !core_dict_replace(&d, (void *)1, (void *)2));

    core_dict_put(&d, 1, 2);

    check("and replaces one that is present",
          core_dict_replace(&d, (void *)1, (void *)3));

    core_dict_free(&d);
    core_free(fx);
}

int main(void)
{
    test_the_list_macros_all_expand_and_work();
    test_the_array_macros_all_expand_and_work();
    test_the_option_macros_all_expand_and_work();
    test_the_tree_macros_all_expand_and_work();
    test_the_grammar_macros_all_expand();
    test_the_remaining_macros_expand();

    return report("lib macros");
}
