/**
 * @file st.h
 * @brief Symbol Table for C Parsing.
 *
 * This symbol table tracks identifiers during parsing to resolve grammar
 * ambiguities and support semantic analysis. Key features:
 *
 *   - Typedef tracking: Distinguishes typedef names from regular identifiers
 *     to resolve the classic C grammar ambiguity (e.g., `foo * bar;`)
 *
 *   - Nested scopes: Push/pop operations for block-structured scoping with
 *     proper shadowing of outer definitions. Each scope tracks its starting
 *     parse tree node for cross-referencing.
 *
 *   - Declaration vs Definition: Properly distinguishes between declarations
 *     (which introduce names) and definitions (which create entities). A symbol
 *     can have multiple declarations but typically only one definition.
 *
 * ## Declarations vs Definitions
 *
 * In C, these are distinct concepts:
 *
 *   Declaration: Introduces a name and its type
 *     - extern int x;           // x is declared, not defined
 *     - int foo(int);           // function declaration (prototype)
 *     - struct point;           // forward declaration
 *     - typedef int myint;      // typedef is both declaration and definition
 *
 *   Definition: Creates the entity (allocates storage or provides body)
 *     - int x;                  // tentative definition
 *     - int x = 5;              // definition with initializer
 *     - int foo(int a) { ... }  // function definition
 *     - struct point { int x; } // struct definition
 *
 * A symbol may have:
 *   - Multiple declarations (e.g., multiple `extern int x;`)
 *   - At most one definition (with exceptions for tentative definitions)
 *
 * ## Usage Example
 *
 * @code
 *   symtab_t st;
 *   st_init(&st);
 *
 *   // Enter a function scope, recording the compound statement node
 *   st_push_scope(&st, function_body_node);
 *
 *   // Add a declaration
 *   st_add_variable(&st, "x", SYM_DECLARATION, extern_decl_node, nullptr);
 *
 *   // Later, add the definition
 *   st_add_variable(&st, "x", SYM_DEFINITION, var_def_node, nullptr);
 *
 *   // Query the symbol
 *   tnode_t *def = st_get_definition(&st, "x");       // The definition
 *   tnode_t **decls = st_get_declarations(&st, "x", &count);  // All declarations
 *
 *   st_pop_scope(&st);
 * @endcode
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dict.h"

#include "lex.h"
#include "types.h"

/** @name Symbol Kinds
 * @{
 */

/**
 * @brief Classification of symbols in the symbol table.
 */
typedef enum {
    SYM_TYPEDEF,    /**< typedef name (acts as a type specifier) */
    SYM_VARIABLE,   /**< variable or function name */
    SYM_TAG,        /**< struct/union/enum tag (in separate namespace) */
    SYM_ENUM_CONST, /**< enumeration constant (ordinary identifier) */
} sym_kind_t;

/**
 * @brief Whether a symbol entry is a declaration or definition.
 */
typedef enum {
    SYM_DECLARATION, /**< Introduces name but doesn't create entity */
    SYM_DEFINITION,  /**< Creates the entity (allocates storage, provides body) */
} sym_decl_type_t;

/** @} */

/** @name Variadic Arguments
 * @brief Metadata for functions with variadic arguments.
 * @{
 */

/**
 * @brief Style of variadic arguments for a function.
 */
typedef enum {
    VARGS_NONE, /**< Function has no variadic arguments */
    VARGS_N00B, /**< n00b-style: ... → n00b_vargs_t * (default) */
    VARGS_CSTD, /**< C-style: va_list ... → standard C ... */
} vargs_style_t;

/**
 * @brief Information about variadic parameters for a function.
 */
typedef struct {
    vargs_style_t style;             /**< Which varargs style to use */
    int           positional_before; /**< Number of positional params before vargs */
    tnode_t      *type_node;         /**< For typed n00b: deep-copied type_name subtree, else nullptr */
} vargs_info_t;

/** @} */

/** @name Symbol Entry
 * @{
 */

/** @brief Initial capacity for declaration list (grows as needed) */
#define SYM_DECL_INITIAL_CAPACITY 4

/**
 * @brief A symbol location pairs a parse tree node with its normalized type tree.
 *
 * This struct is used to associate type information with declarations and
 * definitions in the symbol table.
 */
