#include "test_unicode_helpers.h"
#include "logic/logic_dsl.h"

// ============================================================================
// Lexer tests (via compile — we test indirectly since lexer is internal)
// ============================================================================

TEST(test_lex_basic)
{
    // Compiling a fact should succeed.
    n00b_dsl_result_t r = n00b_dsl_compile(n00b_string_from_cstr("edge(a, b)."));
    ASSERT(r.error == nullptr);
    ASSERT(r.prog != nullptr);
    n00b_dsl_result_free(&r);
}

TEST(test_lex_operators)
{
    // Test each operator individually (not combined, since some are contradictory).
    n00b_dsl_result_t r;

    r = n00b_dsl_compile(n00b_string_from_cstr("var x in 1..10.\nvar y in 1..10.\nx != y.\n"));
    ASSERT(r.error == nullptr);
    n00b_dsl_result_free(&r);

    r = n00b_dsl_compile(n00b_string_from_cstr("var x in 1..10.\nvar y in 1..10.\nx < y.\n"));
    ASSERT(r.error == nullptr);
    n00b_dsl_result_free(&r);

    r = n00b_dsl_compile(n00b_string_from_cstr("var x in 1..10.\nvar y in 1..10.\nx <= y.\n"));
    ASSERT(r.error == nullptr);
    n00b_dsl_result_free(&r);

    r = n00b_dsl_compile(n00b_string_from_cstr("var x in 1..10.\nvar y in 1..10.\nx > y.\n"));
    ASSERT(r.error == nullptr);
    n00b_dsl_result_free(&r);

    r = n00b_dsl_compile(n00b_string_from_cstr("var x in 1..10.\nvar y in 1..10.\nx >= y.\n"));
    ASSERT(r.error == nullptr);
    n00b_dsl_result_free(&r);

    r = n00b_dsl_compile(n00b_string_from_cstr("var x in 1..10.\nvar y in 1..10.\nx == y.\n"));
    ASSERT(r.error == nullptr);
    n00b_dsl_result_free(&r);
}

TEST(test_lex_dotdot)
{
    n00b_dsl_result_t r = n00b_dsl_compile(n00b_string_from_cstr("var x in 1..3."));
    ASSERT(r.error == nullptr);
    n00b_dsl_result_free(&r);
}

TEST(test_lex_implies_query)
{
    n00b_dsl_result_t r = n00b_dsl_compile(n00b_string_from_cstr(
        "edge(a, b).\n"
        "path(X, Y) :- edge(X, Y).\n"
        "?- path(a, b).\n"));
    ASSERT(r.error == nullptr);
    n00b_dsl_result_free(&r);
}

// ============================================================================
// Parser tests
// ============================================================================

TEST(test_parse_fact)
{
    n00b_dsl_result_t r = n00b_dsl_compile(n00b_string_from_cstr("edge(a, b)."));
    ASSERT(r.error == nullptr);
    ASSERT(r.prog != nullptr);
    // Verify the fact was added: edge relation should exist.
    n00b_dl_rel_id_t edge = n00b_logic_relation(r.prog, r"edge", 2);
    ASSERT_EQ((int64_t)n00b_logic_count(r.prog, edge), 1);
    n00b_dsl_result_free(&r);
}

TEST(test_parse_rule)
{
    n00b_dsl_result_t r = n00b_dsl_compile(n00b_string_from_cstr(
        "edge(a, b).\n"
        "edge(b, c).\n"
        "path(X, Y) :- edge(X, Y).\n"
        "path(X, Y) :- path(X, Z), edge(Z, Y).\n"));
    ASSERT(r.error == nullptr);
    // path should have 3 tuples: a->b, b->c, a->c.
    n00b_dl_rel_id_t path = n00b_logic_relation(r.prog, r"path", 2);
    ASSERT_EQ((int64_t)n00b_logic_count(r.prog, path), 3);
    n00b_dsl_result_free(&r);
}

TEST(test_parse_var_decl)
{
    n00b_dsl_result_t r = n00b_dsl_compile(n00b_string_from_cstr("var x in 1..3."));
    ASSERT(r.error == nullptr);
    // Should have a CSP variable 'x'.
    ASSERT(r.prog->store != nullptr);
    n00b_option_t(n00b_csp_var_id_t) opt =
        n00b_csp_find_var(r.prog->store, r"x");
    ASSERT(n00b_option_is_set(opt));
    n00b_dsl_result_free(&r);
}

