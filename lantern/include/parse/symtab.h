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

typedef enum {
    PARSE_SYM_PHASE_NONE      = 0,
    PARSE_SYM_PHASE_COMPTIME  = 1u << 0,
    PARSE_SYM_PHASE_RUNTIME   = 1u << 1,
    PARSE_SYM_PHASE_BOTH      = PARSE_SYM_PHASE_COMPTIME
                              | PARSE_SYM_PHASE_RUNTIME,
} parse_sym_phase_t;

typedef enum {
    PARSE_SYM_ALIAS_MAY,
    PARSE_SYM_ALIAS_MUST,
    PARSE_SYM_ALIAS_DERIVED,
} parse_sym_alias_kind_t;

typedef enum {
    PARSE_SYM_STORAGE_UNSPECIFIED,
    PARSE_SYM_STORAGE_COMPTIME,
    PARSE_SYM_STORAGE_AUTOMATIC,
    PARSE_SYM_STORAGE_MODULE,
    PARSE_SYM_STORAGE_THREAD,
    PARSE_SYM_STORAGE_DYNAMIC,
} parse_sym_storage_duration_t;

typedef enum {
    PARSE_SYM_LIVENESS_UNKNOWN,
    PARSE_SYM_LIVENESS_DEAD,
    PARSE_SYM_LIVENESS_LIVE,
} parse_sym_liveness_t;

typedef struct {
    parse_sym_entry_t    *other;
    parse_sym_alias_kind_t kind;
    uint64_t              derived_at;
    uint64_t              last_use;
    bool                  has_last_use;
    bool                  escapes;
} parse_sym_alias_t;

typedef struct {
    void *function;
    void *binding;
    void *from;
    void *to;
    parse_sym_phase_t phase;
    parse_sym_liveness_t direct;
    parse_sym_liveness_t aliases;
    bool is_exit;
    bool ends_live_range;
} parse_sym_lifetime_edge_t;

typedef struct {
    parse_sym_phase_t phase;
    void *function;
    void *binding;
} parse_sym_function_binding_t;

typedef struct {
    parse_sym_phase_t phase;
    void *function;
    void *binding;
    void *point;
    bool at_entry;
} parse_sym_lifetime_start_t;

typedef struct {
    parse_sym_phase_t phase;
    void *function;
    void *binding;
    void *point;
    void *from;
    void *to;
    bool on_edge;
} parse_sym_lifetime_end_t;

typedef struct {
    parse_sym_phase_t phase;
    void *function;
    void *binding;
    size_t start_count;
    size_t edge_count;
    size_t end_count;
    bool complete;
} parse_sym_lifetime_range_t;

typedef struct {
    uint64_t region;
    uint64_t start_point;
    uint64_t end_point;
    uint64_t finalization_point;
    bool     has_region;
    bool     has_start;
    bool     has_end;
    bool     has_finalization_point;
    bool     escapes;
    parse_sym_lifetime_start_t *starts;
    size_t start_count;
    size_t start_capacity;
    parse_sym_lifetime_range_t *ranges;
    size_t range_count;
    size_t range_capacity;
    parse_sym_lifetime_end_t *ends;
    size_t end_count;
    size_t end_capacity;
    parse_sym_lifetime_edge_t *cfg_edges;
    size_t cfg_edge_count;
    size_t cfg_edge_capacity;
} parse_sym_lifetime_t;

