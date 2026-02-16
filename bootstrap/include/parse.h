/**
 * @file parse.h
 * @brief Complete C2Y Grammar Parser (post preprocessing).
 *
 * This header provides the public API for parsing C source code according to
 * ISO/IEC 9899:202y N3783 Annex A (C2Y working draft).
 *
 * ## Overview
 *
 * The parser implements a recursive descent parser for the complete C2Y grammar,
 * producing a concrete syntax tree (CST) that preserves all syntactic information.
 *
 * Supported grammar sections:
 *   - A.3.1 Expressions (all operators, _Generic, compound literals, etc.)
 *   - A.3.2 Declarations (variables, functions, typedefs, structs, enums, etc.)
 *   - A.3.3 Statements (if, while, for, switch, labeled statements, etc.)
 *   - A.3.4 External definitions (translation units, function definitions)
 *
 * ## Parser State Management
 *
 * The parser maintains the following state during parsing:
 *
 *   - Token stream: The array of tokens from the lexer, with current position
 *   - Current node: The parse tree node currently being constructed
 *   - Recursive node: For left-recursion handling (see below)
 *   - Symbol table: Optional, for typedef name resolution
 *
 * All state is encapsulated in an internal `parser_t` structure. No global
 * state is used, making the parser reentrant and thread-safe (provided each
 * thread uses its own lexer state and symbol table).
 *
 * ### Backtracking
 *
 * The parser uses recursive descent with backtracking. When a grammar branch
 * fails to match, the parser restores its state and tries the next alternative.
 *
 * State saved for backtracking:
 *   - Token position (restored on failure to "unconsume" tokens)
 *   - Current node being built
 *   - Recursive node (for left-recursion)
 *
 * ### Branch Selection
 *
 * Each grammar non-terminal has numbered branches (e.g., primary_expression_0,
 * primary_expression_1, etc.). The parser tries branches in order until one
 * succeeds. Branch ordering matters:
 *
 *   - More specific branches should come before general ones
 *   - Longer matches should be tried before shorter ones
 *
 * ### Left Recursion Handling
 *
 * Left-recursive grammar rules like:
 *
 *   additive-expression:
 *     multiplicative-expression
 *     additive-expression + multiplicative-expression
 *
 * Cannot be implemented directly with recursive descent. The parser handles
 * these using an iterative loop that builds left-associative trees correctly.
 *
 * ## Quick Start
 *
 * @code
 *   #include "parse.h"
 *
 *   // 1. Read source into buffer
 *   ncc_buf_t *buf = read_file("source.c");
 *
 *   // 2. Initialize lexer and tokenize
 *   lex_t state;
 *   lex_init(&state, buf, "source.c");
 *   lex(&state);
 *
 *   // 3. Parse
 *   int pos = 0;
 *   tnode_t *tree = parse_translation_unit(&state, &pos);
 *
 *   // 4. Check for success
 *   if (tree == nullptr) {
 *       fprintf(stderr, "Parse error at position %d\n", pos);
 *   }
 * @endcode
 *
 * ## Symbol Table Usage
 *
 * For correct parsing of typedef names, use the _st variants with a symbol table:
 *
 * @code
 *   symtab_t st;
 *   st_init(&st);
 *
 *   // Pre-register known typedefs (e.g., from headers)
 *   st_add_typedef(&st, "size_t", nullptr, nullptr);
 *   st_add_typedef(&st, "uint32_t", nullptr, nullptr);
 *
 *   int pos = 0;
 *   tnode_t *tree = parse_translation_unit_st(&state, &pos, &st);
 * @endcode
 *
 * Without a symbol table, the parser cannot distinguish typedef names from
 * regular identifiers. This is the classic C grammar ambiguity:
 *
 *   foo * bar;   // Is this: (foo) * (bar) or: foo *bar (declaration)?
 *
 * ## Error Handling
 *
 * All parse functions return nullptr on failure. The `position` parameter is
 * updated to reflect how far parsing progressed, which can help locate errors.
 *
 * ## Memory Management
 *
 * The parser allocates tree nodes with base_calloc(). The caller is responsible
 * for freeing the returned tree. A typical approach:
 *
 * @code
 *   void free_tree(tnode_t *node) {
 *       if (!node || node == &elided_node) return;
 *       for (int i = 0; i < node->num_kids; i++) {
 *           free_tree(node->kids[i]);
 *       }
 *       base_dealloc(node);
 *   }
 * @endcode
 *
 * ## Thread Safety
 *
 * The parser is reentrant and thread-safe under these conditions:
 *   - Each thread uses its own lex_t state
 *   - Each thread uses its own symtab_t (if using _st functions)
 *   - The returned parse trees are not shared between threads
 *
 * ## Grammar Branch Reference
 *
 * The parser has 239 grammar branches across ~106 productions. Each branch
 * is named as `production_N` where N is the alternative number (0-indexed).
 *
 * These mostly are in the same order as in the current draft of the
 * C2Y standard. However, we do reorder branches where the grammar
 * assumes non-determinism in the parsing.
 *
 * Additionally, we do need to keep track of typedef names to properly
 * parse.
 *
 * Major non-terminals and their branch counts:
 *
 *   Expressions:
 *     primary_expression      (6)  - identifier, constant, string, parens, generic, compound
 *     postfix_expression     (10)  - array, call, member access, increment
 *     unary_expression        (9)  - prefix ops, sizeof, alignof, casts
 *
 *   Declarations:
 *     declaration             (4)  - with/without attrs, static_assert, attr-decl
 *     declaration_specifiers  (2)  - specifier chains
 *     declarator              (1)  - pointer + direct-declarator
 *
 *   Statements:
 *     statement               (2)  - labeled or unlabeled
 *     selection_statement     (3)  - if, if-else, switch
 *     iteration_statement     (4)  - while, do-while, for variants
 *     jump_statement          (6)  - goto, continue, break, return
 *
 *   Definitions:
 *     external_declaration    (2)  - function definition or declaration
 *     translation_unit        (2)  - single or multiple external declarations
 */