typedef struct {
    tnode_t     *node; /**< Parse tree node for the declaration/definition */
    norm_node_t *type; /**< Normalized type tree (no free needed, arena-allocated) */
} sym_loc_t;

/**
 * @brief A symbol table entry representing a declared identifier.
 *
 * Each entry tracks the definition (if any), all declarations, and any
 * erroneous redefinitions of the symbol within its scope.
 */
typedef struct sym_entry_t {
    char               *name;             /**< The symbol's identifier (not owned) */
    sym_kind_t          kind;             /**< Classification (typedef, variable, or tag) */
    int                 scope_depth;      /**< Nesting level where defined (0 = file scope) */
    sym_loc_t           def;              /**< Location (node + type) of first/canonical definition */
    sym_loc_t          *decls;            /**< Array of locations for all declarations */
    int                 num_decls;        /**< Number of declarations in decls */
    int                 decl_capacity;    /**< Allocated capacity of decls */
    sym_loc_t          *redefs;           /**< Array of locations for erroneous redefinitions */
    int                 num_redefs;       /**< Number of redefinitions in redefs */
    int                 redef_capacity;   /**< Allocated capacity of redefs */
    struct sym_entry_t *shadowed;         /**< Previous definition at outer scope */
    struct sym_entry_t *next_in_scope;    /**< Next symbol added in same scope */
    struct kw_info_t   *kw_info;          /**< Keyword argument info (for functions) */
    vargs_info_t       *vargs_info;       /**< Variadic argument info (for functions) */
    bool                is_package_local; /**< Declared within a package scope */
} sym_entry_t;

/** @} */

/** @name Keyword Arguments
 * @brief Metadata for functions with keyword arguments.
 * @{
 */

/**
 * @brief Information about a single keyword parameter.
 */
typedef struct {
    char    *name;         /**< Keyword parameter name (not owned) */
    tnode_t *decl_specs;   /**< Declaration specifiers node (for emit) */
    tnode_t *declarator;   /**< Declarator node (for emit, contains pointer) */
    tnode_t *default_val;  /**< Default initializer tree (NULL = required) */
    char    *default_file; /**< File where default was specified */
    int      default_line; /**< Line where default was specified */
} kw_param_info_t;

/**
 * @brief Collection of keyword parameters for a function.
 */
typedef struct kw_info_t {
    list_t *params;                /**< Dynamic list of kw_param_info_t* */
    int     num_positional_params; /**< Number of positional (non-keyword) parameters */
    bool    defaults_set;          /**< Have defaults been specified? */
    bool    struct_emitted;        /**< Has the kargs struct been emitted? */
    bool    is_opaque;             /**< True for "keywords: opaque" passthrough functions */
} kw_info_t;

/** @} */

/** @name Scope
 * @{
 */

/**
 * @brief Scope marker for tracking nested blocks.
 *
 * Each scope records the parse tree node where it begins (e.g., compound
 * statement, function body), enabling scope-aware analysis and navigation.
 *
 * Scopes form a stack via parent pointers. Each scope maintains a linked list
 * of symbols added within it, enabling efficient cleanup on scope exit.
 */
struct scope_t {
    struct scope_t *parent;         /**< Parent scope */
    int             depth;          /**< Nesting depth */
    tnode_t        *scope_node;     /**< Parse tree node where scope begins */
    sym_entry_t    *first_in_scope; /**< Linked list of symbols in this scope */
};

/** @} */

/** @name Symbol Table
 * @{
 */

/**
 * @brief The main symbol table structure.
 *
 * Uses a hash table for O(1) lookup by name, with scope management for
 * nested blocks. When a scope is popped, all symbols added in that scope
 * are removed and any shadowed outer definitions are restored.
 */
typedef struct {
    ncc_dict_t symbols;        /**< name -> sym_entry_t* (ordinary identifiers) */
    ncc_dict_t tags;           /**< name -> sym_entry_t* (struct/union/enum tags) */
    scope_t   *current_scope;  /**< Current (innermost) scope */
    int        depth;          /**< Current nesting depth */
    char      *package_name;   /**< Active package name (nullptr = none) */
    char      *package_prefix; /**< Resolved prefix (after NCC_PACKAGE_MAP) */
} symtab_t;

