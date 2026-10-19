#include "parse/ambiguity.h"
#include "parse/annotation.h"
#include "private/parse/grammar.h"
#include "scanner/token_stream.h"

#include <assert.h>
#include <stdio.h>

#define S(x) CORE_STRING_STATIC(x)

static void
test_first_overlap(void)
{
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    parse_nonterm_t *s = parse_nonterm(g, CORE_STRING_STATIC("S"));

    parse_add_rule(g, s, PARSE_CHAR('a'), PARSE_CHAR('b'));
    parse_add_rule(g, s, PARSE_CHAR('a'), PARSE_CHAR('b'), PARSE_CHAR('c'));

    parse_ambiguity_options_t options = {
        .max_witness_tokens = 2,
        .max_findings = 1,
        .include_candidates = true,
    };
    parse_ambiguity_list_t findings
        = parse_grammar_audit_ambiguity(g, &options);
    assert(findings.len == 1);
    assert(findings.data[0].kind == PARSE_AMBIG_COMMON_PREFIX);
    assert(findings.data[0].witness_is_prefix);
    assert(core_string_eq(parse_ambiguity_kind_name(findings.data[0].kind),
                          S("common-prefix")));
    assert(core_string_eq(
        parse_ambiguity_severity_name(findings.data[0].severity),
        S("warning")));
    core_string_t json = parse_ambiguity_format_json(&findings);
    assert(json.data && json.data[0] == '['
           && core_string_find(json, S("common-prefix")) >= 0);
    core_free(json.data);
    assert(findings.data[0].witness.data);
    assert(core_string_find(findings.data[0].witness, S("<terminal:97>")) >= 0
           && core_string_find(findings.data[0].witness, S("<terminal:98>"))
                  >= 0);
    assert(findings.data[0].explanation.data);
    assert(findings.data[0].prescription.data);

    parse_ambiguity_list_free(&findings);
    parse_grammar_free(g);
}

static void
test_disjoint_alternatives(void)
{
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    parse_nonterm_t *s = parse_nonterm(g, CORE_STRING_STATIC("S"));

    parse_add_rule(g, s, PARSE_CHAR('a'));
    parse_add_rule(g, s, PARSE_CHAR('b'));

    parse_ambiguity_list_t findings = parse_grammar_audit_ambiguity(g, nullptr);
    assert(findings.len == 0);

    parse_ambiguity_list_free(&findings);
    parse_grammar_free(g);
}

static void
test_nullable_prefix(void)
{
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    parse_nonterm_t *s = parse_nonterm(g, CORE_STRING_STATIC("S"));
    parse_nonterm_t *n = parse_nonterm(g, CORE_STRING_STATIC("N"));

    parse_add_rule(g, n, PARSE_EPSILON());
    parse_add_rule(g, s, PARSE_NT(n), PARSE_CHAR('a'));
    parse_add_rule(g, s, PARSE_CHAR('a'));

    parse_ambiguity_list_t findings = parse_grammar_audit_ambiguity(g, nullptr);
    bool found = false;
    for (size_t i = 0; i < findings.len; i++) {
        found |= findings.data[i].kind == PARSE_AMBIG_CONFIRMED;
        if (findings.data[i].kind == PARSE_AMBIG_CONFIRMED) {
            assert(findings.data[i].witness.data);
            assert(!findings.data[i].witness_is_prefix);
            assert(findings.data[i].parse_tree_count > 1);
        }
    }
    assert(found);

    parse_ambiguity_list_free(&findings);
    parse_grammar_free(g);
}

static parse_token_stream_t *
ambiguous_witness_tokens(parse_grammar_t *grammar,
                         const parse_ambiguity_finding_t *finding,
                         void *user_data)
{
    (void)grammar;
    (void)finding;
    (void)user_data;
    return scan_token_stream_from_codepoints(CORE_STRING_STATIC("aaa"));
}

static void
test_dynamic_confirmation(void)
{
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    parse_nonterm_t *s = parse_nonterm(g, CORE_STRING_STATIC("S"));
    parse_add_rule(g, s, PARSE_NT(s), PARSE_NT(s));
    parse_add_rule(g, s, PARSE_CHAR('a'));

    parse_ambiguity_finding_t finding = {
        .kind = PARSE_AMBIG_FIRST_OVERLAP,
        .severity = PARSE_AMBIG_WARNING,
        .nonterminal = parse_nonterm_id(s),
        .rule_a = 0,
        .rule_b = 1,
    };

    assert(parse_ambiguity_confirm_finding(g,
                                           &finding,
                                           ambiguous_witness_tokens,
                                           nullptr));
    assert(finding.kind == PARSE_AMBIG_CONFIRMED);
    assert(finding.severity == PARSE_AMBIG_ERROR);
    assert(finding.parse_tree_count > 1);
    assert(finding.estimated_forest_nodes > 0);
    assert(finding.derivations.len == finding.parse_tree_count);
    assert(finding.derivations.data[0].data);
    assert(finding.derivations.data[1].data);
    for (size_t i = 0; i < finding.derivations.len; i++) {
        core_free(finding.derivations.data[i].data);
    }
    core_list_free(finding.derivations);

    parse_grammar_free(g);
}

