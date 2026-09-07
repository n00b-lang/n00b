#pragma once

#include "parse/types.h"
#include "parse/tree.h"
#include "lib/list.h"
#include "parse/annotation.h"

typedef enum {
    PARSE_SYM_VARIABLE,
    PARSE_SYM_FUNCTION,
    PARSE_SYM_TYPEDEF,
    PARSE_SYM_TAG,
    PARSE_SYM_ENUM_CONST,
    PARSE_SYM_LABEL,
    PARSE_SYM_PARAM,
    PARSE_SYM_MODULE,
} parse_sym_kind_t;

typedef enum {
    PARSE_SYM_MUTABLE,
    PARSE_SYM_IMMUTABLE,
    PARSE_SYM_CONST,
    PARSE_SYM_GLOBAL,
} parse_sym_mutability_t;

typedef struct parse_sym_entry_t parse_sym_entry_t;
typedef struct parse_namespace_t parse_namespace_t;
typedef struct parse_symtab_t    parse_symtab_t;

struct parse_sym_entry_t {
    core_string_t             name;
    parse_sym_kind_t           kind;
    parse_sym_mutability_t     mutability;
    int32_t                  scope_depth;
    parse_tree_t        *decl_node;
    th_type                 *type;
    parse_sym_entry_t         *shadowed;
    parse_sym_entry_t         *next_in_scope;
    parse_scope_t             *scope;
    parse_scope_t             *exposed_scope;
    core_string_t             adt_kind;
    core_string_t             visibility;
    core_string_t             inherits_name;
    core_list_t(core_string_t) implements;
    bool                     is_field;
    bool                     is_method;
    bool                     is_static;
    bool                     is_abstract;
    uint32_t                 uses;
    th_mode                  mode;
    bool                     has_mode;
    uint64_t                 bit_offset;
    uint64_t                 bit_width;
    bool                     has_bit_offset;
    bool                     has_bit_width;
    void                    *user_data;
};

struct parse_scope_t {
    parse_scope_t      *parent;
    parse_namespace_t  *ns;
    core_string_t      name;
    core_string_t      adt_kind;
    core_string_t      scope_tag;
    int32_t           depth;
    int32_t           id;
    parse_sym_entry_t  *first_in_scope;
    parse_tree_t *node;
    parse_scope_t      *all_next;
};

struct parse_namespace_t {
    core_string_t ns_name;
    parse_scope_t *current;
    parse_scope_t *root;
    int32_t      depth;
    core_dict_t  *symbols;
};

struct parse_symtab_t {
    parse_namespace_t **namespaces;
    int32_t           ns_count;
    int32_t           ns_cap;
    int32_t           next_scope_id;
    parse_scope_t      *all_scopes;
    core_dict_t       *scope_by_node;
    core_dict_t       *ns_map;
};

parse_symtab_t *parse_symtab_new(void);
void          parse_symtab_free(parse_symtab_t *st);

parse_namespace_t *parse_symtab_ns(parse_symtab_t *st, core_string_t ns_name);

void parse_symtab_push_scope(parse_symtab_t *st, core_string_t ns_name,
                           core_string_t scope_name);
void parse_symtab_pop_scope(parse_symtab_t *st, core_string_t ns_name);

void parse_symtab_enter_scope(parse_symtab_t *st, core_string_t ns_name,
                            parse_scope_t *scope);
void parse_symtab_leave_scope(parse_symtab_t *st, core_string_t ns_name);

parse_sym_entry_t *parse_symtab_add(parse_symtab_t *st, core_string_t ns_name,
                                core_string_t name, parse_sym_kind_t kind,
                                parse_tree_t *decl_node);

parse_sym_entry_t *parse_symtab_lookup(parse_symtab_t *st, core_string_t ns_name,
                                   core_string_t name);

parse_sym_entry_t *parse_symtab_lookup_scoped(parse_symtab_t *st, parse_scope_t *scope,
                                          core_string_t ns_name,
                                          core_string_t name);

parse_sym_entry_t *parse_symtab_lookup_in(parse_scope_t *scope, core_string_t name);

void         parse_symtab_set_scope_node(parse_symtab_t *st, core_string_t ns_name,
                                       parse_tree_t *node);
parse_scope_t *parse_symtab_scope_for_node(parse_symtab_t     *st,
                                       parse_tree_t *node);

bool         parse_symtab_is_typedef(parse_symtab_t *st, core_string_t name);
int32_t      parse_symtab_depth(parse_symtab_t *st, core_string_t ns_name);
parse_scope_t *parse_symtab_current_scope(parse_symtab_t *st, core_string_t ns_name);
