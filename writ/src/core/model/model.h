#pragma once

#include "../syntax/syntax.h"
#include "writ/model.h"

typedef uint32_t writ_type_id;

#define WRIT_NONE UINT32_MAX

struct writ_platform {
    writ_session_t *session;
    writ_sym_t      triple;
    writ_sym_t      os;
    writ_sym_t      arch;
    writ_sym_t      abi;
    writ_sym_t      libc;
    writ_sym_t      stdlib;
    writ_sym_t      data_model;
    writ_sym_t      object_format;
    uint32_t        pointer_width;
    uint32_t        long_width;
    core_dict_t     versions;
    core_dict_t     features;
};

bool    writ_platform_fact(const writ_platform_t *platform, writ_sym_t name, int64_t *value);
int64_t writ_version_value(writ_str_t text);
bool    writ_glob_match(writ_str_t pattern, writ_str_t text);
size_t  writ_glob_literal_prefix(writ_str_t pattern);

typedef enum : uint8_t {
    WRIT_SCOPE_CONST = 1,
    WRIT_SCOPE_FN,
    WRIT_SCOPE_CHANNEL,
    WRIT_SCOPE_VAR,
} writ_scope_kind_t;

typedef struct {
    writ_type_kind_t kind;
    uint32_t         nullable;
    writ_type_id     target;
    writ_sym_t       name;
    uint32_t         count;
} writ_type_key_t;

typedef struct {
    writ_type_key_t key;
    bool            is_signed;
    uint32_t        size;
    uint32_t        align;
    writ_sym_t      c_spelling;
    uint32_t        decl;
    writ_loc_t      loc;
} writ_type_rec_t;

typedef enum : uint8_t {
    WRIT_REF_NONE,
    WRIT_REF_SLOT,
    WRIT_REF_RESULT,
    WRIT_REF_CONST,
    WRIT_REF_FN,
    WRIT_REF_NULL,
    WRIT_REF_SIZEOF,
    WRIT_REF_BUILTIN,
    WRIT_REF_CHANNEL,
    WRIT_REF_FIELD,
    WRIT_REF_FACT,
    WRIT_REF_ATOM,
    WRIT_REF_VERSION,
    WRIT_REF_NONE_OF,
    WRIT_REF_LIMIT,
} writ_ref_kind_t;

typedef enum : uint8_t {
    WRIT_REF_VALUE,
    WRIT_REF_BEFORE,
    WRIT_REF_AFTER,
} writ_ref_phase_t;

typedef enum : uint8_t {
    WRIT_BUILTIN_MIN,
    WRIT_BUILTIN_MAX,
} writ_builtin_t;

typedef struct {
    writ_ref_kind_t  kind;
    writ_ref_phase_t phase;
    uint32_t         index;
} writ_ref_t;

typedef struct {
    writ_sym_t   name;
    uint32_t     node;
    writ_type_id type;
    int64_t      value;
    writ_sym_t   text;
    uint32_t     owner;
    writ_loc_t   loc;
} writ_const_rec_t;

typedef struct {
    writ_sym_t         name;
    uint32_t           owner;
    writ_type_id       type;
    writ_mode_t        mode;
    writ_extent_kind_t extent_kind;
    uint32_t           count_expr;
    uint32_t           cap_expr;
    uint32_t           len_expr;
    writ_type_id       physical;
    writ_span_kind_t   span;
    uint32_t           span_node;
    writ_sym_t         span_with;
    bool               own;
    bool               null_if_empty;
    bool               is_private;
    writ_sym_t         release;
    writ_sym_t         alloc;
    uint32_t           offset_assert;
    uint32_t           offset;
    bool               userdata;
    writ_sym_t         userdata_for;
    writ_loc_t         userdata_loc;
    writ_loc_t         loc;
} writ_slot_rec_t;

typedef struct {
    writ_sym_t          name;
    writ_outcome_kind_t kind;
    uint32_t            when_expr;
    uint32_t            via_expr;
    bool                retry;
    bool                via_own;
    writ_sym_t          via_release;
    writ_loc_t          loc;
} writ_outcome_rec_t;