// Symtab-aware variants of type normalization (resolve user typedefs)
extern norm_node_t *type_to_normalized_type_tree_st(char     *type_as_string,
                                                    symtab_t *ext_st);
extern char        *get_munged_identifier_st(char *type_as_string, symtab_t *st);

// Pre-populate a symtab with standard C library typedefs (size_t, uint32_t, etc.)
extern void init_standard_typedefs(symtab_t *st);

/** @} */

/** @name Label Table
 * @brief Separate namespace, function scope.
 * @{
 */

/** @brief Initial capacity for label table */
#define LABEL_INITIAL_CAPACITY 16

/**
 * @brief A label entry representing a goto label in a function.
 *
 * Labels in C have function scope - they are visible throughout the entire
 * function body, even before their definition. This allows forward gotos.
 */
typedef struct {
    char     *name;     /**< Label identifier (not owned) */
    tnode_t  *def_node; /**< Where label is defined (may be nullptr) */
    tnode_t **refs;     /**< Array of goto statement nodes */
    int       num_refs; /**< Number of references */
    int       ref_cap;  /**< Allocated capacity of refs array */
} label_entry_t;

/**
 * @brief Label table for tracking goto labels within a function.
 *
 * Labels occupy their own namespace (separate from ordinary identifiers and
 * tags) and have function scope. The label table is created when entering a
 * function.
 *
 * @par Usage Example:
 * @code
 *   label_table_t lt;
 *   lt_init(&lt);
 *   lt_add_definition(&lt, "loop", label_node);
 *   lt_add_reference(&lt, "loop", goto_node);
 * @endcode
 */
typedef struct {
    ncc_dict_t labels; /**< name -> label_entry_t* */
} label_table_t;

/** @} */

/** @name Registration Context
 * @brief Context for registering symbols during parsing.
 * @{
 */

typedef struct lex_t lex_t;

/**
 * @brief Context passed to symbol/label registration functions.
 *
 * This abstracts the parser internals from the symbol table code.
 * The parser populates this struct and passes it to registration functions.
 */
typedef struct {
    ncc_buf_t     *input;      /**< Source buffer for token extraction */
    lex_t         *lex;        /**< Lexer state (for ncc_off range checks) */
    symtab_t      *st;         /**< Symbol table (may be nullptr) */
    label_table_t *lt;         /**< Label table (may be nullptr) */
    tnode_t       *node;       /**< Current parse tree node */
    int            num_tokens; /**< Total token count (for normalization) */
    const tok_t   *tokens;     /**< Token array for source file lookup */
} st_reg_ctx_t;

/** @} */

/** @name Symbol Table Initialization and Cleanup
 * @{
 */

/**
 * @brief Initialize a symbol table.
 *
 * Must be called before any other st_* functions. Creates an initial
 * file-scope (depth 0) for global declarations.
 *
 * @param st Symbol table to initialize
 */
extern void st_init(symtab_t *st);

/** @} */

/** @name Scope Management
 * @{
 */

/**
 * @brief Push a new scope (e.g., entering a compound statement).
 *
 * Increments the nesting depth. Symbols added after this call will be
 * associated with the new scope and removed when it is popped.
 *
 * @param st Symbol table
 * @param scope_node Parse tree node where this scope begins (may be nullptr)
 */
extern void st_push_scope(symtab_t *st, tnode_t *scope_node);

/**
 * @brief Pop the current scope (e.g., leaving a compound statement).
 *
 * Removes all symbols added in the current scope. If any of those symbols
 * shadowed outer definitions, the outer definitions are restored.
 * Does nothing if already at file scope (depth 0).
 *
 * @param st Symbol table
 */
extern void st_pop_scope(symtab_t *st);

/**
 * @brief Get the parse tree node where the current scope begins.
 * @param st Symbol table
 * @return The scope node, or nullptr if at file scope or no node was recorded
 */
extern tnode_t *st_get_scope_node(symtab_t *st);

/**
 * @brief Get the current scope depth.
 * @param st Symbol table
 * @return 0 for file scope, 1 for first nested block, etc.
 */
extern int st_get_scope_depth(symtab_t *st);

/** @} */

/** @name Adding Symbols
 * @{
 */

