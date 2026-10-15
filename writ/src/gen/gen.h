#pragma once

#include "../core/model/model.h"
#include "../core/syntax/syntax.h"
#include "writ/gen.h"

typedef enum {
    WRIT_REPR_CSTR,
    WRIT_REPR_UTF8,
    WRIT_REPR_BYTES,
    WRIT_REPR_REST,
    WRIT_REPR_KEYWORDS,
} writ_repr_form_t;

typedef enum {
    WRIT_NAMING_FN,
    WRIT_NAMING_TYPE,
    WRIT_NAMING_CONST,
    WRIT_NAMING_FIELD,
    WRIT_NAMING_MODULE,
    WRIT_NAMING_COUNT,
} writ_naming_kind_t;

typedef struct {
    writ_sym_t       id;
    writ_repr_form_t form;
    writ_node_kind_t item;
    bool             zero_copy;
    uint32_t         node;
} writ_repr_t;

core_list_decl(writ_repr_t);
core_list_decl(writ_sym_t);

typedef struct {
    writ_sym_t contract;
    writ_str_t native;
} writ_prim_t;

core_list_decl(writ_prim_t);

typedef struct {
    writ_session_t           *session;
    writ_sym_t                name;
    core_list_t(writ_repr_t)  reprs;
    core_list_t(writ_prim_t)  prims;
    core_list_t(writ_sym_t)   reserved;
    writ_case_t               naming[WRIT_NAMING_COUNT];
    writ_str_t                handle_close;
    bool                      lifetimes;
    bool                      variadic_call;
} writ_profile_t;

const writ_profile_t *writ_profile_read(writ_unit_t *unit, writ_sym_t language);
const writ_repr_t    *writ_profile_repr(const writ_profile_t *profile, writ_sym_t id, writ_repr_form_t form);
const writ_repr_t    *writ_profile_default(const writ_profile_t *profile, writ_repr_form_t form);
writ_str_t            writ_profile_name(const writ_profile_t *profile, writ_naming_kind_t kind, writ_str_t name);
writ_str_t            writ_profile_prim(const writ_profile_t *profile, writ_str_t contract);
bool                  writ_fn_has_unknown_facts(const writ_fn_view_t *fn);

typedef struct {
    bool       bound;
    bool       raw;
    bool       is_static;
    writ_str_t name;
    writ_str_t owner;
    writ_str_t error;
    writ_str_t provide;
} writ_bound_fn_t;

const writ_model_t   *writ_binding_model(const writ_binding_t *b);
const writ_profile_t *writ_binding_profile(const writ_binding_t *b);
bool                  writ_binding_exports(const writ_binding_t *b);
writ_str_t            writ_binding_load(const writ_binding_t *b);
writ_str_t            writ_binding_header(const writ_binding_t *b);
writ_str_t            writ_binding_panic_value(const writ_binding_t *b);
writ_str_t            writ_binding_allocator(const writ_binding_t *b);
writ_bound_fn_t       writ_binding_fn(const writ_binding_t *b, size_t fn);
writ_str_t            writ_binding_slot_repr(const writ_binding_t *b, size_t fn, writ_str_t slot);
writ_str_t            writ_binding_type(const writ_binding_t *b, writ_str_t contract);
size_t                writ_binding_library_count(const writ_binding_t *b);
writ_str_t            writ_binding_library_at(const writ_binding_t *b, size_t index);
size_t                writ_binding_search_path_count(const writ_binding_t *b);
writ_str_t            writ_binding_search_path_at(const writ_binding_t *b, size_t index);

enum {
    WRIT_C_MAX_ARITY = 8,
};

typedef struct {
    const writ_model_t *model;
    writ_str_t          prefix;
    writ_str_t          upper;
    bool                normalized;
} writ_c_context_t;

bool writ_c_by_pointer(const writ_c_context_t *c, const writ_plan_item_view_t *item);

typedef struct {
    writ_str_t type;
    writ_str_t name;
} writ_c_param_t;

writ_str_t writ_c_type(const writ_c_context_t *c, const writ_type_view_t *type, bool constant);
bool       writ_c_is_text(const writ_type_view_t *type);
bool       writ_c_returns_directly(const writ_c_context_t *c, const writ_plan_view_t *plan);
writ_step_kind_t writ_c_lift_kind(const writ_plan_view_t *plan, uint32_t item);
bool       writ_c_offset_result(const writ_plan_view_t *plan, uint32_t item);
size_t     writ_c_item_params(const writ_c_context_t *c, const writ_plan_view_t *plan, uint32_t item,
                              writ_c_param_t params[2]);
writ_str_t writ_c_return_type(const writ_c_context_t *c, const writ_plan_view_t *plan);
bool       writ_c_is_input(const writ_plan_item_view_t *item);
size_t     writ_c_param_order(const writ_c_context_t *c, const writ_plan_view_t *plan, uint32_t *order);
void       writ_c_emit_signature(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan);
void       writ_c_emit_real_prototype(const writ_c_context_t *c, writ_emit_t *emit, const writ_fn_view_t *fn,
                                      writ_str_t name, bool variadic_tail, const char *terminator);
writ_str_t writ_c_callee(const writ_c_context_t *c, const writ_fn_view_t *fn);
writ_status_t writ_gen_conformance(const writ_c_context_t *c, const writ_output_t *output);