#pragma once

#include "lex.h"
#include "st.h"
#include "types.h"

/** @name Parser Functions (with symbol table)
 *
 * These functions accept a symbol table for tracking typedef names.
 * Use these for correct parsing of real C code where typedef names
 * must be distinguished from regular identifiers.
 *
 * The symbol table should be pre-populated with any typedef names
 * that are known from included headers or prior declarations.
 *
 * @par Example:
 * @code
 *   symtab_t st;
 *   st_init(&st);
 *   st_add_typedef(&st, "size_t", nullptr, nullptr);
 *   tnode_t *tree = parse_declaration_st(&state, &pos, &st);
 * @endcode
 * @{
 */

/**
 * @brief Parse a type expression with symbol table.
 * @param state Lexer state with tokens from lex()
 * @param position [in/out] Current token index
 * @param st Symbol table for typedef resolution (may be nullptr)
 * @return Parse tree on success, nullptr on failure
 */
extern tnode_t *parse_type_expression_st(lex_t *state, int *position, symtab_t *st);

/**
 * @brief Parse an expression with symbol table.
 * @param state Lexer state with tokens from lex()
 * @param position [in/out] Current token index
 * @param st Symbol table for typedef resolution (may be nullptr)
 * @return Parse tree on success, nullptr on failure
 */
extern tnode_t *parse_expression_st(lex_t *state, int *position, symtab_t *st);

/**
 * @brief Parse a statement with symbol table.
 * @param state Lexer state with tokens from lex()
 * @param position [in/out] Current token index
 * @param st Symbol table for typedef resolution (may be nullptr)
 * @return Parse tree on success, nullptr on failure
 */
extern tnode_t *parse_statement_st(lex_t *state, int *position, symtab_t *st);

/**
 * @brief Parse a declaration with symbol table.
 * @param state Lexer state with tokens from lex()
 * @param position [in/out] Current token index
 * @param st Symbol table for typedef resolution (may be nullptr)
 * @return Parse tree on success, nullptr on failure
 */
extern tnode_t *parse_declaration_st(lex_t *state, int *position, symtab_t *st);

/**
 * @brief Parse a translation unit with symbol table.
 * @param state Lexer state with tokens from lex()
 * @param position [in/out] Current token index
 * @param st Symbol table for typedef resolution (may be nullptr)
 * @return Parse tree on success, nullptr on failure
 */
extern tnode_t *parse_translation_unit_st(lex_t *state, int *position, symtab_t *st);

/** @} */

/** @name Convenience Parser Functions (temporary symbol table)
 *
 * These functions create a temporary symbol table for typedef tracking,
 * call the corresponding _st function, then clean up. Use these for:
 *   - Quick parsing where you don't need to pre-populate typedefs
 *   - Testing and debugging
 *   - Simple cases where typedef resolution from the parsed code itself suffices
 *
 * For production use where you need to pre-register typedefs from headers,
 * use the _st variants above with your own symbol table.
 * @{
 */

/**
 * @brief Generate a convenience parser wrapper with temporary symbol table.
 *
 * Creates a static inline function that:
 * 1. Creates a temporary symtab_t on the stack
 * 2. Calls the corresponding _st function
 * 3. Returns the result
 */
#define DEFINE_PARSER_WRAPPER(name)                          \
    static inline tnode_t *name(lex_t *state, int *position) \
    {                                                        \
        symtab_t st;                                         \
        st_init(&st);                                        \
        return name##_st(state, position, &st);              \
    }

/** Parse a type expression with temporary symbol table. */
DEFINE_PARSER_WRAPPER(parse_type_expression)

/** Parse an expression with temporary symbol table. */
DEFINE_PARSER_WRAPPER(parse_expression)

/** Parse a statement with temporary symbol table. */
DEFINE_PARSER_WRAPPER(parse_statement)

/** Parse a declaration with temporary symbol table. */
DEFINE_PARSER_WRAPPER(parse_declaration)

/** Parse a translation unit with temporary symbol table. */
DEFINE_PARSER_WRAPPER(parse_translation_unit)

#undef DEFINE_PARSER_WRAPPER

/** @} */
