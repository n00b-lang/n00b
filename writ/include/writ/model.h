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
    WRIT_TYPE_RECORD,
    WRIT_TYPE_ENUM,
    WRIT_TYPE_HANDLE,
    WRIT_TYPE_CSTR,
    WRIT_TYPE_VA_LIST,
    WRIT_TYPE_CALLBACK,
    WRIT_TYPE_VIEW,
    WRIT_TYPE_INLINE,
    WRIT_TYPE_UNION,
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
    WRIT_EXTENT_UNTIL,
    WRIT_EXTENT_UNKNOWN,
};

typedef uint32_t writ_span_kind_t;
enum {
    WRIT_SPAN_NONE,
    WRIT_SPAN_CALL,
    WRIT_SPAN_STATIC,
    WRIT_SPAN_STATIC_PER_THREAD,
    WRIT_SPAN_WITH,
    WRIT_SPAN_UNTIL,
    WRIT_SPAN_ONCE,
    WRIT_SPAN_UNKNOWN,
};

typedef uint32_t writ_event_kind_t;
enum {
    WRIT_EVENT_CALL,
    WRIT_EVENT_RELEASE,
    WRIT_EVENT_ANY,
};

typedef uint32_t writ_event_arg_kind_t;
enum {
    WRIT_EVENT_ARG_SLOT,
    WRIT_EVENT_ARG_RESULT,
    WRIT_EVENT_ARG_EXPR,
};

typedef uint32_t writ_on_state_t;
enum {
    WRIT_ON_OWN,
    WRIT_ON_UNSET,
    WRIT_ON_PARTIAL,
    WRIT_ON_NOT_CONSUMED,
    WRIT_ON_SPAN,
};

typedef uint32_t writ_fn_kind_t;
enum {
    WRIT_FN_FUNCTION,
    WRIT_FN_CALLBACK,
};

typedef uint32_t writ_sizing_kind_t;
enum {
    WRIT_SIZING_NONE,
    WRIT_SIZING_NEEDS,
    WRIT_SIZING_QUERY,
    WRIT_SIZING_RETRY,
};

typedef uint32_t writ_varargs_kind_t;
enum {
    WRIT_VARARGS_NONE,
    WRIT_VARARGS_OPAQUE,
    WRIT_VARARGS_OF,
    WRIT_VARARGS_UNTIL,
    WRIT_VARARGS_COUNT,
    WRIT_VARARGS_FORMAT,
    WRIT_VARARGS_VA_LIST,
};

typedef uint32_t writ_thread_kind_t;
enum {
    WRIT_THREAD_UNSTATED,
    WRIT_THREAD_ANY,
    WRIT_THREAD_MAIN,
    WRIT_THREAD_SAME,
    WRIT_THREAD_SAME_AS,
};

typedef uint32_t writ_thread_rule_t;
enum {
    WRIT_THREAD_RULE_SERIALIZED,
    WRIT_THREAD_RULE_SHARED,
    WRIT_THREAD_RULE_AFFINE,
};

typedef uint32_t writ_channel_kind_t;
enum {
    WRIT_CHANNEL_VALUE,
    WRIT_CHANNEL_TEXT,
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
    WRIT_SHAPE_CALLBACK,
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
    WRIT_PART_USERDATA,
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
    WRIT_STEP_PASS_CONST,
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
    WRIT_LANGUAGE_N00B,
};

typedef uint32_t writ_presence_t;
enum {
    WRIT_PRESENCE_NONE,
    WRIT_PRESENCE_MASK,
    WRIT_PRESENCE_SIZE,
    WRIT_PRESENCE_SENTINEL,
    WRIT_PRESENCE_ALWAYS,
};

typedef struct writ_type_view writ_type_view_t;
typedef struct writ_const_view writ_const_view_t;
typedef struct writ_slot_view writ_slot_view_t;
typedef struct writ_event_arg_view writ_event_arg_view_t;
typedef struct writ_event_view writ_event_view_t;
typedef struct writ_policy_arm_view writ_policy_arm_view_t;
typedef struct writ_outcome_view writ_outcome_view_t;
typedef struct writ_fn_view writ_fn_view_t;
typedef struct writ_union_arm_view writ_union_arm_view_t;
typedef struct writ_record_view writ_record_view_t;
typedef struct writ_handle_view writ_handle_view_t;
typedef struct writ_enum_view writ_enum_view_t;
typedef struct writ_channel_view writ_channel_view_t;
typedef struct writ_library_view writ_library_view_t;
typedef struct writ_plan_item_view writ_plan_item_view_t;
typedef struct writ_plan_slot_view writ_plan_slot_view_t;
typedef struct writ_plan_local_view writ_plan_local_view_t;
typedef struct writ_plan_step_view writ_plan_step_view_t;
typedef struct writ_plan_outcome_view writ_plan_outcome_view_t;
typedef struct writ_plan_assign_view writ_plan_assign_view_t;
typedef struct writ_plan_view writ_plan_view_t;