/**
 * @brief Add a typedef name to the current scope.
 *
 * Typedefs are always both a declaration and definition, so decl_type
 * is not needed. The node is recorded as both.
 *
 * @param st Symbol table
 * @param name Identifier (not copied, must outlive the scope)
 * @param node Parse tree node of the typedef (may be nullptr)
 * @param norm_type Normalized type tree (may be nullptr)
 * @return true if added successfully, false if name conflict
 */
extern bool st_add_typedef(symtab_t *st, char *name, tnode_t *node, norm_node_t *norm_type);

/**
 * @brief Add a variable or function to the current scope.
 *
 * If the symbol already exists in the current scope:
 *   - SYM_DECLARATION: Adds to the list of declarations
 *   - SYM_DEFINITION: If already defined, records as redefinition error
 *
 * @param st Symbol table
 * @param name Identifier (not copied, must outlive the scope)
 * @param decl_type Whether this is a declaration or definition
 * @param node Parse tree node (may be nullptr)
 * @param norm_type Normalized type tree (may be nullptr)
 * @return true if added successfully, false only on allocation failure
 */
extern bool st_add_variable(symtab_t *st, char *name, sym_decl_type_t decl_type, tnode_t *node, norm_node_t *norm_type);

/**
 * @brief Add a struct/union/enum tag to the current scope.
 *
 * @note In C, tags occupy a separate namespace from ordinary identifiers.
 *       This implementation uses a single namespace; extend if needed.
 *
 * @param st Symbol table
 * @param name Tag name (not copied, must outlive the scope)
 * @param decl_type Whether this is a forward declaration or definition
 * @param node Parse tree node (may be nullptr)
 * @param norm_type Normalized type tree (may be nullptr)
 * @return true if added successfully, false only on allocation failure
 */
extern bool st_add_tag(symtab_t *st, char *name, sym_decl_type_t decl_type, tnode_t *node, norm_node_t *norm_type);

/**
 * @brief Add an enumeration constant to the current scope.
 *
 * Enumeration constants are ordinary identifiers (same namespace as variables
 * and typedefs). They are always definitions.
 *
 * @param st Symbol table
 * @param name Constant name (not copied, must outlive the scope)
 * @param node Parse tree node (may be nullptr)
 * @param norm_type Normalized type tree (may be nullptr)
 * @return true if added successfully, false only on allocation failure
 */
extern bool st_add_enum_const(symtab_t *st, char *name, tnode_t *node, norm_node_t *norm_type);

/** @} */

/** @name Querying Symbols
 * @{
 */

/**
 * @brief Check if a name is a typedef in the current scope chain.
 *
 * Searches from innermost to outermost scope. This is the key function
 * for resolving the typedef ambiguity during parsing.
 *
 * @param st Symbol table
 * @param name Name to check
 * @return true if name is a typedef, false otherwise
 */
extern bool st_is_typedef(symtab_t *st, char *name);

/**
 * @brief Check if a name is defined (has a definition, not just declared).
 * @param st Symbol table
 * @param name Name to check
 * @return true if name has a definition, false if only declared or not found
 */
extern bool st_is_defined(symtab_t *st, char *name);

/**
 * @brief Check if a name is declared (has any declaration or definition).
 * @param st Symbol table
 * @param name Name to check
 * @return true if name exists in symbol table, false otherwise
 */
extern bool st_is_declared(symtab_t *st, char *name);

/**
 * @brief Get the kind of a symbol.
 * @param st Symbol table
 * @param name Name to look up
 * @return The symbol's kind, or SYM_VARIABLE if not found
 */
extern sym_kind_t st_get_kind(symtab_t *st, char *name);

/**
 * @brief Get the symbol entry for a name.
 * @param st Symbol table
 * @param name Name to look up
 * @return Pointer to the symbol entry, or nullptr if not found
 */
[[nodiscard]] extern sym_entry_t *st_get_entry(symtab_t *st, char *name);

/**
 * @brief Look up a symbol starting from a specific scope.
 *
 * Walks up the scope chain from the given scope, searching for a symbol
 * with the specified name. This allows symbol lookups from a saved scope
 * context (e.g., the scope stored in a tnode_t during tree traversal).
 *
 * Unlike st_get_entry(), this does not use the symbol table's current state.
 * It searches the scope chain as it existed when the scope was created.
 *
 * @param scope Starting scope (typically from tnode_t->scope)
 * @param name Name to look up
 * @return Pointer to the symbol entry, or nullptr if not found
 */
