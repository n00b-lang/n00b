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
typedef struct core_translation_unit_t core_translation_unit_t;

typedef enum {
    PARSE_SYM_STORAGE_UNSPECIFIED,
    PARSE_SYM_STORAGE_COMPTIME,
    PARSE_SYM_STORAGE_AUTOMATIC,
    PARSE_SYM_STORAGE_MODULE,
    PARSE_SYM_STORAGE_THREAD,
    PARSE_SYM_STORAGE_DYNAMIC,
} parse_sym_storage_duration_t;

struct parse_sym_entry_t {
    core_string_t             name;
    core_translation_unit_t  *owner_unit;
    parse_sym_kind_t           kind;
    parse_sym_mutability_t     mutability;
    core_string_t             direction;
    core_string_t             pass_by;
    int32_t                  scope_depth;
    parse_tree_t        *decl_node;
    parse_tree_t        *initializer_node;
    parse_tree_t       **definition_nodes;
    size_t                definition_count;
    size_t                definition_capacity;
    parse_tree_t             **use_nodes;
    size_t                     use_count;
    size_t                     use_capacity;
    bool                       type_is_inferred;
    bool                       type_from_assignment;
    th_type                 *type;
    th_type                 *error_type;
    th_type                 *callable_contract;
    parse_sym_entry_t         *shadowed;
    parse_sym_entry_t         *binding_origin;
    parse_sym_entry_t         *next_in_scope;
    parse_scope_t             *scope;
    parse_scope_t             *member_scope;
    parse_sym_entry_t         *owner_type;
    parse_scope_t             *exposed_scope;
    core_string_t             adt_kind;
    core_string_t             visibility;
    core_list_t(core_string_t) implements;
    bool                     is_field;
    bool                     is_thread_local;
    bool                     is_optional;
    bool                     is_variadic;
    bool                     is_synthetic;
    uint32_t                 uses;
    th_mode                  mode;
    bool                     has_mode;
    uint64_t                 bit_offset;
    uint64_t                 bit_width;
    bool                     has_bit_offset;
    bool                     has_bit_width;
    void                    *comptime_value;
    bool                     has_comptime_value;
    core_string_t            requested_storage_class;
    core_string_t            selected_storage_class;
    bool                     storage_class_required;
    parse_sym_storage_duration_t storage_duration;
    void                    *namespaced_bindings;
    void                    *user_data;
};

struct parse_scope_t {
    parse_scope_t      *parent;
    core_translation_unit_t *owner_unit;
    parse_scope_t      *lexical_scope;
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
    core_dict_t  *scopes_by_lexical_id;
};

struct parse_symtab_t {
    core_translation_unit_t  *owner_unit;
    parse_namespace_t **namespaces;
    int32_t           ns_count;
    size_t            ns_cap;
    int32_t           next_scope_id;
    parse_scope_t      *all_scopes;
    core_dict_t       *scope_by_node;
    core_dict_t       *ns_map;
    core_dict_t       *nominal_types;
};

typedef bool (*parse_symtab_scope_visitor_fn)(parse_scope_t *scope,
                                             void *context);
typedef bool (*parse_symtab_symbol_visitor_fn)(parse_sym_entry_t *symbol,
                                               void *context);

parse_symtab_t *parse_symtab_new(void);
void          parse_symtab_free(parse_symtab_t *st);
void parse_symtab_set_owner_unit(parse_symtab_t *st,
                                 core_translation_unit_t *unit);

parse_namespace_t *parse_symtab_ns(parse_symtab_t *st, core_string_t ns_name);
bool parse_symtab_visit_namespace_scopes(
    parse_symtab_t *st, core_string_t ns_name,
    parse_symtab_scope_visitor_fn visitor, void *context);
bool parse_symtab_visit_name_across_namespaces(
    parse_symtab_t *st, parse_scope_t *scope, core_string_t name,
    parse_symtab_symbol_visitor_fn visitor, void *context);

void parse_symtab_push_scope(parse_symtab_t *st, core_string_t ns_name,
                           core_string_t scope_name);