typedef struct {
    writ_sym_t       outcome;
    uint32_t         slot;
    writ_on_state_t  state;
    writ_span_kind_t span;
    uint32_t         span_node;
    writ_loc_t       loc;
} writ_on_rec_t;

typedef struct {
    uint32_t slot;
    uint32_t expr;
} writ_assign_rec_t;

typedef struct {
    writ_sym_t          name;
    uint32_t            node;
    writ_fn_kind_t      kind;
    uint32_t            first_slot;
    uint32_t            slot_count;
    bool                has_result;
    writ_type_id        result;
    bool                pure;
    writ_sizing_kind_t  sizing;
    uint32_t            sizing_expr;
    uint32_t            sizing_slot;
    uint32_t            first_assign;
    uint32_t            assign_count;
    uint32_t            first_outcome;
    uint32_t            outcome_count;
    uint32_t            first_on;
    uint32_t            on_count;
    writ_varargs_kind_t varargs;
    writ_type_id        varargs_type;
    uint32_t            varargs_end;
    uint32_t            varargs_count_slot;
    uint32_t            format_slot;
    writ_sym_t          source_name;
    bool                source_macro;
    writ_sym_t          link_symbol;
    bool                no_symbol;
    uint32_t            expands_node;
    writ_sym_t          expands_symbol;
    writ_thread_kind_t  thread;
    writ_sym_t          note;
    writ_loc_t          loc;
} writ_fn_rec_t;

typedef struct {
    writ_sym_t   name;
    writ_type_id type;
    uint32_t     first_field;
    uint32_t     field_count;
    bool         explicit_layout;
    uint32_t     pack;
    uint32_t     align_assert;
    uint32_t     size_assert;
    uint32_t     none_node;
    writ_loc_t   loc;
} writ_record_rec_t;

typedef struct {
    writ_sym_t         name;
    writ_type_id       type;
    writ_type_id       base;
    bool               has_none;
    int64_t            none_value;
    writ_sym_t         release;
    writ_thread_rule_t thread_rule;
    writ_loc_t         loc;
} writ_handle_rec_t;

typedef struct {
    writ_sym_t   name;
    writ_type_id type;
    writ_type_id base;
    uint32_t     first_item;
    uint32_t     item_count;
    bool         closed;
    bool         flags;
    writ_loc_t   loc;
} writ_enum_rec_t;

typedef struct {
    writ_sym_t          name;
    bool                per_handle;
    writ_type_id        handle;
    bool                reset;
    writ_channel_kind_t kind;
    uint32_t            source_expr;
    uint32_t            valid_until;
    writ_loc_t          loc;
} writ_channel_rec_t;

typedef struct {
    writ_sym_t name;
    writ_sym_t alloc;
    writ_sym_t release;
    writ_loc_t loc;
} writ_allocator_rec_t;

typedef struct {
    writ_sym_t   name;
    writ_type_id type;
    bool         is_mutable;
    writ_sym_t   link_symbol;
    writ_loc_t   loc;
} writ_var_rec_t;

typedef struct {
    writ_sym_t pattern;
    uint32_t   node;
    writ_loc_t loc;
} writ_convention_rec_t;

core_list_decl(writ_type_rec_t);
core_list_decl(writ_ref_t);
core_list_decl(writ_const_rec_t);
core_list_decl(writ_slot_rec_t);
core_list_decl(writ_outcome_rec_t);
core_list_decl(writ_on_rec_t);
core_list_decl(writ_assign_rec_t);
core_list_decl(writ_fn_rec_t);
core_list_decl(writ_record_rec_t);
core_list_decl(writ_handle_rec_t);
core_list_decl(writ_enum_rec_t);
core_list_decl(writ_channel_rec_t);
core_list_decl(writ_allocator_rec_t);
core_list_decl(writ_var_rec_t);
core_list_decl(writ_convention_rec_t);

typedef enum : uint8_t {
    WRIT_RUST_UNIT,
    WRIT_RUST_PRIM,
    WRIT_RUST_ENUM,
    WRIT_RUST_STRUCT,
    WRIT_RUST_OPAQUE,
    WRIT_RUST_STR,
    WRIT_RUST_SLICE,
    WRIT_RUST_STRING,
    WRIT_RUST_VEC,
} writ_rust_kind_t;