TEST(test_parse_cvar_decl)
{
    n00b_dsl_result_t r = n00b_dsl_compile(n00b_string_from_cstr(
        "edge(a, b).\n"
        "edge(b, c).\n"
        "color(Node) in 1..3 :- edge(Node, _).\n"));
    ASSERT(r.error == nullptr);
    ASSERT(r.prog->store != nullptr);
    // Should have CSP vars for 'a' and 'b' (from col 0 of edge).
    n00b_option_t(n00b_csp_var_id_t) va =
        n00b_csp_find_var(r.prog->store, r"a");
    n00b_option_t(n00b_csp_var_id_t) vb =
        n00b_csp_find_var(r.prog->store, r"b");
    ASSERT(n00b_option_is_set(va));
    ASSERT(n00b_option_is_set(vb));
    n00b_dsl_result_free(&r);
}

TEST(test_parse_constraint)
{
    n00b_dsl_result_t r = n00b_dsl_compile(n00b_string_from_cstr(
        "edge(a, b).\n"
        "edge(b, c).\n"
        "edge(a, c).\n"
        "color(Node) in 1..3 :- edge(Node, _).\n"
        "color(Node) in 1..3 :- edge(_, Node).\n"
        "color(X) != color(Y) :- edge(X, Y).\n"));
    ASSERT(r.error == nullptr);
    n00b_dsl_result_free(&r);
}

TEST(test_parse_query)
{
    n00b_dsl_result_t r = n00b_dsl_compile(n00b_string_from_cstr(
        "edge(a, b).\n"
        "?- edge(a, b).\n"));
    ASSERT(r.error == nullptr);
    n00b_dsl_result_free(&r);
}

TEST(test_parse_domain_set)
{
    n00b_dsl_result_t r = n00b_dsl_compile(n00b_string_from_cstr("var x in {1, 3, 5}."));
    ASSERT(r.error == nullptr);
    ASSERT(r.prog->store != nullptr);

    n00b_option_t(n00b_csp_var_id_t) opt =
        n00b_csp_find_var(r.prog->store, r"x");
    ASSERT(n00b_option_is_set(opt));

    n00b_csp_var_id_t x = n00b_option_get(opt);
    const n00b_csp_domain_t *d = n00b_result_get(
        n00b_csp_var_domain(r.prog->store, x));
    ASSERT_EQ(n00b_csp_dom_size(d), 3);
    ASSERT(n00b_csp_dom_contains(d, 1));
    ASSERT(n00b_csp_dom_contains(d, 3));
    ASSERT(n00b_csp_dom_contains(d, 5));

    n00b_dsl_result_free(&r);
}

// ============================================================================
// Compile/execute tests
// ============================================================================

TEST(test_compile_facts)
{
    n00b_dsl_result_t r = n00b_dsl_compile(n00b_string_from_cstr(
        "edge(a, b).\n"
        "edge(b, c).\n"
        "edge(a, c).\n"));
    ASSERT(r.error == nullptr);
    n00b_dl_rel_id_t edge = n00b_logic_relation(r.prog, r"edge", 2);
    ASSERT_EQ((int64_t)n00b_logic_count(r.prog, edge), 3);
    n00b_dsl_result_free(&r);
}

TEST(test_compile_transitive)
{
    n00b_dsl_result_t r = n00b_dsl_compile(n00b_string_from_cstr(
        "edge(a, b).\n"
        "edge(b, c).\n"
        "edge(c, d).\n"
        "path(X, Y) :- edge(X, Y).\n"
        "path(X, Y) :- path(X, Z), edge(Z, Y).\n"));
    ASSERT(r.error == nullptr);
    n00b_dl_rel_id_t path = n00b_logic_relation(r.prog, r"path", 2);
    // path: a->b, b->c, c->d, a->c, a->d, b->d = 6
    ASSERT_EQ((int64_t)n00b_logic_count(r.prog, path), 6);
    n00b_dsl_result_free(&r);
}

