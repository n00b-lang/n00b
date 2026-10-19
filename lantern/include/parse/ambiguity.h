#pragma once

#include "parse/grammar.h"
#include "parse/pwz.h"
#include "lib/list.h"

#include <stdio.h>

typedef enum {
    PARSE_AMBIG_DUPLICATE,
    PARSE_AMBIG_FIRST_OVERLAP,
    PARSE_AMBIG_NULLABLE_PREFIX,
    PARSE_AMBIG_COMMON_PREFIX,
    PARSE_AMBIG_CONFIRMED,
} parse_ambiguity_kind_t;

typedef enum {
    PARSE_AMBIG_INFO,
    PARSE_AMBIG_WARNING,
    PARSE_AMBIG_ERROR,
} parse_ambiguity_severity_t;

typedef struct {
    parse_ambiguity_kind_t     kind;
    parse_ambiguity_severity_t severity;
    parse_nt_id_t              nonterminal;
    core_string_t              nonterminal_name;
    int32_t                    rule_a;
    int32_t                    rule_b;
    core_string_t              source_origin_a;
    core_string_t              source_origin_b;
    core_string_t              witness;
    bool                        witness_is_prefix;
    core_list_t(core_string_t) derivations;
    core_string_t              explanation;
    core_string_t              prescription;
    uint32_t                   estimated_forest_nodes;
    uint32_t                   estimated_score_calls;
    uint32_t                   score_tie_count;
    uint32_t                   parse_tree_count;
} parse_ambiguity_finding_t;

core_list_decl(parse_ambiguity_finding_t);
typedef core_list_t(parse_ambiguity_finding_t) parse_ambiguity_list_t;

typedef struct {
    uint32_t max_witness_tokens;
    uint32_t max_states;
    uint32_t max_findings;
    bool     include_intentional;
    bool     include_candidates;
    parse_pwz_disambig_fn disambiguator;
    void                  *disambiguator_ud;
} parse_ambiguity_options_t;

typedef parse_token_stream_t *(*parse_ambiguity_tokenize_fn)(
    parse_grammar_t *grammar,
    const parse_ambiguity_finding_t *finding,
    void *user_data);

parse_ambiguity_list_t
parse_grammar_audit_ambiguity(parse_grammar_t *grammar,
                              const parse_ambiguity_options_t *options);

void parse_ambiguity_list_free(parse_ambiguity_list_t *findings);

bool parse_ambiguity_confirm_finding(parse_grammar_t *grammar,
                                     parse_ambiguity_finding_t *finding,
                                     parse_ambiguity_tokenize_fn tokenize,
                                     void *user_data);

core_string_t parse_ambiguity_kind_name(parse_ambiguity_kind_t kind);
core_string_t parse_ambiguity_severity_name(parse_ambiguity_severity_t severity);

core_string_t parse_ambiguity_format_json(
    const parse_ambiguity_list_t *findings);

int parse_ambiguity_cli(int argc, char **argv, FILE *out, FILE *err);
