#include "test_unicode_helpers.h"
#include "logic/asp_engine.h"

// ============================================================================
// Helper: build a binary rule  head(X,Y) :- body(X,Y)
// ============================================================================

static n00b_dl_rule_t
make_rule_2(n00b_dl_engine_t *eng, n00b_dl_rel_id_t head_rel,
            n00b_dl_rel_id_t body_rel, n00b_dl_sym_t x, n00b_dl_sym_t y)
{
    n00b_dl_rule_builder_t rb;
    n00b_dl_rule_builder_init(&rb);
    n00b_dl_rule_builder_head(&rb, head_rel, 2, (n00b_dl_sym_t[]){x, y});
    n00b_dl_rule_builder_add(&rb, body_rel, 2, (n00b_dl_sym_t[]){x, y},
                               false);
    return n00b_dl_rule_builder_finish(&rb);
}

// ============================================================================
// Collect callback for query verification
// ============================================================================

typedef struct {
    n00b_dl_sym_t *tuples;
    int32_t        count;
    int32_t        cap;
    int32_t        arity;
} collect_ctx_t;

static bool
collect_cb(const n00b_dl_sym_t *tuple, int32_t arity, void *ctx)
{
    collect_ctx_t *c = (collect_ctx_t *)ctx;
    if (c->count >= c->cap) {
        return false;
    }
    memcpy(c->tuples + c->count * arity, tuple,
           (size_t)arity * sizeof(n00b_dl_sym_t));
    c->count++;
    c->arity = arity;
    return true;
}

// ============================================================================
// Tests
// ============================================================================

TEST(test_transitive_closure)
{
    n00b_dl_engine_t eng;
    n00b_dl_engine_init(&eng);

    n00b_dl_rel_id_t edge = n00b_dl_engine_relation(&eng, r"edge", 2);
    n00b_dl_rel_id_t path = n00b_dl_engine_relation(&eng, r"path", 2);

    n00b_dl_sym_t a = n00b_dl_const(&eng, r"a");
    n00b_dl_sym_t b = n00b_dl_const(&eng, r"b");
    n00b_dl_sym_t c = n00b_dl_const(&eng, r"c");
    n00b_dl_sym_t d = n00b_dl_const(&eng, r"d");

    n00b_dl_add_fact(&eng, edge, 2, (n00b_dl_sym_t[]){a, b});
    n00b_dl_add_fact(&eng, edge, 2, (n00b_dl_sym_t[]){b, c});
    n00b_dl_add_fact(&eng, edge, 2, (n00b_dl_sym_t[]){c, d});

    n00b_dl_sym_t X = n00b_dl_var(&eng, r"X");
    n00b_dl_sym_t Y = n00b_dl_var(&eng, r"Y");
    n00b_dl_sym_t Z = n00b_dl_var(&eng, r"Z");

    // path(X,Y) :- edge(X,Y)
    n00b_dl_add_rule(&eng, make_rule_2(&eng, path, edge, X, Y));

    // path(X,Y) :- path(X,Z), edge(Z,Y)
    {
        n00b_dl_rule_builder_t rb;
        n00b_dl_rule_builder_init(&rb);
        n00b_dl_rule_builder_head(&rb, path, 2, (n00b_dl_sym_t[]){X, Y});
        n00b_dl_rule_builder_add(&rb, path, 2, (n00b_dl_sym_t[]){X, Z},
                                   false);
        n00b_dl_rule_builder_add(&rb, edge, 2, (n00b_dl_sym_t[]){Z, Y},
                                   false);
        n00b_dl_add_rule(&eng, n00b_dl_rule_builder_finish(&rb));
    }

    ASSERT(n00b_dl_run(&eng));
    ASSERT_EQ((int64_t)n00b_dl_count(&eng, path), 6);
    ASSERT_EQ((int64_t)n00b_dl_count(&eng, edge), 3);

    n00b_dl_engine_free(&eng);
}