TEST(test_graph_coloring)
{
    n00b_dsl_result_t r = n00b_dsl_run(
        n00b_string_from_cstr(
            "edge(a, b).\n"
            "edge(b, c).\n"
            "edge(a, c).\n"
            "color(Node) in 1..3 :- edge(Node, _).\n"
            "color(Node) in 1..3 :- edge(_, Node).\n"
            "color(X) != color(Y) :- edge(X, Y).\n"
            "solve.\n"),
        nullptr, nullptr);
    ASSERT(r.error == nullptr);
    ASSERT(r.solved);

    // Verify the coloring is valid.
    n00b_option_t(n00b_csp_var_id_t) va =
        n00b_csp_find_var(r.prog->store, r"a");
    n00b_option_t(n00b_csp_var_id_t) vb =
        n00b_csp_find_var(r.prog->store, r"b");
    n00b_option_t(n00b_csp_var_id_t) vc =
        n00b_csp_find_var(r.prog->store, r"c");

    ASSERT(n00b_option_is_set(va));
    ASSERT(n00b_option_is_set(vb));
    ASSERT(n00b_option_is_set(vc));

    int64_t ca = n00b_result_get(
        n00b_csp_var_value(r.prog->store, n00b_option_get(va)));
    int64_t cb = n00b_result_get(
        n00b_csp_var_value(r.prog->store, n00b_option_get(vb)));
    int64_t cc = n00b_result_get(
        n00b_csp_var_value(r.prog->store, n00b_option_get(vc)));

    ASSERT(ca != cb);
    ASSERT(cb != cc);
    ASSERT(ca != cc);

    n00b_dsl_result_free(&r);
}

typedef struct {
    int64_t count;
} dsl_count_ctx_t;

static bool
dsl_count_cb(n00b_logic_t *prog, void *ctx)
{
    (void)prog;
    dsl_count_ctx_t *c = (dsl_count_ctx_t *)ctx;
    c->count++;
    return true;
}

TEST(test_solve_all)
{
    dsl_count_ctx_t ctx = { .count = 0 };
    n00b_dsl_result_t r = n00b_dsl_run(
        n00b_string_from_cstr(
            "edge(a, b).\n"
            "edge(b, c).\n"
            "edge(a, c).\n"
            "color(Node) in 1..3 :- edge(Node, _).\n"
            "color(Node) in 1..3 :- edge(_, Node).\n"
            "color(X) != color(Y) :- edge(X, Y).\n"
            "solve all.\n"),
        dsl_count_cb, &ctx);
    ASSERT(r.error == nullptr);
    // Triangle with 3 colors: 3! = 6 valid colorings.
    ASSERT_EQ(r.solution_count, 6);

    n00b_dsl_result_free(&r);
}

TEST(test_error_line)
{
    n00b_dsl_result_t r = n00b_dsl_compile(n00b_string_from_cstr("edge(a, b)\n@@@\n"));
    ASSERT(r.error != nullptr);
    // Error should be on line 1 (missing dot) or line 2 (bad token).
    ASSERT(r.error_line >= 1);
    n00b_dsl_result_free(&r);
}

TEST(test_empty_program)
{
    n00b_dsl_result_t r = n00b_dsl_compile(n00b_string_from_cstr(""));
    ASSERT(r.error == nullptr);
    ASSERT(r.prog != nullptr);
    n00b_dsl_result_free(&r);
}

// ============================================================================
// Alldiff DSL tests
// ============================================================================

TEST(test_alldiff_keyword)
{
    n00b_dsl_result_t r = n00b_dsl_run(
        n00b_string_from_cstr(
            "var x in 1..3.\n"
            "var y in 1..3.\n"
            "var z in 1..3.\n"
            "alldiff(x, y, z).\n"
            "solve.\n"),
        nullptr, nullptr);
    ASSERT(r.error == nullptr);
    ASSERT(r.solved);

    int64_t vx = n00b_result_get(n00b_logic_get_int(r.prog, r"x"));
    int64_t vy = n00b_result_get(n00b_logic_get_int(r.prog, r"y"));
    int64_t vz = n00b_result_get(n00b_logic_get_int(r.prog, r"z"));
    ASSERT(vx != vy);
    ASSERT(vy != vz);
    ASSERT(vx != vz);

    n00b_dsl_result_free(&r);
}

