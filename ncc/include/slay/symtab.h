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

// ============================================================================
// Symbol kinds
// ============================================================================

typedef enum {
    N00B_SYM_VARIABLE,
    N00B_SYM_FUNCTION,
    N00B_SYM_TYPEDEF,
    N00B_SYM_TAG,
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

struct n00b_sym_entry_t {
    n00b_string_t        name;
    n00b_sym_kind_t      kind;
    int32_t              scope_depth;
    n00b_parse_tree_t   *decl_node;
    n00b_sym_entry_t    *shadowed;
    n00b_sym_entry_t    *next_in_scope;
    n00b_parse_tree_t   *type_node;
    n00b_string_t        adt_kind;
    bool                 is_field;
    bool                 is_method;
};

// ============================================================================
// Scope
// ============================================================================

struct n00b_scope_t {
    n00b_scope_t     *parent;
    n00b_string_t     name;
    int32_t           depth;
    n00b_sym_entry_t *first_in_scope;
    n00b_string_t     adt_kind;
};

// ============================================================================
// Namespace
// ============================================================================

struct n00b_namespace_t {
    n00b_string_t     ns_name;
    n00b_scope_t     *current;
    int32_t           depth;
    void             *symbols;  // n00b_dict_t*
};

// ============================================================================
// Symbol table
// ============================================================================

struct n00b_symtab_t {
    n00b_namespace_t *namespaces;
    int32_t           ns_count;
    int32_t           ns_cap;
};

// ============================================================================
// Lifecycle
// ============================================================================

n00b_symtab_t *n00b_symtab_new(void);
void n00b_symtab_free(n00b_symtab_t *st);

// ============================================================================
// Namespace management
// ============================================================================

n00b_namespace_t *n00b_symtab_ns(n00b_symtab_t *st, n00b_string_t ns_name);

// ============================================================================
// Scope management
// ============================================================================

void n00b_symtab_push_scope(n00b_symtab_t *st, n00b_string_t ns_name,
                             n00b_string_t scope_name);
void n00b_symtab_pop_scope(n00b_symtab_t *st, n00b_string_t ns_name);

// ============================================================================
// Symbol operations
// ============================================================================

n00b_sym_entry_t *n00b_symtab_add(n00b_symtab_t *st, n00b_string_t ns_name,
                                    n00b_string_t name, n00b_sym_kind_t kind,
                                    n00b_parse_tree_t *decl_node);

n00b_sym_entry_t *n00b_symtab_lookup(n00b_symtab_t *st, n00b_string_t ns_name,
                                       n00b_string_t name);

bool n00b_symtab_is_typedef(n00b_symtab_t *st, n00b_string_t name);

int32_t n00b_symtab_depth(n00b_symtab_t *st, n00b_string_t ns_name);

n00b_scope_t *n00b_symtab_current_scope(n00b_symtab_t *st,
                                          n00b_string_t ns_name);