static void
test_recursive_postfix_prefix_confirmation(void)
{
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    parse_nonterm_t *expr    = parse_nonterm(g, CORE_STRING_STATIC("expr"));
    parse_nonterm_t *postfix = parse_nonterm(g, CORE_STRING_STATIC("postfix"));
    parse_nonterm_t *args    = parse_nonterm(g, CORE_STRING_STATIC("args"));
    parse_nonterm_t *primary = parse_nonterm(g, CORE_STRING_STATIC("primary"));
    parse_nonterm_t *tagged  = parse_nonterm(g, CORE_STRING_STATIC("tagged"));
    parse_nonterm_t *prefix  = parse_nonterm(g, CORE_STRING_STATIC("prefix"));
    parse_nonterm_t *payload = parse_nonterm(g, CORE_STRING_STATIC("payload"));
    parse_nonterm_t *tuple   = parse_nonterm(g, CORE_STRING_STATIC("tuple"));

    parse_add_rule(g, expr, PARSE_NT(postfix));
    parse_add_rule(g, postfix, PARSE_NT(postfix), PARSE_CHAR('('),
                   PARSE_NT(args), PARSE_CHAR(')'));
    parse_add_rule(g, postfix, PARSE_NT(primary));
    parse_add_rule(g, args, PARSE_NT(expr));
    parse_add_rule(g, primary, PARSE_NT(tagged));
    parse_add_rule(g, primary, PARSE_TERMINAL(PARSE_TOK_IDENTIFIER));
    parse_add_rule(g, tagged, PARSE_NT(prefix), PARSE_NT(payload));
    parse_add_rule(g, prefix, PARSE_TERMINAL(PARSE_TOK_IDENTIFIER));
    parse_add_rule(g, payload, PARSE_NT(tuple));
    parse_add_rule(g, tuple, PARSE_CHAR('('), PARSE_NT(expr), PARSE_CHAR(')'));

    parse_ambiguity_list_t findings = parse_grammar_audit_ambiguity(g, nullptr);
    bool found = false;
    for (size_t i = 0; i < findings.len; i++) {
        found |= findings.data[i].kind == PARSE_AMBIG_CONFIRMED;
        if (findings.data[i].kind == PARSE_AMBIG_CONFIRMED) {
            assert(findings.data[i].witness.data);
            assert(core_string_find(findings.data[i].witness,
                                    S("<identifier>")) >= 0
                   || core_string_find(findings.data[i].witness,
                                       S("IDENTIFIER")) >= 0);
            assert(findings.data[i].parse_tree_count > 1);
        }
    }
    assert(found);

    parse_ambiguity_list_free(&findings);
    parse_grammar_free(g);
}

static void
test_intentional_filter(void)
{
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    parse_nonterm_t *s = parse_nonterm(g, CORE_STRING_STATIC("S"));
    parse_rule_t *r = parse_add_rule(g, s, PARSE_CHAR('a'));
    int32_t rule_ix = (int32_t)(r - g->rules.data);
    parse_add_rule(g, s, PARSE_CHAR('a'), PARSE_CHAR('b'));
    r = parse_get_rule(g, rule_ix);
    parse_annotation_t *annotation = parse_annot_new(PARSE_ANNOT_DISAMBIG);
    annotation->disambig_predicate = core_string_dup(S("intentional"));
    parse_rule_annotate(r, annotation);

    parse_ambiguity_options_t hidden = {0};
    parse_ambiguity_list_t findings
        = parse_grammar_audit_ambiguity(g, &hidden);
    assert(findings.len == 0);
    parse_ambiguity_list_free(&findings);

    parse_ambiguity_options_t shown = {
        .include_intentional = true,
        .include_candidates = true,
    };
    findings = parse_grammar_audit_ambiguity(g, &shown);
    assert(findings.len > 0);
    parse_ambiguity_list_free(&findings);
    parse_grammar_free(g);
}

int
main(void)
{
    test_first_overlap();
    test_disjoint_alternatives();
    test_nullable_prefix();
    test_dynamic_confirmation();
    test_recursive_postfix_prefix_confirmation();
    test_intentional_filter();
    puts("PASS: static grammar ambiguity audit");
    return 0;
}