TEST(test_alldiff_solve_all_dsl)
{
    n00b_dsl_result_t r = n00b_dsl_run(
        n00b_string_from_cstr(
            "var x in 1..3.\n"
            "var y in 1..3.\n"
            "var z in 1..3.\n"
            "alldiff(x, y, z).\n"
            "solve all.\n"),
        nullptr, nullptr);
    ASSERT(r.error == nullptr);
    ASSERT_EQ(r.solution_count, 6);

    n00b_dsl_result_free(&r);
}

TEST(test_alldiff_infeasible_dsl)
{
    // 4 vars in domain 1..3 with alldiff => impossible.
    n00b_dsl_result_t r = n00b_dsl_run(
        n00b_string_from_cstr(
            "var a in 1..3.\n"
            "var b in 1..3.\n"
            "var c in 1..3.\n"
            "var d in 1..3.\n"
            "alldiff(a, b, c, d).\n"
            "solve.\n"),
        nullptr, nullptr);
    // Should fail during propagation or labeling.
    ASSERT(r.error != nullptr || !r.solved);

    n00b_dsl_result_free(&r);
}

// ============================================================================
// Chained != tests
// ============================================================================

TEST(test_chained_ne)
{
    n00b_dsl_result_t r = n00b_dsl_run(
        n00b_string_from_cstr(
            "var x in 1..3.\n"
            "var y in 1..3.\n"
            "var z in 1..3.\n"
            "x != y != z.\n"
            "solve.\n"),
        nullptr, nullptr);
    ASSERT(r.error == nullptr);
    ASSERT(r.solved);

    int64_t vx = n00b_result_get(n00b_logic_get_int(r.prog, r"x"));
    int64_t vy = n00b_result_get(n00b_logic_get_int(r.prog, r"y"));
    int64_t vz = n00b_result_get(n00b_logic_get_int(r.prog, r"z"));
    ASSERT(vx != vy);
    ASSERT(vy != vz);
    ASSERT(vx != vz);

    n00b_dsl_result_free(&r);
}

TEST(test_chained_ne_atoms)
{
    n00b_dsl_result_t r = n00b_dsl_run(
        n00b_string_from_cstr(
            "edge(a, b).\n"
            "edge(b, c).\n"
            "edge(a, c).\n"
            "color(Node) in 1..3 :- edge(Node, _).\n"
            "color(Node) in 1..3 :- edge(_, Node).\n"
            "color(X) != color(Y) != color(Z) :- edge(X, Y), edge(Y, Z).\n"
            "solve.\n"),
        nullptr, nullptr);
    ASSERT(r.error == nullptr);
    ASSERT(r.solved);

    int64_t va = n00b_result_get(n00b_logic_get_int(r.prog, r"a"));
    int64_t vb = n00b_result_get(n00b_logic_get_int(r.prog, r"b"));
    int64_t vc = n00b_result_get(n00b_logic_get_int(r.prog, r"c"));
    ASSERT(va != vb);
    ASSERT(vb != vc);
    ASSERT(va != vc);

    n00b_dsl_result_free(&r);
}

TEST(test_chained_ne_four_vars)
{
    // Four variables with chained !=, domain 1..4 => alldiff.
    n00b_dsl_result_t r = n00b_dsl_run(
        n00b_string_from_cstr(
            "var a in 1..4.\n"
            "var b in 1..4.\n"
            "var c in 1..4.\n"
            "var d in 1..4.\n"
            "a != b != c != d.\n"
            "solve all.\n"),
        nullptr, nullptr);
    ASSERT(r.error == nullptr);
    // 4! = 24 permutations.
    ASSERT_EQ(r.solution_count, 24);

    n00b_dsl_result_free(&r);
}

// ============================================================================
// Linear DSL tests
// ============================================================================