typedef enum : uint8_t {
    WRIT_RUST_FREE,
    WRIT_RUST_BY_REF,
    WRIT_RUST_BY_MUT,
    WRIT_RUST_BY_VALUE,
} writ_rust_receiver_t;

typedef struct {
    writ_rust_kind_t kind;
    writ_rust_kind_t element;
    writ_sym_t       name;
    bool             borrowed;
    bool             optional;
    bool             fallible;
} writ_rust_value_t;

typedef struct {
    writ_sym_t        name;
    writ_rust_value_t value;
} writ_rust_member_t;

typedef struct {
    writ_rust_kind_t   kind;
    writ_sym_t         name;
    writ_thread_rule_t rule;
    uint32_t           first;
    uint32_t           count;
} writ_rust_type_t;

typedef struct {
    writ_sym_t           owner;
    writ_sym_t           name;
    writ_sym_t           symbol;
    writ_sym_t           feature;
    writ_rust_receiver_t receiver;
    uint32_t             first;
    uint32_t             count;
    writ_rust_value_t    result;
} writ_rust_fn_t;

typedef struct {
    writ_sym_t name;
    writ_sym_t cargo;
} writ_rust_feature_t;

core_list_decl(writ_rust_member_t);
core_list_decl(writ_rust_type_t);
core_list_decl(writ_rust_fn_t);
core_list_decl(writ_rust_feature_t);

typedef struct {
    writ_session_t                  *session;
    writ_sym_t                       library;
    writ_sym_t                       crate;
    writ_sym_t                       version;
    writ_sym_t                       error;
    writ_str_t                       prefix;
    core_list_t(writ_rust_feature_t) features;
    core_list_t(writ_rust_type_t)    types;
    core_list_t(writ_rust_member_t)  members;
    core_list_t(writ_rust_fn_t)      fns;
} writ_rust_t;

bool        writ_unit_is_rust(const writ_unit_t *unit);
writ_rust_t *writ_rust_read(writ_unit_t *unit, core_list_t(uint32_t) *skipped);
writ_str_t  writ_rust_derive(const writ_rust_t *rust);
const writ_rust_type_t *writ_rust_type(const writ_rust_t *rust, writ_sym_t name);

struct writ_model {
    writ_session_t                    *session;
    writ_unit_t                       *unit;
    const writ_rust_t                 *rust;
    const writ_platform_t             *platform;
    writ_sym_t                         library;
    writ_sym_t                         encoding;
    core_list_t(uint32_t)              links;
    core_list_t(uint32_t)              headers;
    core_list_t(uint32_t)              defines;
    core_list_t(uint32_t)              include_paths;
    core_list_t(uint32_t)              platforms;
    core_list_t(writ_type_rec_t)       types;
    core_indexed_table_t              *type_index;
    core_dict_t                        type_names;
    core_list_t(writ_const_rec_t)      consts;
    core_list_t(writ_slot_rec_t)       slots;
    core_list_t(writ_outcome_rec_t)    outcomes;
    core_list_t(writ_on_rec_t)         ons;
    core_list_t(writ_assign_rec_t)     assigns;
    core_list_t(writ_fn_rec_t)         fns;
    core_list_t(writ_record_rec_t)     records;
    core_list_t(writ_handle_rec_t)     handles;
    core_list_t(writ_enum_rec_t)       enums;
    core_list_t(writ_channel_rec_t)    channels;
    core_list_t(writ_allocator_rec_t)  allocators;
    core_list_t(writ_var_rec_t)        vars;
    core_list_t(writ_convention_rec_t) conventions;
    core_list_t(uint32_t)              refines;
    core_dict_t                        scope;
    core_list_t(writ_ref_t)            refs;
    core_list_t(uint32_t)              uses;
    writ_library_view_t               *library_view;
    writ_type_view_t                  *type_views;
    writ_const_view_t                 *const_views;
    writ_slot_view_t                  *slot_views;
    writ_fn_view_t                    *fn_views;
    writ_record_view_t                *record_views;
    writ_handle_view_t                *handle_views;
    writ_enum_view_t                  *enum_views;
    writ_channel_view_t               *channel_views;
    const writ_plan_view_t           **plan_views;
};