struct writ_type_view {
    size_t struct_size;
    writ_type_kind_t kind;
    writ_str_t name;
    writ_str_t c_spelling;
    uint32_t size;
    uint32_t align;
    bool is_signed;
    bool nullable;
    const writ_type_view_t *target;
    uint32_t count;
    uint32_t decl;
};

struct writ_const_view {
    size_t struct_size;
    writ_str_t name;
    const writ_type_view_t *type;
    int64_t value;
    writ_str_t text;
    writ_span_t declared_at;
};

struct writ_slot_view {
    size_t struct_size;
    writ_str_t name;
    const writ_type_view_t *type;
    writ_mode_t mode;
    writ_extent_kind_t extent_kind;
    uint32_t count_expr;
    uint32_t cap_expr;
    uint32_t len_expr;
    writ_span_kind_t span;
    writ_str_t span_with;
    bool own;
    writ_str_t release;
    writ_str_t alloc;
    bool null_if_empty;
    bool is_private;
    uint32_t offset;
    writ_span_t declared_at;
    const writ_type_view_t *physical;
    bool userdata;
    writ_str_t userdata_for;
    bool has_default;
    int64_t default_value;
    bool has_none;
    int64_t none_value;
    size_t policy_arms_count;
    const writ_policy_arm_view_t *policy_arms;
    writ_str_t span_by;
    size_t span_events_count;
    const writ_event_view_t *span_events;
    uint32_t tag_field;
};

struct writ_event_arg_view {
    size_t struct_size;
    writ_event_arg_kind_t kind;
    writ_str_t name;
    uint32_t expr;
};

struct writ_event_view {
    size_t struct_size;
    writ_event_kind_t kind;
    writ_str_t function;
    bool next;
    bool on_thread;
    size_t args_count;
    const writ_event_arg_view_t *args;
    writ_span_t declared_at;
};

struct writ_policy_arm_view {
    size_t struct_size;
    writ_str_t constant;
    bool is_else;
    int64_t value;
    writ_str_t target;
    bool has_mode;
    writ_mode_t mode;
    writ_span_kind_t span;
    writ_str_t span_with;
    writ_str_t release;
    bool own;
    writ_span_t declared_at;
    size_t span_events_count;
    const writ_event_view_t *span_events;
};

struct writ_outcome_view {
    size_t struct_size;
    writ_str_t name;
    writ_outcome_kind_t kind;
    uint32_t when_expr;
    uint32_t via_expr;
    bool retry;
};

struct writ_fn_view {
    size_t struct_size;
    writ_str_t contract_name;
    writ_str_t source_name;
    writ_str_t link_symbol;
    writ_fn_kind_t kind;
    size_t slots_count;
    const writ_slot_view_t *slots;
    const writ_slot_view_t *result;
    bool is_pure;
    writ_sizing_kind_t sizing;
    uint32_t sizing_expr;
    uint32_t sizing_slot;
    size_t outcomes_count;
    const writ_outcome_view_t *outcomes;
    writ_varargs_kind_t varargs;
    const writ_type_view_t *varargs_type;
    uint32_t varargs_end_expr;
    uint32_t varargs_count_slot;
    uint32_t format_slot;
    writ_str_t expands_symbol;
    uint32_t expands_node;
    writ_thread_kind_t thread;
    writ_span_t declared_at;
    uint32_t keywords_slot;
};

struct writ_union_arm_view {
    size_t struct_size;
    writ_str_t constant;
    int64_t value;
    uint32_t field;
    writ_span_t declared_at;
};

struct writ_record_view {
    size_t struct_size;
    writ_str_t name;
    const writ_type_view_t *type;
    size_t fields_count;
    const writ_slot_view_t *fields;
    writ_span_t declared_at;
    writ_presence_t presence;
    uint32_t presence_field;
    size_t arms_count;
    const writ_union_arm_view_t *arms;
};

struct writ_handle_view {
    size_t struct_size;
    writ_str_t name;
    const writ_type_view_t *type;
    const writ_type_view_t *base;
    bool has_none;
    int64_t none_value;
    writ_str_t release;
    writ_thread_rule_t thread_rule;
    writ_span_t declared_at;
};

struct writ_enum_view {
    size_t struct_size;
    writ_str_t name;
    const writ_type_view_t *type;
    const writ_type_view_t *base;
    size_t items_count;
    const writ_const_view_t *items;
    bool closed;
    bool is_flags;
    writ_span_t declared_at;
};