void parse_symtab_pop_scope(parse_symtab_t *st, core_string_t ns_name);
void parse_symtab_push_scope_all(parse_symtab_t *st,
                                 core_string_t scope_name);
void parse_symtab_pop_scope_all(parse_symtab_t *st);

void parse_symtab_enter_scope(parse_symtab_t *st, core_string_t ns_name,
                            parse_scope_t *scope);
void parse_symtab_leave_scope(parse_symtab_t *st, core_string_t ns_name);
void parse_symtab_enter_scope_all(parse_symtab_t *st, parse_scope_t *scope);
void parse_symtab_leave_scope_all(parse_symtab_t *st);
parse_scope_t *parse_symtab_scope_in_namespace(parse_symtab_t *st,
                                               parse_scope_t *scope,
                                               core_string_t ns_name);

parse_sym_entry_t *parse_symtab_add(parse_symtab_t *st, core_string_t ns_name,
                                   core_string_t name, parse_sym_kind_t kind,
                                   parse_tree_t *decl_node);
bool parse_sym_set_binding(parse_sym_entry_t *symbol, core_string_t key,
                           void *value);
void *parse_sym_binding(const parse_sym_entry_t *symbol, core_string_t key);
bool parse_sym_set_binding_origin(parse_sym_entry_t *symbol,
                                 parse_sym_entry_t *origin);
parse_sym_entry_t *parse_sym_binding_origin(const parse_sym_entry_t *symbol);
bool parse_sym_add_definition(parse_sym_entry_t *symbol, parse_tree_t *node);
bool parse_sym_add_use(parse_sym_entry_t *symbol, parse_tree_t *node);
bool parse_sym_remove_use(parse_sym_entry_t *symbol, parse_tree_t *node);
void parse_sym_mark_type_inferred(parse_sym_entry_t *symbol);

void parse_sym_set_storage_class(parse_sym_entry_t *symbol,
                                core_string_t requested,
                                bool required,
                                core_string_t selected);
void parse_sym_set_storage_duration(
    parse_sym_entry_t *symbol, parse_sym_storage_duration_t duration);
core_string_t parse_sym_storage_duration_name(
    parse_sym_storage_duration_t duration);

parse_sym_entry_t *parse_symtab_lookup(parse_symtab_t *st, core_string_t ns_name,
                                   core_string_t name);

parse_sym_entry_t *parse_symtab_lookup_scoped(parse_symtab_t *st, parse_scope_t *scope,
                                          core_string_t ns_name,
                                          core_string_t name);

parse_sym_entry_t *parse_symtab_lookup_visible(parse_symtab_t *st,
                                               parse_scope_t *scope,
                                               core_string_t ns_name,
                                               core_string_t name,
                                               parse_tree_t *use);
bool parse_sym_visible_at(const parse_sym_entry_t *symbol, parse_tree_t *use);

parse_sym_entry_t *parse_symtab_lookup_in(parse_scope_t *scope, core_string_t name);
bool parse_symtab_bind_nominal_type(parse_symtab_t *st, th_type *type,
                                    parse_sym_entry_t *symbol);
parse_sym_entry_t *parse_symtab_nominal_type(parse_symtab_t *st,
                                            th_type *type);

parse_sym_entry_t *parse_symtab_resolve_node(parse_symtab_t *st,
                                             parse_tree_t *node,
                                             core_string_t ns_name);

void         parse_symtab_set_scope_node(parse_symtab_t *st, core_string_t ns_name,
                                       parse_tree_t *node);
void         parse_symtab_set_scope_node_all(parse_symtab_t *st,
                                             parse_tree_t *node);
parse_scope_t *parse_symtab_scope_for_node(parse_symtab_t     *st,
                                       parse_tree_t *node);

bool         parse_symtab_is_typedef(parse_symtab_t *st, core_string_t name);
int32_t      parse_symtab_depth(parse_symtab_t *st, core_string_t ns_name);
parse_scope_t *parse_symtab_current_scope(parse_symtab_t *st, core_string_t ns_name);
