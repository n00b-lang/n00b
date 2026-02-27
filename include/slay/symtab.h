#pragma once

/**
 * @file symtab.h
 * @brief Symbol table with named parallel namespaces and push/pop scoping.
 *
 * Each namespace (e.g., "" for variables, "tag" for struct/union/enum,
 * "label" for goto labels) is an independent scope stack. Push/pop
 * operates on one namespace at a time. Designed for post-parse
 * annotation walks on ambiguity-aware parse forests.
 */

#include "slay/types.h"
#include "slay/parse_tree.h"

typedef struct n00b_tc_type_s n00b_tc_type_t;
typedef struct n00b_cfg_t     n00b_cfg_t;

// ============================================================================
// Symbol kinds
// ============================================================================

/** @brief Kind of symbol table entry. */
typedef enum {
    N00B_SYM_VARIABLE,
    N00B_SYM_FUNCTION,
    N00B_SYM_TYPEDEF,
    N00B_SYM_TAG,         /**< struct/union/enum tag */
    N00B_SYM_ENUM_CONST,
    N00B_SYM_LABEL,
    N00B_SYM_PARAM,
} n00b_sym_kind_t;

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct n00b_sym_entry_t  n00b_sym_entry_t;
typedef struct n00b_scope_t      n00b_scope_t;
typedef struct n00b_namespace_t  n00b_namespace_t;
typedef struct n00b_symtab_t     n00b_symtab_t;

// ============================================================================
// Symbol entry
// ============================================================================

/**
 * @brief A symbol table entry.
 *
 * Entries are chained within a scope for O(scope-size) cleanup on pop,
 * and support shadowing via the `shadowed` pointer.
 */
struct n00b_sym_entry_t {
    n00b_string_t        name;
    n00b_sym_kind_t      kind;
    int32_t              scope_depth;
    n00b_parse_tree_t   *decl_node;     /**< Parse tree node of declaration. */
    n00b_sym_entry_t    *shadowed;      /**< Previous entry at outer scope. */
    n00b_sym_entry_t    *next_in_scope; /**< Linked list for scope cleanup. */
    n00b_parse_tree_t   *type_node;     /**< Parse subtree for type (from @type/@field/@method). */
    n00b_tc_type_t      *type_var;      /**< Type variable for inference. */
    n00b_string_t        adt_kind;      /**< ADT kind: "struct", "union", "enum" (from @adt). */
    n00b_scope_t        *exposed_scope; /**< Scope this symbol exposes for dotted access. */
    n00b_cfg_t          *cfg;           /**< Per-function CFG (functions only). */
    bool                 is_field;      /**< True if declared via @field. */
    bool                 is_method;     /**< True if declared via @method. */
};

// ============================================================================
// Scope
// ============================================================================

/** @brief A single scope level within one namespace. */
struct n00b_scope_t {
    n00b_scope_t     *parent;
    n00b_string_t     name;           /**< Optional scope name (e.g., func name). */
    int32_t           depth;
    n00b_sym_entry_t *first_in_scope; /**< Head of this scope's symbol chain. */
    n00b_string_t     adt_kind;       /**< "struct", "union", "enum" if opened by @adt. */
};

// ============================================================================
// Namespace
// ============================================================================

/** @brief One independent namespace with its own scope stack and symbol hash. */
struct n00b_namespace_t {
    n00b_string_t     ns_name;      /**< "" = default, "tag", "label", etc. */
    n00b_scope_t     *current;      /**< Current (innermost) scope. */
    n00b_scope_t    **all_scopes;   /**< All scopes ever created (for cleanup). */
    int32_t           all_count;    /**< Number of scopes in all_scopes. */
    int32_t           all_cap;      /**< Capacity of all_scopes. */
    int32_t           depth;
    void             *symbols;      /**< Hash table: name -> n00b_sym_entry_t*. */
};

// ============================================================================
// Symbol table
// ============================================================================

/** @brief A symbol table: a collection of named namespaces. */
struct n00b_symtab_t {
    n00b_namespace_t *namespaces;
    int32_t           ns_count;
    int32_t           ns_cap;
};

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * @brief Create a new empty symbol table.
 * @return Newly allocated symbol table.
 */