struct parse_sym_entry_t {
    core_string_t             name;
    parse_sym_kind_t           kind;
    parse_sym_mutability_t     mutability;
    core_string_t             direction;
    core_string_t             pass_by;
    int32_t                  scope_depth;
    parse_tree_t        *decl_node;
    parse_tree_t       **definition_nodes;
    size_t                definition_count;
    size_t                definition_capacity;
    parse_tree_t             **use_nodes;
    size_t                     use_count;
    size_t                     use_capacity;
    bool                       type_is_inferred;
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
    core_string_t             inherits_name;
    core_list_t(core_string_t) implements;
    bool                     is_field;
    bool                     is_method;
    bool                     is_static;
    bool                     is_thread_local;
    bool                     is_abstract;
    bool                     is_optional;
    bool                     is_variadic;
    uint32_t                 uses;
    th_mode                  mode;
    bool                     has_mode;
    uint64_t                 bit_offset;
    uint64_t                 bit_width;
    bool                     has_bit_offset;
    bool                     has_bit_width;
    parse_sym_phase_t        phases;
    parse_sym_phase_t        mutable_phases;
    parse_sym_phase_t        initializer_phases;
    void                    *comptime_value;
    void                    *runtime_initializer;
    bool                     has_comptime_value;
    bool                     has_runtime_initializer;
    void                    *comptime_binding;
    void                    *runtime_binding;
    bool                     has_comptime_binding;
    bool                     has_runtime_binding;
    parse_sym_function_binding_t *function_bindings;
    size_t function_binding_count;
    size_t function_binding_capacity;
    core_string_t            requested_storage_class;
    core_string_t            selected_storage_class;
    bool                     storage_class_required;
    parse_sym_storage_duration_t storage_duration;
    parse_sym_lifetime_t     lifetime;
    parse_sym_alias_t       *aliases;
    size_t                   alias_count;
    size_t                   alias_capacity;
    parse_sym_alias_t       *aliased_by;
    size_t                   aliased_by_count;
    size_t                   aliased_by_capacity;
    bool                     alias_analysis_complete;
    void                    *namespaced_bindings;
    void                    *user_data;
};

struct parse_scope_t {
    parse_scope_t      *parent;
    /* The matching scope in the default namespace. */
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
    parse_namespace_t **namespaces;
    int32_t           ns_count;
    int32_t           ns_cap;
    int32_t           next_scope_id;
    parse_scope_t      *all_scopes;
    core_dict_t       *scope_by_node;
    core_dict_t       *ns_map;
};

/* Returning false stops the visit; otherwise every scope in the namespace is
 * visited. Iteration order is unspecified. */
typedef bool (*parse_symtab_scope_visitor_fn)(parse_scope_t *scope,
                                             void *context);
/* Returning false stops symbol visitation and makes the API return false. */
typedef bool (*parse_symtab_symbol_visitor_fn)(parse_sym_entry_t *symbol,
                                               void *context);

parse_symtab_t *parse_symtab_new(void);
void          parse_symtab_free(parse_symtab_t *st);

parse_namespace_t *parse_symtab_ns(parse_symtab_t *st, core_string_t ns_name);
bool parse_symtab_visit_namespace_scopes(
    parse_symtab_t *st, core_string_t ns_name,
    parse_symtab_scope_visitor_fn visitor, void *context);
/* Visit same-name symbols declared in the corresponding lexical scope across
 * namespaces. This does not search parent scopes or impose collision policy. */
bool parse_symtab_visit_name_across_namespaces(
    parse_symtab_t *st, parse_scope_t *scope, core_string_t name,
    parse_symtab_symbol_visitor_fn visitor, void *context);

void parse_symtab_push_scope(parse_symtab_t *st, core_string_t ns_name,
                           core_string_t scope_name);
void parse_symtab_pop_scope(parse_symtab_t *st, core_string_t ns_name);
/* Create/pop the same lexical scope in every namespace. */
void parse_symtab_push_scope_all(parse_symtab_t *st,
                                 core_string_t scope_name);
void parse_symtab_pop_scope_all(parse_symtab_t *st);

void parse_symtab_enter_scope(parse_symtab_t *st, core_string_t ns_name,
                            parse_scope_t *scope);