TEST(test_ancestors)
{
    n00b_dl_engine_t eng;
    n00b_dl_engine_init(&eng);

    n00b_dl_rel_id_t parent   = n00b_dl_engine_relation(&eng,
                                                          r"parent",
                                                          2);
    n00b_dl_rel_id_t ancestor = n00b_dl_engine_relation(&eng,
                                                          r"ancestor",
                                                          2);

    n00b_dl_sym_t tom = n00b_dl_const(&eng, r"tom");
    n00b_dl_sym_t bob = n00b_dl_const(&eng, r"bob");
    n00b_dl_sym_t ann = n00b_dl_const(&eng, r"ann");
    n00b_dl_sym_t sue = n00b_dl_const(&eng, r"sue");

    n00b_dl_add_fact(&eng, parent, 2, (n00b_dl_sym_t[]){tom, bob});
    n00b_dl_add_fact(&eng, parent, 2, (n00b_dl_sym_t[]){bob, ann});
    n00b_dl_add_fact(&eng, parent, 2, (n00b_dl_sym_t[]){ann, sue});

    n00b_dl_sym_t X = n00b_dl_var(&eng, r"X");
    n00b_dl_sym_t Y = n00b_dl_var(&eng, r"Y");
    n00b_dl_sym_t Z = n00b_dl_var(&eng, r"Z");

    // ancestor(X,Y) :- parent(X,Y)
    n00b_dl_add_rule(&eng, make_rule_2(&eng, ancestor, parent, X, Y));

    // ancestor(X,Y) :- parent(X,Z), ancestor(Z,Y)
    {
        n00b_dl_rule_builder_t rb;
        n00b_dl_rule_builder_init(&rb);
        n00b_dl_rule_builder_head(&rb, ancestor, 2,
                                    (n00b_dl_sym_t[]){X, Y});
        n00b_dl_rule_builder_add(&rb, parent, 2,
                                   (n00b_dl_sym_t[]){X, Z}, false);
        n00b_dl_rule_builder_add(&rb, ancestor, 2,
                                   (n00b_dl_sym_t[]){Z, Y}, false);
        n00b_dl_add_rule(&eng, n00b_dl_rule_builder_finish(&rb));
    }

    ASSERT(n00b_dl_run(&eng));
    ASSERT_EQ((int64_t)n00b_dl_count(&eng, ancestor), 6);

    n00b_dl_engine_free(&eng);
}

TEST(test_stratified_negation)
{
    n00b_dl_engine_t eng;
    n00b_dl_engine_init(&eng);

    n00b_dl_rel_id_t node      = n00b_dl_engine_relation(&eng,
                                                           r"node",
                                                           1);
    n00b_dl_rel_id_t connected = n00b_dl_engine_relation(&eng,
                                                           r"connected",
                                                           1);
    n00b_dl_rel_id_t isolated  = n00b_dl_engine_relation(&eng,
                                                           r"isolated",
                                                           1);

    n00b_dl_sym_t a = n00b_dl_const(&eng, r"a");
    n00b_dl_sym_t b = n00b_dl_const(&eng, r"b");
    n00b_dl_sym_t c = n00b_dl_const(&eng, r"c");

    n00b_dl_add_fact(&eng, node, 1, &a);
    n00b_dl_add_fact(&eng, node, 1, &b);
    n00b_dl_add_fact(&eng, node, 1, &c);
    n00b_dl_add_fact(&eng, connected, 1, &a);
    n00b_dl_add_fact(&eng, connected, 1, &b);

    // isolated(X) :- node(X), not connected(X)
    n00b_dl_sym_t X = n00b_dl_var(&eng, r"X");
    {
        n00b_dl_rule_builder_t rb;
        n00b_dl_rule_builder_init(&rb);
        n00b_dl_rule_builder_head(&rb, isolated, 1, &X);
        n00b_dl_rule_builder_add(&rb, node, 1, &X, false);
        n00b_dl_rule_builder_add(&rb, connected, 1, &X, true);
        n00b_dl_add_rule(&eng, n00b_dl_rule_builder_finish(&rb));
    }

    ASSERT(n00b_dl_run(&eng));
    ASSERT_EQ((int64_t)n00b_dl_count(&eng, isolated), 1);

    // Verify isolated node is "c"
    n00b_dl_sym_t *tuples = n00b_alloc_array(n00b_dl_sym_t, 16);
    collect_ctx_t  ctx    = {};
    ctx.tuples            = tuples;
    ctx.cap               = 16;
    n00b_dl_query(&eng, isolated, collect_cb, &ctx);
    ASSERT_EQ(ctx.count, 1);
    ASSERT(tuples[0] == c);

    n00b_free(tuples);
    n00b_dl_engine_free(&eng);
}

