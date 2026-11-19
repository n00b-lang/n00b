#pragma once

// Nonterminals by enum rather than by name. A grammar's nonterminal ids are
// mapped once, per grammar version, to n00bb_rule_t; code that asks several
// questions of a node's nonterminal takes n00bb_rule_of(node) once and
// compares (or switches on) constants.

#include <stdatomic.h>
#include <stdint.h>

#include "parse/tree_util.h"

// The grammar's nonterminals the compiler refers to, by name: each is
// N00BB_RULE_<NAME> (dashes become underscores). Generated from the
// nonterminals grammar/n00b.bnf defines, plus a few names the compiler
// checks for that it doesn't (those never match a node).
#define N00BB_RULE_NAMES(X) \
    X(ADD_EXPR, "add-expr") \
    X(ALIAS_STMT, "alias-stmt") \
    X(ALIAS_TYPE_KEYWORD, "alias-type-keyword") \
    X(AMP_ASSIGN_OP, "amp-assign-op") \
    X(AMP_OP, "amp-op") \
    X(AND_EXPR, "and-expr") \
    X(AND_OP, "and-op") \
    X(AND_RHS, "and-rhs") \
    X(ARRAY_DIMENSION, "array-dimension") \
    X(ARRAY_DIMENSIONS, "array-dimensions") \
    X(ASSERT_STMT, "assert-stmt") \
    X(ASSIGN_OP, "assign-op") \
    X(ASSIGN_PATTERN, "assign-pattern") \
    X(ASSIGN_PATTERN_ITEM, "assign-pattern-item") \
    X(ASSIGN_PATTERN_ITEMS, "assign-pattern-items") \
    X(ASSIGN_PATTERN_TARGET, "assign-pattern-target") \
    X(ASSIGN_STMT, "assign-stmt") \
    X(ASSIGN_TARGET, "assign-target") \
    X(BASE_TSPEC, "base-tspec") \
    X(BASE_TSPEC_CORE, "base-tspec-core") \
    X(BINDING_KIND, "binding-kind") \
    X(BINDING_PATTERN, "binding-pattern") \
    X(BINOP_ASSIGN_STMT, "binop-assign-stmt") \
    X(BITAND_EXPR, "bitand-expr") \
    X(BITOR_EXPR, "bitor-expr") \
    X(BITXOR_EXPR, "bitxor-expr") \
    X(BLOCK, "block") \
    X(BLOCK_END, "block-end") \
    X(BLOCK_START, "block-start") \
    X(BLOCK_VALUE_EXPR, "block-value-expr") \
    X(BODY, "body") \
    X(BODY_STMT, "body-stmt") \
    X(BODY_STMT_LIST, "body-stmt-list") \
    X(BODY_STMTS, "body-stmts") \
    X(BOOL_LIT, "bool-lit") \
    X(BRANCH_LIST, "branch-list") \
    X(BREAK_STMT, "break-stmt") \
    X(BROUGHT_NAMES, "brought-names") \
    X(BROUGHT_SYMBOL, "brought-symbol") \
    X(BROUGHT_SYMBOLS, "brought-symbols") \
    X(CALL_ARGS, "call-args") \
    X(CALL_KW_ARG, "call-kw-arg") \
    X(CALLBACK_BODY, "callback-body") \
    X(CALLBACK_DECL, "callback-decl") \
    X(CALLBACK_EXPR, "callback-expr") \
    X(CALLBACK_ROLE, "callback-role") \
    X(CALLBACK_ROLE_LIST, "callback-role-list") \
    X(CASE_DEFAULT_KEYWORD, "case-default-keyword") \
    X(CASE_ELSE, "case-else") \
    X(CASE_EXPR_ITEM, "case-expr-item") \
    X(CASE_EXPR_LIST, "case-expr-list") \
    X(CASE_FLAG_NAME, "case-flag-name") \
    X(CASE_FLAGS, "case-flags") \
    X(CASE_FLAGS_AND, "case-flags-and") \
    X(CASE_FLAGS_KIND, "case-flags-kind") \
    X(CASE_FLAGS_PRIMARY, "case-flags-primary") \
    X(CASE_FLAGS_TEST, "case-flags-test") \
    X(CASE_FLAGS_UNARY, "case-flags-unary") \
    X(CASE_FLAGS_XOR, "case-flags-xor") \
    X(COMPARE_EXPR, "compare-expr") \
    X(COMPARE_OP, "compare-op") \
    X(CONSTRAINT_LIST, "constraint-list") \
    X(CONTEXT_MANAGER, "context-manager") \
    X(CONTINUE_STMT, "continue-stmt") \
    X(CTIME_ELSE, "ctime-else") \
    X(DECLARATION_PREFIX, "declaration-prefix") \
    X(DEFAULT_PARAM, "default-param") \
    X(DICT_ENTRIES, "dict-entries") \
    X(DICT_ENTRY, "dict-entry") \
    X(DICT_OR_SET_LIT, "dict-or-set-lit") \
    X(DISCARD_STMT, "discard-stmt") \
    X(DIVIDE_ASSIGN_OP, "divide-assign-op") \
    X(DIVIDE_OP, "divide-op") \
    X(DOTTED_NAME, "dotted-name") \
    X(EFFECT_CLAUSE, "effect-clause") \
    X(EFFECT_CLAUSE_LIST, "effect-clause-list") \
    X(EFFECT_DECL, "effect-decl") \
    X(EFFECT_HANDLER, "effect-handler") \
    X(EFFECT_HANDLER_ITEM, "effect-handler-item") \
    X(EFFECT_QUERY, "effect-query") \
    X(EFFECT_QUERY_NAME, "effect-query-name") \
    X(EFFECT_QUERY_WORD, "effect-query-word") \
    X(EFFECT_SCOPE, "effect-scope") \
    X(EFFECT_SCOPE_VALUE, "effect-scope-value") \
    X(EFFECT_SOURCE, "effect-source") \
    X(EFFECT_SOURCE_ITEM, "effect-source-item") \
    X(ELIF_CHAIN, "elif-chain") \
    X(ELIF_CLAUSE, "elif-clause") \
    X(ELIF_KEYWORD, "elif-keyword") \
    X(ELSE_CLAUSE, "else-clause") \
    X(ELSE_WHEN_CLAUSE, "else-when-clause") \
    X(EMBED_LIT, "embed-lit") \
    X(ENUM_ITEM, "enum-item") \
    X(ENUM_ITEMS, "enum-items") \
    X(ENUM_OPEN, "enum-open") \
    X(ENUM_PREFIX, "enum-prefix") \
    X(ENUM_STMT, "enum-stmt") \
    X(ENUM_STORAGE, "enum-storage") \
    X(ENUM_VALUE, "enum-value") \
    X(EOS, "eos") \
    X(EQUALITY_EXPR, "equality-expr") \
    X(EQUALITY_OP, "equality-op") \
    X(ERROR_TYPE, "error-type") \
    X(EXPORT_ALIAS, "export-alias") \
    X(EXPORT_GRANTEE, "export-grantee") \
    X(EXPORT_STMT, "export-stmt") \
    X(EXPR_COMMA_LIST, "expr-comma-list") \
    X(EXPRESSION, "expression") \
    X(EXPRESSION_STMT, "expression-stmt") \
    X(FAIL_STMT, "fail-stmt") \
    X(FLAGSET_INTERSECTION, "flagset-intersection") \
    X(FLAGSET_ITEM, "flagset-item") \
    X(FLAGSET_ITEMS, "flagset-items") \
    X(FLAGSET_NAME, "flagset-name") \
    X(FLAGSET_PREFIX, "flagset-prefix") \
    X(FLAGSET_PRIMARY, "flagset-primary") \
    X(FLAGSET_STMT, "flagset-stmt") \
    X(FLAGSET_UNARY, "flagset-unary") \
    X(FLAGSET_UNION, "flagset-union") \
    X(FLAGSET_VALUE, "flagset-value") \
    X(FLAGSET_XOR, "flagset-xor") \
    X(FLOAT_LIT, "float-lit") \
    X(FOR_BINDING, "for-binding") \
    X(FOR_IN_TARGET_LIST, "for-in-target-list") \
    X(FOR_RANGE_VAR, "for-range-var") \
    X(FOR_STMT, "for-stmt") \
    X(FOR_VAR_LIST, "for-var-list") \
    X(FORMAL, "formal") \
    X(FORMALS, "formals") \
    X(FUNC_DEF, "func-def") \
    X(FUNC_INLINE, "func-inline") \
    X(FUNC_PRELUDE, "func-prelude") \
    X(FUNC_TYPE_SIGNATURE, "func-type-signature") \
    X(FUNCTION_KEYWORD, "function-keyword") \
    X(FUNCTION_RESULT, "function-result") \
    X(IF_STMT, "if-stmt") \
    X(IF_VALUE_EXPR, "if-value-expr") \
    X(IMPORT_STMT, "import-stmt") \
    X(INDEX_CONTENTS, "index-contents") \
    X(INT_LIT, "int-lit") \
    X(INTEGER_SIZE, "integer-size") \
    X(ITEM_SEPARATOR, "item-separator") \
    X(KEYWORD_ARGS, "keyword-args") \
    X(LABEL, "label") \
    X(LABELED_FOR, "labeled-for") \
    X(LABELED_SWITCH, "labeled-switch") \
    X(LABELED_TYPECASE, "labeled-typecase") \
    X(LABELED_WHILE, "labeled-while") \
    X(LEFT_SHIFT_ASSIGN_OP, "left-shift-assign-op") \
    X(LEFT_SHIFT_OP, "left-shift-op") \
    X(LIST_ELEMS, "list-elems") \
    X(LIST_LIT, "list-lit") \
    X(LITERAL, "literal") \
    X(LITERAL_EXPR, "literal-expr") \
    X(LITMOD_ALIAS_NAME, "litmod-alias-name") \
    X(LITMOD_ALIAS_STMT, "litmod-alias-stmt") \
    X(LITMOD_ALIAS_TARGET, "litmod-alias-target") \
    X(LITMOD_KIND, "litmod-kind") \
    X(LITMOD_PRELUDE, "litmod-prelude") \
    X(MARK_ANNOTATION, "mark-annotation") \
    X(MARK_CARRIES, "mark-carries") \
    X(MARK_CLAUSE, "mark-clause") \
    X(MARK_CLAUSE_LIST, "mark-clause-list") \
    X(MARK_CLEARER, "mark-clearer") \
    X(MARK_CLEARER_ITEM, "mark-clearer-item") \
    X(MARK_DECL, "mark-decl") \
    X(MARK_REJECTER, "mark-rejecter") \
    X(MARK_REJECTER_ITEM, "mark-rejecter-item") \
    X(MARK_REJECTS, "mark-rejects") \
    X(MEMBER_CHAIN, "member-chain") \
    X(MEMBER_TAIL, "member-tail") \
    X(MEMBER_VISIBILITY, "member-visibility") \
    X(MINUS_ASSIGN_OP, "minus-assign-op") \
    X(MINUS_OP, "minus-op") \
    X(MODULE, "module") \
    X(MODULE_IMPORT, "module-import") \
    X(MODULE_SOURCE, "module-source") \
    X(MUL_EXPR, "mul-expr") \
    X(MULTIPLY_ASSIGN_OP, "multiply-assign-op") \
    X(MULTIPLY_OP, "multiply-op") \
    X(NAMED_FIELD, "named-field") \
    X(NAMED_FIELD_LIST, "named-field-list") \
    X(NEW_EXPR, "new-expr") \
    X(NIL_KEYWORD, "nil-keyword") \
    X(NOT_OP, "not-op") \
    X(NOT_WORD_OP, "not-word-op") \
    X(OBJECT_DECL, "object-decl") \
    X(ONE_CONSTRAINT, "one-constraint") \
    X(ONE_TSPEC, "one-tspec") \
    X(OR_EXPR, "or-expr") \
    X(OR_OP, "or-op") \
    X(OR_RHS, "or-rhs") \
    X(OTHER_TOP_STMTS, "other-top-stmts") \
    X(PACKAGE_BODY, "package-body") \
    X(PACKAGE_DECL, "package-decl") \
    X(PACKAGE_ITEM, "package-item") \
    X(PACKAGE_ITEMS, "package-items") \
    X(PARAM_DECL, "param-decl") \
    X(PASS_STMT, "pass-stmt") \
    X(PATTERN_BINDING, "pattern-binding") \
    X(PATTERN_ITEM, "pattern-item") \
    X(PATTERN_ITEMS, "pattern-items") \
    X(PATTERN_SKIP, "pattern-skip") \
    X(PATTERN_TARGET, "pattern-target") \
    X(PIPE_ASSIGN_OP, "pipe-assign-op") \
    X(PIPE_OP, "pipe-op") \
    X(PLACE, "place") \
    X(PLUS_ASSIGN_OP, "plus-assign-op") \
    X(PLUS_OP, "plus-op") \
    X(POS_PARAM, "pos-param") \
    X(POSITIONAL_ARGS, "positional-args") \
    X(POSTFIX_EXPR, "postfix-expr") \
    X(PRIMARY, "primary") \
    X(PRIVATE_FIELD_LIST, "private-field-list") \
    X(PRIVATE_FIELDS, "private-fields") \
    X(RANGE, "range") \
    X(RANGE_LOOP, "range-loop") \
    X(RANGE_PARTS, "range-parts") \
    X(RANGE_SEP, "range-sep") \
    X(RANGE_STEP, "range-step") \
    X(RECORD_DECL, "record-decl") \
    X(RECORD_KEYWORD, "record-keyword") \
    X(REMAINDER_ASSIGN_OP, "remainder-assign-op") \
    X(REMAINDER_OP, "remainder-op") \
    X(RETURN_SIGIL, "return-sigil") \
    X(RETURN_STMT, "return-stmt") \
    X(RETURN_TYPE, "return-type") \
    X(RIGHT_SHIFT_ASSIGN_OP, "right-shift-assign-op") \
    X(RIGHT_SHIFT_OP, "right-shift-op") \
    X(SET_ENTRIES, "set-entries") \
    X(SHIFT_EXPR, "shift-expr") \
    X(SIMPLE_LIT, "simple-lit") \
    X(SIZED_INT_TSPEC, "sized-int-tspec") \
    X(SLICE_CONTENTS, "slice-contents") \
    X(STORAGE_CLASS, "storage-class") \
    X(STRING_LIT, "string-lit") \
    X(SWITCH_CASE_BLOCK, "switch-case-block") \
    X(SWITCH_CASES, "switch-cases") \
    X(SWITCH_STMT, "switch-stmt") \
    X(SWITCH_VALUE_BRANCH_LIST, "switch-value-branch-list") \
    X(SWITCH_VALUE_CASE_BLOCK, "switch-value-case-block") \
    X(SWITCH_VALUE_CASE_PREFIX, "switch-value-case-prefix") \
    X(SWITCH_VALUE_CASES, "switch-value-cases") \
    X(SWITCH_VALUE_ELSE, "switch-value-else") \
    X(SWITCH_VALUE_EXPR, "switch-value-expr") \
    X(SYM_DECL, "sym-decl") \
    X(SYM_DECL_LIST, "sym-decl-list") \
    X(TOP_CTIME_ELSE, "top-ctime-else") \
    X(TOP_ELSE_WHEN_CLAUSE, "top-else-when-clause") \
    X(TOP_LEVEL_STMT, "top-level-stmt") \
    X(TOP_WHEN_BODY, "top-when-body") \
    X(TOP_WHEN_STMT, "top-when-stmt") \
    X(TRAILING_PAREN, "trailing-paren") \
    X(TRAIT_BODY, "trait-body") \
    X(TRAIT_CHECK, "trait-check") \
    X(TRAIT_CHECK_AND, "trait-check-and") \
    X(TRAIT_CHECK_TERM, "trait-check-term") \
    X(TRAIT_CHECK_UNARY, "trait-check-unary") \
    X(TRAIT_DECL, "trait-decl") \
    X(TRAIT_FUNC_PRELUDE, "trait-func-prelude") \
    X(TRAIT_FUNC_TYPE_SIGNATURE, "trait-func-type-signature") \
    X(TRAIT_NAME, "trait-name") \
    X(TRAIT_OBJ_TREF, "trait-obj-tref") \
    X(TRAIT_PARAM_DECL, "trait-param-decl") \
    X(TRAIT_SPEC, "trait-spec") \
    X(TRAIT_SPEC_LIST, "trait-spec-list") \
    X(TRY_BODY, "try-body") \
    X(TRY_BODY_STMTS, "try-body-stmts") \
    X(TRY_ERROR_BINDING, "try-error-binding") \
    X(TRY_ERROR_CLAUSE, "try-error-clause") \
    X(TRY_HANDLER_PARAMS, "try-handler-params") \
    X(TRY_STMT, "try-stmt") \
    X(TRY_VALUE_ERROR_CLAUSE, "try-value-error-clause") \
    X(TRY_VALUE_EXPR, "try-value-expr") \
    X(TSPEC_APPLICATION, "tspec-application") \
    X(TSPEC_FIELD, "tspec-field") \
    X(TSPEC_FIELD_LIST, "tspec-field-list") \
    X(TSPEC_FUNC, "tspec-func") \
    X(TSPEC_FUNC_PARAM, "tspec-func-param") \
    X(TSPEC_FUNC_PARAMS, "tspec-func-params") \
    X(TSPEC_FUNCTION_RESULT, "tspec-function-result") \
    X(TSPEC_KARG_ENTRY, "tspec-karg-entry") \
    X(TSPEC_KARGS, "tspec-kargs") \
    X(TSPEC_LATE_PARAM, "tspec-late-param") \
    X(TSPEC_LIST, "tspec-list") \
    X(TSPEC_NAMED_POSITIONAL, "tspec-named-positional") \
    X(TSPEC_PARAM_MODE, "tspec-param-mode") \
    X(TSPEC_TVAR, "tspec-tvar") \
    X(TSPEC_VARGS, "tspec-vargs") \
    X(TUPLE_OR_PAREN, "tuple-or-paren") \
    X(TYPE_ALIGN, "type-align") \
    X(TYPE_BODY, "type-body") \
    X(TYPE_CONSTRAINT, "type-constraint") \
    X(TYPE_DECL, "type-decl") \
    X(TYPE_DECL_ONE, "type-decl-one") \
    X(TYPE_DECL_SPEC_LIST, "type-decl-spec-list") \
    X(TYPE_LAYOUT, "type-layout") \
    X(TYPE_LAYOUT_MODIFIER, "type-layout-modifier") \
    X(TYPE_MEMBER, "type-member") \
    X(TYPE_SPEC, "type-spec") \
    X(TYPE_SPEC_LIST, "type-spec-list") \
    X(TYPECASE_CASE_BLOCK, "typecase-case-block") \
    X(TYPECASE_CASES, "typecase-cases") \
    X(TYPECASE_STMT, "typecase-stmt") \
    X(TYPECASE_VALUE_BRANCH_LIST, "typecase-value-branch-list") \
    X(TYPECASE_VALUE_CASE_BLOCK, "typecase-value-case-block") \
    X(TYPECASE_VALUE_CASE_PREFIX, "typecase-value-case-prefix") \
    X(TYPECASE_VALUE_CASES, "typecase-value-cases") \
    X(TYPECASE_VALUE_ELSE, "typecase-value-else") \
    X(TYPECASE_VALUE_EXPR, "typecase-value-expr") \
    X(TYPEOF_EXPR, "typeof-expr") \
    X(TYPESPEC_EXPR, "typespec-expr") \
    X(UNARY_EXPR, "unary-expr") \
    X(UNION_TSPEC, "union-tspec") \
    X(VALUE_BODY, "value-body") \
    X(VALUE_BODY_STMTS, "value-body-stmts") \
    X(VALUE_ELIF_CHAIN, "value-elif-chain") \
    X(VALUE_ELIF_CLAUSE, "value-elif-clause") \
    X(VALUE_ELSE_CLAUSE, "value-else-clause") \
    X(VALUE_SIGIL, "value-sigil") \
    X(VALUE_STMT, "value-stmt") \
    X(VARGS_PARAM, "vargs-param") \
    X(VARIABLE_DECL, "variable-decl") \
    X(VARIANT_BODY, "variant-body") \
    X(VARIANT_DECL, "variant-decl") \
    X(VARIANT_DECL_ONE, "variant-decl-one") \
    X(VARIANT_DECL_SPEC_LIST, "variant-decl-spec-list") \
    X(VISIBLE_INFERRED_ASSIGN, "visible-inferred-assign") \
    X(WHEN_STMT, "when-stmt") \
    X(WHERE_CLAUSE, "where-clause") \
    X(WHILE_STMT, "while-stmt") \
    X(WITH_STMT, "with-stmt") \
    X(WITH_VALUE_EXPR, "with-value-expr") \
    X(XOR_ASSIGN_OP, "xor-assign-op") \
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