n00b_symtab_t *n00b_symtab_new(void);

/**
 * @brief Free a symbol table and all associated state.
 * @param st  Symbol table to free (NULL is a no-op).
 */
void n00b_symtab_free(n00b_symtab_t *st);

// ============================================================================
// Namespace management
// ============================================================================

/**
 * @brief Get or create a namespace by name.
 *
 * "" is the default variable namespace. Other common names:
 * "tag" (struct/union/enum), "label" (goto), "config" (n00b config).
 *
 * @param st       Symbol table.
 * @param ns_name  Namespace name.
 * @return Pointer to the namespace (valid until next ns creation).
 */
n00b_namespace_t *n00b_symtab_ns(n00b_symtab_t *st, n00b_string_t ns_name);

// ============================================================================
// Scope management
// ============================================================================

/**
 * @brief Push a new scope onto a namespace's scope stack.
 * @param st          Symbol table.
 * @param ns_name     Namespace to push in.
 * @param scope_name  Optional name for the scope (e.g., function name).
 */
void n00b_symtab_push_scope(n00b_symtab_t *st, n00b_string_t ns_name,
                             n00b_string_t scope_name);

/**
 * @brief Pop the current scope from a namespace's scope stack.
 *
 * Restores shadowed entries and removes entries that were added in
 * this scope from the hash table. O(symbols-in-scope), no full scan.
 *
 * @param st       Symbol table.
 * @param ns_name  Namespace to pop from.
 */
void n00b_symtab_pop_scope(n00b_symtab_t *st, n00b_string_t ns_name);

// ============================================================================
// Symbol operations
// ============================================================================

/**
 * @brief Add a symbol to a namespace.
 *
 * If a symbol with the same name already exists, it is shadowed
 * (saved in the new entry's `shadowed` pointer).
 *
 * @param st         Symbol table.
 * @param ns_name    Namespace to add to.
 * @param name       Symbol name.
 * @param kind       Symbol kind.
 * @param decl_node  Parse tree node of the declaration (may be NULL).
 * @return The newly created entry.
 */
n00b_sym_entry_t *n00b_symtab_add(n00b_symtab_t *st, n00b_string_t ns_name,
                                    n00b_string_t name, n00b_sym_kind_t kind,
                                    n00b_parse_tree_t *decl_node);

/**
 * @brief Look up a symbol in a namespace.
 * @param st       Symbol table.
 * @param ns_name  Namespace to search.
 * @param name     Symbol name.
 * @return The entry, or NULL if not found.
 */
n00b_sym_entry_t *n00b_symtab_lookup(n00b_symtab_t *st, n00b_string_t ns_name,
                                       n00b_string_t name);

/**
 * @brief Look up a symbol across all scopes (including popped ones).
 *
 * Searches the all_scopes list for any scope containing a symbol with
 * the given name. Returns the first match found (innermost first since
 * scopes are tracked in push order).
 *
 * @param st       Symbol table.
 * @param ns_name  Namespace to search.
 * @param name     Symbol name.
 * @return The entry, or NULL if not found in any scope.
 */
n00b_sym_entry_t *n00b_symtab_lookup_all(n00b_symtab_t *st, n00b_string_t ns_name,
                                            n00b_string_t name);

/**
 * @brief Check if a name is a typedef in the default namespace.
 * @param st    Symbol table.
 * @param name  Name to check.
 * @return True if the name is registered as a typedef.
 */
bool n00b_symtab_is_typedef(n00b_symtab_t *st, n00b_string_t name);

/**
 * @brief Get the current scope depth of a namespace.
 * @param st       Symbol table.
 * @param ns_name  Namespace to query.
 * @return Current depth (0 = file scope), or -1 if namespace doesn't exist.
 */
int32_t n00b_symtab_depth(n00b_symtab_t *st, n00b_string_t ns_name);

/**
 * @brief Get the current (innermost) scope of a namespace.
 * @param st       Symbol table.
 * @param ns_name  Namespace to query (must already exist).
 * @return The current scope, or NULL if namespace doesn't exist or has no scope.
 */
n00b_scope_t *n00b_symtab_current_scope(n00b_symtab_t *st, n00b_string_t ns_name);