TEST(test_multi_stratum)
{
    n00b_dl_engine_t eng;
    n00b_dl_engine_init(&eng);

    n00b_dl_rel_id_t base_rel  = n00b_dl_engine_relation(&eng,
                                                           r"base",
                                                           1);
    n00b_dl_rel_id_t derived   = n00b_dl_engine_relation(&eng,
                                                           r"derived",
                                                           1);
    n00b_dl_rel_id_t excluded  = n00b_dl_engine_relation(&eng,
                                                           r"excluded",
                                                           1);
    n00b_dl_rel_id_t final_rel = n00b_dl_engine_relation(&eng,
                                                           r"final",
                                                           1);

    n00b_dl_sym_t v1 = n00b_dl_int(&eng, 1);
    n00b_dl_sym_t v2 = n00b_dl_int(&eng, 2);
    n00b_dl_sym_t v3 = n00b_dl_int(&eng, 3);

    n00b_dl_add_fact(&eng, base_rel, 1, &v1);
    n00b_dl_add_fact(&eng, base_rel, 1, &v2);
    n00b_dl_add_fact(&eng, base_rel, 1, &v3);

    n00b_dl_sym_t X = n00b_dl_var(&eng, r"X");

    // derived(X) :- base(X)
    {
        n00b_dl_rule_builder_t rb;
        n00b_dl_rule_builder_init(&rb);
        n00b_dl_rule_builder_head(&rb, derived, 1, &X);
        n00b_dl_rule_builder_add(&rb, base_rel, 1, &X, false);
        n00b_dl_add_rule(&eng, n00b_dl_rule_builder_finish(&rb));
    }

    // excluded(X) :- base(X), not derived(X)
    {
        n00b_dl_rule_builder_t rb;
        n00b_dl_rule_builder_init(&rb);
        n00b_dl_rule_builder_head(&rb, excluded, 1, &X);
        n00b_dl_rule_builder_add(&rb, base_rel, 1, &X, false);
        n00b_dl_rule_builder_add(&rb, derived, 1, &X, true);
        n00b_dl_add_rule(&eng, n00b_dl_rule_builder_finish(&rb));
    }

    // final(X) :- derived(X), not excluded(X)
    {
        n00b_dl_rule_builder_t rb;
        n00b_dl_rule_builder_init(&rb);
        n00b_dl_rule_builder_head(&rb, final_rel, 1, &X);
        n00b_dl_rule_builder_add(&rb, derived, 1, &X, false);
        n00b_dl_rule_builder_add(&rb, excluded, 1, &X, true);
        n00b_dl_add_rule(&eng, n00b_dl_rule_builder_finish(&rb));
    }

    ASSERT(n00b_dl_run(&eng));
    ASSERT_EQ((int64_t)n00b_dl_count(&eng, derived), 3);
    ASSERT_EQ((int64_t)n00b_dl_count(&eng, excluded), 0);
    ASSERT_EQ((int64_t)n00b_dl_count(&eng, final_rel), 3);

    n00b_dl_engine_free(&eng);
}

