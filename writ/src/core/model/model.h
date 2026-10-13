#pragma once

#include "../syntax/syntax.h"
#include "writ/model.h"

typedef uint32_t writ_type_id;

enum {
    WRIT_SCOPE_CONST = 1,
    WRIT_SCOPE_FN    = 2,
};

typedef struct {
    writ_type_kind_t kind;
    uint32_t         nullable;
    writ_type_id     target;
    writ_sym_t       name;
} writ_type_key_t;

typedef struct {
    writ_type_key_t key;
    bool            is_signed;
    uint32_t        size;
    uint32_t        align;
    writ_sym_t      c_spelling;
} writ_type_rec_t;

typedef enum : uint8_t {
    WRIT_REF_NONE,
    WRIT_REF_SLOT,
    WRIT_REF_RESULT,
    WRIT_REF_CONST,
    WRIT_REF_FN,
} writ_ref_kind_t;

typedef enum : uint8_t {
    WRIT_REF_VALUE,
    WRIT_REF_BEFORE,
    WRIT_REF_AFTER,
} writ_ref_phase_t;

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
    writ_loc_t   loc;
} writ_const_rec_t;

typedef struct {
    writ_sym_t         name;
    uint32_t           fn;
    writ_type_id       type;
    writ_mode_t        mode;
    writ_extent_kind_t extent_kind;
    uint32_t           count_expr;
    uint32_t           cap_expr;
    uint32_t           len_expr;
    writ_loc_t         loc;
} writ_slot_rec_t;

typedef struct {
    writ_sym_t   name;
    uint32_t     node;
    uint32_t     first_slot;
    uint32_t     slot_count;
    writ_type_id result;
    bool         pure;
    uint32_t     sizing_expr;
    uint32_t     error_expr;
    uint32_t     via_expr;
    writ_loc_t   loc;
} writ_fn_rec_t;

core_list_decl(writ_type_rec_t);
core_list_decl(writ_ref_t);
core_list_decl(writ_const_rec_t);
core_list_decl(writ_slot_rec_t);
core_list_decl(writ_fn_rec_t);

struct writ_model {
    writ_session_t                *session;
    writ_unit_t                   *unit;
    writ_sym_t                     library;
    core_list_t(uint32_t)          links;
    core_list_t(uint32_t)          headers;
    core_list_t(writ_type_rec_t)   types;
    core_indexed_table_t          *type_index;
    core_dict_t                    type_names;
    core_list_t(writ_const_rec_t)  consts;
    core_list_t(writ_slot_rec_t)   slots;
    core_list_t(writ_fn_rec_t)     fns;
    core_dict_t                    scope;
    core_list_t(writ_ref_t)        refs;
    writ_library_view_t           *library_view;
    writ_type_view_t              *type_views;
    writ_const_view_t             *const_views;
    writ_fn_view_t                *fn_views;
    const writ_plan_view_t       **plan_views;
};

typedef enum {
#define WRIT_DEFAULT(id, value, section) WRIT_DEFAULT_##id,
#include "defaults.def"
#undef WRIT_DEFAULT
} writ_default_t;

uint32_t writ_default(writ_default_t id);

void         writ_types_init(writ_model_t *model);
writ_type_id writ_type_intern(writ_model_t *model, writ_type_key_t key);
writ_type_id writ_type_named(writ_model_t *model, writ_sym_t name);
const writ_type_rec_t *writ_type(const writ_model_t *model, writ_type_id id);
bool         writ_type_is_integer(const writ_model_t *model, writ_type_id id);

typedef struct writ_expr_env writ_expr_env_t;
struct writ_expr_env {
    bool (*value)(writ_expr_env_t *env, writ_ref_t ref, int64_t *out);
    bool (*call)(writ_expr_env_t *env, uint32_t fn, const int64_t *args, uint32_t count, int64_t *out);
    void (*print_ref)(writ_expr_env_t *env, writ_ref_t ref, core_buffer_t *out);
};

bool       writ_expr_check(writ_model_t *model, uint32_t fn, uint32_t expr);
bool       writ_expr_eval(const writ_model_t *model, uint32_t expr, writ_expr_env_t *env, int64_t *out);
void       writ_expr_print(const writ_model_t *model, uint32_t expr, writ_language_t language,
                           writ_expr_env_t *env, core_buffer_t *out);
writ_ref_t writ_expr_ref(const writ_model_t *model, uint32_t expr);
bool       writ_expr_mentions(const writ_model_t *model, uint32_t expr, writ_ref_kind_t kind, uint32_t index);

void writ_model_check(writ_model_t *model);
void writ_model_build_views(writ_model_t *model);
const writ_plan_view_t *writ_plan_build(writ_model_t *model, uint32_t fn);
uint32_t                writ_plan_cell_for_slot(const writ_plan_view_t *plan, uint32_t slot);
uint32_t                writ_plan_contract_slot(const writ_model_t *model, const writ_plan_view_t *plan,
                                                writ_ref_t ref);