[[nodiscard]] extern sym_entry_t *st_lookup_in_scope(scope_t *scope, char *name);

/**
 * @brief Get the definition node for a symbol.
 * @param st Symbol table
 * @param name Name to look up
 * @return The definition node, or nullptr if symbol has no definition
 */
[[nodiscard]] extern tnode_t *st_get_definition(symtab_t *st, char *name);

/**
 * @brief Get all declaration nodes for a symbol.
 *
 * Returns a list of all parse tree nodes where this symbol was declared
 * (not including the definition, if separate).
 *
 * @param st Symbol table
 * @param name Symbol name to look up
 * @param count [out] If non-nullptr, receives the number of declarations
 * @return Array of tnode_t* pointers (caller must free), or nullptr
 */
extern tnode_t **st_get_declarations(symtab_t *st, char *name, int *count);

/**
 * @brief Get the first declaration node for a symbol.
 *
 * Convenience function that returns the first declaration if any exist.
 *
 * @param st Symbol table
 * @param name Symbol name to look up
 * @return The first declaration node, or nullptr if no declarations
 */
extern tnode_t *st_get_first_declaration(symtab_t *st, char *name);

/** @} */

/** @name Redefinition Tracking
 * @{
 */

/**
 * @brief Check if a symbol has erroneous redefinitions.
 *
 * A redefinition occurs when a symbol is defined more than once in the
 * same scope. The first definition is stored in def; subsequent
 * definitions are captured as redefinitions for error reporting.
 *
 * @param st Symbol table
 * @param name Symbol name to check
 * @return true if symbol has one or more redefinitions, false otherwise
 */
extern bool st_has_redefinitions(symtab_t *st, char *name);

/**
 * @brief Get all redefinition nodes for a symbol.
 *
 * Returns a list of all parse tree nodes where this symbol was erroneously
 * redefined (defined after already having a definition in the same scope).
 *
 * @param st Symbol table
 * @param name Symbol name to look up
 * @param count [out] If non-nullptr, receives the number of redefinitions
 * @return Array of tnode_t* pointers (caller must free), or nullptr
 */
extern tnode_t **st_get_redefinitions(symtab_t *st, char *name, int *count);

/** @} */

/** @name Type Information
 * @{
 */

/**
 * @brief Get the normalized type tree for a symbol's definition.
 * @param st Symbol table
 * @param name Symbol name to look up
 * @return The normalized type tree, or nullptr if not available
 */
extern norm_node_t *st_get_def_type(symtab_t *st, char *name);

/**
 * @brief Get the normalized type trees for all declarations of a symbol.
 * @param st Symbol table
 * @param name Symbol name to look up
 * @param count [out] If non-nullptr, receives the number of declarations
 * @return Array of norm_node_t* pointers (caller must free), or nullptr
 */
extern norm_node_t **st_get_decl_types(symtab_t *st, char *name, int *count);

/** @} */

/** @name Label Table Functions
 * @{
 */

/**
 * @brief Initialize a label table.
 *
 * Must be called before any other lt_* functions. Typically called when
 * entering a function body.
 *
 * @param lt Label table to initialize
 */
extern void lt_init(label_table_t *lt);

/**
 * @brief Add a label definition (low-level).
 *
 * Records where a label is defined. Typically called from parser internals.
 *
 * @param lt Label table (may be nullptr, no-op)
 * @param name Label name (extracted from identifier token)
 * @param node Parse tree node of the labeled statement
 * @return true if added, false if already defined or allocation failure
 */
extern bool lt_add_definition(label_table_t *lt, char *name, tnode_t *node);

/**
 * @brief Add a label reference (low-level).
 *
 * Records a goto reference to a label. Typically called from parser internals.
 *
 * @param lt Label table (may be nullptr, no-op)
 * @param name Label name (extracted from identifier token)
 * @param node Parse tree node of the goto statement
 * @return true if added, false only on allocation failure
 */
extern bool lt_add_reference(label_table_t *lt, char *name, tnode_t *node);

/**
 * @brief Check if a label is defined.
 * @param lt Label table
 * @param name Label name to check
 * @return true if label has a definition, false otherwise
 */
extern bool lt_is_defined(label_table_t *lt, char *name);