void parse_symtab_leave_scope(parse_symtab_t *st, core_string_t ns_name);
void parse_symtab_enter_scope_all(parse_symtab_t *st, parse_scope_t *scope);
void parse_symtab_leave_scope_all(parse_symtab_t *st);
/* Find the namespace-specific peer for a lexical scope. */
parse_scope_t *parse_symtab_scope_in_namespace(parse_symtab_t *st,
                                               parse_scope_t *scope,
                                               core_string_t ns_name);

parse_sym_entry_t *parse_symtab_add(parse_symtab_t *st, core_string_t ns_name,
                                   core_string_t name, parse_sym_kind_t kind,
                                   parse_tree_t *decl_node);
/* Namespaced opaque semantic attachments are borrowed by the symbol table.
 * They may be populated during analysis, then read concurrently after the
 * symbol graph is finalized. */
bool parse_sym_set_binding(parse_sym_entry_t *symbol, core_string_t key,
                           void *value);
void *parse_sym_binding(const parse_sym_entry_t *symbol, core_string_t key);
/* Associate a semantic shadow/view with the symbol supplying its runtime
 * storage. Ordinary lexical shadowing leaves this unset. */
bool parse_sym_set_binding_origin(parse_sym_entry_t *symbol,
                                 parse_sym_entry_t *origin);
parse_sym_entry_t *parse_sym_binding_origin(const parse_sym_entry_t *symbol);
bool parse_sym_add_definition(parse_sym_entry_t *symbol, parse_tree_t *node);
bool parse_sym_add_use(parse_sym_entry_t *symbol, parse_tree_t *node);
bool parse_sym_remove_use(parse_sym_entry_t *symbol, parse_tree_t *node);
void parse_sym_mark_type_inferred(parse_sym_entry_t *symbol);

void parse_sym_set_phases(parse_sym_entry_t *symbol,
                          parse_sym_phase_t available,
                          parse_sym_phase_t mutable_in,
                          parse_sym_phase_t initializable_in);
bool parse_sym_can_read(const parse_sym_entry_t *symbol,
                        parse_sym_phase_t phase);
bool parse_sym_can_write(const parse_sym_entry_t *symbol,
                         parse_sym_phase_t phase);
bool parse_sym_can_initialize(const parse_sym_entry_t *symbol,
                              parse_sym_phase_t phase);
void parse_sym_set_phase_values(parse_sym_entry_t *symbol,
                                void *comptime_value,
                                void *runtime_initializer);
/* Use explicit presence flags when a valid phase value may have a null
 * payload; the convenience setter above treats null as absent. */
void parse_sym_set_phase_values_with_presence(
    parse_sym_entry_t *symbol, bool has_comptime_value,
    void *comptime_value, bool has_runtime_initializer,
    void *runtime_initializer);
bool parse_sym_get_comptime_value(const parse_sym_entry_t *symbol,
                                  void **value);
bool parse_sym_get_runtime_initializer(const parse_sym_entry_t *symbol,
                                      void **value);
/* Bindings are borrowed phase-specific IR/source handles, not serialized
 * values. Passing null clears that phase's binding. */
bool parse_sym_set_phase_binding(parse_sym_entry_t *symbol,
                                 parse_sym_phase_t phase, void *binding);
void *parse_sym_phase_binding(const parse_sym_entry_t *symbol,
                              parse_sym_phase_t phase);
bool parse_sym_set_function_binding(parse_sym_entry_t *symbol,
                                    parse_sym_phase_t phase,
                                    void *function, void *binding);
size_t parse_sym_function_binding_count(const parse_sym_entry_t *symbol,
                                        parse_sym_phase_t phase);
const parse_sym_function_binding_t *parse_sym_function_binding_at(
    const parse_sym_entry_t *symbol, parse_sym_phase_t phase, size_t index);
void parse_sym_set_storage_class(parse_sym_entry_t *symbol,
                                core_string_t requested,
                                bool required,
                                core_string_t selected);
void parse_sym_set_storage_duration(
    parse_sym_entry_t *symbol, parse_sym_storage_duration_t duration);
