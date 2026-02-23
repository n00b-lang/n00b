/**
 * @file branch_symbols.h
 * @brief Symbolic branch names for grammar-independent transform code.
 *
 * This file defines symbolic names for grammar branches that are checked in
 * transforms. By using symbolic names instead of magic numbers, transforms
 * can work with both the reference grammar (parse.c) and an optimized grammar
 * (parse_opt.c) that may have different branch numbering.
 *
 * Usage:
 *   // Instead of:  if (node->branch != 1) { ... }
 *   // Write:       if (node->branch != BRANCH(postfix_expression, CALL)) { ... }
 *
 * Grammar selection is controlled by the NCC_REFERENCE_GRAMMAR environment
 * variable. When set to "1", the reference grammar is used; otherwise, the
 * optimized grammar is used (default).
 */
#pragma once

#include <stdbool.h>
#include <stdlib.h>

/**
 * X-macro list of all semantically-meaningful branches.
 * Format: X(NONTERMINAL, SYMBOL_NAME, REF_BRANCH, OPT_BRANCH)
 *
 * REF_BRANCH is the branch number in the reference grammar (parse.c).
 * OPT_BRANCH is the branch number in the optimized grammar (parse_opt.c).
 * Initially these are the same; they will diverge when left-factoring is applied.
 */
#define BRANCH_LIST(X)                                \
    /* postfix_expression branches */                 \
    X(postfix_expression, SUBSCRIPT, 0, 0)            \
    X(postfix_expression, CALL, 1, 1)                 \
    X(postfix_expression, MEMBER_DOT, 2, 2)           \
    X(postfix_expression, MEMBER_ARROW, 3, 3)         \
    X(postfix_expression, INCREMENT, 4, 4)            \
    X(postfix_expression, DECREMENT, 5, 5)            \
    X(postfix_expression, COMPOUND_LITERAL, 6, 6)     \
    X(postfix_expression, DESIGNATED_INIT, 7, 7)      \
    X(postfix_expression, STORAGE_INIT, 8, 8)         \
    X(postfix_expression, PRIMARY, 9, 9)              \
    X(postfix_expression, BANG, 10, 10)               \
    /* init_declarator branches */                    \
    X(init_declarator, WITH_INIT, 0, 0)               \
    X(init_declarator, WITHOUT_INIT, 1, 1)            \
    /* keyword_clause branches */                     \
    X(keyword_clause, REGULAR, 0, 0)                  \
    X(keyword_clause, OPAQUE, 1, 1)                   \
    /* declaration branches */                        \
    X(declaration, ATTRS_INIT_LIST, 0, 0)             \
    X(declaration, PLAIN_INIT_LIST, 1, 1)             \
    X(declaration, STATIC_ASSERT, 2, 2)               \
    X(declaration, ATTRIBUTE_DECL, 3, 3)              \
    X(declaration, WITH_KEYWORDS, 4, 4)               \
    /* function_definition branches */                \
    X(function_definition, WITH_KEYWORDS, 0, 0)       \
    X(function_definition, WITHOUT_KEYWORDS, 1, 1)    \
    /* struct_or_union_specifier branches */          \
    X(struct_or_union_specifier, WITH_BODY, 0, 0)     \
    X(struct_or_union_specifier, EMPTY_BODY, 1, 1)    \
    X(struct_or_union_specifier, FORWARD_DECL, 2, 2)  \
    /* enum_specifier branches */                     \
    X(enum_specifier, WITH_BODY, 0, 0)                \
    X(enum_specifier, TRAILING_COMMA, 1, 1)           \
    X(enum_specifier, FORWARD_DECL, 2, 2)             \
    /* parameter_declaration branches */              \
    X(parameter_declaration, WITH_DECLARATOR, 0, 0)   \
    X(parameter_declaration, ABSTRACT_ONLY, 1, 1)     \
    /* parameter_type_list branches */                    \
    X(parameter_type_list, N00B_VA_PLUS, 0, 0)             \
    X(parameter_type_list, N00B_VA_COMMA_PLUS, 1, 1)       \
    X(parameter_type_list, N00B_VA_ONLY, 2, 2)             \
    X(parameter_type_list, C_VA_WITH_PARAMS, 3, 3)         \
    X(parameter_type_list, C_VA_ONLY, 4, 4)                \
    X(parameter_type_list, PARAM_LIST_ONLY, 5, 5)          \
    /* primary_expression branches */                 \
    X(synthetic_identifier, TYPEID, 0, 0)             \
    X(synthetic_identifier, CONSTEXPR_PASTE, 1, 1)    \
    X(synthetic_string_literal, TYPESTR, 0, 0)        \
    X(primary_expression, TYPEHASH, 10, 10)           \
    /* selection_statement branches */                \
    X(selection_statement, IF_ELSE, 0, 0)             \
    X(selection_statement, IF_ONLY, 1, 1)             \
    X(selection_statement, SWITCH, 2, 2)              \
    /* external_declaration branches */               \
    X(external_declaration, FUNC_DEF, 0, 0)           \
    X(external_declaration, DECLARATION, 1, 1)        \
    X(external_declaration, EMPTY, 2, 2)              \
    X(external_declaration, PACKAGE, 3, 3)            \
    /* compound_statement branches */                 \
    X(compound_statement, WITH_ITEMS, 0, 0)           \
    X(compound_statement, EMPTY, 1, 1)

