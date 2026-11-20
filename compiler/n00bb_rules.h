#pragma once

// Nonterminals by enum rather than by name. A grammar's nonterminal ids are
// mapped once, per grammar version, to n00bb_rule_t; code that asks several
// questions of a node's nonterminal takes n00bb_rule_of(node) once and
// compares (or switches on) constants.

#include <stdatomic.h>
#include <stdint.h>

#include "parse/tree_util.h"

// The grammar's nonterminals the compiler refers to, by name: each is
// N00BB_RULE_<NAME> (dashes become underscores). Only the nonterminals some
// RULE(X) test names are listed; add a name here when a test needs it. A
// name grammar/n00b.bnf doesn't define never matches a node.
#define N00BB_RULE_NAMES(X) \
    X(ADD_EXPR, "add-expr") \
    X(ALIAS_STMT, "alias-stmt") \
    X(AMP_OP, "amp-op") \
    X(AND_EXPR, "and-expr") \
    X(AND_OP, "and-op") \
    X(AND_RHS, "and-rhs") \
    X(ARRAY_DIMENSIONS, "array-dimensions") \
    X(ASSERT_STMT, "assert-stmt") \
    X(ASSIGN_STMT, "assign-stmt") \
    X(BASE_TSPEC, "base-tspec") \
    X(BASE_TSPEC_CORE, "base-tspec-core") \
    X(BINDING_PATTERN, "binding-pattern") \
    X(BINOP_ASSIGN_STMT, "binop-assign-stmt") \
    X(BITAND_EXPR, "bitand-expr") \
    X(BITOR_EXPR, "bitor-expr") \
    X(BITXOR_EXPR, "bitxor-expr") \
    X(BLOCK_START, "block-start") \
    X(BLOCK_VALUE_EXPR, "block-value-expr") \
    X(BODY, "body") \
    X(BOOL_LIT, "bool-lit") \
    X(CALLBACK_DECL, "callback-decl") \
    X(CALLBACK_EXPR, "callback-expr") \
    X(CALLBACK_ROLE, "callback-role") \
    X(CASE_DEFAULT_KEYWORD, "case-default-keyword") \
    X(CASE_ELSE, "case-else") \
    X(CASE_EXPR_ITEM, "case-expr-item") \
    X(COMPARE_EXPR, "compare-expr") \
    X(COMPARE_OP, "compare-op") \
    X(DEFAULT_PARAM, "default-param") \
    X(DICT_ENTRIES, "dict-entries") \
    X(DICT_ENTRY, "dict-entry") \
    X(DICT_OR_SET_LIT, "dict-or-set-lit") \
    X(DIVIDE_OP, "divide-op") \
    X(EFFECT_QUERY, "effect-query") \
    X(ELIF_KEYWORD, "elif-keyword") \
    X(EMBED_LIT, "embed-lit") \
    X(ENUM_ITEM, "enum-item") \
    X(ENUM_STMT, "enum-stmt") \
    X(EQUALITY_EXPR, "equality-expr") \
    X(EQUALITY_OP, "equality-op") \
    X(EXPR_COMMA_LIST, "expr-comma-list") \
    X(EXPRESSION, "expression") \
    X(EXPRESSION_STMT, "expression-stmt") \
    X(FAIL_STMT, "fail-stmt") \
    X(FLAGSET_STMT, "flagset-stmt") \
    X(FOR_STMT, "for-stmt") \
    X(FORMALS, "formals") \
    X(FUNC_DEF, "func-def") \
    X(FUNCTION_KEYWORD, "function-keyword") \
    X(IMPORT_STMT, "import-stmt") \
    X(LABELED_FOR, "labeled-for") \
    X(LABELED_SWITCH, "labeled-switch") \
    X(LABELED_TYPECASE, "labeled-typecase") \
    X(LABELED_WHILE, "labeled-while") \
    X(LEFT_SHIFT_OP, "left-shift-op") \
    X(LIST_ELEMS, "list-elems") \
    X(LIST_LIT, "list-lit") \
    X(LITMOD_ALIAS_STMT, "litmod-alias-stmt") \
    X(MARK_ANNOTATION, "mark-annotation") \
    X(MEMBER_CHAIN, "member-chain") \
    X(MINUS_OP, "minus-op") \
    X(MODULE_IMPORT, "module-import") \
    X(MUL_EXPR, "mul-expr") \
    X(MULTIPLY_OP, "multiply-op") \
    X(NAMED_FIELD_LIST, "named-field-list") \
    X(NEW_EXPR, "new-expr") \
    X(NIL_KEYWORD, "nil-keyword") \
    X(NOT_OP, "not-op") \
    X(NOT_WORD_OP, "not-word-op") \
    X(OBJECT_DECL, "object-decl") \
    X(ONE_TSPEC, "one-tspec") \
    X(OR_EXPR, "or-expr") \
    X(OR_OP, "or-op") \
    X(OR_RHS, "or-rhs") \
    X(PATTERN_BINDING, "pattern-binding") \
    X(PIPE_OP, "pipe-op") \
    X(PLACE, "place") \
    X(PLUS_OP, "plus-op") \
    X(POS_PARAM, "pos-param") \
    X(POSTFIX_EXPR, "postfix-expr") \
    X(PRIMARY, "primary") \
    X(RANGE_PARTS, "range-parts") \
    X(RECORD_DECL, "record-decl") \
    X(RECORD_KEYWORD, "record-keyword") \
    X(REMAINDER_OP, "remainder-op") \
    X(RETURN_SIGIL, "return-sigil") \
    X(RETURN_STMT, "return-stmt") \
    X(RETURN_TYPE, "return-type") \
    X(RIGHT_SHIFT_OP, "right-shift-op") \
    X(SET_ENTRIES, "set-entries") \
    X(SHIFT_EXPR, "shift-expr") \
    X(SIMPLE_LIT, "simple-lit") \
    X(SLICE_CONTENTS, "slice-contents") \
    X(SWITCH_CASE_BLOCK, "switch-case-block") \
    X(SWITCH_STMT, "switch-stmt") \
    X(SWITCH_VALUE_CASE_BLOCK, "switch-value-case-block") \
    X(SWITCH_VALUE_ELSE, "switch-value-else") \
    X(SWITCH_VALUE_EXPR, "switch-value-expr") \
    X(SYM_DECL, "sym-decl") \
    X(TOP_LEVEL_STMT, "top-level-stmt") \
    X(TOP_WHEN_BODY, "top-when-body") \
    X(TOP_WHEN_STMT, "top-when-stmt") \
    X(TRAIT_DECL, "trait-decl") \
    X(TRAIT_NAME, "trait-name") \
    X(TRAIT_SPEC, "trait-spec") \
    X(TRY_BODY, "try-body") \
    X(TSPEC_APPLICATION, "tspec-application") \
    X(TSPEC_FUNC, "tspec-func") \
    X(TSPEC_FUNC_PARAM, "tspec-func-param") \
    X(TSPEC_FUNC_PARAMS, "tspec-func-params") \
    X(TSPEC_KARGS, "tspec-kargs") \
    X(TSPEC_LIST, "tspec-list") \
    X(TSPEC_NAMED_POSITIONAL, "tspec-named-positional") \
    X(TSPEC_TVAR, "tspec-tvar") \
    X(TUPLE_OR_PAREN, "tuple-or-paren") \
    X(TYPE_CONSTRAINT, "type-constraint") \
    X(TYPE_DECL, "type-decl") \
    X(TYPE_DECL_ONE, "type-decl-one") \
    X(TYPE_SPEC, "type-spec") \
    X(TYPE_SPEC_LIST, "type-spec-list") \
    X(TYPECASE_CASE_BLOCK, "typecase-case-block") \
    X(TYPECASE_STMT, "typecase-stmt") \
    X(TYPECASE_VALUE_CASE_BLOCK, "typecase-value-case-block") \
    X(TYPECASE_VALUE_ELSE, "typecase-value-else") \
    X(TYPECASE_VALUE_EXPR, "typecase-value-expr") \
    X(TYPEOF_EXPR, "typeof-expr") \
    X(TYPESPEC_EXPR, "typespec-expr") \
    X(UNARY_EXPR, "unary-expr") \
    X(UNION_TSPEC, "union-tspec") \
    X(VALUE_BODY, "value-body") \
    X(VALUE_STMT, "value-stmt") \
    X(VARGS_PARAM, "vargs-param") \
    X(VARIABLE_DECL, "variable-decl") \
    X(VARIANT_DECL, "variant-decl") \
    X(VARIANT_DECL_ONE, "variant-decl-one") \
    X(WHEN_STMT, "when-stmt") \
    X(WHERE_CLAUSE, "where-clause") \
    X(WITH_STMT, "with-stmt") \
    X(WITH_VALUE_EXPR, "with-value-expr") \
    X(XOR_OP, "xor-op")

