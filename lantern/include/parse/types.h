#pragma once

#include "parse/token.h"
#include "th/typehash.h"


typedef int64_t                    parse_nt_id_t;
typedef struct parse_terminal_t     parse_terminal_t;
typedef struct parse_nonterm_t      parse_nonterm_t;
typedef struct parse_rule_t   parse_rule_t;
typedef struct parse_rule_group_t   parse_rule_group_t;
typedef struct parse_grammar_t      parse_grammar_t;
typedef struct parse_annotation_t   parse_annotation_t;
typedef struct parse_scope_t        parse_scope_t;
typedef struct parse_sym_entry_t    parse_sym_entry_t;

#define PARSE_EMPTY_STRING         (-126)
#define PARSE_GROUP_ID             (1 << 28)

typedef enum {
    PARSE_QUERY_RULE_ANNOT,
    PARSE_QUERY_GRAMMAR_ANNOT,
    PARSE_QUERY_CHILD_BY_NT_NAME,
    PARSE_QUERY_NTH_NT_CHILD,
    PARSE_QUERY_FIRST_TERMINAL,
    PARSE_QUERY_CHILD_REF,
} parse_query_t;

typedef enum {
    PARSE_MATCH_EMPTY,
    PARSE_MATCH_NT,
    PARSE_MATCH_TERMINAL,
    PARSE_MATCH_ANY,
    PARSE_MATCH_CLASS,
    PARSE_MATCH_SET,
    PARSE_MATCH_GROUP,
} parse_match_kind_t;

typedef enum {
    PARSE_CC_ID_START,
    PARSE_CC_ID_CONTINUE,
    PARSE_CC_ASCII_DIGIT,
    PARSE_CC_UNICODE_DIGIT,
    PARSE_CC_ASCII_UPPER,
    PARSE_CC_ASCII_LOWER,
    PARSE_CC_ASCII_ALPHA,
    PARSE_CC_WHITESPACE,
    PARSE_CC_HEX_DIGIT,
    PARSE_CC_NONZERO_DIGIT,
    PARSE_CC_PRINTABLE,
    PARSE_CC_NON_WS_PRINTABLE,
    PARSE_CC_NON_NL_WS,
    PARSE_CC_NON_NL_PRINTABLE,
    PARSE_CC_JSON_STRING_CHAR,
    PARSE_CC_REGEX_BODY_CHAR,
} parse_char_class_t;

typedef enum {
    PARSE_EO_PREDICT_NT,
    PARSE_EO_PREDICT_G,
    PARSE_EO_FIRST_GROUP_ITEM,
    PARSE_EO_SCAN_TOKEN,
    PARSE_EO_SCAN_ANY,
    PARSE_EO_SCAN_NULL,
    PARSE_EO_SCAN_CLASS,
    PARSE_EO_SCAN_SET,
    PARSE_EO_COMPLETE_N,
    PARSE_EO_ITEM_END,
} parse_earley_op_t;

typedef enum {
    PARSE_SI_NONE             = 0,
    PARSE_SI_NT_RULE_START    = 1,
    PARSE_SI_NT_RULE_END      = 2,
    PARSE_SI_GROUP_START      = 3,
    PARSE_SI_GROUP_END        = 4,
    PARSE_SI_GROUP_ITEM_START = 5,
    PARSE_SI_GROUP_ITEM_END   = 6,
} parse_subtree_info_t;

typedef enum {
    PARSE_CMP_EQ = 0,
    PARSE_CMP_LT = -1,
    PARSE_CMP_GT = 1,
} parse_earley_cmp_t;

typedef struct {
    const int64_t *ids;
    size_t         n;
    bool           negate;
} parse_match_set_t;

bool parse_match_set_holds(const parse_match_set_t *s, int64_t tid);

typedef struct parse_match_t {
    parse_match_kind_t kind;
    union {
        int64_t           nt_id;
        int64_t           terminal_id;
        parse_char_class_t char_class;
        void             *set_items;
        void             *group;
    };
} parse_match_t;

typedef enum : uint8_t {
    PARSE_NODE_RESOLUTION_NONE,
    PARSE_NODE_RESOLUTION_SCOPE,
    PARSE_NODE_RESOLUTION_SYMBOL,
} parse_node_resolution_kind_t;

typedef struct {
    parse_node_resolution_kind_t kind;
    union {
        parse_scope_t *scope;
        parse_sym_entry_t *symbol;
    } target;
} parse_node_resolution_t;

typedef enum : uint8_t {
    PARSE_JUNCTION_NONE,
    PARSE_JUNCTION_AND,
    PARSE_JUNCTION_OR,
} parse_junction_t;

typedef struct parse_node_facts_t {
    parse_sym_entry_t      *sym;
    parse_sym_entry_t      *reference_target;
    core_string_t          unresolved_name;
    parse_node_resolution_t resolution;
    bool                    direct_symbol_reference;
    bool                    declares_binding;
    bool                    discards_value;
    parse_sym_entry_t      *narrowed_subject;
    parse_sym_entry_t      *call_function;
    void                   *call_receiver;
    void                   *call_callee;
    core_string_t           call_method;
    core_string_t           call_spelled;
    bool                    call_classified;
    bool                    call_unqualified;
    bool                    call_propagated;
    bool                    call_written;
    void                   *unwrapped;
    bool                    field_written;
    bool                    field_write_reads;
    void                   *query_function;
    bool                    names_called;
    parse_junction_t        junction;
    bool                    comparison;
    bool                    in_effect_query;
    void                   *value_owner;
    core_string_t           control_label;
    bool                    negated_literal;
    bool                    in_pattern;
    core_string_t           case_spelled;
} parse_node_facts_t;

typedef struct {
    void               *parent;
    parse_scope_t      *scope;
    th_type            *type;
    void               *frontend_data;
    parse_node_facts_t *facts;
    void               *core;
} parse_node_semantic_t;

typedef struct parse_nt_node_t {
    core_string_t         name;
    int64_t               id;
    int32_t               start;
    int32_t               end;
    int32_t               rule_index;
    uint32_t              penalty;
    uint32_t              cost;
    uint16_t              penalty_location;
    bool                  group_item;
    bool                  group_top;
    bool                  missing;
    bool                  bad_prefix;
    parse_node_semantic_t semantic;
} parse_nt_node_t;

typedef void *(*parse_walk_action_t)(parse_nt_node_t *node,
                                     void *children,
                                     void *thunk);

typedef bool (*parse_tree_walker_fn)(void *node, int32_t depth, void *thunk);
