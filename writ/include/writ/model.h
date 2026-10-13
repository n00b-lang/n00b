#ifndef WRIT_MODEL_H
#define WRIT_MODEL_H

#include "writ/writ.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t writ_type_kind_t;
enum {
    WRIT_TYPE_VOID,
    WRIT_TYPE_INT,
    WRIT_TYPE_FLOAT,
    WRIT_TYPE_BOOL,
    WRIT_TYPE_POINTER,
    WRIT_TYPE_ARRAY,
    WRIT_TYPE_OPAQUE,
};

typedef uint32_t writ_mode_t;
enum {
    WRIT_MODE_IN,
    WRIT_MODE_OUT,
    WRIT_MODE_INOUT,
    WRIT_MODE_CONSUME,
    WRIT_MODE_KEEP,
    WRIT_MODE_MOVED,
    WRIT_MODE_UNKNOWN,
};

typedef uint32_t writ_extent_kind_t;
enum {
    WRIT_EXTENT_NONE,
    WRIT_EXTENT_COUNT,
    WRIT_EXTENT_BOUNDED,
};

typedef uint32_t writ_plan_role_t;
enum {
    WRIT_ROLE_INPUT,
    WRIT_ROLE_OUTPUT,
    WRIT_ROLE_INPUT_OUTPUT,
    WRIT_ROLE_RESULT,
    WRIT_ROLE_ERROR_DETAIL,
    WRIT_ROLE_PACK,
};

typedef uint32_t writ_plan_shape_t;
enum {
    WRIT_SHAPE_SCALAR,
    WRIT_SHAPE_VIEW,
    WRIT_SHAPE_HANDLE,
    WRIT_SHAPE_OPTIONAL,
    WRIT_SHAPE_BYTES,
    WRIT_SHAPE_TEXT,
    WRIT_SHAPE_ARRAY,
    WRIT_SHAPE_RECORD,
    WRIT_SHAPE_REST_PACK,
    WRIT_SHAPE_KEYWORD_PACK,
};

typedef uint32_t writ_after_call_t;
enum {
    WRIT_AFTER_NONE,
    WRIT_AFTER_BORROWED,
    WRIT_AFTER_CALLER_OWNS,
    WRIT_AFTER_CALLEE_OWNS,
    WRIT_AFTER_BORROWED_UNTIL,
    WRIT_AFTER_BORROWED_WITH,
    WRIT_AFTER_STATIC,
};

typedef uint32_t writ_pass_t;
enum {
    WRIT_PASS_VALUE,
    WRIT_PASS_IN_POINTER,
    WRIT_PASS_OUT_POINTER,
    WRIT_PASS_INOUT_POINTER,
    WRIT_PASS_VARIADIC_TAIL,
};

typedef uint32_t writ_part_t;
enum {
    WRIT_PART_WHOLE,
    WRIT_PART_POINTER,
    WRIT_PART_LENGTH,
    WRIT_PART_CAPACITY,
    WRIT_PART_CAPACITY_AND_LENGTH,
    WRIT_PART_ELEMENTS,
};

typedef uint32_t writ_local_kind_t;
enum {
    WRIT_LOCAL_BUFFER,
    WRIT_LOCAL_CELL,
    WRIT_LOCAL_TEXT,
};

typedef uint32_t writ_step_kind_t;
enum {
    WRIT_STEP_CHECK,
    WRIT_STEP_PASS,
    WRIT_STEP_SPLIT,
    WRIT_STEP_ENCODE_CSTR,
    WRIT_STEP_EXPAND,
    WRIT_STEP_ALLOC_OUT,
    WRIT_STEP_BIND_OUT,
    WRIT_STEP_CALL,
    WRIT_STEP_SIZE_QUERY,
    WRIT_STEP_SIZE_RETRY,
    WRIT_STEP_CLASSIFY,
    WRIT_STEP_FETCH_DETAIL,
    WRIT_STEP_LIFT_VALUE,
    WRIT_STEP_LIFT_BUFFER,
    WRIT_STEP_WRAP_HANDLE,
    WRIT_STEP_COPY_BORROWED,
    WRIT_STEP_MARK_CONSUMED,
    WRIT_STEP_RELEASE_LOCAL,
};

typedef uint32_t writ_phase_t;
enum {
    WRIT_PHASE_PREPARE,
    WRIT_PHASE_ATTEMPT,
    WRIT_PHASE_CLASSIFY,
    WRIT_PHASE_OUTCOME,
    WRIT_PHASE_FINALLY,
};

typedef uint32_t writ_outcome_kind_t;
enum {
    WRIT_OUTCOME_SUCCESS,
    WRIT_OUTCOME_FAILURE,
};