typedef enum : uint16_t {
    N00BB_RULE_NONE,
#define N00BB_RULE_ENUM_(id, name) N00BB_RULE_##id,
    N00BB_RULE_NAMES(N00BB_RULE_ENUM_)
#undef N00BB_RULE_ENUM_
    N00BB_RULE_COUNT,
} n00bb_rule_t;

// The rule of each nonterminal id of one grammar version (N00BB_RULE_NONE
// for a nonterminal the list doesn't name); immutable once published.
typedef struct {
    uint64_t version;
    size_t count;
    uint16_t rules[];
} n00bb_rule_table_t;

extern _Atomic(const n00bb_rule_table_t *) n00bb_rule_table_latest;

// The table for `grammar` as it is now, built on first use of a version.
const n00bb_rule_table_t *n00bb_rule_table_for(parse_grammar_t *grammar);

static inline const n00bb_rule_table_t *
n00bb_rule_table(parse_grammar_t *grammar)
{
    const n00bb_rule_table_t *table = atomic_load_explicit(
        &n00bb_rule_table_latest, memory_order_acquire);
    if (table && table->version == parse_grammar_version(grammar))
        return table;
    return n00bb_rule_table_for(grammar);
}

// The rule `node` is a node of: N00BB_RULE_NONE for a null node, a leaf, a
// group or a nonterminal the list doesn't name. A node is of rule
// N00BB_RULE_X exactly when NT_IS(grammar, node, "x").
static inline n00bb_rule_t
n00bb_rule_of(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!grammar || !node || core_tree_is_leaf(node)) return N00BB_RULE_NONE;
    const parse_nt_node_t *pn = &core_tree_node_value(node);
    if (pn->group_top || pn->id < 0) return N00BB_RULE_NONE;
    const n00bb_rule_table_t *table = n00bb_rule_table(grammar);
    return (uint64_t)pn->id < table->count
        ? (n00bb_rule_t)table->rules[pn->id] : N00BB_RULE_NONE;
}

#define RULE(id) N00BB_RULE_##id