typedef enum {
#define WRIT_DEFAULT(id, value, section) WRIT_DEFAULT_##id,
#include "defaults.def"
#undef WRIT_DEFAULT
} writ_default_t;

uint32_t writ_default(writ_default_t id);

void                   writ_types_init(writ_model_t *model);
writ_type_id           writ_type_intern(writ_model_t *model, writ_type_key_t key);
writ_type_id           writ_type_named(const writ_model_t *model, writ_sym_t name);
bool                   writ_type_declare(writ_model_t *model, writ_sym_t name, writ_type_id type, writ_loc_t loc);
writ_type_id           writ_type_builtin(writ_model_t *model, const char *name);
const writ_type_rec_t *writ_type(const writ_model_t *model, writ_type_id id);
writ_type_id           writ_type_physical(const writ_model_t *model, writ_type_id id);
bool                   writ_type_is_integer(const writ_model_t *model, writ_type_id id);
bool                   writ_type_is_pointer(const writ_model_t *model, writ_type_id id);
void                   writ_type_layout_records(writ_model_t *model);

typedef struct writ_expr_env writ_expr_env_t;
struct writ_expr_env {
    bool (*value)(writ_expr_env_t *env, writ_ref_t ref, uint32_t expr, int64_t *out);
    bool (*call)(writ_expr_env_t *env, uint32_t fn, const int64_t *args, uint32_t count, int64_t *out);
    void (*print_ref)(writ_expr_env_t *env, writ_ref_t ref, uint32_t expr, core_buffer_t *out);
};

typedef enum : uint8_t {
    WRIT_EXPR_SCOPE_FN,
    WRIT_EXPR_SCOPE_RECORD,
    WRIT_EXPR_SCOPE_CONSTANT,
    WRIT_EXPR_SCOPE_CONDITION,
    WRIT_EXPR_SCOPE_CHANNEL,
} writ_expr_scope_t;

bool         writ_expr_check(writ_model_t *model, uint32_t fn, uint32_t expr);
bool         writ_expr_check_detail(writ_model_t *model, uint32_t fn, uint32_t expr);
bool         writ_expr_check_in(writ_model_t *model, writ_expr_scope_t scope, uint32_t fn, uint32_t expr);
bool         writ_expr_eval(const writ_model_t *model, uint32_t expr, writ_expr_env_t *env, int64_t *out);
bool         writ_condition_holds(writ_model_t *model, uint32_t expr);
void         writ_expr_print(const writ_model_t *model, uint32_t expr, writ_language_t language,
                             writ_expr_env_t *env, core_buffer_t *out);
writ_ref_t   writ_expr_ref(const writ_model_t *model, uint32_t expr);
bool         writ_expr_mentions(const writ_model_t *model, uint32_t expr, writ_ref_kind_t kind, uint32_t index);
writ_type_id writ_expr_type(const writ_model_t *model, uint32_t expr);

bool                   writ_scope_lookup(const writ_model_t *model, writ_sym_t name, writ_scope_kind_t want,
                                         uint32_t *index);
const writ_slot_rec_t *writ_fn_slot(const writ_model_t *model, const writ_fn_rec_t *fn, uint32_t index);
uint32_t               writ_fn_slot_named(const writ_model_t *model, const writ_fn_rec_t *fn, writ_sym_t name);
bool                   writ_is_text(const writ_model_t *model, writ_sym_t sym, const char *text);

void                    writ_model_check(writ_model_t *model);
void                    writ_model_build_views(writ_model_t *model);
const writ_plan_view_t *writ_plan_build(writ_model_t *model, uint32_t fn);
uint32_t                writ_plan_cell_for_slot(const writ_plan_view_t *plan, uint32_t slot);
uint32_t                writ_plan_contract_slot(const writ_model_t *model, const writ_plan_view_t *plan,
                                                writ_ref_t ref);
uint32_t                writ_model_fn_index(const writ_model_t *model, const writ_fn_view_t *fn);
