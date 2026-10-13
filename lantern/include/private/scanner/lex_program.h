#pragma once

#include "scanner/lex.h"
#include "private/scanner/unicode_profile.h"
#include "lib/list.h"

typedef struct {
    uint32_t start;
    uint32_t count;
} lex_bucket_t;

typedef struct {
    int32_t edge;
    int32_t tid;
    bool    word_boundary;
} lex_term_node_t;

typedef struct {
    int32_t  next_edge;
    uint32_t child;
    uint8_t  byte;
} lex_term_edge_t;

typedef struct {
    uint64_t lo;
    uint64_t hi;
} lex_ascii_set_t;

typedef struct {
    core_string_t   key;
    lex_ascii_set_t bits;
} lex_ascii_slot_t;

typedef struct {
    int32_t *slots;
    size_t   capacity;
    size_t   count;
} lex_tid_set_t;

typedef struct {
    const scan_lex_literal_hook_t *recipe;
    int32_t                        tid;
} lex_literal_hook_t;

typedef struct {
    const scan_lex_recovery_t *recipe;
    int32_t                    tid;
    int32_t                    replacement_tid;
} lex_recovery_t;

core_list_decl(lex_term_node_t);
core_list_decl(lex_term_edge_t);

bool scan_lex_analyze_spec(const scan_lex_spec_t *spec,
                           parse_grammar_t *grammar,
                           char *error, size_t error_bytes,
                           scan_lex_issue_t **issues, size_t *issue_count);
void scan_lex_register_output_terminals(const scan_lex_spec_t *spec,
                                        parse_grammar_t *grammar);

struct scan_lex_t {
    const scan_lex_spec_t *spec;
    parse_grammar_t       *g;
    size_t                 refs;

    size_t opwidth[256];
    core_list_t(lex_term_node_t) term_nodes;
    core_list_t(lex_term_edge_t) term_edges;
    core_list_t(lex_term_node_t) opener_nodes;
    core_list_t(lex_term_edge_t) opener_edges;
    core_list_t(lex_term_node_t) string_nodes;
    core_list_t(lex_term_edge_t) string_edges;
    core_list_t(lex_term_node_t) number_nodes;
    core_list_t(lex_term_edge_t) number_edges;
    core_list_t(lex_term_node_t) number_suffix_nodes;
    core_list_t(lex_term_edge_t) number_suffix_edges;
    core_list_t(int32_t)         number_suffix_roots;
    lex_bucket_t comment_buckets[256];
    lex_bucket_t sigil_buckets[256];
    lex_bucket_t regex_buckets[256];
    lex_bucket_t heredoc_buckets[256];
    lex_bucket_t embed_buckets[256];
    lex_bucket_t embed_close_buckets[256];
    lex_bucket_t fence_buckets[256];
    const void **comment_dispatch;
    const void **sigil_dispatch;
    const void **regex_dispatch;
    const void **heredoc_dispatch;
    const void **embed_dispatch;
    const void **embed_close_dispatch;
    const void **fence_dispatch;
    size_t       fence_count;
    size_t       string_count;
    size_t       regex_count;
    size_t       embed_count;
    scan_lex_t **embed_programs;
    lex_tid_set_t *regex_not_after;
    const scan_lex_escape_t **string_escape_dispatch;
    bool                    *string_escape_complex;
    size_t       number_form_count;
    bool         ident_enabled;
    bool         number_enabled;
    scan_unicode_profile_t *identifier_profile;
    scan_lex_issue_t       *issues;
    size_t                  issue_count;

    int32_t tid_newline;
    int32_t tid_indent;
    int32_t tid_dedent;
    int32_t tid_eof;
    int32_t tid_ident;
    int32_t tid_ident_upper;
    int32_t tid_terminator;

    lex_tid_set_t is_literal;
    lex_literal_hook_t *literal_hooks;
    size_t              literal_hook_count;
    lex_recovery_t     *recoveries;
    size_t              recovery_count;
    lex_tid_set_t codepoint_terminals;
    lex_ascii_slot_t *ascii_sets;
    size_t            ascii_set_capacity;
    size_t            ascii_set_count;
    core_list_t(core_string_t) term_after;
};