const char *parse_sym_storage_duration_name(
    parse_sym_storage_duration_t duration);
void parse_sym_set_lifetime(parse_sym_entry_t *symbol,
                            parse_sym_lifetime_t lifetime);
/* Start anchors are borrowed IR points, keyed by owning function and binding.
 * Multiple anchors allow mutually exclusive binding initializations. */
bool parse_sym_set_lifetime_start(parse_sym_entry_t *symbol,
                                  parse_sym_lifetime_start_t start);
size_t parse_sym_lifetime_start_count(const parse_sym_entry_t *symbol);
const parse_sym_lifetime_start_t *parse_sym_lifetime_start_at(
    const parse_sym_entry_t *symbol, size_t index);
bool parse_sym_begin_lifetime_range(parse_sym_entry_t *symbol,
                                    parse_sym_phase_t phase,
                                    void *function, void *binding);
bool parse_sym_finish_lifetime_range(parse_sym_entry_t *symbol,
                                     parse_sym_phase_t phase,
                                     void *function, void *binding,
                                     bool complete);
size_t parse_sym_lifetime_range_count(const parse_sym_entry_t *symbol);
const parse_sym_lifetime_range_t *parse_sym_lifetime_range_at(
    const parse_sym_entry_t *symbol, size_t index);
bool parse_sym_set_lifetime_end(parse_sym_entry_t *symbol,
                                parse_sym_lifetime_end_t end);
size_t parse_sym_lifetime_end_count(const parse_sym_entry_t *symbol);
const parse_sym_lifetime_end_t *parse_sym_lifetime_end_at(
    const parse_sym_entry_t *symbol, size_t index);
/* CFG nodes and owners are opaque and borrowed from the frontend's IR. One
 * entry records a binding's value/alias liveness for an edge or terminal exit. */
bool parse_sym_set_lifetime_edge(parse_sym_entry_t *symbol,
                                 parse_sym_lifetime_edge_t edge);
size_t parse_sym_lifetime_edge_count(const parse_sym_entry_t *symbol);
const parse_sym_lifetime_edge_t *parse_sym_lifetime_edge_at(
    const parse_sym_entry_t *symbol, size_t index);
bool parse_symtab_add_alias(parse_sym_entry_t *alias,
                            parse_sym_entry_t *target,
                            parse_sym_alias_kind_t kind,
                            uint64_t derived_at,
                            bool escapes);
bool parse_symtab_update_alias_use(parse_sym_entry_t *alias,
                                   parse_sym_entry_t *target,
                                   parse_sym_alias_kind_t kind,
                                   uint64_t derived_at,
                                   uint64_t last_use,
                                   bool escapes);
void parse_sym_set_alias_analysis_complete(parse_sym_entry_t *symbol,
                                           bool complete);
/* Points use one monotonically ordered analysis timeline. This is
 * conservative: incomplete, unknown, or escaping aliases make finalization
 * unsafe. */
bool parse_sym_can_finalize_at(const parse_sym_entry_t *symbol,
                               uint64_t point);
size_t parse_sym_alias_count(const parse_sym_entry_t *symbol);
const parse_sym_alias_t *parse_sym_alias_at(const parse_sym_entry_t *symbol,
                                            size_t index);
size_t parse_sym_aliased_by_count(const parse_sym_entry_t *symbol);
const parse_sym_alias_t *parse_sym_aliased_by_at(
    const parse_sym_entry_t *symbol, size_t index);

parse_sym_entry_t *parse_symtab_lookup(parse_symtab_t *st, core_string_t ns_name,
                                   core_string_t name);

parse_sym_entry_t *parse_symtab_lookup_scoped(parse_symtab_t *st, parse_scope_t *scope,
                                          core_string_t ns_name,
                                          core_string_t name);

parse_sym_entry_t *parse_symtab_lookup_in(parse_scope_t *scope, core_string_t name);

/* Resolve a single-name reference node in its attached lexical scope. */
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