typedef uint32_t writ_language_t;
enum {
    WRIT_LANGUAGE_C,
};

typedef struct writ_type_view writ_type_view_t;
typedef struct writ_const_view writ_const_view_t;
typedef struct writ_slot_view writ_slot_view_t;
typedef struct writ_fn_view writ_fn_view_t;
typedef struct writ_library_view writ_library_view_t;
typedef struct writ_plan_item_view writ_plan_item_view_t;
typedef struct writ_plan_slot_view writ_plan_slot_view_t;
typedef struct writ_plan_local_view writ_plan_local_view_t;
typedef struct writ_plan_step_view writ_plan_step_view_t;
typedef struct writ_plan_outcome_view writ_plan_outcome_view_t;
typedef struct writ_plan_view writ_plan_view_t;

struct writ_type_view {
    size_t struct_size;
    uint32_t kind;
    writ_str_t name;
    writ_str_t c_spelling;
    uint32_t size;
    uint32_t align;
    bool is_signed;
    bool nullable;
    const writ_type_view_t *target;
};

struct writ_const_view {
    size_t struct_size;
    writ_str_t name;
    const writ_type_view_t *type;
    int64_t value;
    writ_span_t declared_at;
};

struct writ_slot_view {
    size_t struct_size;
    writ_str_t name;
    const writ_type_view_t *type;
    uint32_t mode;
    uint32_t extent_kind;
    uint32_t count_expr;
    uint32_t cap_expr;
    uint32_t len_expr;
    writ_span_t declared_at;
};

struct writ_fn_view {
    size_t struct_size;
    writ_str_t contract_name;
    writ_str_t link_symbol;
    size_t slots_count;
    const writ_slot_view_t *slots;
    const writ_type_view_t *result;
    bool is_pure;
    uint32_t sizing_expr;
    uint32_t error_expr;
    uint32_t via_expr;
    writ_span_t declared_at;
};

struct writ_library_view {
    size_t struct_size;
    writ_str_t name;
    size_t links_count;
    const writ_str_t *links;
    size_t headers_count;
    const writ_str_t *headers;
};

struct writ_plan_item_view {
    size_t struct_size;
    writ_str_t name;
    uint32_t role;
    uint32_t shape;
    uint32_t after_call;
    const writ_type_view_t *type;
    uint32_t contract_slot;
};

struct writ_plan_slot_view {
    size_t struct_size;
    const writ_type_view_t *c_type;
    uint32_t pass;
    uint32_t part;
    uint32_t item;
    uint32_t contract_slot;
};

struct writ_plan_local_view {
    size_t struct_size;
    uint32_t kind;
    const writ_type_view_t *type;
    uint32_t item;
    uint32_t capacity_expr;
    uint32_t capacity_item;
};

struct writ_plan_step_view {
    size_t struct_size;
    uint32_t kind;
    uint32_t phase;
    uint32_t outcome;
    uint32_t item;
    uint32_t slot;
    uint32_t second_slot;
    uint32_t local;
    uint32_t source_local;
    uint32_t expr;
};

struct writ_plan_outcome_view {
    size_t struct_size;
    writ_str_t name;
    uint32_t kind;
    uint32_t when_expr;
    uint32_t via_expr;
    bool retry;
};

struct writ_plan_view {
    size_t struct_size;
    const writ_fn_view_t *fn;
    size_t items_count;
    const writ_plan_item_view_t *items;
    size_t slots_count;
    const writ_plan_slot_view_t *slots;
    size_t locals_count;
    const writ_plan_local_view_t *locals;
    size_t steps_count;
    const writ_plan_step_view_t *steps;
    size_t outcomes_count;
    const writ_plan_outcome_view_t *outcomes;
};

#define WRIT_PLAN_NONE UINT32_MAX

const writ_library_view_t *writ_model_library(const writ_model_t *model);
size_t writ_model_fn_count(const writ_model_t *model);
const writ_fn_view_t *writ_model_fn_at(const writ_model_t *model, size_t index);
const writ_fn_view_t *writ_model_fn_named(const writ_model_t *model, writ_str_t name);
size_t writ_model_const_count(const writ_model_t *model);
const writ_const_view_t *writ_model_const_at(const writ_model_t *model, size_t index);
const writ_plan_view_t *writ_model_fn_plan(const writ_model_t *model, const writ_fn_view_t *fn);
writ_status_t writ_model_plan_render(const writ_model_t *model, const writ_plan_view_t *plan, writ_str_t *rendered);

#ifdef __cplusplus
}
#endif

#endif