TEST(test_unstratifiable)
{
    n00b_dl_engine_t eng;
    n00b_dl_engine_init(&eng);

    n00b_dl_rel_id_t a_rel = n00b_dl_engine_relation(&eng, r"a", 1);
    n00b_dl_rel_id_t b_rel = n00b_dl_engine_relation(&eng, r"b", 1);

    n00b_dl_sym_t v1 = n00b_dl_const(&eng, r"v1");
    n00b_dl_add_fact(&eng, b_rel, 1, &v1);

    // a(X) :- b(X), not a(X)  — cyclic negation
    n00b_dl_sym_t X = n00b_dl_var(&eng, r"X");
    {
        n00b_dl_rule_builder_t rb;
        n00b_dl_rule_builder_init(&rb);
        n00b_dl_rule_builder_head(&rb, a_rel, 1, &X);
        n00b_dl_rule_builder_add(&rb, b_rel, 1, &X, false);
        n00b_dl_rule_builder_add(&rb, a_rel, 1, &X, true);
        n00b_dl_add_rule(&eng, n00b_dl_rule_builder_finish(&rb));
    }

    ASSERT(!n00b_dl_run(&eng));

    n00b_dl_engine_free(&eng);
}

TEST(test_facts_only)
{
    n00b_dl_engine_t eng;
    n00b_dl_engine_init(&eng);

    n00b_dl_rel_id_t fact_rel = n00b_dl_engine_relation(&eng,
                                                          r"fact", 3);

    n00b_dl_sym_t a = n00b_dl_const(&eng, r"a");
    n00b_dl_sym_t b = n00b_dl_const(&eng, r"b");
    n00b_dl_sym_t c = n00b_dl_const(&eng, r"c");

    n00b_dl_add_fact(&eng, fact_rel, 3, (n00b_dl_sym_t[]){a, b, c});
    n00b_dl_add_fact(&eng, fact_rel, 3, (n00b_dl_sym_t[]){b, c, a});

    ASSERT(n00b_dl_run(&eng));
    ASSERT_EQ((int64_t)n00b_dl_count(&eng, fact_rel), 2);

    n00b_dl_engine_free(&eng);
}

TEST(test_join)
{
    n00b_dl_engine_t eng;
    n00b_dl_engine_init(&eng);

    n00b_dl_rel_id_t parent  = n00b_dl_engine_relation(&eng,
                                                         r"parent",
                                                         2);
    n00b_dl_rel_id_t sibling = n00b_dl_engine_relation(&eng,
                                                         r"sibling",
                                                         2);

    n00b_dl_sym_t a = n00b_dl_const(&eng, r"a");
    n00b_dl_sym_t b = n00b_dl_const(&eng, r"b");
    n00b_dl_sym_t c = n00b_dl_const(&eng, r"c");
    n00b_dl_sym_t d = n00b_dl_const(&eng, r"d");

    n00b_dl_add_fact(&eng, parent, 2, (n00b_dl_sym_t[]){a, b});
    n00b_dl_add_fact(&eng, parent, 2, (n00b_dl_sym_t[]){a, c});
    n00b_dl_add_fact(&eng, parent, 2, (n00b_dl_sym_t[]){b, d});
    n00b_dl_add_fact(&eng, parent, 2, (n00b_dl_sym_t[]){c, d});

    // sibling(X,Y) :- parent(Z,X), parent(Z,Y)
    n00b_dl_sym_t X = n00b_dl_var(&eng, r"X");
    n00b_dl_sym_t Y = n00b_dl_var(&eng, r"Y");
    n00b_dl_sym_t Z = n00b_dl_var(&eng, r"Z");
    {
        n00b_dl_rule_builder_t rb;
        n00b_dl_rule_builder_init(&rb);
        n00b_dl_rule_builder_head(&rb, sibling, 2,
                                    (n00b_dl_sym_t[]){X, Y});
        n00b_dl_rule_builder_add(&rb, parent, 2,
                                   (n00b_dl_sym_t[]){Z, X}, false);
        n00b_dl_rule_builder_add(&rb, parent, 2,
                                   (n00b_dl_sym_t[]){Z, Y}, false);
        n00b_dl_add_rule(&eng, n00b_dl_rule_builder_finish(&rb));
    }

    ASSERT(n00b_dl_run(&eng));
    // parent a -> {b,c}: (b,b),(b,c),(c,b),(c,c)
    // parent b -> {d}: (d,d)
    // parent c -> {d}: (d,d) duplicate suppressed
    ASSERT_EQ((int64_t)n00b_dl_count(&eng, sibling), 5);

    n00b_dl_engine_free(&eng);
}