TEST(test_linear_dsl_basic)
{
    // 2*x + 3*y == 12 with domains 0..10.
    n00b_dsl_result_t r = n00b_dsl_run(
        n00b_string_from_cstr(
            "var x in 0..10.\n"
            "var y in 0..10.\n"
            "2*x + 3*y == 12.\n"
            "solve.\n"),
        nullptr, nullptr);
    ASSERT(r.error == nullptr);
    ASSERT(r.solved);

    int64_t vx = n00b_result_get(n00b_logic_get_int(r.prog, r"x"));
    int64_t vy = n00b_result_get(n00b_logic_get_int(r.prog, r"y"));
    ASSERT_EQ(2 * vx + 3 * vy, 12);

    n00b_dsl_result_free(&r);
}

TEST(test_linear_dsl_negative_coeffs)
{
    // x - y == 0 means x == y.
    n00b_dsl_result_t r = n00b_dsl_run(
        n00b_string_from_cstr(
            "var x in 1..5.\n"
            "var y in 1..5.\n"
            "x - y == 0.\n"
            "solve.\n"),
        nullptr, nullptr);
    ASSERT(r.error == nullptr);
    ASSERT(r.solved);

    int64_t vx = n00b_result_get(n00b_logic_get_int(r.prog, r"x"));
    int64_t vy = n00b_result_get(n00b_logic_get_int(r.prog, r"y"));
    ASSERT_EQ(vx, vy);

    n00b_dsl_result_free(&r);
}

TEST(test_linear_implicit_coeff)
{
    // x + y == 7 (implicit coefficient 1).
    n00b_dsl_result_t r = n00b_dsl_run(
        n00b_string_from_cstr(
            "var x in 1..6.\n"
            "var y in 1..6.\n"
            "x + y == 7.\n"
            "solve.\n"),
        nullptr, nullptr);
    ASSERT(r.error == nullptr);
    ASSERT(r.solved);

    int64_t vx = n00b_result_get(n00b_logic_get_int(r.prog, r"x"));
    int64_t vy = n00b_result_get(n00b_logic_get_int(r.prog, r"y"));
    ASSERT_EQ(vx + vy, 7);

    n00b_dsl_result_free(&r);
}

TEST(test_linear_plus_alldiff_dsl)
{
    // alldiff + sum == 6 with domain 1..3 => permutations of (1,2,3).
    n00b_dsl_result_t r = n00b_dsl_run(
        n00b_string_from_cstr(
            "var x in 1..3.\n"
            "var y in 1..3.\n"
            "var z in 1..3.\n"
            "alldiff(x, y, z).\n"
            "x + y + z == 6.\n"
            "solve all.\n"),
        nullptr, nullptr);
    ASSERT(r.error == nullptr);
    ASSERT_EQ(r.solution_count, 6);

    n00b_dsl_result_free(&r);
}

// ============================================================================
// Test runner
// ============================================================================

static void
run_tests(void)
{
    // Lexer
    RUN_TEST(test_lex_basic);
    RUN_TEST(test_lex_operators);
    RUN_TEST(test_lex_dotdot);
    RUN_TEST(test_lex_implies_query);

    // Parser
    RUN_TEST(test_parse_fact);
    RUN_TEST(test_parse_rule);
    RUN_TEST(test_parse_var_decl);
    RUN_TEST(test_parse_cvar_decl);
    RUN_TEST(test_parse_constraint);
    RUN_TEST(test_parse_query);
    RUN_TEST(test_parse_domain_set);

    // Compile/execute
    RUN_TEST(test_compile_facts);
    RUN_TEST(test_compile_transitive);
    RUN_TEST(test_graph_coloring);
    RUN_TEST(test_solve_all);
    RUN_TEST(test_error_line);
    RUN_TEST(test_empty_program);

    // Alldiff
    RUN_TEST(test_alldiff_keyword);
    RUN_TEST(test_alldiff_solve_all_dsl);
    RUN_TEST(test_alldiff_infeasible_dsl);

    // Chained !=
    RUN_TEST(test_chained_ne);
    RUN_TEST(test_chained_ne_atoms);
    RUN_TEST(test_chained_ne_four_vars);

    // Linear
    RUN_TEST(test_linear_dsl_basic);
    RUN_TEST(test_linear_dsl_negative_coeffs);
    RUN_TEST(test_linear_implicit_coeff);
    RUN_TEST(test_linear_plus_alldiff_dsl);
}

TEST_MAIN()