/**
 * @brief Get the definition node for a label.
 * @param lt Label table
 * @param name Label name to look up
 * @return The labeled-statement node, or nullptr if not defined
 */
extern tnode_t *lt_get_definition(label_table_t *lt, char *name);

/**
 * @brief Get all references to a label.
 * @param lt Label table
 * @param name Label name
 * @param count [out] If non-nullptr, receives the number of references
 * @return Array of goto statement nodes (owned by entry), or nullptr
 */
extern tnode_t **lt_get_references(label_table_t *lt, char *name, int *count);

/**
 * @brief Get the label entry for a name.
 * @param lt Label table
 * @param name Label name to look up
 * @return Pointer to the label entry, or nullptr if not found
 */
extern label_entry_t *lt_get_entry(label_table_t *lt, char *name);

/** @} */

/** @name Keyword Argument Functions
 * @{
 */

/**
 * @brief Get keyword info for a function symbol.
 * @param st Symbol table
 * @param name Function name to look up
 * @return Pointer to kw_info_t, or nullptr if function has no keywords
 */
extern kw_info_t *st_get_kw_info(symtab_t *st, char *name);

/**
 * @brief Set keyword info for a function symbol.
 * @param st Symbol table
 * @param name Function name
 * @param kw_info Keyword info to set (ownership transferred)
 * @return true if set, false if symbol not found
 */
extern bool st_set_kw_info(symtab_t *st, char *name, kw_info_t *kw_info);

/** @} */

/** @name Variadic Argument Functions
 * @{
 */

/**
 * @brief Get variadic argument info for a function symbol.
 * @param st Symbol table
 * @param name Function name to look up
 * @return Pointer to vargs_info_t, or nullptr if function has no vargs
 */
extern vargs_info_t *st_get_vargs_info(symtab_t *st, char *name);

/**
 * @brief Set variadic argument info for a function symbol.
 * @param st Symbol table
 * @param name Function name
 * @param vargs_info Variadic info to set (ownership transferred)
 * @return true if set, false if symbol not found
 */
extern bool st_set_vargs_info(symtab_t *st, char *name, vargs_info_t *vargs_info);

/** @} */

/** @name Parse Tree Registration
 * @brief Functions to register symbols from parse tree nodes.
 * @{
 */

/**
 * @brief Register symbols from a declaration parse node.
 *
 * Extracts and registers typedefs, variables, tags, and enum constants
 * from a declaration node.
 *
 * @param ctx Registration context with symtab and current node
 */
extern void st_register_declaration(st_reg_ctx_t *ctx);

/**
 * @brief Register a function definition.
 *
 * Registers the function parameters in the current (function) scope.
 * The function name is registered separately by st_register_function_name.
 *
 * @param ctx Registration context with symtab and current node
 */
extern void st_register_function_definition(st_reg_ctx_t *ctx);

/**
 * @brief Register a function name.
 *
 * Registers the function name in the current (file) scope.
 * Should be called after function_body succeeds to avoid polluting
 * the symbol table when the parser backtracks.
 *
 * @param ctx Registration context with symtab and current node
 */
extern void st_register_function_name(st_reg_ctx_t *ctx);

/**
 * @brief Register a label definition.
 *
 * Finds the identifier in the current node and registers it as a label.
 *
 * @param ctx Registration context with label_table and current node
 */
extern void st_register_label_def(st_reg_ctx_t *ctx);

/**
 * @brief Register a label reference (goto).
 *
 * Finds the identifier in the current node and registers it as a goto target.
 *
 * @param ctx Registration context with label_table and current node
 */
extern void st_register_label_ref(st_reg_ctx_t *ctx);

/**
 * @brief Register a package declaration.
 *
 * Extracts the package name from an external_declaration_3 node and
 * sets the package context in the symbol table.
 *
 * @param ctx Registration context with symtab and current node
 */
extern void st_register_package(st_reg_ctx_t *ctx);

/**
 * @brief Set the active package for the symbol table.
 *
 * Sets st->package_name and resolves st->package_prefix via NCC_PACKAGE_MAP.
 * Only one package per file is allowed; calling this twice is an error.
 *
 * @param st Symbol table
 * @param name Package name (will be copied)
 */
extern void st_set_package(symtab_t *st, const char *name);

/** @} */