struct writ_channel_view {
    size_t struct_size;
    writ_str_t name;
    bool per_handle;
    const writ_type_view_t *handle;
    bool reset_before_call;
    writ_channel_kind_t kind;
    uint32_t source_expr;
    writ_span_t declared_at;
};

struct writ_library_view {
    size_t struct_size;
    writ_str_t name;
    writ_str_t platform;
    writ_str_t encoding;
    size_t links_count;
    const writ_str_t *links;
    size_t headers_count;
    const writ_str_t *headers;
    size_t defines_count;
    const writ_str_t *defines;
    size_t include_paths_count;
    const writ_str_t *include_paths;
};

struct writ_plan_item_view {
    size_t struct_size;
    writ_str_t name;
    writ_plan_role_t role;
    writ_plan_shape_t shape;
    writ_after_call_t after_call;
    const writ_type_view_t *type;
    uint32_t contract_slot;
    writ_str_t release;
    uint32_t with_item;
    bool nullable;
    bool optional;
};

struct writ_plan_slot_view {
    size_t struct_size;
    const writ_type_view_t *c_type;
    writ_pass_t pass;
    writ_part_t part;
    uint32_t item;
    uint32_t contract_slot;
};

struct writ_plan_local_view {
    size_t struct_size;
    writ_local_kind_t kind;
    const writ_type_view_t *type;
    uint32_t item;
    uint32_t capacity_expr;
    uint32_t capacity_item;
};

struct writ_plan_step_view {
    size_t struct_size;
    writ_step_kind_t kind;
    writ_phase_t phase;
    uint32_t outcome;
    uint32_t item;
    uint32_t slot;
    uint32_t second_slot;
    uint32_t local;
    uint32_t source_local;
    uint32_t expr;
    writ_str_t literal;
};

struct writ_plan_outcome_view {
    size_t struct_size;
    writ_str_t name;
    writ_outcome_kind_t kind;
    uint32_t when_expr;
    uint32_t via_expr;
    bool retry;
};

struct writ_plan_assign_view {
    size_t struct_size;
    uint32_t slot;
    uint32_t expr;
};

struct writ_plan_view {
    size_t struct_size;
    const writ_fn_view_t *fn;
    size_t query_count;
    const writ_plan_assign_view_t *query;
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
    uint32_t serialize_item;
    size_t reset_channels_count;
    const uint32_t *reset_channels;
    bool variadic;
    uint32_t policy_slot;
    uint32_t policy_arm;
};

#define WRIT_PLAN_NONE UINT32_MAX

const writ_library_view_t *writ_model_library(const writ_model_t *model);
size_t writ_model_fn_count(const writ_model_t *model);
const writ_fn_view_t *writ_model_fn_at(const writ_model_t *model, size_t index);
const writ_fn_view_t *writ_model_fn_named(const writ_model_t *model, writ_str_t name);
size_t writ_model_callback_count(const writ_model_t *model);
const writ_fn_view_t *writ_model_callback_at(const writ_model_t *model, size_t index);
const writ_fn_view_t *writ_model_callback_named(const writ_model_t *model, writ_str_t name);
size_t writ_model_const_count(const writ_model_t *model);
const writ_const_view_t *writ_model_const_at(const writ_model_t *model, size_t index);
const writ_const_view_t *writ_model_const_named(const writ_model_t *model, writ_str_t name);
size_t writ_model_record_count(const writ_model_t *model);
const writ_record_view_t *writ_model_record_at(const writ_model_t *model, size_t index);
const writ_record_view_t *writ_model_record_named(const writ_model_t *model, writ_str_t name);
size_t writ_model_handle_count(const writ_model_t *model);
const writ_handle_view_t *writ_model_handle_at(const writ_model_t *model, size_t index);
const writ_handle_view_t *writ_model_handle_named(const writ_model_t *model, writ_str_t name);
size_t writ_model_enum_count(const writ_model_t *model);
const writ_enum_view_t *writ_model_enum_at(const writ_model_t *model, size_t index);
const writ_enum_view_t *writ_model_enum_named(const writ_model_t *model, writ_str_t name);
size_t writ_model_channel_count(const writ_model_t *model);
const writ_channel_view_t *writ_model_channel_at(const writ_model_t *model, size_t index);
const writ_channel_view_t *writ_model_channel_named(const writ_model_t *model, writ_str_t name);
const writ_plan_view_t *writ_model_fn_plan(const writ_model_t *model, const writ_fn_view_t *fn);
const writ_plan_view_t *writ_model_fn_arm_plan(const writ_model_t *model, const writ_fn_view_t *fn, size_t arm);
writ_status_t writ_model_plan_render(const writ_model_t *model, const writ_plan_view_t *plan, writ_str_t *rendered);
writ_status_t writ_lower_json(const writ_model_t *model, const writ_output_t *output);

#ifdef __cplusplus
}
#endif

#endif