TEST(test_introspection)
{
    n00b_dl_engine_t eng;
    n00b_dl_engine_init(&eng);

    n00b_dl_rel_id_t r = n00b_dl_engine_relation(&eng, r"test_rel",
                                                    3);
    auto found = n00b_dl_find_relation(&eng, r"test_rel");
    ASSERT(n00b_option_is_set(found));
    ASSERT_EQ(n00b_option_get(found), r);

    auto not_found = n00b_dl_find_relation(&eng, r"nonexistent");
    ASSERT(!n00b_option_is_set(not_found));

    auto arity_opt = n00b_dl_relation_arity(&eng, r);
    ASSERT(n00b_option_is_set(arity_opt));
    ASSERT_EQ(n00b_option_get(arity_opt), 3);

    n00b_string_t *name = n00b_dl_relation_name(&eng, r);
    ASSERT(n00b_unicode_str_eq(name, r"test_rel"));

    ASSERT_EQ(n00b_dl_num_relations(&eng), 1);

    n00b_dl_engine_free(&eng);
}

TEST(test_sym_to_str)
{
    n00b_dl_engine_t eng;
    n00b_dl_engine_init(&eng);

    n00b_dl_sym_t a = n00b_dl_const(&eng, r"hello");
    n00b_dl_sym_t i = n00b_dl_int(&eng, 42);
    n00b_dl_sym_t v = n00b_dl_var(&eng, r"X");

    n00b_string_t *s1 = n00b_dl_sym_to_str(&eng, a);
    ASSERT(n00b_unicode_str_eq(s1, r"hello"));

    n00b_string_t *s2 = n00b_dl_sym_to_str(&eng, i);
    ASSERT(n00b_unicode_str_eq(s2, r"42"));

    n00b_string_t *s3 = n00b_dl_sym_to_str(&eng, v);
    ASSERT(n00b_unicode_str_eq(s3, r"X"));

    n00b_dl_engine_free(&eng);
}

TEST(test_count_by_name)
{
    n00b_dl_engine_t eng;
    n00b_dl_engine_init(&eng);

    n00b_dl_rel_id_t r = n00b_dl_engine_relation(&eng, r"items", 1);
    n00b_dl_sym_t    a = n00b_dl_const(&eng, r"a");
    n00b_dl_sym_t    b = n00b_dl_const(&eng, r"b");
    n00b_dl_add_fact(&eng, r, 1, &a);
    n00b_dl_add_fact(&eng, r, 1, &b);

    ASSERT(n00b_dl_run(&eng));
    ASSERT_EQ((int64_t)n00b_dl_count_by_name(&eng, r"items"), 2);
    ASSERT_EQ((int64_t)n00b_dl_count_by_name(&eng, r"nope"), 0);

    n00b_dl_engine_free(&eng);
}

// ============================================================================
// Test runner
// ============================================================================

static void
run_tests(void)
{
    RUN_TEST(test_transitive_closure);
    RUN_TEST(test_ancestors);
    RUN_TEST(test_facts_only);
    RUN_TEST(test_join);
    RUN_TEST(test_stratified_negation);
    RUN_TEST(test_multi_stratum);
    RUN_TEST(test_unstratifiable);
    RUN_TEST(test_introspection);
    RUN_TEST(test_sym_to_str);
    RUN_TEST(test_count_by_name);
}

TEST_MAIN()