/**
 * Runtime grammar selection flag.
 * When true, use reference grammar branch numbers; when false, use optimized.
 */
extern bool ncc_use_reference_grammar;

/**
 * Initialize grammar selection from environment variable.
 * Call this once at startup before any parsing/transforms.
 *
 * If NCC_REFERENCE_GRAMMAR=1 is set, uses the reference grammar;
 * otherwise uses the optimized grammar (default).
 */
static inline void
branch_symbols_init(void)
{
    const char *env           = getenv("NCC_REFERENCE_GRAMMAR");
    ncc_use_reference_grammar = (env && env[0] == '1');
}

/**
 * Generate constants for reference grammar branch numbers as macros.
 * Using #define instead of static const to allow use in case labels.
 */
#define X_BRANCH_REF_CONST(nt, sym, ref, opt) \
    enum {                                    \
        BRANCH_REF_##nt##_##sym = ref         \
    };
BRANCH_LIST(X_BRANCH_REF_CONST)
#undef X_BRANCH_REF_CONST

/**
 * Generate constants for optimized grammar branch numbers as macros.
 * Using #define instead of static const to allow use in case labels.
 */
#define X_BRANCH_OPT_CONST(nt, sym, ref, opt) \
    enum {                                    \
        BRANCH_OPT_##nt##_##sym = opt         \
    };
BRANCH_LIST(X_BRANCH_OPT_CONST)
#undef X_BRANCH_OPT_CONST

/**
 * Branch accessor macro.
 * Returns the appropriate branch number based on which grammar is active.
 *
 * Usage:
 *   if (node->branch == BRANCH(postfix_expression, CALL)) { ... }
 *   switch (node->branch) {
 *       case BRANCH(parameter_type_list, C_VA_WITH_PARAMS): ...
 *   }
 *
 * Note: In switch statements, the macro expands to a compile-time constant
 * expression since ncc_use_reference_grammar is typically constant after init.
 * However, for maximum portability, consider using if/else chains when
 * reference grammar support is needed.
 */
#define BRANCH(nt, sym) \
    (ncc_use_reference_grammar ? BRANCH_REF_##nt##_##sym : BRANCH_OPT_##nt##_##sym)

/**
 * Static branch accessor for cases where we know we're using reference grammar.
 * Useful in contexts where the ternary operator isn't allowed (e.g., case labels).
 */
#define BRANCH_REF(nt, sym) BRANCH_REF_##nt##_##sym
#define BRANCH_OPT(nt, sym) BRANCH_OPT_##nt##_##sym
