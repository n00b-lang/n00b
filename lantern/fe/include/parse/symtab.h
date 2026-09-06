#pragma once

#include "parse/types.h"
#include "parse/parse_tree.h"
#include "lib/list.h"

typedef enum {
    NCC_SYM_VARIABLE,
    NCC_SYM_FUNCTION,
    NCC_SYM_TYPEDEF,
    NCC_SYM_TAG,
    NCC_SYM_ENUM_CONST,
    NCC_SYM_LABEL,
    NCC_SYM_PARAM,
    NCC_SYM_MODULE,
} ncc_sym_kind_t;

typedef enum {
    NCC_SYM_MUTABLE,
    NCC_SYM_IMMUTABLE,
    NCC_SYM_CONST,
    NCC_SYM_GLOBAL,
} ncc_sym_mutability_t;

typedef struct ncc_sym_entry_t ncc_sym_entry_t;
typedef struct ncc_namespace_t ncc_namespace_t;
typedef struct ncc_symtab_t    ncc_symtab_t;

struct ncc_sym_entry_t {
    ncc_string_t             name;
    ncc_sym_kind_t           kind;
    ncc_sym_mutability_t     mutability;
    int32_t                  scope_depth;
    ncc_parse_tree_t        *decl_node;
    th_type                 *type;
    ncc_sym_entry_t         *shadowed;
    ncc_sym_entry_t         *next_in_scope;
    ncc_scope_t             *scope;
    ncc_scope_t             *exposed_scope;
    ncc_string_t             adt_kind;
    ncc_string_t             visibility;
    ncc_string_t             inherits_name;
    ncc_list_t(ncc_string_t) implements;
    bool                     is_field;
    bool                     is_method;
    bool                     is_static;
    bool                     is_abstract;
    uint32_t                 uses;
    ncc_string_t             direction;
    ncc_string_t             constraints;
    void                    *user_data;
};

struct ncc_scope_t {
    ncc_scope_t      *parent;
    ncc_namespace_t  *ns;
    ncc_string_t      name;
    ncc_string_t      adt_kind;
    ncc_string_t      scope_tag;
    int32_t           depth;
    int32_t           id;
    ncc_sym_entry_t  *first_in_scope;
    ncc_parse_tree_t *node;
    ncc_scope_t      *all_next;
};

struct ncc_namespace_t {
    ncc_string_t ns_name;
    ncc_scope_t *current;
    ncc_scope_t *root;
    int32_t      depth;
    ncc_dict_t  *symbols;
};

struct ncc_symtab_t {
    ncc_namespace_t **namespaces;
    int32_t           ns_count;
    int32_t           ns_cap;
    int32_t           next_scope_id;
    ncc_scope_t      *all_scopes;
    ncc_dict_t       *scope_by_node;
};

ncc_symtab_t *ncc_symtab_new(void);
void          ncc_symtab_free(ncc_symtab_t *st);

ncc_namespace_t *ncc_symtab_ns(ncc_symtab_t *st, ncc_string_t ns_name);

void ncc_symtab_push_scope(ncc_symtab_t *st, ncc_string_t ns_name,
                           ncc_string_t scope_name);
void ncc_symtab_pop_scope(ncc_symtab_t *st, ncc_string_t ns_name);

void ncc_symtab_enter_scope(ncc_symtab_t *st, ncc_string_t ns_name,
                            ncc_scope_t *scope);
void ncc_symtab_leave_scope(ncc_symtab_t *st, ncc_string_t ns_name);

ncc_sym_entry_t *ncc_symtab_add(ncc_symtab_t *st, ncc_string_t ns_name,
                                ncc_string_t name, ncc_sym_kind_t kind,
                                ncc_parse_tree_t *decl_node);

ncc_sym_entry_t *ncc_symtab_lookup(ncc_symtab_t *st, ncc_string_t ns_name,
                                   ncc_string_t name);

ncc_sym_entry_t *ncc_symtab_lookup_scoped(ncc_symtab_t *st, ncc_scope_t *scope,
                                          ncc_string_t ns_name,
                                          ncc_string_t name);

ncc_sym_entry_t *ncc_symtab_lookup_in(ncc_scope_t *scope, ncc_string_t name);

void         ncc_symtab_set_scope_node(ncc_symtab_t *st, ncc_string_t ns_name,
                                       ncc_parse_tree_t *node);
ncc_scope_t *ncc_symtab_scope_for_node(ncc_symtab_t     *st,
                                       ncc_parse_tree_t *node);

bool         ncc_symtab_is_typedef(ncc_symtab_t *st, ncc_string_t name);
int32_t      ncc_symtab_depth(ncc_symtab_t *st, ncc_string_t ns_name);
ncc_scope_t *ncc_symtab_current_scope(ncc_symtab_t *st, ncc_string_t ns_name);
