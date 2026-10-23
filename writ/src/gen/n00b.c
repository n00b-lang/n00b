#include "gen.h"

core_list_decl(writ_n00b_extern_t);

enum { N00B_FFI_WRITE = 1 };

typedef struct {
    writ_handle_view_t        handle;
    writ_type_view_t          type;
    const writ_record_view_t *record;
    writ_str_t                raw_type;
} record_handle_t;

core_list_decl(record_handle_t);

typedef struct {
    const writ_plan_view_t *from;
    const writ_plan_view_t *to;
} plan_patch_t;

core_list_decl(plan_patch_t);

typedef struct {
    const writ_binding_t *b;
    const writ_model_t   *model;
    const writ_profile_t *profile;
    writ_session_t       *session;
    writ_c_context_t      c;
    writ_str_t            shim;
    core_buffer_t        *map;
    writ_str_t            type_namespace;
    core_list_t(writ_n00b_extern_t) *externs;
    core_list_t(record_handle_t) *record_handles;
    core_list_t(plan_patch_t) *patches;
} n00b_t;

static bool
canonical_primitive(writ_str_t name)
{
    static const char *const names[] = {
        "int", "uint", "bool", "string", "byte", "nil", "float", "i8", "i16", "i32", "i128",
        "u8", "u16", "u32", "u128", "f32", "f64",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (writ_str_eq(name, writ_cstr(names[i]))) {
            return true;
        }
    }
    return false;
}

static void
canonical_name(const n00b_t *g, core_buffer_t *out, writ_str_t name)
{
    static const struct {
        const char *from;
        const char *to;
    } renames[] = {
        {"Ref", "ref"},
        {"Array", "array"},
        {"List", "builtin.containers.list.List"},
        {"Ffi_Bytes", "builtin.ffi.Ffi_Bytes"},
        {"i64", "int"},
        {"u64", "uint"},
    };
    for (size_t i = 0; i < sizeof(renames) / sizeof(renames[0]); i++) {
        if (writ_str_eq(name, writ_cstr(renames[i].from))) {
            core_buffer_printf(out, "%s", renames[i].to);
            return;
        }
    }
    if (canonical_primitive(name) || (name.data[0] >= '0' && name.data[0] <= '9') || !g->type_namespace.length) {
        core_buffer_printf(out, "%.*s", S(name));
        return;
    }
    core_buffer_printf(out, "%.*s.%.*s", S(g->type_namespace), S(name));
}

static void
canonical_type(const n00b_t *g, core_buffer_t *out, writ_str_t text)
{
    for (size_t i = 0; i < text.length;) {
        char c = text.data[i];
        bool word = c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!word) {
            core_buffer_printf(out, "%c", c);
            i++;
            continue;
        }
        size_t start = i;
        while (i < text.length
               && (text.data[i] == '_' || (text.data[i] >= 'a' && text.data[i] <= 'z')
                   || (text.data[i] >= 'A' && text.data[i] <= 'Z') || (text.data[i] >= '0' && text.data[i] <= '9'))) {
            i++;
        }
        canonical_name(g, out, (writ_str_t){text.data + start, i - start});
    }
}

static void
add_extern_with(const n00b_t *g, writ_str_t name, writ_str_t symbol, const writ_str_t *params, size_t count,
                writ_str_t result, bool pure)
{
    core_buffer_t signature = {0};
    core_buffer_printf(&signature, "(");
    for (size_t i = 0; i < count; i++) {
        core_buffer_printf(&signature, "%s", i ? ", " : "");
        canonical_type(g, &signature, params[i]);
    }
    core_buffer_printf(&signature, ") -> ");
    canonical_type(g, &signature, result);
    core_buffer_printf(&signature, " fail: nil");
    core_list_push(*g->externs, ((writ_n00b_extern_t){
                                    .name = name,
                                    .symbol = symbol,
                                    .signature = writ_str_copy(g->session, signature.data, signature.byte_len),
                                    .pure = pure,
                                }));
    core_buffer_release(&signature);
}

static void
add_extern(const n00b_t *g, writ_str_t name, writ_str_t symbol, const writ_str_t *params, size_t count,
           writ_str_t result)
{
    add_extern_with(g, name, symbol, params, count, result, false);
}

static void
mark(const n00b_t *g, const writ_emit_t *emit, writ_span_t span, writ_str_t name)
{
    size_t line = 1;
    for (size_t i = 0; i < emit->buffer->byte_len; i++) {
        line += emit->buffer->data[i] == '\n';
    }
    const writ_source_t *source = writ_source_at(g->session, span.source_id);
    writ_str_t path = source ? source->path : writ_cstr("");
    core_buffer_printf(g->map, "%zu\t%.*s:%u:%u\t%.*s\n", line, S(path), span.line, span.column, S(name));
}

typedef enum {
    PARAM_SCALAR,
    PARAM_VALUE,
    PARAM_ADDRESS,
    PARAM_RECORD,
    PARAM_STORAGE,
    PARAM_OUT,
    PARAM_CALLBACK,
    PARAM_OWNED,
    PARAM_PROVIDED,
} param_kind_t;

typedef struct {
    param_kind_t kind;
    uint32_t     item;
    writ_str_t   name;
    writ_str_t   c_type;
    writ_str_t   n00b_type;
    size_t       parts;
    bool         storage;
    bool         borrow;
    bool         text;
} shim_param_t;

core_list_decl(shim_param_t);

static const writ_type_view_t *record_target(const writ_plan_item_view_t *item);
static bool expressible(const n00b_t *g, const writ_record_view_t *record);

static bool
opaque_record(const n00b_t *g, const writ_type_view_t *type)
{
    const writ_record_view_t *record = type ? writ_model_record_named(g->model, type->name) : nullptr;
    return record && !expressible(g, record);
}

static writ_str_t
prim(const n00b_t *g, const char *format, unsigned bits)
{
    return writ_profile_prim(g->profile, writ_str_format(g->session, format, bits));
}

static writ_str_t
scalar_type(const n00b_t *g, const writ_type_view_t *type)
{
    if (!type) {
        return writ_cstr("nil");
    }
    switch (type->kind) {
    case WRIT_TYPE_BOOL:
        return writ_profile_prim(g->profile, writ_cstr("bool"));
    case WRIT_TYPE_INT:
        return prim(g, type->is_signed ? "i%u" : "u%u", type->size * 8);
    case WRIT_TYPE_FLOAT:
        return prim(g, "f%u", type->size * 8);
    case WRIT_TYPE_ENUM:
    case WRIT_TYPE_HANDLE:
    case WRIT_TYPE_RECORD:
        return writ_binding_type(g->b, type->name);
    case WRIT_TYPE_VIEW:
        return type->target ? scalar_type(g, type->target) : writ_binding_type(g->b, type->name);
    default:
        return writ_cstr("uint");
    }
}

static writ_str_t
code_type(const n00b_t *g)
{
    return writ_profile_prim(g->profile, writ_cstr("i64"));
}

static bool
is_text_repr(writ_str_t repr)
{
    return writ_str_eq(repr, writ_cstr("string"));
}

static writ_str_t
buffer_type(const n00b_t *g)
{
    return writ_str_format(g->session, "Array[%.*s]", S(prim(g, "u%u", 8)));
}

static bool is_result(const writ_plan_item_view_t *item);

static bool
optional_result(const writ_plan_item_view_t *item)
{
    return item->nullable && item->role == WRIT_ROLE_RESULT;
}

static bool
counted_field(const writ_model_t *model, const writ_record_view_t *record, writ_str_t name)
{
    for (size_t f = 0; f < record->fields_count; f++) {
        if (writ_str_eq(writ_c_count_field(model, &record->fields[f]), name)) {
            return true;
        }
    }
    return false;
}

static bool
liftable_record(const writ_model_t *model, const writ_record_view_t *record)
{
    if (record && record->type->kind == WRIT_TYPE_UNION) {
        return false;
    }
    bool arrays = false;
    for (size_t f = 0; record && f < record->fields_count; f++) {
        const writ_slot_view_t *field = &record->fields[f];
        bool counted_array = writ_c_count_field(model, field).length && field->type->target
                          && field->type->target->kind == WRIT_TYPE_INT;
        if (!counted_array && !counted_field(model, record, field->name)) {
            return false;
        }
        arrays = arrays || counted_array;
    }
    return arrays;
}

static const writ_record_view_t *
lifted_record(const n00b_t *g, const writ_type_view_t *type)
{
    const writ_record_view_t *record = type ? writ_model_record_named(g->model, type->name) : nullptr;
    return liftable_record(g->model, record) ? record : nullptr;
}

static writ_str_t
item_type(const n00b_t *g, size_t fn, const writ_plan_view_t *plan, uint32_t index)
{
    const writ_plan_item_view_t *item = &plan->items[index];
    writ_str_t repr = writ_binding_slot_repr(g->b, fn, item->name);
    const writ_type_view_t *element = item->type && item->type->target ? item->type->target : item->type;
    if (item->optional && is_result(item)) {
        writ_plan_item_view_t present = *item;
        present.optional = false;
        present.nullable = false;
        writ_plan_view_t view = *plan;
        view.items = &present;
        return writ_str_format(g->session, "%.*s?", S(item_type(g, fn, &view, 0)));
    }
    switch (item->shape) {
    case WRIT_SHAPE_BYTES:
        if (is_text_repr(repr)) {
            return writ_cstr("string");
        }
        return optional_result(item) ? writ_str_format(g->session, "%.*s?", S(buffer_type(g))) : buffer_type(g);
    case WRIT_SHAPE_TEXT:
        return repr.length && !is_text_repr(repr) ? buffer_type(g) : writ_cstr("string");
    case WRIT_SHAPE_ARRAY:
        return writ_str_format(g->session, "Array[%.*s]", S(scalar_type(g, element)));
    case WRIT_SHAPE_REST_PACK:
        return writ_str_format(g->session, writ_str_eq(repr, writ_cstr("array")) ? "Array[%.*s]" : "List[%.*s]",
                               S(scalar_type(g, item->type)));
    case WRIT_SHAPE_OPTIONAL:
        return writ_str_format(g->session, "%.*s?", S(scalar_type(g, element)));
    case WRIT_SHAPE_CALLBACK:
        return writ_binding_type(g->b, item->type->name);
    default:
        if (record_target(item) && lifted_record(g, record_target(item)) && optional_result(item)) {
            return writ_str_format(g->session, "%.*s?", S(scalar_type(g, record_target(item))));
        }
        return scalar_type(g, record_target(item) ? record_target(item) : item->type);
    }
}

static bool
ends_with(writ_str_t text, const char *suffix)
{
    size_t length = strlen(suffix);
    return text.length >= length && !memcmp(text.data + text.length - length, suffix, length);
}

static bool
is_pointer(writ_str_t c_type)
{
    return ends_with(c_type, "*");
}

static const writ_type_view_t *
record_target(const writ_plan_item_view_t *item)
{
    const writ_type_view_t *type = item->type;
    if (type && type->kind == WRIT_TYPE_POINTER && type->target && type->target->kind == WRIT_TYPE_RECORD) {
        return type->target;
    }
    return nullptr;
}

static bool
is_handle(const writ_plan_item_view_t *item)
{
    return item->shape == WRIT_SHAPE_HANDLE || (item->type && item->type->kind == WRIT_TYPE_HANDLE);
}

static bool
is_result(const writ_plan_item_view_t *item)
{
    return item->role == WRIT_ROLE_RESULT || item->role == WRIT_ROLE_OUTPUT
        || (item->role == WRIT_ROLE_INPUT_OUTPUT && !record_target(item));
}

static writ_str_t
provided_callback(const n00b_t *g, size_t fn, const writ_plan_view_t *plan, uint32_t item)
{
    const writ_plan_item_view_t *view = &plan->items[item];
    if (view->shape != WRIT_SHAPE_CALLBACK || !view->type) {
        return (writ_str_t){0};
    }
    const writ_fn_view_t *callback = writ_model_callback_named(g->model, view->type->name);
    for (size_t s = 0; callback && s < callback->slots_count; s++) {
        if (callback->slots[s].userdata) {
            return (writ_str_t){0};
        }
    }
    return writ_binding_slot_provide(g->b, fn, view->name);
}

static writ_str_t provider(const n00b_t *g, writ_str_t provide);

static writ_str_t
provided_trampoline(const n00b_t *g, const writ_plan_view_t *plan, uint32_t item)
{
    return writ_str_format(g->session, "%.*s_%.*s_%.*s_provided", S(g->shim), S(plan->fn->contract_name),
                           S(plan->items[item].name));
}

static void
emit_provided_trampoline(const n00b_t *g, writ_emit_t *emit, size_t fn, const writ_plan_view_t *plan,
                         uint32_t item)
{
    const writ_fn_view_t *callback = writ_model_callback_named(g->model, plan->items[item].type->name);
    writ_str_t symbol = provider(g, provided_callback(g, fn, plan, item));
    writ_c_emit_real_prototype(&g->c, emit, callback, symbol, false, ";");
    writ_c_emit_real_prototype(&g->c, emit, callback, provided_trampoline(g, plan, item), false, "");
    writ_emit_open(emit, "");
    bool returns = callback->result && callback->result->type && callback->result->type->kind != WRIT_TYPE_VOID;
    writ_emit_text(emit, "%s%.*s(", returns ? "return " : "", S(symbol));
    for (size_t s = 0; s < callback->slots_count; s++) {
        writ_emit_text(emit, "%s%.*s", s ? ", " : "", S(callback->slots[s].name));
    }
    writ_emit_line(emit, ");");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
}

static void
shim_params(const n00b_t *g, size_t fn, const writ_plan_view_t *plan, core_list_t(shim_param_t) *params)
{
    uint32_t *order = writ_alloc(g->session, sizeof(uint32_t) * (plan->items_count + 1));
    size_t ordered = writ_c_param_order(&g->c, plan, order);
    for (size_t o = 0; o < ordered; o++) {
        uint32_t index = order[o];
        const writ_plan_item_view_t *item = &plan->items[index];
        writ_c_param_t c_params[2];
        size_t count = writ_c_item_params(&g->c, plan, index, c_params);
        shim_param_t param = {.item = index, .name = item->name, .c_type = c_params[0].type, .parts = count};
        if (item->shape == WRIT_SHAPE_CALLBACK) {
            bool owned = plan->policy_slot != WRIT_PLAN_NONE && item->contract_slot == plan->policy_slot
                      && plan->fn->slots[plan->policy_slot].policy_arms[plan->policy_arm].is_else;
            param.kind = owned ? PARAM_OWNED
                       : provided_callback(g, fn, plan, index).length ? PARAM_PROVIDED
                                                                      : PARAM_CALLBACK;
            param.c_type = writ_cstr("uint64_t");
            param.n00b_type = writ_cstr("u64");
            core_list_push(*params, param);
            continue;
        }
        if (writ_c_is_input(item)) {
            if (count == 2) {
                param.kind = PARAM_VALUE;
                param.text = writ_str_eq(item_type(g, fn, plan, index), writ_cstr("string"));
                param.n00b_type = param.text ? writ_cstr("Ref[Array[u8]]")
                                             : writ_str_format(g->session, "Ref[%.*s]",
                                                               S(item_type(g, fn, plan, index)));
                param.borrow = item->after_call == WRIT_AFTER_BORROWED
                            && writ_str_eq(writ_binding_slot_repr(g->b, fn, item->name), writ_cstr("borrow"));
            } else if (writ_c_by_pointer(&g->c, item) || record_target(item)) {
                const writ_type_view_t *record = record_target(item) ? record_target(item) : item->type;
                bool opaque = opaque_record(g, record);
                param.kind = opaque ? PARAM_STORAGE : PARAM_RECORD;
                param.n00b_type = opaque ? writ_str_format(g->session, "Ref[%.*s]", S(buffer_type(g)))
                                         : writ_str_format(g->session, "Ref[%.*s]", S(scalar_type(g, record)));
            } else if (is_pointer(param.c_type)) {
                param.kind = PARAM_ADDRESS;
                param.n00b_type = writ_cstr("uint");
            } else {
                param.kind = PARAM_SCALAR;
                param.n00b_type = item_type(g, fn, plan, index);
            }
            core_list_push(*params, param);
            continue;
        }
        param.kind = PARAM_OUT;
        bool bytes = ends_with(param.c_type, "_bytes_t *");
        bool address = ends_with(param.c_type, "**") || count == 2;
        param.storage = !bytes && !address && opaque_record(g, item->type);
        param.n00b_type = bytes           ? writ_cstr("Ref[Ffi_Bytes]")
                          : address       ? writ_cstr("Ref[uint]")
                          : param.storage ? writ_str_format(g->session, "Ref[%.*s]", S(buffer_type(g)))
                                          : writ_str_format(g->session, "Ref[%.*s]", S(scalar_type(g, item->type)));
        core_list_push(*params, param);
        if (count == 2) {
            shim_param_t length = {.kind = PARAM_OUT, .item = index, .c_type = c_params[1].type,
                                   .name = c_params[1].name, .n00b_type = writ_cstr("Ref[uint]"), .parts = 1};
            core_list_push(*params, length);
        }
    }
}

static writ_str_t
shim_symbol(const n00b_t *g, const writ_plan_view_t *plan)
{
    return writ_str_format(g->session, "%.*s_n00b_%.*s", S(g->shim), S(writ_c_plan_name(&g->c, plan)));
}

static const record_handle_t *
record_handle_for(const n00b_t *g, const writ_type_view_t *type)
{
    for (size_t h = 0; type && type->kind == WRIT_TYPE_POINTER && h < g->record_handles->len; h++) {
        if (type->target == g->record_handles->data[h].record->type) {
            return &g->record_handles->data[h];
        }
    }
    return nullptr;
}

static const record_handle_t *
typed_handle(const n00b_t *g, const writ_type_view_t *type)
{
    for (size_t h = 0; type && type->kind == WRIT_TYPE_HANDLE && h < g->record_handles->len; h++) {
        if (writ_str_eq(type->name, g->record_handles->data[h].handle.name)) {
            return &g->record_handles->data[h];
        }
    }
    return nullptr;
}

static const record_handle_t *
released_handle(const n00b_t *g, writ_str_t fn)
{
    for (size_t h = 0; h < g->record_handles->len; h++) {
        if (writ_str_eq(fn, g->record_handles->data[h].handle.release)) {
            return &g->record_handles->data[h];
        }
    }
    return nullptr;
}

static writ_str_t
raw_of(const n00b_t *g, const writ_type_view_t *type, writ_str_t owner)
{
    return writ_str_format(g->session, typed_handle(g, type) ? "unsafe_cast[uint] %.*s.raw" : "%.*s.raw", S(owner));
}

static const char *
raw_empty(const n00b_t *g, const writ_type_view_t *type)
{
    return typed_handle(g, type) ? "nil" : "0";
}

static writ_bound_fn_t
bound_fn(const n00b_t *g, size_t fn)
{
    writ_bound_fn_t bound = writ_binding_fn(g->b, fn);
    const record_handle_t *released = released_handle(g, writ_model_fn_at(g->model, fn)->contract_name);
    if (released && bound.bound && !bound.raw) {
        bound.owner = writ_binding_type(g->b, released->handle.name);
        bound.name = g->profile->handle_close.length ? g->profile->handle_close : writ_cstr("close");
        bound.is_static = false;
    }
    return bound;
}

static const writ_plan_view_t *
patched_plan(const n00b_t *g, const writ_plan_view_t *plan)
{
    if (!plan || !g->record_handles->len) {
        return plan;
    }
    core_list_foreach(*g->patches, patch) {
        if (patch->from == plan) {
            return patch->to;
        }
    }
    writ_plan_item_view_t *items = writ_alloc(g->session, sizeof(writ_plan_item_view_t) * (plan->items_count + 1));
    bool touched = false;
    for (size_t i = 0; i < plan->items_count; i++) {
        items[i] = plan->items[i];
        const record_handle_t *handle = record_handle_for(g, items[i].type);
        if (handle) {
            writ_type_view_t *type = writ_alloc(g->session, sizeof(writ_type_view_t));
            *type = handle->type;
            type->nullable = items[i].type->nullable;
            type->target = items[i].type;
            items[i].type = type;
            items[i].shape = WRIT_SHAPE_HANDLE;
            touched = true;
        }
    }
    writ_plan_view_t *patched = writ_alloc(g->session, sizeof(writ_plan_view_t));
    *patched = *plan;
    patched->items = items;
    const writ_plan_view_t *chosen = touched ? patched : plan;
    core_list_push(*g->patches, ((plan_patch_t){plan, chosen}));
    return chosen;
}

static const writ_plan_view_t *
bound_plan(const n00b_t *g, size_t fn, size_t k)
{
    writ_bound_fn_t bound = bound_fn(g, fn);
    const writ_fn_view_t *view = writ_model_fn_at(g->model, fn);
    if (bound.policy_arm != WRIT_PLAN_NONE) {
        return k ? nullptr : patched_plan(g, writ_model_fn_arm_plan(g->model, view, bound.policy_arm));
    }
    return patched_plan(g, writ_model_fn_arm_plan(g->model, view, k));
}

static void
find_record_handles(n00b_t *g)
{
    const writ_model_t *model = g->model;
    size_t count = writ_model_fn_count(model);
    for (size_t i = 0; !writ_binding_exports(g->b) && i < count; i++) {
        const writ_slot_view_t *result = writ_model_fn_at(model, i)->result;
        const writ_type_view_t *type = result ? result->type : nullptr;
        if (!type || !result->own || !result->release.length || type->kind != WRIT_TYPE_POINTER || !type->target
            || type->target->kind != WRIT_TYPE_RECORD || record_handle_for(g, type)
            || !writ_model_fn_named(model, result->release)) {
            continue;
        }
        const writ_record_view_t *record = writ_model_record_at(model, type->target->decl);
        if (!expressible(g, record) || liftable_record(model, record)) {
            continue;
        }
        bool suffixed = record->name.length > 2 && !memcmp(record->name.data + record->name.length - 2, "_t", 2);
        writ_str_t name = suffixed ? writ_str_copy(g->session, record->name.data, record->name.length - 2)
                                   : writ_str_format(g->session, "%.*s_handle", S(record->name));
        core_list_push(*g->record_handles, ((record_handle_t){
            .handle   = {.struct_size = sizeof(writ_handle_view_t), .name = name, .base = type,
                         .release = result->release, .declared_at = record->declared_at},
            .type     = {.struct_size = sizeof(writ_type_view_t), .kind = WRIT_TYPE_HANDLE, .name = name,
                         .size = type->size, .align = type->align, .target = type},
            .record   = record,
            .raw_type = writ_str_format(g->session, "Foreign[%.*s]?", S(writ_binding_type(g->b, record->name))),
        }));
    }
    core_list_foreach(*g->record_handles, handle) {
        handle->handle.type = &handle->type;
    }
}

static const writ_policy_arm_view_t *
plan_arm(const writ_plan_view_t *plan)
{
    return plan->policy_slot == WRIT_PLAN_NONE ? nullptr
                                               : &plan->fn->slots[plan->policy_slot].policy_arms[plan->policy_arm];
}

static bool
owned_arm(const writ_plan_view_t *plan)
{
    return plan_arm(plan) && plan_arm(plan)->is_else;
}

static writ_str_t
trampoline_name(const n00b_t *g, writ_str_t callback)
{
    return writ_str_format(g->session, "%.*s_%.*s_trampoline", S(g->shim), S(callback));
}

static void
emit_owned_free(const n00b_t *g, writ_emit_t *emit, const writ_plan_view_t *plan)
{
    const writ_slot_view_t *slot = &plan->fn->slots[plan->policy_slot];
    const writ_fn_view_t *callback = writ_model_callback_named(g->model, slot->type->name);
    if (!callback || !callback->slots_count) {
        return;
    }
    writ_c_emit_real_prototype(&g->c, emit, callback, writ_str_format(g->session, "%.*s_free", S(shim_symbol(g, plan))),
                               false, " {");
    writ_emit_indent(emit, 1);
    writ_emit_line(emit, "free((void *)%.*s);", S(callback->slots[0].name));
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
}

static bool
keeps_until(const writ_plan_view_t *plan, const shim_param_t *param)
{
    return param->kind == PARAM_VALUE && plan->items[param->item].after_call == WRIT_AFTER_BORROWED_UNTIL;
}

static bool
text_result(const n00b_t *g, size_t fn, const writ_plan_view_t *plan, const shim_param_t *param)
{
    return param->kind == PARAM_OUT && is_result(&plan->items[param->item])
        && ends_with(param->n00b_type, "Ffi_Bytes]")
        && writ_str_eq(item_type(g, fn, plan, param->item), writ_cstr("string"));
}

static bool
cstr_result(const n00b_t *g, size_t fn, const writ_plan_view_t *plan, const shim_param_t *param)
{
    (void)g;
    (void)fn;
    return param->kind == PARAM_OUT && is_result(&plan->items[param->item])
        && plan->items[param->item].shape == WRIT_SHAPE_TEXT && !ends_with(param->n00b_type, "Ffi_Bytes]");
}

typedef bool (*param_match_fn)(const n00b_t *g, size_t fn, const writ_plan_view_t *plan,
                               const core_list_t(shim_param_t) *params, const shim_param_t *param);
typedef bool (*plan_match_fn)(const n00b_t *g, const writ_plan_view_t *plan);

static bool
any_param(const n00b_t *g, param_match_fn match, plan_match_fn plan_match)
{
    bool found = false;
    for (size_t i = 0; i < writ_model_fn_count(g->model) && !found; i++) {
        writ_bound_fn_t bound = bound_fn(g, i);
        for (size_t k = 0; bound.bound && !bound.raw && bound_plan(g, i, k) && !found; k++) {
            const writ_plan_view_t *plan = bound_plan(g, i, k);
            found = plan_match && plan_match(g, plan);
            core_list_t(shim_param_t) params = core_list_new(shim_param_t);
            shim_params(g, i, plan, &params);
            for (size_t p = 0; match && p < params.len && !found; p++) {
                found = match(g, i, plan, &params, &params.data[p]);
            }
            core_list_free(params);
        }
    }
    return found;
}

static bool
fetches_detail_text(const n00b_t *g, const writ_plan_view_t *plan)
{
    (void)g;
    for (size_t s = 0; s < plan->steps_count; s++) {
        const writ_plan_step_view_t *step = &plan->steps[s];
        if (step->kind == WRIT_STEP_FETCH_DETAIL && step->item != WRIT_PLAN_NONE
            && plan->items[step->item].shape == WRIT_SHAPE_TEXT) {
            return true;
        }
    }
    return false;
}

static bool
returns_text(const n00b_t *g, const writ_plan_view_t *plan)
{
    for (size_t t = 0; writ_c_returns_directly(&g->c, plan) && t < plan->items_count; t++) {
        if (is_result(&plan->items[t]) && plan->items[t].shape == WRIT_SHAPE_TEXT) {
            return true;
        }
    }
    return false;
}

static bool
text_param(const n00b_t *g, size_t fn, const writ_plan_view_t *plan, const core_list_t(shim_param_t) *params,
           const shim_param_t *param)
{
    (void)params;
    return text_result(g, fn, plan, param);
}

static bool
cstr_param(const n00b_t *g, size_t fn, const writ_plan_view_t *plan, const core_list_t(shim_param_t) *params,
           const shim_param_t *param)
{
    (void)params;
    return cstr_result(g, fn, plan, param);
}

static bool
kept_param(const n00b_t *g, size_t fn, const writ_plan_view_t *plan, const core_list_t(shim_param_t) *params,
           const shim_param_t *param)
{
    (void)g;
    (void)fn;
    (void)params;
    return keeps_until(plan, param);
}

static bool
optional_bytes_param(const n00b_t *g, size_t fn, const writ_plan_view_t *plan, const core_list_t(shim_param_t) *params,
                     const shim_param_t *param)
{
    (void)params;
    return param->kind == PARAM_OUT && ends_with(param->n00b_type, "Ffi_Bytes]")
        && optional_result(&plan->items[param->item]) && !record_target(&plan->items[param->item])
        && !text_result(g, fn, plan, param);
}

static bool
takes_optional_bytes(const n00b_t *g)
{
    return any_param(g, optional_bytes_param, nullptr);
}

static bool
takes_detail_text(const n00b_t *g)
{
    return any_param(g, nullptr, fetches_detail_text);
}

static bool
takes_text(const n00b_t *g)
{
    return any_param(g, text_param, fetches_detail_text);
}

static bool
copies_cstr(const n00b_t *g)
{
    return any_param(g, cstr_param, returns_text);
}

static bool
shim_keeps(const n00b_t *g)
{
    return any_param(g, kept_param, nullptr);
}

static void
emit_kept_free(const n00b_t *g, writ_emit_t *emit)
{
    writ_emit_open(emit, "void %.*s_kept_free(uint64_t kept)", S(g->shim));
    writ_emit_line(emit, "free((void *)(uintptr_t)kept);");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
}

static void
emit_c_shim(const n00b_t *g, writ_emit_t *emit, size_t fn, const writ_plan_view_t *plan)
{
    core_list_t(shim_param_t) params = core_list_new(shim_param_t);
    shim_params(g, fn, plan, &params);
    bool direct = writ_c_returns_directly(&g->c, plan);
    writ_str_t ret = direct ? writ_c_return_type(&g->c, plan) : writ_cstr("int32_t");
    writ_str_t ret_type = is_pointer(ret) ? writ_cstr("uint64_t") : ret;
    if (owned_arm(plan)) {
        emit_owned_free(g, emit, plan);
    }
    for (size_t i = 0; i < params.len; i++) {
        if (params.data[i].kind == PARAM_PROVIDED) {
            emit_provided_trampoline(g, emit, fn, plan, params.data[i].item);
        }
    }
    writ_emit_text(emit, "%.*s %.*s(", S(ret_type), S(shim_symbol(g, plan)));
    bool any = false;
    for (size_t i = 0; i < params.len; i++) {
        const shim_param_t *p = &params.data[i];
        if (p->kind == PARAM_OWNED || p->kind == PARAM_PROVIDED) {
            continue;
        }
        writ_str_t type = p->kind == PARAM_SCALAR || p->kind == PARAM_CALLBACK ? p->c_type
                          : p->kind == PARAM_ADDRESS                            ? writ_cstr("uint64_t")
                          : p->kind == PARAM_OUT     ? writ_cstr("void *")
                                                     : writ_cstr("const void *");
        writ_emit_text(emit, "%s%.*s%s%.*s", any ? ", " : "", S(type), is_pointer(type) ? "" : " ", S(p->name));
        any = true;
    }
    for (size_t i = 0; i < params.len; i++) {
        if (keeps_until(plan, &params.data[i])) {
            writ_emit_text(emit, "%suint64_t *%.*s_kept", any ? ", " : "", S(params.data[i].name));
            any = true;
        }
    }
    writ_emit_text(emit, "%s)", any ? "" : "void");
    writ_emit_open(emit, "");
    size_t copies = 0;
    for (size_t i = 0; i < params.len; i++) {
        const shim_param_t *p = &params.data[i];
        if (p->kind == PARAM_STORAGE || (p->kind == PARAM_VALUE && p->borrow)) {
            writ_emit_line(emit, "size_t %.*s_length = 0;", S(p->name));
            writ_emit_line(emit, "const void *%.*s_data = n00b_ffi_borrow((void *)%.*s, &%.*s_length, 0);",
                           S(p->name), S(p->name), S(p->name));
        } else if (p->kind == PARAM_VALUE) {
            writ_emit_line(emit, "size_t %.*s_length = 0;", S(p->name));
            writ_emit_line(emit, "const void *%.*s_borrowed = n00b_ffi_borrow((void *)%.*s, &%.*s_length, 0);",
                           S(p->name), S(p->name), S(p->name));
            writ_emit_line(emit, "uint8_t *%.*s_data = malloc(%.*s_length + 1);", S(p->name), S(p->name));
            writ_emit_open(emit, "if (%.*s_data && %.*s_length)", S(p->name), S(p->name));
            writ_emit_line(emit, "memcpy(%.*s_data, %.*s_borrowed, %.*s_length);", S(p->name), S(p->name),
                           S(p->name));
            writ_emit_close(emit, nullptr);
            writ_emit_open(emit, "if (%.*s_data)", S(p->name));
            writ_emit_line(emit, "%.*s_data[%.*s_length] = 0;", S(p->name), S(p->name));
            writ_emit_close(emit, nullptr);
            writ_emit_line(emit, "n00b_ffi_release((void *)%.*s);", S(p->name));
            copies++;
        }
    }
    if (copies) {
        writ_emit_text(emit, "if (");
        size_t emitted = 0;
        for (size_t i = 0; i < params.len; i++) {
            if (params.data[i].kind == PARAM_VALUE && !params.data[i].borrow) {
                writ_emit_text(emit, "%s!%.*s_data", emitted++ ? " || " : "", S(params.data[i].name));
            }
        }
        writ_emit_text(emit, ")");
        writ_emit_open(emit, "");
        for (size_t i = 0; i < params.len; i++) {
            if (params.data[i].kind == PARAM_VALUE && !params.data[i].borrow) {
                writ_emit_line(emit, "free(%.*s_data);", S(params.data[i].name));
            } else if (params.data[i].kind == PARAM_STORAGE || params.data[i].kind == PARAM_VALUE) {
                writ_emit_line(emit, "n00b_ffi_release((void *)%.*s);", S(params.data[i].name));
            }
        }
        if (direct) {
            writ_emit_line(emit, "n00bb_runtime_trap(\"out of memory\", 13);");
        } else {
            writ_emit_line(emit, "return %.*s_OUT_OF_MEMORY;", S(g->c.upper));
        }
        writ_emit_close(emit, nullptr);
    }
    writ_emit_text(emit, "%.*s writ_n00b_value = %.*s_%.*s(", S(ret), S(g->c.prefix), S(writ_c_plan_name(&g->c, plan)));
    bool first = true;
    for (size_t i = 0; i < params.len; i++) {
        const shim_param_t *p = &params.data[i];
        writ_emit_text(emit, "%s", first ? "" : ", ");
        first = false;
        switch (p->kind) {
        case PARAM_VALUE:
            writ_emit_text(emit, "(%.*s)%.*s_data, %.*s_length", S(p->c_type), S(p->name), S(p->name));
            break;
        case PARAM_STORAGE:
            writ_emit_text(emit, "(%.*s)%.*s_data", S(p->c_type), S(p->name));
            break;
        case PARAM_ADDRESS:
            writ_emit_text(emit, "(%.*s)(uintptr_t)%.*s", S(p->c_type), S(p->name));
            break;
        case PARAM_SCALAR:
            writ_emit_text(emit, "%.*s", S(p->name));
            break;
        case PARAM_CALLBACK:
            writ_emit_text(emit, "%.*s", S(trampoline_name(g, plan->items[p->item].type->name)));
            if (p->parts == 2) {
                writ_emit_text(emit, ", (void *)(uintptr_t)%.*s", S(p->name));
            }
            break;
        case PARAM_PROVIDED:
            writ_emit_text(emit, "%.*s", S(provided_trampoline(g, plan, p->item)));
            break;
        case PARAM_OWNED:
            writ_emit_text(emit, "%.*s_free", S(shim_symbol(g, plan)));
            break;
        default:
            writ_emit_text(emit, "(%.*s)%.*s", S(p->c_type), S(p->name));
            break;
        }
    }
    writ_emit_line(emit, ");");
    for (size_t i = 0; i < params.len; i++) {
        writ_after_call_t after = plan->items[params.data[i].item].after_call;
        bool handed_over = (owned_arm(plan) && after == WRIT_AFTER_CALLEE_OWNS) || after == WRIT_AFTER_BORROWED_UNTIL;
        if (keeps_until(plan, &params.data[i])) {
            writ_emit_line(emit, "*%.*s_kept = (uint64_t)(uintptr_t)%.*s_data;", S(params.data[i].name),
                           S(params.data[i].name));
        } else if (params.data[i].kind == PARAM_VALUE && !params.data[i].borrow && !handed_over) {
            writ_emit_line(emit, "free(%.*s_data);", S(params.data[i].name));
        } else if (params.data[i].kind == PARAM_STORAGE || params.data[i].borrow) {
            writ_emit_line(emit, "n00b_ffi_release((void *)%.*s);", S(params.data[i].name));
        }
    }
    writ_emit_line(emit, "return %swrit_n00b_value;", is_pointer(ret) ? "(uint64_t)(uintptr_t)" : "");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    core_list_free(params);
}

static uint32_t
userdata_receiver(const writ_fn_view_t *callback)
{
    for (size_t i = 0; i < callback->slots_count; i++) {
        if (callback->slots[i].userdata) {
            return (uint32_t)i;
        }
    }
    return WRIT_PLAN_NONE;
}

static writ_str_t
callback_result_c_type(const n00b_t *g, const writ_fn_view_t *callback)
{
    const writ_type_view_t *result = callback->result ? callback->result->type : nullptr;
    return writ_c_type(&g->c, result, writ_c_is_text(result) && callback->result && !callback->result->own);
}

static writ_str_t callback_table(const n00b_t *g, writ_str_t type, const char *suffix);
static writ_str_t callback_dispatch_symbol(const n00b_t *g, const writ_fn_view_t *callback);

static writ_str_t
callback_helper_symbol(const n00b_t *g, const writ_fn_view_t *callback, const char *role)
{
    return writ_str_format(g->session, "%.*s_%.*s_%s", S(g->shim), S(callback->contract_name), role);
}

static void
emit_trampoline(const n00b_t *g, writ_emit_t *emit, const writ_fn_view_t *callback)
{
    uint32_t receiver = userdata_receiver(callback);
    if (receiver == WRIT_PLAN_NONE) {
        return;
    }
    writ_str_t arguments = writ_str_format(g->session, "%.*s_%.*s_arguments", S(g->shim), S(callback->contract_name));
    bool packed = callback->slots_count > 1;
    if (packed) {
        writ_emit_open(emit, "typedef struct");
        for (size_t i = 0; i < callback->slots_count; i++) {
            if (i != receiver) {
                writ_str_t type = writ_c_real_param_type(&g->c, &callback->slots[i]);
                writ_emit_line(emit, "%.*s%s%.*s;", S(type), is_pointer(type) ? "" : " ", S(callback->slots[i].name));
            }
        }
        writ_emit_close(emit, writ_str_format(g->session, " %.*s;", S(arguments)).data);
        writ_emit_line(emit, "");
    }
    writ_str_t ret = callback_result_c_type(g, callback);
    bool returns = !writ_str_eq(ret, writ_cstr("void"));
    writ_emit_text(emit, "%.*s %.*s(", S(ret), S(trampoline_name(g, callback->contract_name)));
    for (size_t i = 0; i < callback->slots_count; i++) {
        writ_str_t type = writ_c_real_param_type(&g->c, &callback->slots[i]);
        writ_emit_text(emit, "%s%.*s%s%.*s", i ? ", " : "", S(type), is_pointer(type) ? "" : " ",
                       S(callback->slots[i].name));
    }
    writ_emit_text(emit, "%s)", callback->slots_count ? "" : "void");
    writ_emit_open(emit, "");
    if (packed) {
        writ_emit_text(emit, "%.*s arguments = {", S(arguments));
        bool first = true;
        for (size_t i = 0; i < callback->slots_count; i++) {
            if (i != receiver) {
                writ_emit_text(emit, "%s%.*s", first ? "" : ", ", S(callback->slots[i].name));
                first = false;
            }
        }
        writ_emit_line(emit, "};");
    }
    if (returns) {
        writ_emit_line(emit, "%.*s result;", S(ret));
        writ_emit_line(emit, "memset(&result, 0, sizeof result);");
    }
    writ_emit_line(emit, "%.*s((uint64_t)(uintptr_t)%.*s, %s, %s);", S(callback_dispatch_symbol(g, callback)),
                   S(callback->slots[receiver].name), packed ? "&arguments" : "NULL", returns ? "&result" : "NULL");
    if (returns) {
        writ_emit_line(emit, "return result;");
    }
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    if (packed) {
        writ_emit_open(emit, "void %.*s(const void *arguments, void *out)",
                       S(callback_helper_symbol(g, callback, "arguments_copy")));
        writ_emit_line(emit, "memcpy(out, arguments, sizeof(%.*s));", S(arguments));
        writ_emit_close(emit, nullptr);
        writ_emit_line(emit, "");
    }
    if (returns) {
        writ_emit_open(emit, "void %.*s(void *result, %.*s value)", S(callback_helper_symbol(g, callback, "result_store")),
                       S(ret));
        writ_emit_line(emit, "*(%.*s *)result = value;", S(ret));
        writ_emit_close(emit, nullptr);
        writ_emit_line(emit, "");
    }
}

static bool
array_result(const writ_plan_item_view_t *item)
{
    return item->shape == WRIT_SHAPE_ARRAY && !writ_c_is_input(item) && item->type && item->type->target;
}

static writ_str_t
array_element_name(const n00b_t *g, const writ_type_view_t *element)
{
    return scalar_type(g, element);
}

typedef const writ_type_view_t *type_view_ptr_t;
core_list_decl(type_view_ptr_t);

static core_list_t(type_view_ptr_t)
array_elements(const n00b_t *g)
{
    core_list_t(type_view_ptr_t) elements = core_list_new(type_view_ptr_t);
    for (size_t i = 0; i < writ_model_fn_count(g->model); i++) {
        writ_bound_fn_t bound = bound_fn(g, i);
        for (size_t k = 0; bound.bound && !bound.raw && bound_plan(g, i, k); k++) {
            const writ_plan_view_t *plan = bound_plan(g, i, k);
            for (size_t t = 0; t < plan->items_count; t++) {
                if (!array_result(&plan->items[t])) {
                    continue;
                }
                const writ_type_view_t *element = plan->items[t].type->target;
                bool seen = false;
                for (size_t e = 0; e < elements.len && !seen; e++) {
                    seen = writ_str_eq(array_element_name(g, elements.data[e]), array_element_name(g, element));
                }
                if (!seen) {
                    core_list_push(elements, element);
                }
            }
        }
    }
    return elements;
}


static void
emit_c_array_takers(const n00b_t *g, writ_emit_t *emit)
{
    core_list_t(type_view_ptr_t) elements = array_elements(g);
    for (size_t e = 0; e < elements.len; e++) {
        writ_emit_open(emit, "void %.*s_take_array_%.*s(uint64_t data, uint64_t length, void *out)", S(g->shim),
                       S(array_element_name(g, elements.data[e])));
        writ_emit_line(emit, "size_t capacity = 0;");
        writ_emit_line(emit, "uint8_t *items = n00b_ffi_borrow(out, &capacity, %d);", N00B_FFI_WRITE);
        writ_emit_line(emit, "size_t count = length < capacity ? (size_t)length : capacity;");
        writ_emit_open(emit, "if (items && data && count)");
        writ_emit_line(emit, "memcpy(items, (const void *)(uintptr_t)data, count * sizeof(%.*s));",
                       S(writ_c_type(&g->c, elements.data[e], false)));
        writ_emit_close(emit, nullptr);
        writ_emit_line(emit, "n00b_ffi_release(out);");
        writ_emit_line(emit, "%.*s_free((void *)(uintptr_t)data);", S(g->c.prefix));
        writ_emit_close(emit, nullptr);
        writ_emit_line(emit, "");
    }
    core_list_free(elements);
}

static void
emit_n00b_array_takers(const n00b_t *g, writ_emit_t *emit)
{
    core_list_t(type_view_ptr_t) elements = array_elements(g);
    for (size_t e = 0; e < elements.len; e++) {
        writ_str_t name = array_element_name(g, elements.data[e]);
        writ_str_t array = writ_str_format(g->session, "Array[%.*s]", S(name));
        writ_str_t take = writ_str_format(g->session, "__ffi_array_take_%.*s", S(name));
        writ_str_t params[] = {writ_cstr("uint"), writ_cstr("uint"), writ_str_format(g->session, "Ref[%.*s]", S(array))};
        add_extern(g, take, writ_str_format(g->session, "%.*s_take_array_%.*s", S(g->shim), S(name)), params, 3,
                   writ_cstr("nil"));
        writ_emit_line(emit, "");
        writ_emit_open(emit, "func __ffi_take_array_%.*s(data: uint, length: uint) -> %.*s", S(name), S(array));
        writ_emit_line(emit, "var items: %.*s = new[%.*s](cast[int]length)", S(array), S(array));
        writ_emit_line(emit, "%.*s(data, length, ref items)", S(take));
        writ_emit_line(emit, "return items");
        writ_emit_close(emit, nullptr);
    }
    core_list_free(elements);
}

static size_t
counted_fields(const n00b_t *g, const writ_record_view_t *record, const writ_slot_view_t **out)
{
    size_t count = 0;
    for (size_t f = 0; liftable_record(g->model, record) && f < record->fields_count && count < WRIT_C_MAX_ARITY; f++) {
        if (writ_c_count_field(g->model, &record->fields[f]).length) {
            out[count++] = &record->fields[f];
        }
    }
    return count;
}

static writ_str_t
field_symbol(const n00b_t *g, const writ_record_view_t *record, writ_str_t field, const char *suffix)
{
    return writ_str_format(g->session, "%.*s_%.*s_%.*s%s", S(g->shim), S(record->name), S(field), suffix);
}

static void
emit_c_record_taker(const n00b_t *g, writ_emit_t *emit, const writ_record_view_t *record,
                    const writ_slot_view_t **fields, size_t count)
{
    writ_str_t type = writ_c_type(&g->c, record->type, false);
    for (size_t f = 0; f < count; f++) {
        writ_emit_open(emit, "uint64_t %.*s(uint64_t block)", S(field_symbol(g, record, fields[f]->name, "_length")));
        writ_emit_line(emit, "return block ? (uint64_t)((const %.*s *)(uintptr_t)block)->%.*s : 0;", S(type),
                       S(writ_c_count_field(g->model, fields[f])));
        writ_emit_close(emit, nullptr);
        writ_emit_line(emit, "");
    }
    writ_emit_text(emit, "void %.*s(%.*s_bytes_t *bytes", S(field_symbol(g, record, writ_cstr("take"), "")),
                   S(g->c.prefix));
    for (size_t f = 0; f < count; f++) {
        writ_emit_text(emit, ", void *%.*s", S(fields[f]->name));
    }
    writ_emit_open(emit, ")");
    writ_emit_line(emit, "const %.*s *record = (const %.*s *)bytes->data;", S(type), S(type));
    writ_emit_line(emit, "size_t capacity = 0;");
    for (size_t f = 0; f < count; f++) {
        writ_str_t name = fields[f]->name;
        writ_emit_line(emit, "uint8_t *%.*s_items = n00b_ffi_borrow(%.*s, &capacity, %d);", S(name), S(name),
                       N00B_FFI_WRITE);
        writ_emit_open(emit, "if (%.*s_items && record && record->%.*s)", S(name), S(name));
        writ_emit_line(emit, "memcpy(%.*s_items, record->%.*s, capacity * sizeof(%.*s));", S(name), S(name),
                       S(writ_c_type(&g->c, fields[f]->type->target, false)));
        writ_emit_close(emit, nullptr);
        writ_emit_line(emit, "n00b_ffi_release(%.*s);", S(name));
    }
    writ_emit_line(emit, "%.*s_bytes_free(bytes);", S(g->c.prefix));
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
}

static void
emit_optional_taker(writ_emit_t *emit, writ_str_t name, writ_str_t type)
{
    writ_emit_line(emit, "");
    writ_emit_open(emit, "func __ffi_take_optional_%.*s(bytes: Ref[Ffi_Bytes]) -> %.*s?", S(name), S(type));
    writ_emit_line(emit, "let value: Ffi_Bytes = bytes.__value");
    writ_emit_open(emit, "if value.data == 0");
    writ_emit_line(emit, "return nil");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "return __ffi_take_%.*s(bytes)", S(name));
    writ_emit_close(emit, nullptr);
}

static void
emit_n00b_record_taker(const n00b_t *g, writ_emit_t *emit, const writ_record_view_t *record,
                       const writ_slot_view_t **fields, size_t count)
{
    writ_str_t type = writ_binding_type(g->b, record->name);
    writ_str_t take_params[WRIT_C_MAX_ARITY + 1] = {writ_cstr("Ref[Ffi_Bytes]")};
    writ_emit_line(emit, "");
    writ_emit_open(emit, "func __ffi_take_%.*s(bytes: Ref[Ffi_Bytes]) -> %.*s", S(record->name), S(type));
    writ_emit_line(emit, "let value: Ffi_Bytes = bytes.__value");
    writ_emit_text(emit, "var lifted: %.*s = (", S(type));
    for (size_t f = 0; f < count; f++) {
        writ_str_t length = writ_str_format(g->session, "__ffi_%.*s_%.*s_length", S(record->name),
                                            S(fields[f]->name));
        writ_str_t length_params[] = {writ_cstr("uint")};
        add_extern(g, length, field_symbol(g, record, fields[f]->name, "_length"), length_params, 1,
                   writ_cstr("uint"));
        writ_str_t array = writ_str_format(g->session, "Array[%.*s]", S(scalar_type(g, fields[f]->type->target)));
        take_params[f + 1] = writ_str_format(g->session, "Ref[%.*s]", S(array));
        writ_emit_text(emit, "%s%.*s = new[%.*s](cast[int]%.*s(value.data))", f ? ", " : "",
                       S(writ_profile_name(g->profile, WRIT_NAMING_FIELD, fields[f]->name)), S(array), S(length));
    }
    writ_emit_line(emit, ")");
    add_extern(g, writ_str_format(g->session, "__ffi_%.*s_take", S(record->name)),
               field_symbol(g, record, writ_cstr("take"), ""), take_params, count + 1, writ_cstr("nil"));
    writ_emit_text(emit, "__ffi_%.*s_take(bytes", S(record->name));
    for (size_t f = 0; f < count; f++) {
        writ_emit_text(emit, ", ref lifted.%.*s", S(writ_profile_name(g->profile, WRIT_NAMING_FIELD, fields[f]->name)));
    }
    writ_emit_line(emit, ")");
    writ_emit_line(emit, "return lifted");
    writ_emit_close(emit, nullptr);
    emit_optional_taker(emit, record->name, type);
}

typedef void (*record_taker_fn)(const n00b_t *g, writ_emit_t *emit, const writ_record_view_t *record,
                                const writ_slot_view_t **fields, size_t count);

static void
emit_record_takers(const n00b_t *g, writ_emit_t *emit, record_taker_fn taker)
{
    for (size_t i = 0; i < writ_model_record_count(g->model); i++) {
        const writ_record_view_t *record = writ_model_record_at(g->model, i);
        const writ_slot_view_t *fields[WRIT_C_MAX_ARITY];
        size_t count = counted_fields(g, record, fields);
        if (count) {
            taker(g, emit, record, fields, count);
        }
    }
}

static void
emit_cstr_c_helpers(const n00b_t *g, writ_emit_t *emit)
{
    writ_emit_open(emit, "uint64_t %.*s_cstr_length(uint64_t address)", S(g->shim));
    writ_emit_line(emit, "return address ? strlen((const char *)(uintptr_t)address) : 0;");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_emit_open(emit, "void %.*s_cstr_copy(uint64_t address, void *out)", S(g->shim));
    writ_emit_line(emit, "size_t length = 0;");
    writ_emit_line(emit, "uint8_t *data = n00b_ffi_borrow(out, &length, %d);", N00B_FFI_WRITE);
    writ_emit_open(emit, "if (data && address && length)");
    writ_emit_line(emit, "memcpy(data, (const void *)(uintptr_t)address, length);");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "n00b_ffi_release(out);");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
}

static void
emit_cstr_n00b_helpers(const n00b_t *g, writ_emit_t *emit)
{
    writ_str_t length_params[] = {writ_cstr("uint")};
    writ_str_t copy_params[] = {writ_cstr("uint"), writ_cstr("Ref[Array[u8]]")};
    add_extern(g, writ_cstr("__ffi_cstr_length"), writ_str_format(g->session, "%.*s_cstr_length", S(g->shim)),
               length_params, 1, writ_cstr("uint"));
    add_extern(g, writ_cstr("__ffi_cstr_copy"), writ_str_format(g->session, "%.*s_cstr_copy", S(g->shim)),
               copy_params, 2, writ_cstr("nil"));
    writ_emit_line(emit, "");
    writ_emit_open(emit, "func __ffi_copy_cstr(address: uint, text: Ref[string]) -> bool");
    writ_emit_line(emit, "var bytes: Array[u8] = new[Array[u8]](cast[int]__ffi_cstr_length(address))");
    writ_emit_line(emit, "__ffi_cstr_copy(address, ref bytes)");
    writ_emit_line(emit, "return __string_from_utf8(ref bytes, text)");
    writ_emit_close(emit, nullptr);
}

static writ_str_t
shim_c_text(const n00b_t *g, const writ_plan_view_t *const *plans)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_line(&emit, "#include <stdint.h>");
    writ_emit_line(&emit, "#include <stddef.h>");
    writ_emit_line(&emit, "#include <stdlib.h>");
    writ_emit_line(&emit, "#include <string.h>");
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#include \"%.*s_writ.h\"", S(g->c.prefix));
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "void *n00b_ffi_borrow(void *object, size_t *length, uint32_t access);");
    writ_emit_line(&emit, "void n00b_ffi_release(void *object);");
    writ_emit_line(&emit, "void n00bb_runtime_trap(const char *reason, uint64_t length);");
    writ_emit_line(&emit, "");
    if (shim_keeps(g)) {
        emit_kept_free(g, &emit);
    }
    writ_emit_open(&emit, "void %.*s_take_bytes(%.*s_bytes_t *bytes, void *out)", S(g->shim), S(g->c.prefix));
    writ_emit_line(&emit, "size_t length = 0;");
    writ_emit_line(&emit, "uint8_t *data = n00b_ffi_borrow(out, &length, 1);");
    writ_emit_open(&emit, "if (data && bytes->data)");
    writ_emit_line(&emit, "memcpy(data, bytes->data, length < bytes->length ? length : bytes->length);");
    writ_emit_close(&emit, nullptr);
    writ_emit_line(&emit, "n00b_ffi_release(out);");
    writ_emit_line(&emit, "%.*s_bytes_free(bytes);", S(g->c.prefix));
    writ_emit_close(&emit, nullptr);
    writ_emit_line(&emit, "");
    emit_c_array_takers(g, &emit);
    emit_record_takers(g, &emit, emit_c_record_taker);
    if (copies_cstr(g)) {
        emit_cstr_c_helpers(g, &emit);
    }
    for (size_t i = 0; i < writ_model_callback_count(g->model); i++) {
        const writ_fn_view_t *callback = writ_model_callback_at(g->model, i);
        if (userdata_receiver(callback) != WRIT_PLAN_NONE) {
            writ_emit_line(&emit, "int32_t %.*s(uint64_t handle, const void *arguments, void *result);",
                           S(callback_dispatch_symbol(g, callback)));
        }
    }
    for (size_t i = 0; i < writ_model_callback_count(g->model); i++) {
        emit_trampoline(g, &emit, writ_model_callback_at(g->model, i));
    }
    for (size_t i = 0; i < writ_model_fn_count(g->model); i++) {
        writ_bound_fn_t bound = bound_fn(g, i);
        for (size_t k = 0; bound.bound && !bound.raw && plans[i] && bound_plan(g, i, k); k++) {
            emit_c_shim(g, &emit, i, bound_plan(g, i, k));
        }
    }
    return writ_emit_finish(&emit, g->session);
}

static writ_str_t field_spec(const n00b_t *g, const writ_record_view_t *record, const writ_slot_view_t *field);

static writ_str_t
foreign_element(const n00b_t *g, const writ_type_view_t *type)
{
    const writ_model_t *model = g->model;
    switch (type ? type->kind : WRIT_TYPE_VOID) {
    case WRIT_TYPE_BOOL:
    case WRIT_TYPE_INT:
    case WRIT_TYPE_ENUM:
        return scalar_type(g, type);
    case WRIT_TYPE_FLOAT:
        return type->size == 8 ? scalar_type(g, type) : (writ_str_t){0};
    case WRIT_TYPE_CSTR:
        return prim(g, "i%u", 8);
    case WRIT_TYPE_RECORD: {
        const writ_record_view_t *record = writ_model_record_at(model, type->decl);
        return expressible(g, record) && !liftable_record(model, record) ? writ_binding_type(g->b, type->name)
                                                                         : (writ_str_t){0};
    }
    default:
        return (writ_str_t){0};
    }
}

static bool
sibling_field(const n00b_t *g, const writ_record_view_t *record, uint32_t expr, writ_str_t *name)
{
    const writ_model_t *model = g->model;
    writ_ref_t ref = writ_expr_ref(model, expr);
    if (ref.kind != WRIT_REF_SLOT || ref.phase != WRIT_REF_VALUE || ref.index >= model->slots.len) {
        return false;
    }
    writ_str_t wanted = writ_sym_str(model->session, model->slots.data[ref.index].name);
    for (size_t f = 0; f < record->fields_count; f++) {
        if (writ_str_eq(record->fields[f].name, wanted)) {
            *name = writ_profile_name(g->profile, WRIT_NAMING_FIELD, wanted);
            return true;
        }
    }
    return false;
}

static bool
foreign_clause(const n00b_t *g, const writ_record_view_t *record, const writ_slot_view_t *field, writ_str_t *clause)
{
    const writ_type_view_t *type = field->type;
    writ_str_t name = {0};
    int64_t sentinel = 0;
    *clause = writ_cstr("");
    if (type->kind == WRIT_TYPE_POINTER) {
        *clause = type->target->kind == WRIT_TYPE_CSTR ? writ_cstr(" until 0") : *clause;
        return true;
    }
    switch (field->extent_kind) {
    case WRIT_EXTENT_NONE:
        return true;
    case WRIT_EXTENT_COUNT:
    case WRIT_EXTENT_BOUNDED:
        if (!sibling_field(g, record, field->extent_kind == WRIT_EXTENT_COUNT ? field->count_expr : field->len_expr,
                           &name)) {
            return false;
        }
        *clause = writ_str_format(g->session, " count %.*s", S(name));
        return true;
    case WRIT_EXTENT_UNTIL:
        if (!writ_expr_eval(g->model, field->count_expr, nullptr, &sentinel)) {
            return false;
        }
        *clause = writ_str_format(g->session, " until %lld", (long long)sentinel);
        return true;
    default:
        return false;
    }
}

static writ_str_t
foreign_pointee(const n00b_t *g, const writ_type_view_t *target)
{
    if (target && (target->kind == WRIT_TYPE_POINTER || target->kind == WRIT_TYPE_ARRAY)) {
        writ_str_t inner = foreign_pointee(g, target->target);
        return writ_str_format(g->session, "Foreign[%.*s]?", S(inner.length ? inner : writ_cstr("nil")));
    }
    return foreign_element(g, target);
}

static writ_str_t
foreign_pointer(const n00b_t *g, const writ_record_view_t *record, const writ_slot_view_t *field)
{
    writ_str_t element = record ? foreign_pointee(g, field->type->target) : foreign_element(g, field->type->target);
    writ_str_t clause = {0};
    if (!element.length || (record && !foreign_clause(g, record, field, &clause))) {
        return writ_cstr(record ? "Foreign[nil]?" : "uint");
    }
    return writ_str_format(g->session, "Foreign[%.*s]?%.*s", S(element), S(clause));
}

static writ_str_t
foreign_callback(const n00b_t *g, const writ_type_view_t *type)
{
    const writ_fn_view_t *fn = writ_model_callback_named(g->model, type->name);
    if (!fn) {
        return writ_cstr("uint");
    }
    writ_emit_t emit;
    writ_emit_init(&emit);
    for (size_t s = 0; s < fn->slots_count; s++) {
        writ_str_t param = field_spec(g, nullptr, &fn->slots[s]);
        if (!param.length || writ_str_eq(param, writ_cstr("union"))) {
            writ_emit_discard(&emit);
            return writ_cstr("uint");
        }
        writ_emit_text(&emit, "%s%.*s", s ? ", " : "", S(param));
    }
    writ_str_t result = fn->result ? field_spec(g, nullptr, fn->result) : writ_cstr("nil");
    writ_str_t params = writ_emit_finish(&emit, g->session);
    return result.length ? writ_str_format(g->session, "Foreign_Fn[(%.*s) -> %.*s]?", S(params), S(result))
                         : writ_cstr("uint");
}

static bool
union_expressible(const n00b_t *g, const writ_record_view_t *tagged)
{
    for (size_t f = 0; f < tagged->fields_count; f++) {
        writ_str_t spec = field_spec(g, tagged, &tagged->fields[f]);
        if (!spec.length || writ_str_eq(spec, writ_cstr("union"))) {
            return false;
        }
    }
    return tagged->arms_count != 0;
}

static writ_str_t
field_spec(const n00b_t *g, const writ_record_view_t *record, const writ_slot_view_t *field)
{
    const writ_type_view_t *type = field->type;
    switch (type ? type->kind : WRIT_TYPE_VOID) {
    case WRIT_TYPE_BOOL:
    case WRIT_TYPE_INT:
    case WRIT_TYPE_ENUM:
    case WRIT_TYPE_HANDLE:
        return scalar_type(g, type);
    case WRIT_TYPE_FLOAT:
        return type->size == 8 ? scalar_type(g, type) : (writ_str_t){0};
    case WRIT_TYPE_VIEW: {
        writ_slot_view_t inner = *field;
        inner.type = type->target;
        return field_spec(g, record, &inner);
    }
    case WRIT_TYPE_POINTER:
    case WRIT_TYPE_ARRAY:
        return foreign_pointer(g, record, field);
    case WRIT_TYPE_CALLBACK:
        return foreign_callback(g, type);
    case WRIT_TYPE_RECORD:
        return foreign_element(g, type);
    case WRIT_TYPE_UNION:
        return record && union_expressible(g, writ_model_record_at(g->model, type->decl)) ? writ_cstr("union")
                                                                                          : (writ_str_t){0};
    default:
        return (writ_str_t){0};
    }
}

static bool
expressible(const n00b_t *g, const writ_record_view_t *record)
{
    if (record->type->kind == WRIT_TYPE_UNION) {
        return false;
    }
    for (size_t f = 0; f < record->fields_count; f++) {
        if (!field_spec(g, record, &record->fields[f]).length) {
            return false;
        }
    }
    return true;
}

static void
emit_layout_field(const n00b_t *g, writ_emit_t *emit, const writ_record_view_t *record, const writ_slot_view_t *field)
{
    writ_str_t name = writ_profile_name(g->profile, WRIT_NAMING_FIELD, field->name);
    writ_str_t spec = field_spec(g, record, field);
    if (field->type->kind != WRIT_TYPE_UNION) {
        writ_emit_line(emit, "public %.*s: %.*s", S(name), S(spec));
        return;
    }
    const writ_record_view_t *tagged = writ_model_record_at(g->model, field->type->decl);
    writ_emit_open(emit, "public %.*s: union by %.*s", S(name),
                   S(writ_profile_name(g->profile, WRIT_NAMING_FIELD, record->fields[field->tag_field].name)));
    for (size_t a = 0; a < tagged->arms_count; a++) {
        const writ_union_arm_view_t *arm = &tagged->arms[a];
        if (arm->field == WRIT_PLAN_NONE) {
            writ_emit_line(emit, "%lld => _", (long long)arm->value);
            continue;
        }
        const writ_slot_view_t *member = &tagged->fields[arm->field];
        writ_emit_line(emit, "%lld => %.*s: %.*s", (long long)arm->value,
                       S(writ_profile_name(g->profile, WRIT_NAMING_FIELD, member->name)),
                       S(field_spec(g, tagged, member)));
    }
    writ_emit_close(emit, nullptr);
}

static void
emit_opaque_record(const n00b_t *g, writ_emit_t *emit, const writ_record_view_t *record)
{
    writ_str_t type = writ_binding_type(g->b, record->name);
    writ_emit_open(emit, "record %.*s", S(type));
    writ_emit_line(emit, "private storage: %.*s", S(buffer_type(g)));
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_emit_open(emit, "func %.*s() -> %.*s", S(writ_profile_name(g->profile, WRIT_NAMING_FN, record->name)),
                   S(type));
    writ_emit_line(emit, "let value: %.*s = (storage = new[%.*s](%u))", S(type), S(buffer_type(g)),
                   record->type ? record->type->size : 0);
    writ_emit_line(emit, "return value");
    writ_emit_close(emit, nullptr);
    for (size_t f = 0; f < record->fields_count; f++) {
        const writ_slot_view_t *field = &record->fields[f];
        if (field->type && (field->type->kind == WRIT_TYPE_UNION || field->type->kind == WRIT_TYPE_RECORD)) {
            continue;
        }
        writ_str_t name = writ_profile_name(g->profile, WRIT_NAMING_FIELD, field->name);
        writ_str_t value = field->type && field->type->kind == WRIT_TYPE_FLOAT ? prim(g, "f%u", 64)
                                                                              : scalar_type(g, field->type);
        writ_str_t layout = writ_str_format(g->session, "%c%u", field->type && field->type->kind == WRIT_TYPE_FLOAT
                                                                    ? 'f' : field->type && field->type->is_signed
                                                                    ? 'i' : 'u',
                                            field->type ? field->type->size * 8 : 0);
        writ_emit_line(emit, "");
        writ_emit_open(emit, "func %.*s(self: %.*s) -> %.*s", S(name), S(type), S(value));
        writ_emit_line(emit, "return __ffi_read_%.*s(ref self.storage, %u)", S(layout), field->offset);
        writ_emit_close(emit, nullptr);
        writ_emit_line(emit, "");
        writ_emit_open(emit, "func set_%.*s(modify self: %.*s, value: %.*s)", S(name), S(type), S(value));
        writ_emit_line(emit, "__ffi_write_%.*s(ref self.storage, %u, value)", S(layout), field->offset);
        writ_emit_close(emit, nullptr);
    }
}

static int
single_bit(int64_t value)
{
    if (value <= 0 || (value & (value - 1))) {
        return -1;
    }
    int bit = 0;
    while (!(value & 1)) {
        value >>= 1;
        bit++;
    }
    return bit;
}

static void
emit_flagset(const n00b_t *g, writ_emit_t *emit, const writ_enum_view_t *view)
{
    writ_emit_open(emit, "flagset %.*s", S(writ_binding_type(g->b, view->name)));
    int highest = -1;
    for (size_t k = 0; k < view->items_count; k++) {
        int bit = single_bit(view->items[k].value);
        highest = bit > highest ? bit : highest;
    }
    for (int bit = 0; bit <= highest; bit++) {
        const char *name = nullptr;
        writ_str_t chosen = {0};
        for (size_t k = 0; k < view->items_count && !name; k++) {
            if (single_bit(view->items[k].value) == bit) {
                chosen = writ_profile_name(g->profile, WRIT_NAMING_CONST, view->items[k].name);
                name = chosen.data;
            }
        }
        if (name) {
            writ_emit_line(emit, "%.*s,", S(chosen));
        } else {
            writ_emit_line(emit, "unused_bit_%d,", bit);
        }
    }
    for (size_t k = 0; k < view->items_count; k++) {
        int64_t value = view->items[k].value;
        if (single_bit(value) >= 0 || value <= 0) {
            continue;
        }
        core_buffer_t *line = core_buffer_empty();
        for (size_t p = 0; p < view->items_count; p++) {
            int bit = single_bit(view->items[p].value);
            if (bit >= 0 && (value & view->items[p].value)) {
                writ_str_t part = writ_profile_name(g->profile, WRIT_NAMING_CONST, view->items[p].name);
                core_buffer_printf(line, "%s%.*s", line->byte_len ? " + " : "", S(part));
            }
        }
        writ_emit_line(emit, "%.*s = %.*s,", S(writ_profile_name(g->profile, WRIT_NAMING_CONST, view->items[k].name)),
                       (int)line->byte_len, line->data);
        core_buffer_free(line);
    }
    writ_emit_close(emit, nullptr);
}

static void
emit_callback_role(const n00b_t *g, writ_emit_t *emit, const writ_fn_view_t *callback, const char *role,
                   uint32_t receiver)
{
    writ_emit_text(emit, "%s(", role);
    bool first = true;
    for (size_t i = 0; i < callback->slots_count; i++) {
        if (i != receiver) {
            writ_emit_text(emit, "%s%.*s: %.*s", first ? "" : ", ", S(writ_profile_name(g->profile, WRIT_NAMING_FIELD,
                                                                                        callback->slots[i].name)),
                           S(scalar_type(g, callback->slots[i].type)));
            first = false;
        }
    }
    writ_emit_line(emit, ") -> %.*s", S(scalar_type(g, callback->result ? callback->result->type : nullptr)));
}

static writ_str_t
callback_table(const n00b_t *g, writ_str_t type, const char *suffix)
{
    return writ_str_format(g->session, "__%.*s_%s", S(writ_name_case(g->session, type, WRIT_CASE_SNAKE)), suffix);
}

static writ_str_t
callback_dispatch_symbol(const n00b_t *g, const writ_fn_view_t *callback)
{
    return writ_str_format(g->session, "%.*s_%.*s", S(g->shim),
                           S(callback_table(g, writ_binding_type(g->b, callback->contract_name), "dispatch")));
}

static void
emit_table(writ_emit_t *emit, writ_str_t table, writ_str_t element)
{
    writ_emit_line(emit,
                   "var %.*s: Ffi_Table[%.*s] = (slots = new[List[%.*s?]](), free = new[List[int]](), owners = "
                   "new[List[uint]]())",
                   S(table), S(element), S(element));
}

static void
emit_callback_invoker(const n00b_t *g, writ_emit_t *emit, const writ_fn_view_t *callback, writ_str_t name,
                      uint32_t receiver, bool packed)
{
    writ_str_t snake = writ_name_case(g->session, name, WRIT_CASE_SNAKE);
    writ_str_t result = scalar_type(g, callback->result ? callback->result->type : nullptr);
    bool returns = !writ_str_eq(result, writ_cstr("nil"));
    writ_str_t copy = writ_str_format(g->session, "__%.*s_arguments_copy", S(snake));
    writ_str_t store = writ_str_format(g->session, "__%.*s_result_store", S(snake));
    if (packed) {
        writ_str_t params[] = {writ_cstr("uint"), writ_str_format(g->session, "Ref[%.*s_Arguments]", S(name))};
        add_extern(g, copy, callback_helper_symbol(g, callback, "arguments_copy"), params, 2, writ_cstr("nil"));
    }
    if (returns) {
        writ_str_t params[] = {writ_cstr("uint"), result};
        add_extern(g, store, callback_helper_symbol(g, callback, "result_store"), params, 2, writ_cstr("nil"));
    }
    writ_str_t entry = writ_str_format(g->session, "%.*s_Callback", S(name));
    writ_str_t table = callback_table(g, name, "callbacks");
    writ_emit_line(emit, "");
    writ_emit_open(emit, "record %.*s", S(entry));
    writ_emit_line(emit, "public value: %.*s", S(name));
    writ_emit_line(emit, "public once: bool");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    emit_table(emit, table, entry);
    writ_emit_line(emit, "");
    writ_emit_open(emit, "func %.*s(value: %.*s, once: bool) -> u64", S(callback_table(g, name, "register")), S(name));
    writ_emit_line(emit, "let entry: %.*s = (value = value, once = once)", S(entry));
    writ_emit_line(emit, "return __ffi_table_insert(%.*s, entry)", S(table));
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_emit_open(emit, "func %.*s(handle: u64)", S(callback_table(g, name, "free")));
    writ_emit_line(emit, "__ffi_table_free(%.*s, handle)", S(table));
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_emit_open(emit, "func %.*s(handle: u64, arguments: uint, result: uint) -> i32",
                   S(callback_table(g, name, "dispatch")));
    writ_emit_line(emit, "let entry = __ffi_table_get(%.*s, handle)", S(table));
    writ_emit_open(emit, "if entry == nil");
    writ_emit_line(emit, "return -1");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "let value = entry.value");
    writ_emit_line(emit, "let once = entry.once");
    if (packed) {
        writ_emit_line(emit, "var packed: %.*s_Arguments", S(name));
        writ_emit_line(emit, "%.*s(arguments, ref packed)", S(copy));
    }
    writ_emit_text(emit, returns ? "%.*s(result, value.call(" : "value.call(", S(store));
    bool first = true;
    for (size_t i = 0; i < callback->slots_count; i++) {
        if (i != receiver) {
            writ_emit_text(emit, "%spacked.%.*s", first ? "" : ", ",
                           S(writ_profile_name(g->profile, WRIT_NAMING_FIELD, callback->slots[i].name)));
            first = false;
        }
    }
    writ_emit_line(emit, returns ? "))" : ")");
    writ_emit_open(emit, "if once");
    writ_emit_line(emit, "%.*s(handle)", S(callback_table(g, name, "free")));
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "return 0");
    writ_emit_close(emit, nullptr);
}

static void
emit_callback_type(const n00b_t *g, writ_emit_t *emit, const writ_fn_view_t *callback)
{
    uint32_t receiver = userdata_receiver(callback);
    if (receiver == WRIT_PLAN_NONE) {
        return;
    }
    writ_str_t name = writ_binding_type(g->b, callback->contract_name);
    writ_emit_line(emit, "");
    mark(g, emit, callback->declared_at, callback->contract_name);
    writ_emit_open(emit, "callback %.*s", S(name));
    emit_callback_role(g, emit, callback, "call", receiver);
    emit_callback_role(g, emit, callback, "handler", receiver);
    writ_emit_close(emit, nullptr);
    bool packed = callback->slots_count > 1;
    if (packed) {
        writ_emit_line(emit, "");
        writ_emit_open(emit, "record %.*s_Arguments layout(c)", S(name));
        for (size_t i = 0; i < callback->slots_count; i++) {
            if (i != receiver) {
                writ_emit_line(emit, "public %.*s: %.*s",
                               S(writ_profile_name(g->profile, WRIT_NAMING_FIELD, callback->slots[i].name)),
                               S(scalar_type(g, callback->slots[i].type)));
            }
        }
        writ_emit_close(emit, nullptr);
    }
    emit_callback_invoker(g, emit, callback, name, receiver, packed);
}

static bool contract_has_event(const n00b_t *g, writ_str_t id);
static void emit_event(writ_emit_t *emit, writ_str_t id, writ_str_t keys);

static writ_str_t
cell_constructor(const n00b_t *g, writ_str_t type)
{
    return writ_str_format(g->session, "__%.*s_cell", S(writ_name_case(g->session, type, WRIT_CASE_SNAKE)));
}

static const writ_plan_view_t *
release_plan(const n00b_t *g, const writ_handle_view_t *handle)
{
    for (size_t i = 0; i < writ_model_fn_count(g->model); i++) {
        writ_bound_fn_t bound = bound_fn(g, i);
        if (bound.bound && !bound.raw && writ_str_eq(writ_model_fn_at(g->model, i)->contract_name, handle->release)) {
            return bound_plan(g, i, 0);
        }
    }
    return nullptr;
}

static const writ_slot_view_t *
with_source_slot(const writ_fn_view_t *fn)
{
    if (!fn->result || fn->result->span != WRIT_SPAN_WITH || !fn->result->span_with.length || !fn->result->type
        || fn->result->type->kind != WRIT_TYPE_HANDLE) {
        return nullptr;
    }
    for (size_t s = 0; s < fn->slots_count; s++) {
        const writ_slot_view_t *slot = &fn->slots[s];
        if (writ_str_eq(slot->name, fn->result->span_with) && slot->type && slot->type->kind == WRIT_TYPE_HANDLE) {
            return slot;
        }
    }
    return nullptr;
}

static writ_str_t
keep_field(const n00b_t *g, writ_str_t source_type)
{
    return writ_str_format(g->session, "keep_%.*s", S(writ_name_case(g->session, source_type, WRIT_CASE_SNAKE)));
}

static writ_str_t
with_constructor(const n00b_t *g, writ_str_t type, writ_str_t source_type)
{
    return writ_str_format(g->session, "%.*s_with_%.*s", S(cell_constructor(g, type)),
                           S(writ_name_case(g->session, source_type, WRIT_CASE_SNAKE)));
}

static size_t
handle_sources(const n00b_t *g, const writ_handle_view_t *handle, writ_str_t *out)
{
    size_t count = 0;
    for (size_t i = 0; i < writ_model_fn_count(g->model); i++) {
        const writ_fn_view_t *fn = writ_model_fn_at(g->model, i);
        const writ_slot_view_t *source = with_source_slot(fn);
        if (!bound_fn(g, i).bound || !source || !writ_str_eq(fn->result->type->name, handle->name)) {
            continue;
        }
        writ_str_t type = writ_binding_type(g->b, source->type->name);
        bool seen = false;
        for (size_t s = 0; s < count && !seen; s++) {
            seen = writ_str_eq(out[s], type);
        }
        if (!seen) {
            out[count++] = type;
        }
    }
    return count;
}

static void
emit_cell_constructor(writ_emit_t *emit, writ_str_t type, writ_str_t name, writ_str_t keep_param,
                      const writ_str_t *fields, size_t count, size_t kept, writ_str_t raw)
{
    writ_emit_open(emit, "func %.*s(raw: uint%.*s) -> %.*s", S(name), S(keep_param), S(type));
    writ_emit_text(emit, "var cell: %.*s_Cell = (raw = %.*s", S(type), S(raw));
    for (size_t f = 0; f < count; f++) {
        writ_emit_text(emit, ", %.*s = %s", S(fields[f]), f == kept ? "keep" : "nil");
    }
    writ_emit_line(emit, ")");
    writ_emit_line(emit, "return ref cell");
    writ_emit_close(emit, nullptr);
}

static void
emit_handle_type(const n00b_t *g, writ_emit_t *emit, const writ_handle_view_t *handle)
{
    writ_str_t type = writ_binding_type(g->b, handle->name);
    writ_str_t *sources = writ_alloc(g->session, sizeof(writ_str_t) * (writ_model_fn_count(g->model) + 1));
    size_t source_count = handle_sources(g, handle, sources);
    writ_str_t *fields = writ_alloc(g->session, sizeof(writ_str_t) * (source_count + 1));
    writ_emit_line(emit, "");
    mark(g, emit, handle->declared_at, handle->name);
    const record_handle_t *typed = typed_handle(g, handle->type);
    writ_str_t raw = raw_of(g, handle->type, writ_cstr("self"));
    writ_str_t initial = typed ? writ_str_format(g->session, "unsafe_cast[%.*s] raw", S(typed->raw_type))
                               : writ_cstr("raw");
    writ_emit_open(emit, "record %.*s_Cell", S(type));
    writ_emit_line(emit, "public raw: %.*s", S(typed ? typed->raw_type : writ_cstr("uint")));
    for (size_t s = 0; s < source_count; s++) {
        fields[s] = keep_field(g, sources[s]);
        writ_emit_line(emit, "public %.*s: %.*s?", S(fields[s]), S(sources[s]));
    }
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_emit_line(emit, "alias %.*s = Ref[%.*s_Cell]", S(type), S(type));
    writ_emit_line(emit, "");
    emit_cell_constructor(emit, type, cell_constructor(g, type), writ_cstr(""), fields, source_count, source_count,
                          initial);
    for (size_t s = 0; s < source_count; s++) {
        writ_emit_line(emit, "");
        emit_cell_constructor(emit, type, with_constructor(g, type, sources[s]),
                              writ_str_format(g->session, ", keep: %.*s", S(sources[s])), fields, source_count, s,
                              initial);
    }
    const writ_plan_view_t *release = release_plan(g, handle);
    if (!release) {
        return;
    }
    writ_emit_line(emit, "");
    writ_emit_open(emit, "func __finalize(consume self: Ref[%.*s_Cell])", S(type));
    if (typed) {
        writ_emit_open(emit, "if self.raw != nil and debug_enabled() and !__abi_baked_address(%.*s)", S(raw));
    } else {
        writ_emit_open(emit, "if self.raw != 0 and debug_enabled()");
    }
    writ_emit_line(emit, "__fd_write(2, \"%.*s collected while open; releasing it\\n\")", S(type));
    writ_str_t key = writ_str_format(g->session, "cast[u64](%.*s)", S(raw));
    writ_str_t release_event = writ_str_format(g->session, "%.*s:release:%.*s", S(g->shim), S(handle->name));
    if (contract_has_event(g, release_event)) {
        emit_event(emit, release_event, key);
    }
    writ_str_t release_call = writ_str_format(g->session, "%.*s:%.*s", S(g->shim), S(release->fn->contract_name));
    if (contract_has_event(g, release_call)) {
        emit_event(emit, release_call, key);
    }
    writ_emit_line(emit, "let released = __%.*s(%.*s)", S(writ_c_plan_name(&g->c, release)), S(raw));
    writ_emit_close(emit, nullptr);
    writ_emit_close(emit, nullptr);
}

static void
emit_types(const n00b_t *g, writ_emit_t *emit, bool handles)
{
    const writ_model_t *model = g->model;
    writ_emit_open(emit, "record Error");
    writ_emit_line(emit, "public code: %.*s", S(code_type(g)));
    writ_emit_line(emit, "public text: string?");
    writ_emit_line(emit, "public fn: string");
    writ_emit_close(emit, nullptr);
    for (size_t i = 0; handles && i < writ_model_handle_count(model); i++) {
        emit_handle_type(g, emit, writ_model_handle_at(model, i));
    }
    for (size_t i = 0; i < writ_model_record_count(model); i++) {
        const writ_record_view_t *record = writ_model_record_at(model, i);
        if (record->arms_count) {
            continue;
        }
        writ_emit_line(emit, "");
        mark(g, emit, record->declared_at, record->name);
        if (liftable_record(model, record)) {
            writ_emit_open(emit, "record %.*s", S(writ_binding_type(g->b, record->name)));
            for (size_t f = 0; f < record->fields_count; f++) {
                const writ_slot_view_t *field = &record->fields[f];
                if (writ_c_count_field(model, field).length) {
                    writ_emit_line(emit, "public %.*s: Array[%.*s]",
                                   S(writ_profile_name(g->profile, WRIT_NAMING_FIELD, field->name)),
                                   S(scalar_type(g, field->type->target)));
                }
            }
            writ_emit_close(emit, nullptr);
            continue;
        }
        if (!expressible(g, record)) {
            emit_opaque_record(g, emit, record);
            continue;
        }
        writ_emit_open(emit, "record %.*s layout(c)", S(writ_binding_type(g->b, record->name)));
        for (size_t f = 0; f < record->fields_count; f++) {
            emit_layout_field(g, emit, record, &record->fields[f]);
        }
        writ_emit_close(emit, nullptr);
    }
    for (size_t h = 0; handles && h < g->record_handles->len; h++) {
        emit_handle_type(g, emit, &g->record_handles->data[h].handle);
    }
    for (size_t i = 0; i < writ_model_enum_count(model); i++) {
        const writ_enum_view_t *view = writ_model_enum_at(model, i);
        writ_emit_line(emit, "");
        mark(g, emit, view->declared_at, view->name);
        if (view->is_flags) {
            emit_flagset(g, emit, view);
            continue;
        }
        writ_emit_open(emit, "enum %.*s", S(writ_binding_type(g->b, view->name)));
        for (size_t k = 0; k < view->items_count; k++) {
            writ_str_t item = writ_profile_name(g->profile, WRIT_NAMING_CONST, view->items[k].name);
            writ_emit_line(emit, "%.*s = %lld%s", S(item), (long long)view->items[k].value,
                           k + 1 < view->items_count ? "," : "");
        }
        writ_emit_close(emit, nullptr);
    }
    for (size_t i = 0; i < writ_model_callback_count(model); i++) {
        emit_callback_type(g, emit, writ_model_callback_at(model, i));
    }
    if (writ_model_const_count(model)) {
        writ_emit_line(emit, "");
    }
    for (size_t i = 0; i < writ_model_const_count(model); i++) {
        const writ_const_view_t *constant = writ_model_const_at(model, i);
        writ_type_kind_t kind = constant->type ? constant->type->kind : WRIT_TYPE_VOID;
        if (kind == WRIT_TYPE_ENUM
            || (!constant->text.length && (kind == WRIT_TYPE_POINTER || kind == WRIT_TYPE_CALLBACK))) {
            continue;
        }
        writ_str_t name = writ_profile_name(g->profile, WRIT_NAMING_CONST, constant->name);
        mark(g, emit, constant->declared_at, constant->name);
        if (constant->text.length) {
            writ_emit_line(emit, "public const %.*s: string = \"%.*s\"", S(name), S(constant->text));
        } else {
            writ_emit_line(emit, "public const %.*s: %.*s = %lld", S(name), S(scalar_type(g, constant->type)),
                           (long long)constant->value);
        }
    }
}

static void
emit_extern(const n00b_t *g, writ_emit_t *emit, size_t fn, const writ_plan_view_t *plan)
{
    (void)emit;
    core_list_t(shim_param_t) params = core_list_new(shim_param_t);
    shim_params(g, fn, plan, &params);
    writ_str_t *types = writ_alloc(g->session, sizeof(writ_str_t) * (2 * params.len + 1));
    size_t typed = 0;
    for (size_t i = 0; i < params.len; i++) {
        if (params.data[i].kind != PARAM_OWNED && params.data[i].kind != PARAM_PROVIDED) {
            types[typed++] = params.data[i].n00b_type;
        }
    }
    for (size_t i = 0; i < params.len; i++) {
        if (keeps_until(plan, &params.data[i])) {
            types[typed++] = writ_cstr("Ref[uint]");
        }
    }
    writ_str_t ret = writ_c_returns_directly(&g->c, plan) ? writ_c_return_type(&g->c, plan) : writ_cstr("int32_t");
    const writ_plan_item_view_t *direct = nullptr;
    for (size_t i = 0; i < plan->items_count; i++) {
        direct = plan->items[i].role == WRIT_ROLE_RESULT ? &plan->items[i] : direct;
    }
    writ_str_t n00b_ret = !writ_c_returns_directly(&g->c, plan) ? prim(g, "i%u", 32)
                          : is_pointer(ret)               ? writ_cstr("uint")
                                                          : scalar_type(g, direct ? direct->type : nullptr);
    add_extern_with(g, writ_str_format(g->session, "__%.*s", S(writ_c_plan_name(&g->c, plan))), shim_symbol(g, plan),
                    types, typed, n00b_ret, plan->fn->is_pure);
    core_list_free(params);
}

typedef struct {
    const n00b_t            *g;
    size_t                   fn;
    const writ_plan_view_t  *plan;
    writ_bound_fn_t          bound;
    uint32_t                 receiver;
    writ_str_t               suffix;
} wrapper_t;

static writ_str_t
local_name(const wrapper_t *w, uint32_t item)
{
    if (item == w->receiver) {
        return writ_cstr("self");
    }
    return writ_profile_name(w->g->profile, WRIT_NAMING_FIELD, w->plan->items[item].name);
}

static uint32_t
find_receiver(const wrapper_t *w)
{
    if (!w->bound.owner.length || w->bound.is_static) {
        return WRIT_PLAN_NONE;
    }
    uint32_t first = WRIT_PLAN_NONE;
    for (uint32_t i = 0; i < w->plan->items_count; i++) {
        const writ_plan_item_view_t *item = &w->plan->items[i];
        if (!writ_c_is_input(item)) {
            continue;
        }
        if (writ_str_eq(item_type(w->g, w->fn, w->plan, i), w->bound.owner)) {
            return i;
        }
        first = first == WRIT_PLAN_NONE ? i : first;
    }
    return first;
}

static writ_str_t
receiver_type(const wrapper_t *w, uint32_t item)
{
    return item == w->receiver ? w->bound.owner : item_type(w->g, w->fn, w->plan, item);
}

static writ_str_t
error_type(const wrapper_t *w)
{
    writ_str_t error = w->bound.error;
    return writ_str_eq(error, writ_cstr("Error")) || ends_with(error, ".Error") ? writ_cstr("Error") : error;
}

static bool
consumes(const wrapper_t *w, uint32_t item)
{
    uint32_t slot = w->plan->items[item].contract_slot;
    return slot != WRIT_PLAN_NONE && w->plan->fn->slots[slot].mode == WRIT_MODE_CONSUME;
}

static writ_str_t
unescaped(const n00b_t *g, writ_str_t name)
{
    if (!ends_with(name, "_")) {
        return name;
    }
    writ_str_t base = {name.data, name.length - 1};
    return writ_str_eq(writ_profile_name(g->profile, WRIT_NAMING_FN, base), name) ? base : name;
}

static writ_str_t
wrapper_name(const wrapper_t *w)
{
    writ_str_t name = w->bound.name;
    if (w->bound.owner.length && w->bound.is_static) {
        writ_str_t owner = writ_name_case(w->g->session, w->bound.owner, WRIT_CASE_SNAKE);
        name = writ_profile_name(w->g->profile, WRIT_NAMING_FN,
                                 writ_str_format(w->g->session, "%.*s_%.*s", S(owner), S(unescaped(w->g, name))));
    }
    return w->suffix.length ? writ_str_format(w->g->session, "%.*s_%.*s", S(name), S(w->suffix)) : name;
}

static writ_str_t
arm_member(const n00b_t *g, const writ_policy_arm_view_t *arm)
{
    return arm->is_else ? writ_cstr("OWNED") : writ_profile_name(g->profile, WRIT_NAMING_CONST, arm->constant);
}

static bool
policy_item(const writ_plan_view_t *plan, uint32_t item)
{
    return plan->policy_slot != WRIT_PLAN_NONE && plan->items[item].contract_slot == plan->policy_slot;
}

static size_t
results(const wrapper_t *w, uint32_t *out)
{
    size_t count = 0;
    for (uint32_t i = 0; i < w->plan->items_count; i++) {
        if (is_result(&w->plan->items[i]) && w->plan->items[i].role != WRIT_ROLE_INPUT_OUTPUT) {
            out[count++] = i;
        }
    }
    for (uint32_t i = 0; i < w->plan->items_count; i++) {
        if (w->plan->items[i].role == WRIT_ROLE_INPUT_OUTPUT && is_result(&w->plan->items[i])) {
            out[count++] = i;
        }
    }
    return count;
}

static bool
can_fail(const wrapper_t *w)
{
    return !writ_c_returns_directly(&w->g->c, w->plan);
}

static bool
guards_release(const wrapper_t *w)
{
    return w->receiver != WRIT_PLAN_NONE && released_handle(w->g, w->plan->fn->contract_name)
        && typed_handle(w->g, w->plan->items[w->receiver].type);
}

static bool
declares_failure(const wrapper_t *w)
{
    return can_fail(w) || guards_release(w);
}

static size_t
success_count(const writ_plan_view_t *plan)
{
    size_t count = 0;
    for (size_t i = 0; i < plan->outcomes_count; i++) {
        count += plan->outcomes[i].kind == WRIT_OUTCOME_SUCCESS;
    }
    return count;
}

static bool
lifts_in(const writ_plan_view_t *plan, uint32_t outcome, uint32_t item)
{
    for (size_t i = 0; i < plan->steps_count; i++) {
        const writ_plan_step_view_t *step = &plan->steps[i];
        if (step->phase == WRIT_PHASE_OUTCOME && step->outcome == outcome && step->item == item) {
            return true;
        }
    }
    return false;
}

static uint32_t
optional_result_item(const writ_plan_view_t *plan)
{
    uint32_t found = WRIT_PLAN_NONE;
    for (uint32_t i = 0; i < plan->items_count; i++) {
        if (is_result(&plan->items[i])) {
            if (found != WRIT_PLAN_NONE || !plan->items[i].optional) {
                return WRIT_PLAN_NONE;
            }
            found = i;
        }
    }
    size_t lifting = 0, absent = 0;
    for (uint32_t o = 0; found != WRIT_PLAN_NONE && o < plan->outcomes_count; o++) {
        if (plan->outcomes[o].kind == WRIT_OUTCOME_SUCCESS) {
            lifts_in(plan, o, found) ? lifting++ : absent++;
        }
    }
    return lifting == 1 && absent == 1 ? found : WRIT_PLAN_NONE;
}

static bool
outcome_enum(const writ_plan_view_t *plan)
{
    return success_count(plan) > 1 && optional_result_item(plan) == WRIT_PLAN_NONE;
}

static writ_str_t
result_type_name(const wrapper_t *w)
{
    return writ_str_format(w->g->session, "%.*s_Result", S(writ_name_case(w->g->session, wrapper_name(w),
                                                                            WRIT_CASE_TITLE_SNAKE)));
}

static writ_str_t
outcome_type_name(const wrapper_t *w)
{
    return writ_str_format(w->g->session, "%.*s_Outcome", S(writ_name_case(w->g->session, wrapper_name(w),
                                                                             WRIT_CASE_TITLE_SNAKE)));
}

static void
emit_result_types(const wrapper_t *w, writ_emit_t *emit, const uint32_t *outs, size_t count)
{
    if (outcome_enum(w->plan)) {
        writ_emit_line(emit, "");
        writ_emit_open(emit, "enum %.*s", S(outcome_type_name(w)));
        size_t emitted = 0;
        size_t total = success_count(w->plan);
        for (size_t i = 0; i < w->plan->outcomes_count; i++) {
            if (w->plan->outcomes[i].kind == WRIT_OUTCOME_SUCCESS) {
                writ_emit_line(emit, "%.*s = %zu%s", S(w->plan->outcomes[i].name), i, ++emitted < total ? "," : "");
            }
        }
        writ_emit_close(emit, nullptr);
    }
    bool multiple_success = outcome_enum(w->plan);
    if (count > 1 || (count && multiple_success)) {
        writ_emit_line(emit, "");
        writ_emit_open(emit, "record %.*s", S(result_type_name(w)));
        if (multiple_success) {
            writ_emit_line(emit, "public outcome: %.*s", S(outcome_type_name(w)));
        }
        for (size_t i = 0; i < count; i++) {
            writ_emit_line(emit, "public %.*s: %.*s", S(local_name(w, outs[i])), S(item_type(w->g, w->fn, w->plan, outs[i])));
        }
        writ_emit_close(emit, nullptr);
    }
}

static void
emit_fail(const wrapper_t *w, writ_emit_t *emit, writ_str_t code, writ_str_t text)
{
    writ_str_t type = error_type(w);
    if (writ_str_eq(type, writ_cstr("Error"))) {
        writ_emit_line(emit, "let failure: Error = (code = %.*s, text = %.*s, fn = \"%.*s\")", S(code), S(text),
                       S(w->plan->fn->contract_name));
        writ_emit_line(emit, "fail failure");
        return;
    }
    writ_emit_line(emit, "fail new[%.*s](%.*s, %.*s, \"%.*s\")", S(type), S(code), S(text),
                   S(w->plan->fn->contract_name));
}

static writ_str_t
detail_text(const wrapper_t *w, uint32_t outcome, bool text)
{
    const writ_plan_view_t *plan = w->plan;
    writ_session_t *session = w->g->session;
    for (size_t s = 0; s < plan->steps_count; s++) {
        const writ_plan_step_view_t *step = &plan->steps[s];
        if (step->kind != WRIT_STEP_FETCH_DETAIL || step->outcome != outcome || step->item == WRIT_PLAN_NONE) {
            continue;
        }
        const writ_plan_item_view_t *item = &plan->items[step->item];
        bool is_text = item->shape == WRIT_SHAPE_TEXT;
        if (text && is_text) {
            return writ_str_format(session, "__ffi_take_string(ref %.*s)", S(local_name(w, step->item)));
        }
        if (!text && !is_text) {
            return writ_str_format(session, "cast[%.*s]%.*s", S(code_type(w->g)), S(local_name(w, step->item)));
        }
    }
    return text ? writ_cstr("nil") : writ_str_format(session, "cast[%.*s]outcome", S(code_type(w->g)));
}

static writ_str_t
construct_handle(const wrapper_t *w, uint32_t index, writ_str_t raw)
{
    const writ_plan_item_view_t *item = &w->plan->items[index];
    writ_str_t type = item_type(w->g, w->fn, w->plan, index);
    const writ_slot_view_t *source = with_source_slot(w->plan->fn);
    if (source && item->role == WRIT_ROLE_RESULT) {
        for (uint32_t i = 0; i < w->plan->items_count; i++) {
            if (writ_str_eq(w->plan->items[i].name, source->name) && w->plan->items[i].role == WRIT_ROLE_INPUT) {
                writ_str_t source_type = writ_binding_type(w->g->b, source->type->name);
                return writ_str_format(w->g->session, "%.*s(%.*s, %.*s)", S(with_constructor(w->g, type, source_type)),
                                       S(raw), S(local_name(w, i)));
            }
        }
    }
    return writ_str_format(w->g->session, "%.*s(%.*s)", S(cell_constructor(w->g, type)), S(raw));
}

static writ_str_t
lifted(const wrapper_t *w, const shim_param_t *param)
{
    const writ_plan_item_view_t *item = &w->plan->items[param->item];
    writ_str_t name = local_name(w, param->item);
    writ_session_t *session = w->g->session;
    if (ends_with(param->n00b_type, "Ffi_Bytes]")) {
        const writ_record_view_t *record = lifted_record(w->g, record_target(item));
        if (record) {
            return writ_str_format(session, "__ffi_take_%s%.*s(ref %.*s)", optional_result(item) ? "optional_" : "",
                                   S(record->name), S(name));
        }
        const char *format = text_result(w->g, w->fn, w->plan, param) ? "%.*s_text"
                           : optional_result(item)                   ? "__ffi_take_optional_bytes(ref %.*s)"
                                                                     : "__ffi_take_bytes(ref %.*s)";
        return writ_str_format(session, format, S(name));
    }
    if (item->shape == WRIT_SHAPE_HANDLE || (item->type && item->type->kind == WRIT_TYPE_HANDLE)) {
        return construct_handle(w, param->item, name);
    }
    if (item->shape == WRIT_SHAPE_TEXT) {
        return writ_str_format(session, "%.*s_text", S(name));
    }
    if (item->shape == WRIT_SHAPE_ARRAY) {
        return writ_str_format(session, "__ffi_take_array_%.*s(%.*s, %.*s_length)",
                               S(array_element_name(w->g, item->type->target)), S(name), S(name));
    }
    return name;
}

static void
emit_values(const wrapper_t *w, writ_emit_t *emit, const core_list_t(shim_param_t) *params, const uint32_t *outs,
            size_t count, bool with_outcome)
{
    writ_emit_text(emit, "let values: %.*s = (%s", S(result_type_name(w)), with_outcome ? "outcome = which" : "");
    for (size_t r = 0; r < count; r++) {
        for (size_t i = 0; i < params->len; i++) {
            const shim_param_t *p = &params->data[i];
            if (p->kind == PARAM_OUT && p->item == outs[r] && writ_str_eq(p->name, w->plan->items[outs[r]].name)) {
                writ_emit_text(emit, "%s%.*s = %.*s", r || with_outcome ? ", " : "", S(local_name(w, outs[r])),
                               S(lifted(w, p)));
            }
        }
    }
    writ_emit_line(emit, ")");
    writ_emit_line(emit, "return values");
}

static void
emit_text_check(const wrapper_t *w, writ_emit_t *emit, writ_str_t name, bool bytes)
{
    writ_emit_line(emit, "var %.*s_text: string = \"\"", S(name));
    if (bytes) {
        writ_emit_open(emit, "if !__ffi_take_text(ref %.*s, ref %.*s_text)", S(name), S(name));
    } else {
        writ_emit_open(emit, "if !__ffi_copy_cstr(%.*s, ref %.*s_text)", S(name), S(name));
    }
    if (can_fail(w)) {
        emit_fail(w, emit, writ_str_format(w->g->session, "%d", WRIT_C_CONTRACT_VIOLATION),
                  writ_cstr("\"invalid UTF-8 text\""));
    } else {
        writ_emit_line(emit, "panic(\"%.*s returned invalid UTF-8 text\")", S(w->plan->fn->contract_name));
    }
    writ_emit_close(emit, nullptr);
}

static void
emit_fixup(const wrapper_t *w, writ_emit_t *emit, writ_str_t name)
{
    writ_str_t problem = writ_str_format(w->g->session, "__fixup_%.*s", S(name));
    writ_emit_line(emit, "let %.*s: string? = __ffi_fixup(%.*s.raw)", S(problem), S(name));
    writ_emit_open(emit, "if %.*s != nil", S(problem));
    if (declares_failure(w)) {
        emit_fail(w, emit, writ_str_format(w->g->session, "%d", WRIT_C_ARGUMENT_ERROR), problem);
    } else {
        writ_emit_line(emit, "panic(%.*s.get())", S(problem));
    }
    writ_emit_close(emit, nullptr);
}

static void
emit_closed_checks(const wrapper_t *w, writ_emit_t *emit, const core_list_t(shim_param_t) *params)
{
    for (size_t i = 0; i < params->len; i++) {
        const shim_param_t *p = &params->data[i];
        const writ_plan_item_view_t *item = &w->plan->items[p->item];
        bool guarded = p->item == w->receiver && guards_release(w);
        if (p->kind != PARAM_ADDRESS || !is_handle(item)) {
            continue;
        }
        writ_str_t name = local_name(w, p->item);
        if (!item->type || !item->type->nullable || guarded) {
            writ_emit_open(emit, "if %.*s.raw == %s", S(name), raw_empty(w->g, item->type));
            if (declares_failure(w)) {
                emit_fail(w, emit, writ_str_format(w->g->session, "%d", WRIT_C_ARGUMENT_ERROR),
                          writ_cstr("\"handle is closed\""));
            } else {
                writ_emit_line(emit, "panic(\"%.*s: handle is closed\")", S(w->plan->fn->contract_name));
            }
            writ_emit_close(emit, nullptr);
        }
        if (guarded) {
            writ_emit_open(emit, "if __abi_baked_address(%.*s)", S(raw_of(w->g, item->type, name)));
            emit_fail(w, emit, writ_str_format(w->g->session, "%d", WRIT_C_ARGUMENT_ERROR),
                      writ_cstr("\"a baked object cannot be released\""));
            writ_emit_close(emit, nullptr);
        } else if (typed_handle(w->g, item->type)) {
            emit_fixup(w, emit, name);
        }
    }
}

static void
emit_text_checks(const wrapper_t *w, writ_emit_t *emit, const core_list_t(shim_param_t) *params)
{
    for (size_t i = 0; i < params->len; i++) {
        const shim_param_t *p = &params->data[i];
        bool bytes = text_result(w->g, w->fn, w->plan, p);
        if ((!bytes && !cstr_result(w->g, w->fn, w->plan, p))
            || !writ_str_eq(p->name, w->plan->items[p->item].name)) {
            continue;
        }
        emit_text_check(w, emit, local_name(w, p->item), bytes);
    }
}

static void
emit_zero_record(const n00b_t *g, writ_emit_t *emit, writ_str_t local, const writ_type_view_t *type)
{
    const writ_record_view_t *record = writ_model_record_named(g->model, type->name);
    writ_emit_text(emit, "var %.*s: %.*s = (", S(local), S(scalar_type(g, type)));
    for (size_t f = 0; record && f < record->fields_count; f++) {
        const writ_type_view_t *field = record->fields[f].type;
        writ_str_t zero = field && field->kind == WRIT_TYPE_BOOL ? writ_cstr("false") : writ_cstr("0");
        writ_emit_text(emit, "%s%.*s = %.*s", f ? ", " : "",
                       S(writ_profile_name(g->profile, WRIT_NAMING_FIELD, record->fields[f].name)), S(zero));
    }
    writ_emit_line(emit, ")");
}

static const writ_record_view_t *
keyword_record(const n00b_t *g, const writ_plan_item_view_t *item)
{
    return item->shape == WRIT_SHAPE_KEYWORD_PACK && item->type && item->type->target
             ? writ_model_record_named(g->model, item->type->target->name)
             : nullptr;
}

static bool
keywords_as_record(const wrapper_t *w, uint32_t item)
{
    return writ_str_eq(writ_binding_slot_repr(w->g->b, w->fn, w->plan->items[item].name), writ_cstr("record"));
}

static writ_str_t
options_type_name(const wrapper_t *w)
{
    return writ_str_format(w->g->session, "%.*s_Options",
                           S(writ_name_case(w->g->session, wrapper_name(w), WRIT_CASE_TITLE_SNAKE)));
}

static writ_str_t
keyword_name(const n00b_t *g, const writ_slot_view_t *field)
{
    return writ_profile_name(g->profile, WRIT_NAMING_FIELD, field->name);
}

static writ_str_t
keyword_literal(const n00b_t *g, const writ_slot_view_t *field, int64_t value)
{
    if (field->type && field->type->kind == WRIT_TYPE_BOOL) {
        return writ_cstr(value ? "true" : "false");
    }
    return writ_str_format(g->session, "%lld", (long long)value);
}

static void
emit_options_type(const wrapper_t *w, writ_emit_t *emit, const writ_record_view_t *record)
{
    writ_emit_line(emit, "");
    writ_emit_open(emit, "record %.*s", S(options_type_name(w)));
    for (size_t f = 0; f < record->fields_count; f++) {
        if (f != record->presence_field) {
            writ_emit_line(emit, "public %.*s: %.*s?", S(keyword_name(w->g, &record->fields[f])),
                           S(scalar_type(w->g, record->fields[f].type)));
        }
    }
    writ_emit_close(emit, nullptr);
}

static void
emit_keyword_params(const wrapper_t *w, writ_emit_t *emit, const writ_record_view_t *record, bool *first)
{
    bool optional = record->presence == WRIT_PRESENCE_MASK || record->presence == WRIT_PRESENCE_SENTINEL;
    for (int defaulted = 0; defaulted < 2; defaulted++) {
        for (size_t f = 0; f < record->fields_count; f++) {
            const writ_slot_view_t *field = &record->fields[f];
            bool has_value = optional || field->has_default;
            if (f == record->presence_field || has_value != (defaulted == 1)) {
                continue;
            }
            writ_emit_text(emit, "%s%.*s: %.*s%s", *first ? "" : ", ", S(keyword_name(w->g, field)),
                           S(scalar_type(w->g, field->type)), optional ? "?" : "");
            if (optional) {
                writ_emit_text(emit, " = nil");
            } else if (field->has_default) {
                writ_emit_text(emit, " = %.*s", S(keyword_literal(w->g, field, field->default_value)));
            }
            *first = false;
        }
    }
}

static bool
retry_capacity(const writ_plan_view_t *plan, uint32_t item)
{
    for (size_t s = 0; s < plan->steps_count; s++) {
        const writ_plan_step_view_t *step = &plan->steps[s];
        if (step->kind == WRIT_STEP_SIZE_RETRY && step->local != WRIT_PLAN_NONE
            && plan->locals[step->local].capacity_item == item) {
            return true;
        }
    }
    return false;
}

static bool
exposed_input(const wrapper_t *w, uint32_t item)
{
    const writ_plan_view_t *plan = w->plan;
    return writ_c_is_input(&plan->items[item]) && !policy_item(plan, item) && !retry_capacity(plan, item)
        && !provided_callback(w->g, w->fn, plan, item).length;
}

static void
emit_wrapper_params(const wrapper_t *w, writ_emit_t *emit, uint32_t keywords, bool options_record, bool *first)
{
    const writ_plan_view_t *plan = w->plan;
    for (uint32_t i = 0; i < plan->items_count; i++) {
        if (!exposed_input(w, i) || (i == keywords && !options_record)) {
            continue;
        }
        bool modify = (i == w->receiver && consumes(w, i))
                   || (plan->items[i].role == WRIT_ROLE_INPUT_OUTPUT && record_target(&plan->items[i]));
        writ_str_t type = i == keywords ? options_type_name(w) : receiver_type(w, i);
        writ_emit_text(emit, "%s%s%.*s: %.*s", *first ? "" : ", ", modify ? "modify " : "", S(local_name(w, i)),
                       S(type));
        *first = false;
    }
    if (keywords != WRIT_PLAN_NONE && !options_record) {
        emit_keyword_params(w, emit, keyword_record(w->g, &plan->items[keywords]), first);
    }
}

static void
emit_wrapper_arguments(const wrapper_t *w, writ_emit_t *emit, uint32_t keywords, bool options_record)
{
    const writ_plan_view_t *plan = w->plan;
    bool first = true;
    for (uint32_t i = 0; i < plan->items_count; i++) {
        if (exposed_input(w, i) && (i != keywords || options_record)) {
            writ_emit_text(emit, "%s%.*s", first ? "" : ", ", S(local_name(w, i)));
            first = false;
        }
    }
    const writ_record_view_t *record = keywords != WRIT_PLAN_NONE && !options_record
                                         ? keyword_record(w->g, &plan->items[keywords])
                                         : nullptr;
    for (size_t f = 0; record && f < record->fields_count; f++) {
        if (f != record->presence_field) {
            writ_str_t key = keyword_name(w->g, &record->fields[f]);
            writ_emit_text(emit, "%s%.*s = %.*s", first ? "" : ", ", S(key), S(key));
            first = false;
        }
    }
}

static void
emit_static_init(const wrapper_t *w, writ_emit_t *emit, uint32_t keywords, bool options_record)
{
    writ_emit_line(emit, "");
    writ_emit_text(emit, "func __init(modify self: %.*s", S(w->bound.owner));
    bool first = false;
    emit_wrapper_params(w, emit, keywords, options_record, &first);
    writ_emit_text(emit, ")");
    writ_emit_open(emit, "");
    if (can_fail(w)) {
        writ_emit_open(emit, "try");
    }
    writ_emit_text(emit, "self = %.*s(", S(wrapper_name(w)));
    emit_wrapper_arguments(w, emit, keywords, options_record);
    writ_emit_line(emit, ")%s", can_fail(w) ? "!" : "");
    if (can_fail(w)) {
        writ_emit_close(emit, " else failure {");
        writ_emit_indent(emit, 1);
        writ_emit_line(emit, "panic(\"%.*s failed\")", S(wrapper_name(w)));
        writ_emit_close(emit, nullptr);
    }
    writ_emit_close(emit, nullptr);
}

static void
emit_keyword_record(const wrapper_t *w, writ_emit_t *emit, uint32_t item, const writ_record_view_t *record)
{
    const n00b_t *g = w->g;
    bool as_record = keywords_as_record(w, item);
    bool optional = as_record || record->presence == WRIT_PRESENCE_MASK || record->presence == WRIT_PRESENCE_SENTINEL;
    writ_str_t local = writ_str_format(g->session, "%.*s_record", S(local_name(w, item)));
    emit_zero_record(g, emit, local, w->plan->items[item].type->target);
    writ_str_t presence = record->presence_field != WRIT_PLAN_NONE
                            ? keyword_name(g, &record->fields[record->presence_field])
                            : writ_cstr("");
    uint32_t bit = 0;
    for (size_t f = 0; f < record->fields_count; f++) {
        const writ_slot_view_t *field = &record->fields[f];
        if (f == record->presence_field) {
            continue;
        }
        writ_str_t key = keyword_name(g, field);
        writ_str_t source = key;
        if (as_record) {
            source = writ_str_format(g->session, "%.*s_%.*s", S(local_name(w, item)), S(key));
            writ_emit_line(emit, "let %.*s = %.*s.%.*s", S(source), S(local_name(w, item)), S(key));
        }
        bool has_fallback = record->presence == WRIT_PRESENCE_SENTINEL ? field->has_none : field->has_default;
        int64_t fallback = record->presence == WRIT_PRESENCE_SENTINEL ? field->none_value : field->default_value;
        if (!optional) {
            writ_emit_line(emit, "%.*s.%.*s = %.*s", S(local), S(key), S(source));
        } else {
            writ_emit_open(emit, "if %.*s != nil", S(source));
            writ_emit_line(emit, "%.*s.%.*s = %.*s.get()", S(local), S(key), S(source));
            if (record->presence == WRIT_PRESENCE_MASK) {
                writ_emit_line(emit, "%.*s.%.*s = %.*s.%.*s | %llu", S(local), S(presence), S(local), S(presence),
                               (unsigned long long)1 << bit);
            }
            if (has_fallback) {
                writ_emit_close(emit, " else {");
                writ_emit_indent(emit, 1);
                writ_emit_line(emit, "%.*s.%.*s = %.*s", S(local), S(key), S(keyword_literal(g, field, fallback)));
            }
            writ_emit_close(emit, nullptr);
        }
        bit++;
    }
    if (record->presence == WRIT_PRESENCE_SIZE) {
        writ_emit_line(emit, "%.*s.%.*s = %u", S(local), S(presence), w->plan->items[item].type->target->size);
    }
}

typedef struct {
    writ_expr_env_t base;
    const wrapper_t *w;
    const core_list_t(shim_param_t) *params;
} key_env_t;

static bool
item_is_local(const wrapper_t *w, const core_list_t(shim_param_t) *params, uint32_t index)
{
    if (index == w->receiver) {
        return true;
    }
    for (size_t i = 0; i < params->len; i++) {
        if (params->data[i].item == index && params->data[i].kind != PARAM_OWNED
            && params->data[i].kind != PARAM_PROVIDED) {
            return true;
        }
    }
    return false;
}

static writ_str_t
result_local(const wrapper_t *w)
{
    for (uint32_t i = 0; can_fail(w) && i < w->plan->items_count; i++) {
        if (w->plan->items[i].role == WRIT_ROLE_RESULT) {
            return local_name(w, i);
        }
    }
    return writ_cstr("result");
}

static writ_str_t
item_value(const wrapper_t *w, const core_list_t(shim_param_t) *params, uint32_t index)
{
    if (index == WRIT_PLAN_NONE || !item_is_local(w, params, index)) {
        return writ_cstr("0");
    }
    const writ_plan_item_view_t *item = &w->plan->items[index];
    writ_str_t name = local_name(w, index);
    if (item->shape == WRIT_SHAPE_CALLBACK) {
        return writ_str_format(w->g->session, "%.*s_handle", S(name));
    }
    if (is_handle(item)) {
        return raw_of(w->g, item->type, name);
    }
    if (item->type && (item->type->kind == WRIT_TYPE_INT || item->type->kind == WRIT_TYPE_ENUM)) {
        return name;
    }
    return writ_cstr("0");
}

static uint32_t
item_for_contract_slot(const writ_plan_view_t *plan, uint32_t slot)
{
    for (uint32_t i = 0; i < plan->items_count; i++) {
        if (plan->items[i].contract_slot == slot && plan->items[i].role != WRIT_ROLE_RESULT) {
            return i;
        }
    }
    return WRIT_PLAN_NONE;
}

static uint32_t
item_for_slot_name(const writ_plan_view_t *plan, writ_str_t name)
{
    for (uint32_t s = 0; s < plan->fn->slots_count; s++) {
        if (writ_str_eq(plan->fn->slots[s].name, name)) {
            return item_for_contract_slot(plan, s);
        }
    }
    return WRIT_PLAN_NONE;
}

static void
key_ref(writ_expr_env_t *base, writ_ref_t ref, uint32_t expr, core_buffer_t *out)
{
    (void)expr;
    key_env_t *env = (key_env_t *)base;
    writ_str_t text = writ_cstr("0");
    if (ref.kind == WRIT_REF_RESULT) {
        text = result_local(env->w);
    } else if (ref.kind == WRIT_REF_SLOT) {
        uint32_t slot = writ_plan_contract_slot(env->w->g->model, env->w->plan, ref);
        text = item_value(env->w, env->params,
                          slot < env->w->plan->slots_count ? env->w->plan->slots[slot].item : WRIT_PLAN_NONE);
    }
    core_buffer_printf(out, "%.*s", S(text));
}

static writ_str_t
event_key(const wrapper_t *w, const core_list_t(shim_param_t) *params, const writ_event_arg_view_t *arg)
{
    writ_str_t value = writ_cstr("0");
    if (arg->kind == WRIT_EVENT_ARG_RESULT) {
        value = result_local(w);
    } else if (arg->kind == WRIT_EVENT_ARG_SLOT) {
        value = item_value(w, params, item_for_slot_name(w->plan, arg->name));
    } else {
        key_env_t env = {{.print_ref = key_ref}, w, params};
        core_buffer_t *buffer = core_buffer_empty();
        writ_expr_print(w->g->model, arg->expr, WRIT_LANGUAGE_N00B, &env.base, buffer);
        value = writ_str_from_buffer(w->g->session, buffer);
    }
    return writ_str_format(w->g->session, "cast[u64](%.*s)", S(value));
}

static const writ_slot_view_t *
event_subject(const writ_fn_view_t *fn, const writ_event_view_t *event)
{
    if (!event->args_count) {
        return nullptr;
    }
    if (event->args[0].kind == WRIT_EVENT_ARG_RESULT) {
        return fn->result;
    }
    for (size_t s = 0; s < fn->slots_count; s++) {
        if (writ_str_eq(fn->slots[s].name, event->args[0].name)) {
            return &fn->slots[s];
        }
    }
    return nullptr;
}

static writ_str_t
event_id(const n00b_t *g, const writ_fn_view_t *fn, const writ_event_view_t *event)
{
    const writ_slot_view_t *subject = event_subject(fn, event);
    writ_str_t type = subject && subject->type ? subject->type->name : writ_cstr("");
    return event->kind == WRIT_EVENT_RELEASE ? writ_str_format(g->session, "%.*s:release:%.*s", S(g->shim), S(type))
         : event->kind == WRIT_EVENT_ANY     ? writ_str_format(g->session, "%.*s:any:%.*s", S(g->shim), S(type))
                                             : writ_str_format(g->session, "%.*s:%.*s", S(g->shim), S(event->function));
}

static bool
contract_has_event(const n00b_t *g, writ_str_t id)
{
    for (size_t f = 0; f < writ_model_fn_count(g->model); f++) {
        const writ_fn_view_t *fn = writ_model_fn_at(g->model, f);
        for (size_t s = 0; s < fn->slots_count; s++) {
            const writ_slot_view_t *slot = &fn->slots[s];
            for (size_t e = 0; e < slot->span_events_count; e++) {
                if (writ_str_eq(event_id(g, fn, &slot->span_events[e]), id)) {
                    return true;
                }
            }
            for (size_t a = 0; a < slot->policy_arms_count; a++) {
                const writ_policy_arm_view_t *arm = &slot->policy_arms[a];
                for (size_t e = 0; e < arm->span_events_count; e++) {
                    if (writ_str_eq(event_id(g, fn, &arm->span_events[e]), id)) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static void
emit_event(writ_emit_t *emit, writ_str_t id, writ_str_t keys)
{
    writ_emit_line(emit, "__ffi_event(\"%.*s\", [%.*s]'array, __os_thread_id())", S(id), S(keys));
}

static void
emit_event_sites(const wrapper_t *w, writ_emit_t *emit, const core_list_t(shim_param_t) *params)
{
    const n00b_t *g = w->g;
    const writ_fn_view_t *fn = w->plan->fn;
    writ_str_t call = writ_str_format(g->session, "%.*s:%.*s", S(g->shim), S(fn->contract_name));
    if (contract_has_event(g, call)) {
        core_buffer_t *keys = core_buffer_empty();
        for (uint32_t s = 0; s < fn->slots_count; s++) {
            writ_str_t value = item_value(w, params, item_for_contract_slot(w->plan, s));
            core_buffer_printf(keys, "%scast[u64](%.*s)", s ? ", " : "", S(value));
        }
        emit_event(emit, call, writ_str_from_buffer(g->session, keys));
    }
    for (uint32_t s = 0; s < fn->slots_count; s++) {
        const writ_type_view_t *type = fn->slots[s].type;
        if (!type || type->kind != WRIT_TYPE_HANDLE) {
            continue;
        }
        writ_str_t value = item_value(w, params, item_for_contract_slot(w->plan, s));
        writ_str_t key = writ_str_format(g->session, "cast[u64](%.*s)", S(value));
        writ_str_t any = writ_str_format(g->session, "%.*s:any:%.*s", S(g->shim), S(type->name));
        if (contract_has_event(g, any)) {
            emit_event(emit, any, key);
        }
        writ_str_t release = writ_str_format(g->session, "%.*s:release:%.*s", S(g->shim), S(type->name));
        if (fn->slots[s].mode == WRIT_MODE_CONSUME && contract_has_event(g, release)) {
            emit_event(emit, release, key);
        }
    }
}

static const writ_slot_view_t *
callback_slot(const writ_plan_view_t *plan, uint32_t item)
{
    uint32_t slot = plan->items[item].contract_slot;
    return slot < plan->fn->slots_count ? &plan->fn->slots[slot] : nullptr;
}

static void
emit_callback_registrations(const wrapper_t *w, writ_emit_t *emit, const core_list_t(shim_param_t) *params)
{
    for (size_t i = 0; i < params->len; i++) {
        const shim_param_t *p = &params->data[i];
        if (keeps_until(w->plan, p)) {
            writ_emit_line(emit, "var %.*s_kept: uint = 0", S(local_name(w, p->item)));
        }
        if (p->kind != PARAM_CALLBACK) {
            continue;
        }
        const writ_slot_view_t *slot = callback_slot(w->plan, p->item);
        writ_str_t name = local_name(w, p->item);
        writ_emit_line(emit, "let %.*s_handle = %.*s(%.*s, %s)", S(name),
                       S(callback_table(w->g, writ_binding_type(w->g->b, w->plan->items[p->item].type->name),
                                        "register")),
                       S(name), slot && slot->span == WRIT_SPAN_ONCE ? "true" : "false");
    }
}

static void
emit_callback_failure_frees(const wrapper_t *w, writ_emit_t *emit, const core_list_t(shim_param_t) *params)
{
    for (size_t i = 0; i < params->len; i++) {
        if (params->data[i].kind == PARAM_CALLBACK) {
            writ_emit_line(emit, "%.*s(%.*s_handle)",
                           S(callback_table(w->g, writ_binding_type(w->g->b, w->plan->items[params->data[i].item].type->name),
                                            "free")),
                           S(local_name(w, params->data[i].item)));
        }
        if (keeps_until(w->plan, &params->data[i])) {
            writ_emit_line(emit, "__ffi_kept_free(cast[u64](%.*s_kept))", S(local_name(w, params->data[i].item)));
        }
    }
}

static void
emit_holds(const wrapper_t *w, writ_emit_t *emit, const core_list_t(shim_param_t) *params, writ_str_t name,
           writ_str_t token, writ_str_t release, const writ_event_view_t *events, size_t count)
{
    if (!count) {
        return;
    }
    writ_emit_line(emit, "let %.*s_group = __ffi_hold_group()", S(name));
    for (size_t e = 0; e < count; e++) {
        const writ_event_view_t *event = &events[e];
        core_buffer_t *keys = core_buffer_empty();
        for (size_t a = 0; a < event->args_count; a++) {
            writ_str_t key = event_key(w, params, &event->args[a]);
            core_buffer_printf(keys, "%s%.*s", a ? ", " : "", S(key));
        }
        writ_str_t list = writ_str_from_buffer(w->g->session, keys);
        writ_emit_line(emit, "__ffi_hold(%.*s_group, cast[u64](%.*s), ref %.*s, \"%.*s\", [%.*s]'array, %s)", S(name),
                       S(token), S(release), S(event_id(w->g, w->plan->fn, event)), S(list),
                       event->on_thread ? "__os_thread_id()" : "0");
    }
}

static const writ_event_view_t *
item_events(const writ_plan_view_t *plan, uint32_t item, size_t *count)
{
    const writ_policy_arm_view_t *arm = plan_arm(plan);
    if (arm && arm->span == WRIT_SPAN_UNTIL && writ_str_eq(arm->target, plan->items[item].name)) {
        *count = arm->span_events_count;
        return arm->span_events;
    }
    uint32_t slot = plan->items[item].contract_slot;
    const writ_slot_view_t *view = slot < plan->fn->slots_count ? &plan->fn->slots[slot] : nullptr;
    *count = view && view->span == WRIT_SPAN_UNTIL ? view->span_events_count : 0;
    return view ? view->span_events : nullptr;
}

static void
emit_callback_spans(const wrapper_t *w, writ_emit_t *emit, const core_list_t(shim_param_t) *params)
{
    for (size_t i = 0; i < params->len; i++) {
        const shim_param_t *p = &params->data[i];
        writ_str_t name = local_name(w, p->item);
        size_t count = 0;
        const writ_event_view_t *events = item_events(w->plan, p->item, &count);
        if (keeps_until(w->plan, p)) {
            emit_holds(w, emit, params, writ_str_format(w->g->session, "%.*s_kept", S(name)),
                       writ_str_format(w->g->session, "%.*s_kept", S(name)), writ_cstr("__ffi_kept_free"), events, count);
            continue;
        }
        const writ_slot_view_t *slot = p->kind == PARAM_CALLBACK ? callback_slot(w->plan, p->item) : nullptr;
        if (!slot) {
            continue;
        }
        writ_str_t release = callback_table(w->g, writ_binding_type(w->g->b, w->plan->items[p->item].type->name),
                                            "free");
        if (slot->span == WRIT_SPAN_CALL || slot->span == WRIT_SPAN_NONE) {
            writ_emit_line(emit, "%.*s(%.*s_handle)", S(release), S(name));
        }
        emit_holds(w, emit, params, name, writ_str_format(w->g->session, "%.*s_handle", S(name)), release,
                   events, count);
    }
}

static void
emit_wrapper(const n00b_t *g, writ_emit_t *emit, size_t fn, const writ_plan_view_t *plan)
{
    wrapper_t w = {.g = g, .fn = fn, .plan = plan, .bound = bound_fn(g, fn), .receiver = WRIT_PLAN_NONE};
    w.receiver = find_receiver(&w);
    if (plan_arm(plan) && w.bound.policy_arm == WRIT_PLAN_NONE) {
        w.suffix = writ_name_case(g->session, arm_member(g, plan_arm(plan)), WRIT_CASE_SNAKE);
    }
    uint32_t *outs = writ_alloc(g->session, sizeof(uint32_t) * (plan->items_count + 1));
    size_t out_count = results(&w, outs);
    emit_result_types(&w, emit, outs, out_count);
    uint32_t keywords = WRIT_PLAN_NONE;
    for (uint32_t i = 0; i < plan->items_count; i++) {
        if (keyword_record(g, &plan->items[i])) {
            keywords = i;
        }
    }
    const writ_record_view_t *keyword_view = keywords != WRIT_PLAN_NONE ? keyword_record(g, &plan->items[keywords])
                                                                        : nullptr;
    bool options_record = keyword_view && keywords_as_record(&w, keywords);
    if (options_record) {
        emit_options_type(&w, emit, keyword_view);
    }
    writ_emit_line(emit, "");
    mark(g, emit, plan->fn->declared_at, plan->fn->contract_name);
    writ_emit_text(emit, "func %.*s(", S(wrapper_name(&w)));
    bool first = true;
    emit_wrapper_params(&w, emit, keywords, options_record, &first);
    writ_emit_text(emit, ")");
    bool multiple_success = outcome_enum(plan);
    if (multiple_success && out_count) {
        writ_emit_text(emit, " -> %.*s", S(result_type_name(&w)));
    } else if (multiple_success) {
        writ_emit_text(emit, " -> %.*s", S(outcome_type_name(&w)));
    } else if (out_count == 1) {
        writ_emit_text(emit, " -> %.*s", S(item_type(g, fn, plan, outs[0])));
    } else if (out_count > 1) {
        writ_emit_text(emit, " -> %.*s", S(result_type_name(&w)));
    }
    if (declares_failure(&w)) {
        writ_emit_text(emit, " fail: %.*s", S(error_type(&w)));
    }
    writ_emit_open(emit, "");
    core_list_t(shim_param_t) params = core_list_new(shim_param_t);
    shim_params(g, fn, plan, &params);
    if (keyword_view) {
        emit_keyword_record(&w, emit, keywords, keyword_view);
    }
    for (size_t i = 0; i < params.len; i++) {
        const shim_param_t *p = &params.data[i];
        if (p->kind == PARAM_VALUE && p->text) {
            writ_emit_line(emit, "var %.*s_value: Array[u8] = __text_byte_array(%.*s)", S(local_name(&w, p->item)),
                           S(local_name(&w, p->item)));
        } else if (p->kind == PARAM_VALUE && p->item != w.receiver) {
            writ_emit_line(emit, "var %.*s_value = %.*s", S(local_name(&w, p->item)), S(local_name(&w, p->item)));
        } else if (p->kind == PARAM_OUT && p->storage) {
            writ_emit_line(emit, "var %.*s = %.*s()", S(local_name(&w, p->item)),
                           S(writ_profile_name(g->profile, WRIT_NAMING_FN, plan->items[p->item].type->name)));
        } else if (p->kind == PARAM_OUT && plan->items[p->item].type
                   && plan->items[p->item].type->kind == WRIT_TYPE_RECORD) {
            emit_zero_record(g, emit, local_name(&w, p->item), plan->items[p->item].type);
        } else if (p->kind == PARAM_OUT) {
            writ_str_t local = p->name.length && !writ_str_eq(p->name, plan->items[p->item].name)
                                 ? writ_str_format(g->session, "%.*s_length", S(local_name(&w, p->item)))
                                 : local_name(&w, p->item);
            writ_str_t type = writ_str_eq(p->n00b_type, writ_cstr("Ref[Ffi_Bytes]")) ? writ_cstr("Ffi_Bytes")
                              : writ_str_eq(p->n00b_type, writ_cstr("Ref[uint]"))
                                  ? writ_cstr("uint")
                                  : writ_str_format(g->session, "%.*s", (int)(p->n00b_type.length - 5),
                                                    p->n00b_type.data + 4);
            const writ_type_view_t *item_type_view = plan->items[p->item].type;
            writ_str_t zero = writ_str_eq(type, writ_cstr("Ffi_Bytes")) ? writ_cstr("(data = 0, length = 0)")
                              : writ_str_eq(type, writ_cstr("bool"))    ? writ_cstr("false")
                              : item_type_view && item_type_view->kind == WRIT_TYPE_ENUM
                                  ? writ_str_format(g->session, "cast[%.*s]0", S(type))
                                  : writ_cstr("0");
            writ_emit_line(emit, "var %.*s: %.*s = %.*s", S(local), S(type), S(zero));
        }
    }
    emit_closed_checks(&w, emit, &params);
    emit_callback_registrations(&w, emit, &params);
    writ_emit_text(emit, "%s__%.*s(", can_fail(&w) ? "let outcome = " : out_count ? "let result = " : "",
                   S(writ_c_plan_name(&g->c, plan)));
    bool passed = false;
    for (size_t i = 0; i < params.len; i++) {
        const shim_param_t *p = &params.data[i];
        writ_str_t name = local_name(&w, p->item);
        if (p->kind == PARAM_OWNED || p->kind == PARAM_PROVIDED) {
            continue;
        }
        writ_emit_text(emit, "%s", passed ? ", " : "");
        passed = true;
        if (p->kind == PARAM_VALUE) {
            writ_emit_text(emit, p->item == w.receiver && !p->text ? "ref %.*s" : "ref %.*s_value", S(name));
        } else if (p->kind == PARAM_ADDRESS && (is_handle(&plan->items[p->item]) || p->item == w.receiver)) {
            writ_emit_text(emit, "%.*s", S(raw_of(g, plan->items[p->item].type, name)));
        } else if (p->kind == PARAM_RECORD) {
            writ_emit_text(emit, p->item == keywords ? "ref %.*s_record" : "ref %.*s", S(name));
        } else if (p->kind == PARAM_STORAGE) {
            writ_emit_text(emit, "ref %.*s.storage", S(name));
        } else if (p->kind == PARAM_OUT && p->storage) {
            writ_emit_text(emit, "ref %.*s.storage", S(name));
        } else if (p->kind == PARAM_OUT) {
            bool length = !writ_str_eq(p->name, plan->items[p->item].name);
            writ_emit_text(emit, length ? "ref %.*s_length" : "ref %.*s", S(name));
        } else if (p->kind == PARAM_CALLBACK) {
            writ_emit_text(emit, "%.*s_handle", S(name));
        } else if (retry_capacity(plan, p->item)) {
            writ_emit_text(emit, "%u", writ_default(WRIT_DEFAULT_RETRY_CAPACITY));
        } else {
            writ_emit_text(emit, "%.*s", S(name));
        }
    }
    for (size_t i = 0; i < params.len; i++) {
        if (keeps_until(plan, &params.data[i])) {
            writ_emit_text(emit, "%sref %.*s_kept", passed ? ", " : "", S(local_name(&w, params.data[i].item)));
            passed = true;
        }
    }
    writ_emit_line(emit, ")");
    if (can_fail(&w)) {
        writ_emit_open(emit, "if outcome < 0");
        emit_callback_failure_frees(&w, emit, &params);
        emit_fail(&w, emit, writ_str_format(g->session, "cast[%.*s]outcome", S(code_type(g))), writ_cstr("nil"));
        writ_emit_close(emit, nullptr);
        for (size_t o = 0; o < plan->outcomes_count; o++) {
            if (plan->outcomes[o].kind == WRIT_OUTCOME_FAILURE) {
                writ_emit_open(emit, "if outcome == %zu", o);
                emit_callback_failure_frees(&w, emit, &params);
                emit_fail(&w, emit, detail_text(&w, (uint32_t)o, false), detail_text(&w, (uint32_t)o, true));
                writ_emit_close(emit, nullptr);
            }
        }
    }
    emit_event_sites(&w, emit, &params);
    emit_callback_spans(&w, emit, &params);
    if (w.receiver != WRIT_PLAN_NONE && consumes(&w, w.receiver)) {
        writ_emit_line(emit, "self.raw = %s", raw_empty(g, plan->items[w.receiver].type));
    }
    emit_text_checks(&w, emit, &params);
    if (multiple_success) {
        writ_str_t outcome_type = outcome_type_name(&w);
        size_t last = plan->outcomes_count;
        for (size_t o = 0; o < plan->outcomes_count; o++) {
            if (plan->outcomes[o].kind == WRIT_OUTCOME_SUCCESS) {
                last = o;
            }
        }
        writ_emit_line(emit, "var which: %.*s = %.*s.%.*s", S(outcome_type), S(outcome_type),
                       S(plan->outcomes[last].name));
        for (size_t o = 0; o < last; o++) {
            if (plan->outcomes[o].kind == WRIT_OUTCOME_SUCCESS) {
                writ_emit_open(emit, "if outcome == %zu", o);
                writ_emit_line(emit, "which = %.*s.%.*s", S(outcome_type), S(plan->outcomes[o].name));
                writ_emit_close(emit, nullptr);
            }
        }
        if (!out_count) {
            writ_emit_line(emit, "return which");
        } else {
            emit_values(&w, emit, &params, outs, out_count, true);
        }
    } else if (!can_fail(&w) && out_count == 1 && plan->items[outs[0]].shape == WRIT_SHAPE_TEXT) {
        emit_text_check(&w, emit, writ_cstr("result"), false);
        writ_emit_line(emit, "return result_text");
    } else if (!can_fail(&w) && out_count == 1 && is_handle(&plan->items[outs[0]])) {
        writ_emit_line(emit, "return %.*s", S(construct_handle(&w, outs[0], writ_cstr("result"))));
    } else if (!can_fail(&w) && out_count) {
        writ_emit_line(emit, "return result");
    } else if (out_count == 1) {
        for (uint32_t o = 0; plan->items[outs[0]].optional && o < plan->outcomes_count; o++) {
            if (plan->outcomes[o].kind == WRIT_OUTCOME_SUCCESS && !lifts_in(plan, o, outs[0])) {
                writ_emit_open(emit, "if outcome == %u", o);
                writ_emit_line(emit, "return nil");
                writ_emit_close(emit, nullptr);
            }
        }
        for (size_t i = 0; i < params.len; i++) {
            if (params.data[i].kind == PARAM_OUT && params.data[i].item == outs[0]
                && writ_str_eq(params.data[i].name, plan->items[outs[0]].name)) {
                writ_emit_line(emit, "return %.*s", S(lifted(&w, &params.data[i])));
            }
        }
    } else if (out_count > 1) {
        emit_values(&w, emit, &params, outs, out_count, false);
    }
    writ_emit_close(emit, nullptr);
    core_list_free(params);
    if (w.bound.is_static && w.bound.owner.length && out_count == 1 && !multiple_success
        && writ_str_eq(item_type(g, fn, plan, outs[0]), w.bound.owner)) {
        emit_static_init(&w, emit, keywords, options_record);
    }
}

static writ_str_t
raw_type(const n00b_t *g, const writ_type_view_t *type)
{
    if (!type || type->kind == WRIT_TYPE_VOID) {
        return writ_cstr("nil");
    }
    switch (type->kind) {
    case WRIT_TYPE_BOOL:
    case WRIT_TYPE_INT:
    case WRIT_TYPE_FLOAT:
    case WRIT_TYPE_ENUM:
        return scalar_type(g, type);
    default:
        return writ_cstr("uint");
    }
}

static void
emit_raw(const n00b_t *g, writ_emit_t *emit, const writ_fn_view_t *fn, writ_bound_fn_t bound)
{
    writ_str_t *types = writ_alloc(g->session, sizeof(writ_str_t) * (fn->slots_count ? fn->slots_count : 1));
    for (size_t s = 0; s < fn->slots_count; s++) {
        types[s] = raw_type(g, fn->slots[s].type);
    }
    writ_str_t result = raw_type(g, fn->result ? fn->result->type : nullptr);
    add_extern_with(g, writ_str_format(g->session, "__%.*s_raw", S(bound.name)), fn->link_symbol, types,
                    fn->slots_count, result, fn->is_pure);
    writ_emit_line(emit, "");
    mark(g, emit, fn->declared_at, fn->contract_name);
    writ_emit_text(emit, "public func %.*s_raw(", S(bound.name));
    for (size_t s = 0; s < fn->slots_count; s++) {
        writ_str_t name = writ_profile_name(g->profile, WRIT_NAMING_FIELD, fn->slots[s].name);
        writ_emit_text(emit, "%s%.*s: %.*s", s ? ", " : "", S(name), S(types[s]));
    }
    writ_emit_line(emit, ") -> %.*s {", S(result));
    writ_emit_text(emit, "    %s__%.*s_raw(", writ_str_eq(result, writ_cstr("nil")) ? "" : "return ", S(bound.name));
    for (size_t s = 0; s < fn->slots_count; s++) {
        writ_str_t name = writ_profile_name(g->profile, WRIT_NAMING_FIELD, fn->slots[s].name);
        writ_emit_text(emit, "%s%.*s", s ? ", " : "", S(name));
    }
    writ_emit_line(emit, ")");
    writ_emit_line(emit, "}");
}

static void
emit_policy_dispatch(const n00b_t *g, writ_emit_t *emit, size_t fn)
{
    const writ_plan_view_t *plan = bound_plan(g, fn, 0);
    wrapper_t w = {.g = g, .fn = fn, .plan = plan, .bound = bound_fn(g, fn), .receiver = WRIT_PLAN_NONE};
    w.receiver = find_receiver(&w);
    uint32_t *outs = writ_alloc(g->session, sizeof(uint32_t) * (plan->items_count + 1));
    size_t out_count = results(&w, outs);
    if (out_count > 1 || outcome_enum(plan)) {
        return;
    }
    const writ_slot_view_t *policy = &plan->fn->slots[plan->policy_slot];
    writ_str_t policy_name = writ_profile_name(g->profile, WRIT_NAMING_FIELD, policy->name);
    writ_str_t enum_name = writ_str_format(
        g->session, "%.*s_%.*s", S(writ_name_case(g->session, wrapper_name(&w), WRIT_CASE_TITLE_SNAKE)),
        S(writ_name_case(g->session, policy->name, WRIT_CASE_TITLE_SNAKE)));
    writ_emit_line(emit, "");
    writ_emit_open(emit, "enum %.*s", S(enum_name));
    for (size_t a = 0; a < policy->policy_arms_count; a++) {
        writ_emit_line(emit, "%.*s = %zu%s", S(arm_member(g, &policy->policy_arms[a])), a,
                       a + 1 < policy->policy_arms_count ? "," : "");
    }
    writ_emit_close(emit, nullptr);
    uint32_t keywords = WRIT_PLAN_NONE;
    for (uint32_t i = 0; i < plan->items_count; i++) {
        keywords = keyword_record(g, &plan->items[i]) ? i : keywords;
    }
    bool options_record = keywords != WRIT_PLAN_NONE && keywords_as_record(&w, keywords);
    writ_emit_line(emit, "");
    writ_emit_text(emit, "func %.*s(", S(wrapper_name(&w)));
    bool first = true;
    emit_wrapper_params(&w, emit, keywords, options_record, &first);
    writ_emit_text(emit, "%s%.*s: %.*s)", first ? "" : ", ", S(policy_name), S(enum_name));
    if (out_count == 1) {
        writ_emit_text(emit, " -> %.*s", S(item_type(g, fn, plan, outs[0])));
    }
    if (can_fail(&w)) {
        writ_emit_text(emit, " fail: %.*s", S(error_type(&w)));
    }
    writ_emit_open(emit, "");
    for (size_t a = 0; a < policy->policy_arms_count; a++) {
        const writ_plan_view_t *arm_plan = bound_plan(g, fn, a);
        wrapper_t arm = {.g = g, .fn = fn, .plan = arm_plan, .bound = w.bound, .receiver = w.receiver};
        arm.suffix = writ_name_case(g->session, arm_member(g, &policy->policy_arms[a]), WRIT_CASE_SNAKE);
        bool last = a + 1 == policy->policy_arms_count;
        if (!last) {
            writ_emit_open(emit, "if %.*s == %.*s.%.*s", S(policy_name), S(enum_name),
                           S(arm_member(g, &policy->policy_arms[a])));
        }
        writ_emit_text(emit, "%s%.*s(", out_count ? "return " : "", S(wrapper_name(&arm)));
        emit_wrapper_arguments(&arm, emit, keywords, options_record);
        writ_emit_line(emit, ")%s", can_fail(&arm) ? "!" : "");
        if (!last) {
            if (!out_count) {
                writ_emit_line(emit, "return");
            }
            writ_emit_close(emit, nullptr);
        }
    }
    writ_emit_close(emit, nullptr);
}

static writ_str_t
n00b_text(const n00b_t *g)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    emit_types(g, &emit, true);
    size_t count = writ_model_fn_count(g->model);
    for (size_t i = 0; i < count; i++) {
        writ_bound_fn_t bound = bound_fn(g, i);
        if (bound.bound && bound.raw) {
            emit_raw(g, &emit, writ_model_fn_at(g->model, i), bound);
        } else if (bound.bound) {
            for (size_t k = 0; bound_plan(g, i, k); k++) {
                emit_extern(g, &emit, i, bound_plan(g, i, k));
            }
        }
    }
    bool takes_bytes = false;
    for (size_t i = 0; i < g->externs->len; i++) {
        takes_bytes = takes_bytes || strstr(g->externs->data[i].signature.data, "Ffi_Bytes");
    }
    if (shim_keeps(g)) {
        writ_str_t kept_params[] = {writ_cstr("u64")};
        add_extern(g, writ_cstr("__ffi_kept_release"), writ_str_format(g->session, "%.*s_kept_free", S(g->shim)),
                   kept_params, 1, writ_cstr("nil"));
        writ_emit_line(&emit, "");
        writ_emit_line(&emit, "func __ffi_kept_free(kept: u64) {");
        writ_emit_line(&emit, "    __ffi_kept_release(kept)");
        writ_emit_line(&emit, "}");
    }
    if (takes_bytes) {
        writ_str_t take_params[] = {writ_cstr("Ref[Ffi_Bytes]"), writ_cstr("Ref[Array[u8]]")};
        add_extern(g, writ_cstr("__ffi_bytes_take"), writ_str_format(g->session, "%.*s_take_bytes", S(g->shim)),
                   take_params, 2, writ_cstr("nil"));
        writ_emit_line(&emit, "");
        writ_emit_line(&emit, "func __ffi_take_bytes(bytes: Ref[Ffi_Bytes]) -> Array[u8] {");
        writ_emit_line(&emit, "    let value: Ffi_Bytes = bytes.__value");
        writ_emit_line(&emit, "    var copied: Array[u8] = new[Array[u8]](cast[int]value.length)");
        writ_emit_line(&emit, "    __ffi_bytes_take(bytes, ref copied)");
        writ_emit_line(&emit, "    return copied");
        writ_emit_line(&emit, "}");
    }
    if (takes_bytes && takes_optional_bytes(g)) {
        emit_optional_taker(&emit, writ_cstr("bytes"), writ_cstr("Array[u8]"));
    }
    if (takes_bytes && takes_text(g)) {
        writ_emit_line(&emit, "");
        writ_emit_open(&emit, "func __ffi_take_text(bytes: Ref[Ffi_Bytes], text: Ref[string]) -> bool");
        writ_emit_line(&emit, "var copied = __ffi_take_bytes(bytes)");
        writ_emit_line(&emit, "return __string_from_utf8(ref copied, text)");
        writ_emit_close(&emit, nullptr);
    }
    if (takes_bytes && takes_detail_text(g)) {
        writ_emit_line(&emit, "");
        writ_emit_open(&emit, "func __ffi_take_string(bytes: Ref[Ffi_Bytes]) -> string?");
        writ_emit_line(&emit, "var text: string = \"\"");
        writ_emit_open(&emit, "if __ffi_take_text(bytes, ref text)");
        writ_emit_line(&emit, "return text");
        writ_emit_close(&emit, nullptr);
        writ_emit_line(&emit, "return nil");
        writ_emit_close(&emit, nullptr);
    }
    if (copies_cstr(g)) {
        emit_cstr_n00b_helpers(g, &emit);
    }
    emit_n00b_array_takers(g, &emit);
    emit_record_takers(g, &emit, emit_n00b_record_taker);
    for (size_t i = 0; i < count; i++) {
        writ_bound_fn_t bound = bound_fn(g, i);
        for (size_t k = 0; bound.bound && !bound.raw && bound_plan(g, i, k); k++) {
            emit_wrapper(g, &emit, i, bound_plan(g, i, k));
        }
        if (bound.bound && !bound.raw && bound.policy_arm == WRIT_PLAN_NONE && bound_plan(g, i, 1)) {
            emit_policy_dispatch(g, &emit, i);
        }
    }
    return writ_emit_finish(&emit, g->session);
}

static writ_str_t
provider(const n00b_t *g, writ_str_t provide)
{
    core_buffer_t *buffer = core_buffer_empty();
    core_buffer_puts(buffer, "n00b_");
    for (size_t i = 0; i < provide.length; i++) {
        core_buffer_putc(buffer, provide.data[i] == '.' ? '_' : provide.data[i]);
    }
    return writ_str_from_buffer(g->session, buffer);
}

static bool
export_fails(const writ_plan_view_t *plan)
{
    for (size_t o = 0; o < plan->outcomes_count; o++) {
        if (plan->outcomes[o].kind == WRIT_OUTCOME_FAILURE) {
            return true;
        }
    }
    return false;
}

static writ_str_t
export_adapter(const n00b_t *g, writ_str_t contract_name)
{
    return writ_str_format(g->session, "__export_%.*s", S(contract_name));
}

static writ_str_t
export_symbol(const n00b_t *g, writ_str_t adapter)
{
    return writ_str_format(g->session, "%.*s_%.*s", S(g->shim), S(adapter));
}

static bool
export_pointer(const writ_type_view_t *type)
{
    switch (type ? type->kind : WRIT_TYPE_VOID) {
    case WRIT_TYPE_POINTER:
    case WRIT_TYPE_HANDLE:
    case WRIT_TYPE_ARRAY:
    case WRIT_TYPE_CALLBACK:
    case WRIT_TYPE_CSTR:
        return true;
    case WRIT_TYPE_VIEW:
        return export_pointer(type->target);
    default:
        return false;
    }
}

static writ_str_t
export_value_type(const n00b_t *g, const writ_type_view_t *type)
{
    if (export_pointer(type)) {
        return writ_cstr("uint");
    }
    while (type && (type->kind == WRIT_TYPE_ENUM || type->kind == WRIT_TYPE_VIEW) && type->target) {
        type = type->target;
    }
    return scalar_type(g, type);
}

static writ_str_t
export_slot_named(const n00b_t *g, const writ_plan_view_t *plan, uint32_t item, writ_part_t part)
{
    for (uint32_t s = 0; s < plan->slots_count; s++) {
        if (plan->slots[s].item == item && plan->slots[s].part == part && s < plan->fn->slots_count) {
            return writ_str_format(g->session, "c_%.*s", S(plan->fn->slots[s].name));
        }
    }
    return (writ_str_t){0};
}

static writ_str_t
export_local(const n00b_t *g, const writ_plan_view_t *plan, uint32_t item)
{
    return writ_str_format(g->session, "%.*s_value", S(plan->items[item].name));
}

static writ_str_t
export_implementation(const n00b_t *g, writ_str_t handle)
{
    return writ_binding_implementation(g->b, handle);
}

static bool
export_code_for(const n00b_t *g, const writ_plan_view_t *plan, bool lifting, uint32_t item, int64_t *code)
{
    for (uint32_t o = 0; o < plan->outcomes_count; o++) {
        int64_t channel = 0;
        if (plan->outcomes[o].kind == WRIT_OUTCOME_SUCCESS
            && (item == WRIT_PLAN_NONE || lifts_in(plan, o, item) == lifting)
            && writ_c_find_outcome(g->model, plan, o, code, &channel)) {
            return true;
        }
    }
    return false;
}

static void
emit_export_success(const n00b_t *g, writ_emit_t *emit, const writ_plan_view_t *plan, bool lifting, uint32_t item)
{
    int64_t code = 0;
    if (!plan->fn->result) {
        writ_emit_line(emit, "return");
    } else if (export_pointer(plan->fn->result->type)) {
        writ_emit_line(emit, "return 1");
    } else if (export_code_for(g, plan, lifting, item, &code)) {
        writ_emit_line(emit, "return %lld", (long long)code);
    } else {
        writ_emit_line(emit, "return 0");
    }
}

static void
emit_export_failure(const n00b_t *g, writ_emit_t *emit, const writ_plan_view_t *plan, const char *code)
{
    (void)g;
    writ_emit_line(emit, "__export_failed(failed, %d)", strcmp(code, "-1") ? 1 : 2);
    if (!plan->fn->result) {
        writ_emit_line(emit, "return");
    } else if (export_pointer(plan->fn->result->type)) {
        writ_emit_line(emit, "return 0");
    } else {
        writ_emit_line(emit, "return %s", code);
    }
}

static void
emit_export_thread_checks(const n00b_t *g, writ_emit_t *emit, const wrapper_t *w)
{
    const writ_plan_view_t *plan = w->plan;
    if (plan->fn->thread == WRIT_THREAD_MAIN) {
        writ_emit_line(emit, "assert __os_thread_is_main() != 0");
    }
    for (uint32_t i = 0; plan->fn->thread == WRIT_THREAD_UNSTATED && i < plan->items_count; i++) {
        const writ_handle_view_t *handle = exposed_input(w, i) && is_handle(&plan->items[i])
                                               ? writ_model_handle_named(g->model, plan->items[i].type->name)
                                               : nullptr;
        if (handle && handle->thread_rule == WRIT_THREAD_RULE_AFFINE) {
            writ_emit_line(emit, "assert __ffi_table_owner(__handles_%.*s, cast[u64]%.*s) == __os_thread_id()",
                           S(handle->name), S(export_slot_named(g, plan, i, WRIT_PART_WHOLE)));
        }
    }
}

static void
emit_export_inputs(const n00b_t *g, writ_emit_t *emit, const wrapper_t *w)
{
    const writ_plan_view_t *plan = w->plan;
    emit_export_thread_checks(g, emit, w);
    for (uint32_t i = 0; i < plan->items_count; i++) {
        if (!exposed_input(w, i)) {
            continue;
        }
        const writ_plan_item_view_t *item = &plan->items[i];
        writ_str_t local = export_local(g, plan, i);
        writ_str_t whole = export_slot_named(g, plan, i, WRIT_PART_WHOLE);
        if (is_handle(item)) {
            writ_emit_line(emit, "let %.*s = __export_%.*s(%.*s)", S(local), S(item->type->name), S(whole));
            writ_emit_open(emit, "if %.*s == nil", S(local));
            emit_export_failure(g, emit, plan, "-1");
            writ_emit_close(emit, nullptr);
        } else if (item->shape == WRIT_SHAPE_BYTES || item->shape == WRIT_SHAPE_ARRAY) {
            writ_str_t pointer = export_slot_named(g, plan, i, WRIT_PART_POINTER);
            writ_str_t length = export_slot_named(g, plan, i, WRIT_PART_LENGTH);
            writ_str_t array = item_type(g, w->fn, plan, i);
            writ_emit_line(emit, "var %.*s: %.*s = new[%.*s](cast[int]%.*s)", S(local), S(array), S(array),
                           S(length));
            writ_emit_line(emit, "__export_copy_in(%.*s, ref %.*s)", S(pointer), S(local));
        } else if (item->shape == WRIT_SHAPE_TEXT) {
            writ_emit_line(emit, "var %.*s: string = \"\"", S(local));
            writ_emit_open(emit, "if !__ffi_copy_cstr(%.*s, ref %.*s)", S(whole), S(local));
            emit_export_failure(g, emit, plan, "-1");
            writ_emit_close(emit, nullptr);
        } else {
            writ_emit_line(emit, "let %.*s = %.*s", S(local), S(whole));
        }
    }
}

static void
emit_export_call(const n00b_t *g, writ_emit_t *emit, const wrapper_t *w, bool binds)
{
    const writ_plan_view_t *plan = w->plan;
    writ_str_t annotation = {0};
    for (uint32_t i = 0; binds && i < plan->items_count; i++) {
        if (is_result(&plan->items[i]) && is_handle(&plan->items[i])) {
            annotation = writ_str_format(g->session, ": %.*s",
                                         S(export_implementation(g, plan->items[i].type->name)));
        }
    }
    mark(g, emit, w->bound.provided_at, w->bound.provide);
    writ_emit_text(emit, "%s%.*s%s%.*s(", binds ? "let result" : "", S(annotation), binds ? " = " : "",
                   S(w->bound.provide));
    bool first = true;
    for (uint32_t i = 0; i < plan->items_count; i++) {
        if (exposed_input(w, i)) {
            writ_emit_text(emit, "%s%.*s", first ? "" : ", ", S(export_local(g, plan, i)));
            first = false;
        }
    }
    writ_emit_line(emit, ")%s", export_fails(w->plan) ? "!" : "");
}

static size_t export_handle_ordinal(const n00b_t *g, writ_str_t name);

static writ_str_t
export_subject(const n00b_t *g, const wrapper_t *w)
{
    for (uint32_t i = 0; i < w->plan->items_count; i++) {
        if (exposed_input(w, i) && is_handle(&w->plan->items[i])) {
            return writ_str_format(g->session, "%zu * 4294967296 + %.*s",
                                   export_handle_ordinal(g, w->plan->items[i].type->name),
                                   S(export_slot_named(g, w->plan, i, WRIT_PART_WHOLE)));
        }
    }
    return writ_cstr("0");
}

static void
emit_export_outputs(const n00b_t *g, writ_emit_t *emit, const wrapper_t *w, const uint32_t *outs, size_t count)
{
    const writ_plan_view_t *plan = w->plan;
    uint32_t optional = count == 1 && plan->items[outs[0]].optional ? outs[0] : WRIT_PLAN_NONE;
    if (optional != WRIT_PLAN_NONE) {
        writ_emit_open(emit, "if result == nil");
        emit_export_success(g, emit, plan, false, optional);
        writ_emit_close(emit, nullptr);
    }
    for (size_t r = 0; r < count; r++) {
        const writ_plan_item_view_t *item = &plan->items[outs[r]];
        writ_str_t whole = export_slot_named(g, plan, outs[r], WRIT_PART_WHOLE);
        if (is_handle(item) && whole.length) {
            writ_emit_line(emit, "__export_store(%.*s, cast[uint]__ffi_table_insert(__handles_%.*s, result))", S(whole),
                           S(item->type->name));
        } else if (is_handle(item)) {
            writ_emit_line(emit, "return cast[uint]__ffi_table_insert(__handles_%.*s, result)", S(item->type->name));
            return;
        } else if (item->shape == WRIT_SHAPE_TEXT) {
            writ_emit_line(emit, "return __export_text(result)");
            return;
        } else if (lifted_record(g, record_target(item))) {
            if (item->nullable) {
                writ_emit_open(emit, "if result == nil");
                writ_emit_line(emit, "return 0");
                writ_emit_close(emit, nullptr);
            }
            const writ_record_view_t *record = lifted_record(g, record_target(item));
            const writ_slot_view_t *fields[WRIT_C_MAX_ARITY];
            size_t field_count = counted_fields(g, record, fields);
            writ_emit_text(emit, "return __export_keep_%.*s(%.*s", S(record->name), S(export_subject(g, w)));
            for (size_t f = 0; f < field_count; f++) {
                writ_emit_text(emit, ", result.%.*s",
                               S(writ_profile_name(g->profile, WRIT_NAMING_FIELD, fields[f]->name)));
            }
            writ_emit_line(emit, ")");
            return;
        } else if (item->shape == WRIT_SHAPE_SCALAR && !whole.length) {
            writ_emit_line(emit, optional != WRIT_PLAN_NONE ? "return cast[i64]result.get()" : "return cast[i64]result");
            return;
        } else if (item->shape == WRIT_SHAPE_BYTES) {
            writ_emit_line(emit, optional != WRIT_PLAN_NONE ? "var bytes: Array[u8] = result.get()"
                                                            : "var bytes: Array[u8] = result");
            writ_emit_line(emit, "__export_own_bytes(ref bytes, %.*s, %.*s)",
                           S(export_slot_named(g, plan, outs[r], WRIT_PART_POINTER)),
                           S(export_slot_named(g, plan, outs[r], WRIT_PART_LENGTH)));
        }
    }
    emit_export_success(g, emit, plan, true, optional);
}

static void
emit_export_adapter(const n00b_t *g, writ_emit_t *emit, size_t index, const writ_plan_view_t *plan)
{
    const writ_fn_view_t *fn = plan->fn;
    wrapper_t w = {.g = g, .fn = index, .plan = plan, .bound = bound_fn(g, index),
                   .receiver = WRIT_PLAN_NONE};
    uint32_t *outs = writ_alloc(g->session, sizeof(uint32_t) * (plan->items_count + 1));
    size_t count = results(&w, outs);
    writ_emit_line(emit, "");
    mark(g, emit, fn->declared_at, fn->contract_name);
    writ_emit_text(emit, "func %.*s(", S(export_adapter(g, fn->contract_name)));
    for (size_t s = 0; s < fn->slots_count; s++) {
        writ_emit_text(emit, "c_%.*s: %.*s, ", S(fn->slots[s].name), S(export_value_type(g, fn->slots[s].type)));
    }
    writ_emit_text(emit, "failed: uint)");
    if (fn->result) {
        writ_emit_text(emit, " -> %s", export_pointer(fn->result->type) ? "uint" : "i64");
    }
    writ_emit_open(emit, "");
    emit_export_inputs(g, emit, &w);
    if (export_fails(plan)) {
        writ_emit_open(emit, "try");
        emit_export_call(g, emit, &w, count != 0);
        emit_export_outputs(g, emit, &w, outs, count);
        writ_emit_close(emit, " else failure {");
        writ_emit_indent(emit, 1);
        emit_export_failure(g, emit, plan, "cast[i64]failure.code");
        writ_emit_close(emit, nullptr);
    } else {
        emit_export_call(g, emit, &w, count != 0);
        emit_export_outputs(g, emit, &w, outs, count);
    }
    writ_emit_close(emit, nullptr);
}

static size_t
export_handle_ordinal(const n00b_t *g, writ_str_t name)
{
    for (size_t i = 0; i < writ_model_handle_count(g->model); i++) {
        if (writ_str_eq(writ_model_handle_at(g->model, i)->name, name)) {
            return i + 1;
        }
    }
    return 0;
}

static void
emit_export_handles(const n00b_t *g, writ_emit_t *emit)
{
    for (size_t i = 0; i < writ_model_handle_count(g->model); i++) {
        const writ_handle_view_t *handle = writ_model_handle_at(g->model, i);
        writ_str_t implementation = export_implementation(g, handle->name);
        if (!implementation.length) {
            continue;
        }
        writ_emit_line(emit, "");
        emit_table(emit, writ_str_format(g->session, "__handles_%.*s", S(handle->name)), implementation);
        writ_emit_line(emit, "");
        writ_emit_open(emit, "func __export_%.*s(handle: uint) -> %.*s?", S(handle->name), S(implementation));
        writ_emit_line(emit, "return __ffi_table_get(__handles_%.*s, cast[u64]handle)", S(handle->name));
        writ_emit_close(emit, nullptr);
        writ_emit_line(emit, "");
        writ_emit_open(emit, "func __export_release_%.*s(handle: uint)", S(handle->name));
        if (handle->thread_rule == WRIT_THREAD_RULE_AFFINE) {
            writ_emit_line(emit, "assert __ffi_table_owner(__handles_%.*s, cast[u64]handle) == __os_thread_id()",
                           S(handle->name));
        }
        writ_emit_line(emit, "__export_drop(%zu * 4294967296 + handle)", i + 1);
        writ_emit_line(emit, "__ffi_table_free(__handles_%.*s, cast[u64]handle)", S(handle->name));
        writ_emit_close(emit, nullptr);
    }
}

static void
emit_export_holds(const n00b_t *g, writ_emit_t *emit)
{
    writ_str_t address[] = {writ_cstr("Ref[Array[u8]]")};
    add_extern(g, writ_cstr("__export_address"), writ_str_format(g->session, "%.*s_export_address", S(g->shim)),
               address, 1, writ_cstr("uint"));
    writ_emit_line(emit, "");
    writ_emit_open(emit, "record Export_Hold");
    writ_emit_line(emit, "private handle: uint");
    writ_emit_line(emit, "private items: List[Array[u8]]");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_emit_line(emit, "var __export_holds: List[Export_Hold] = new[List[Export_Hold]]()");
    writ_emit_line(emit, "var __export_texts: List[Array[u8]] = new[List[Array[u8]]]()");
    writ_emit_line(emit, "var __export_text_addresses: List[uint] = new[List[uint]]()");
    writ_emit_line(emit, "");
    writ_emit_open(emit, "func __export_drop(handle: uint)");
    writ_emit_line(emit, "var kept: List[Export_Hold] = new[List[Export_Hold]]()");
    writ_emit_open(emit, "for hold in __export_holds");
    writ_emit_open(emit, "if hold.handle != handle");
    writ_emit_line(emit, "kept.append(hold)");
    writ_emit_close(emit, nullptr);
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "__export_holds = kept");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_emit_open(emit, "func __export_hold(handle: uint, items: List[Array[u8]])");
    writ_emit_line(emit, "__export_drop(handle)");
    writ_emit_line(emit, "let hold: Export_Hold = (handle = handle, items = items)");
    writ_emit_line(emit, "__export_holds.append(hold)");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_emit_open(emit, "func __export_same(left: Array[u8], right: Array[u8]) -> bool");
    writ_emit_open(emit, "if left.len() != right.len()");
    writ_emit_line(emit, "return false");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "var i: uint = 0");
    writ_emit_open(emit, "while i < left.len()");
    writ_emit_open(emit, "if left[i] != right[i]");
    writ_emit_line(emit, "return false");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "i += 1");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "return true");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_emit_open(emit, "func __export_text(text: string) -> uint");
    writ_emit_line(emit, "let source: Array[u8] = __text_byte_array(text)");
    writ_emit_line(emit, "var bytes: Array[u8] = new[Array[u8]](cast[int]source.len() + 1)");
    writ_emit_line(emit, "var k: uint = 0");
    writ_emit_open(emit, "while k < source.len()");
    writ_emit_line(emit, "bytes[k] = source[k]");
    writ_emit_line(emit, "k += 1");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "var i = 0");
    writ_emit_open(emit, "while i < cast[int]__export_texts.len()");
    writ_emit_open(emit, "if __export_same(__export_texts[i], bytes)");
    writ_emit_line(emit, "return __export_text_addresses[i]");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "i += 1");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "__export_texts.append(bytes)");
    writ_emit_line(emit, "let address = __export_address(ref bytes)");
    writ_emit_line(emit, "__export_text_addresses.append(address)");
    writ_emit_line(emit, "return address");
    writ_emit_close(emit, nullptr);
}

static void
emit_export_record_keep(const n00b_t *g, writ_emit_t *emit, const writ_record_view_t *record,
                        const writ_slot_view_t **fields, size_t count)
{
    writ_str_t fill = writ_str_format(g->session, "__export_%.*s_fill", S(record->name));
    writ_str_t params[WRIT_C_MAX_ARITY + 1] = {writ_cstr("Ref[Array[u8]]")};
    for (size_t f = 0; f < count; f++) {
        params[f + 1] = writ_str_format(g->session, "Ref[Array[%.*s]]", S(scalar_type(g, fields[f]->type->target)));
    }
    add_extern(g, fill, writ_str_format(g->session, "%.*s_export_%.*s_fill", S(g->shim), S(record->name)), params,
               count + 1, writ_cstr("nil"));
    writ_emit_line(emit, "");
    writ_emit_text(emit, "func __export_keep_%.*s(handle: uint", S(record->name));
    for (size_t f = 0; f < count; f++) {
        writ_emit_text(emit, ", %.*s: Array[%.*s]", S(writ_profile_name(g->profile, WRIT_NAMING_FIELD, fields[f]->name)),
                       S(scalar_type(g, fields[f]->type->target)));
    }
    writ_emit_open(emit, ") -> uint");
    for (size_t f = 0; f < count; f++) {
        writ_str_t name = writ_profile_name(g->profile, WRIT_NAMING_FIELD, fields[f]->name);
        writ_emit_line(emit, "var %.*s_copy = %.*s", S(name), S(name));
    }
    writ_emit_line(emit, "var storage: Array[u8] = new[Array[u8]](%u)", record->type ? record->type->size : 0);
    writ_emit_line(emit, "var items: List[Array[u8]] = new[List[Array[u8]]]()");
    writ_emit_line(emit, "items.append(storage)");
    writ_emit_text(emit, "%.*s(ref storage", S(fill));
    for (size_t f = 0; f < count; f++) {
        writ_emit_text(emit, ", ref %.*s_copy", S(writ_profile_name(g->profile, WRIT_NAMING_FIELD, fields[f]->name)));
    }
    writ_emit_line(emit, ")");
    for (size_t f = 0; f < count; f++) {
        writ_emit_line(emit, "items.append(%.*s_copy)",
                       S(writ_profile_name(g->profile, WRIT_NAMING_FIELD, fields[f]->name)));
    }
    writ_emit_line(emit, "__export_hold(handle, items)");
    writ_emit_line(emit, "return __export_address(ref storage)");
    writ_emit_close(emit, nullptr);
}

static void
emit_export_c_record_fill(const n00b_t *g, writ_emit_t *emit, const writ_record_view_t *record,
                          const writ_slot_view_t **fields, size_t count)
{
    writ_str_t type = writ_c_type(&g->c, record->type, false);
    writ_emit_text(emit, "void %.*s_export_%.*s_fill(void *storage", S(g->shim), S(record->name));
    for (size_t f = 0; f < count; f++) {
        writ_emit_text(emit, ", void *%.*s", S(fields[f]->name));
    }
    writ_emit_open(emit, ")");
    writ_emit_line(emit, "size_t length = 0;");
    writ_emit_line(emit, "%.*s *record = n00b_ffi_borrow(storage, &length, %d);", S(type), N00B_FFI_WRITE);
    for (size_t f = 0; f < count; f++) {
        writ_emit_line(emit, "record->%.*s = n00b_ffi_borrow(%.*s, &length, 0);", S(fields[f]->name),
                       S(fields[f]->name));
        writ_emit_line(emit, "record->%.*s = length;", S(writ_c_count_field(g->model, fields[f])));
    }
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
}

static void
emit_export_imports(const n00b_t *g, writ_emit_t *emit)
{
    core_list_t(writ_sym_t) seen = core_list_new(writ_sym_t);
    for (size_t i = 0; i < writ_model_fn_count(g->model) + writ_model_handle_count(g->model); i++) {
        writ_str_t target = i < writ_model_fn_count(g->model)
                                ? bound_fn(g, i).provide
                                : export_implementation(g, writ_model_handle_at(g->model,
                                                                                i - writ_model_fn_count(g->model))
                                                               ->name);
        const char *dot = target.length ? memchr(target.data, '.', target.length) : nullptr;
        if (!dot) {
            continue;
        }
        writ_sym_t module = writ_intern_str(g->session, (writ_str_t){target.data, (size_t)(dot - target.data)});
        bool known = false;
        for (size_t s = 0; s < seen.len && !known; s++) {
            known = seen.data[s] == module;
        }
        if (!known) {
            core_list_push(seen, module);
            writ_str_t name = writ_sym_str(g->session, module);
            writ_emit_line(emit, "import %.*s", S(name));
        }
    }
    core_list_free(seen);
    writ_emit_line(emit, "");
}

typedef void (*export_emitter_t)(const n00b_t *, writ_emit_t *, size_t, const writ_plan_view_t *);

static void
emit_provided(const n00b_t *g, writ_emit_t *emit, export_emitter_t each)
{
    for (size_t i = 0; i < writ_model_fn_count(g->model); i++) {
        writ_bound_fn_t bound = bound_fn(g, i);
        const writ_plan_view_t *plan = bound.provide.length ? bound_plan(g, i, 0) : nullptr;
        if (plan) {
            each(g, emit, i, plan);
        }
    }
}

static writ_str_t
export_n00b_text(const n00b_t *g)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    emit_export_imports(g, &emit);
    emit_types(g, &emit, false);
    writ_str_t address[] = {writ_cstr("uint"), writ_cstr("uint")};
    writ_str_t copy_in[] = {writ_cstr("uint"), writ_cstr("Ref[Array[u8]]")};
    writ_str_t store[] = {writ_cstr("uint"), writ_cstr("uint")};
    writ_str_t own[] = {writ_cstr("Ref[Array[u8]]"), writ_cstr("uint"), writ_cstr("uint")};
    add_extern(g, writ_cstr("__export_failed"), writ_str_format(g->session, "%.*s_export_failed", S(g->shim)),
               address, 2, writ_cstr("nil"));
    add_extern(g, writ_cstr("__export_copy_in"), writ_str_format(g->session, "%.*s_export_copy_in", S(g->shim)),
               copy_in, 2, writ_cstr("nil"));
    add_extern(g, writ_cstr("__export_store"), writ_str_format(g->session, "%.*s_export_store", S(g->shim)), store,
               2, writ_cstr("nil"));
    add_extern(g, writ_cstr("__export_own_bytes"),
               writ_str_format(g->session, "%.*s_export_own_bytes", S(g->shim)), own, 3, writ_cstr("nil"));
    emit_cstr_n00b_helpers(g, &emit);
    emit_export_holds(g, &emit);
    emit_record_takers(g, &emit, emit_export_record_keep);
    emit_export_handles(g, &emit);
    emit_provided(g, &emit, emit_export_adapter);
    return writ_emit_finish(&emit, g->session);
}

typedef struct {
    writ_expr_env_t base;
} export_env_t;

static void
export_ref(writ_expr_env_t *base, writ_ref_t ref, uint32_t expr, core_buffer_t *out)
{
    (void)base;
    (void)expr;
    ref.kind == WRIT_REF_RESULT ? core_buffer_puts(out, "value") : core_buffer_puts(out, "0");
}

static writ_str_t
export_allocation(const n00b_t *g)
{
    writ_str_t name = writ_binding_allocator(g->b);
    for (size_t i = 0; name.length && i < g->model->allocators.len; i++) {
        const writ_allocator_rec_t *allocator = &g->model->allocators.data[i];
        if (writ_str_eq(writ_sym_str(g->session, allocator->name), name)) {
            return writ_sym_str(g->session, allocator->alloc);
        }
    }
    return (writ_str_t){0};
}

static void
emit_export_c_helpers(const n00b_t *g, writ_emit_t *emit)
{
    writ_emit_open(emit, "uint64_t %.*s_export_address(void *array)", S(g->shim));
    writ_emit_line(emit, "size_t length = 0;");
    writ_emit_line(emit, "return (uint64_t)(uintptr_t)n00b_ffi_borrow(array, &length, 0);");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_emit_open(emit, "void %.*s_export_failed(uint64_t address, uint64_t kind)", S(g->shim));
    writ_emit_line(emit, "*(int *)(uintptr_t)address = (int)kind;");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_emit_open(emit, "void %.*s_export_copy_in(uint64_t data, void *out)", S(g->shim));
    writ_emit_line(emit, "size_t length = 0;");
    writ_emit_line(emit, "uint8_t *items = n00b_ffi_borrow(out, &length, %d);", N00B_FFI_WRITE);
    writ_emit_open(emit, "if (items && data && length)");
    writ_emit_line(emit, "memcpy(items, (const void *)(uintptr_t)data, length);");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "n00b_ffi_release(out);");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_emit_open(emit, "void %.*s_export_store(uint64_t address, uint64_t value)", S(g->shim));
    writ_emit_open(emit, "if (address)");
    writ_emit_line(emit, "*(void **)(uintptr_t)address = (void *)(uintptr_t)value;");
    writ_emit_close(emit, nullptr);
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_str_t alloc = export_allocation(g);
    writ_emit_open(emit, "void %.*s_export_own_bytes(void *bytes, uint64_t data_out, uint64_t length_out)", S(g->shim));
    writ_emit_line(emit, "size_t length = 0;");
    writ_emit_line(emit, "const uint8_t *data = n00b_ffi_borrow(bytes, &length, 0);");
    if (alloc.length) {
        writ_emit_line(emit, "uint8_t *copy = %.*s(length ? length : 1);", S(alloc));
    } else {
        writ_emit_line(emit, "uint8_t *copy = 0;");
    }
    writ_emit_open(emit, "if (copy && data && length)");
    writ_emit_line(emit, "memcpy(copy, data, length);");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "n00b_ffi_release(bytes);");
    writ_emit_line(emit, "*(uint8_t **)(uintptr_t)data_out = copy;");
    writ_emit_line(emit, "*(size_t *)(uintptr_t)length_out = length;");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
}

static void
emit_export_entry_body(const n00b_t *g, writ_emit_t *emit, writ_str_t panic, bool pointer, bool has_result)
{
    (void)g;
    writ_emit_line(emit, "uint64_t buffer[24];");
    writ_emit_line(emit, "uint64_t previous = n00b_ffi_enter((uint64_t)(uintptr_t)buffer);");
    writ_emit_open(emit, "if (n00b_ffi_unwind_save(buffer))");
    writ_emit_line(emit, "n00b_ffi_leave(previous);");
    if (!panic.length) {
        writ_emit_line(emit, "__builtin_trap();");
    } else if (!has_result) {
        writ_emit_line(emit, "return;");
    } else {
        writ_str_t value = pointer ? writ_cstr("0") : panic;
        writ_emit_line(emit, "return %.*s;", S(value));
    }
    writ_emit_close(emit, nullptr);
}

static void
emit_export_entry(const n00b_t *g, writ_emit_t *emit, size_t index, const writ_plan_view_t *plan)
{
    (void)index;
    const writ_fn_view_t *fn = plan->fn;
    writ_str_t panic = writ_binding_panic_value(g->b);
    bool has_result = fn->result != nullptr;
    bool pointer = has_result && export_pointer(fn->result->type);
    writ_str_t symbol = export_symbol(g, export_adapter(g, fn->contract_name));
    writ_emit_text(emit, "%s %.*s(", has_result ? (pointer ? "uint64_t" : "int64_t") : "void", S(symbol));
    for (size_t s = 0; s < fn->slots_count; s++) {
        writ_str_t type = export_pointer(fn->slots[s].type) ? writ_cstr("uint64_t")
                                                            : writ_c_type(&g->c, fn->slots[s].type, false);
        writ_emit_text(emit, "%.*s, ", S(type));
    }
    writ_emit_line(emit, "uint64_t failed);");
    writ_c_emit_real_prototype(&g->c, emit, fn, fn->link_symbol, false, "");
    writ_emit_open(emit, "");
    emit_export_entry_body(g, emit, panic, pointer, has_result);
    writ_emit_line(emit, "int failed = 0;");
    writ_emit_text(emit, "%s%.*s(", has_result ? (pointer ? "uint64_t value = " : "int64_t value = ") : "",
                   S(symbol));
    for (size_t s = 0; s < fn->slots_count; s++) {
        writ_emit_text(emit, export_pointer(fn->slots[s].type) ? "(uint64_t)(uintptr_t)%.*s, " : "%.*s, ",
                       S(fn->slots[s].name));
    }
    writ_emit_line(emit, "(uint64_t)(uintptr_t)&failed);");
    writ_emit_line(emit, "n00b_ffi_leave(previous);");
    if (!has_result) {
        writ_emit_close(emit, nullptr);
        writ_emit_line(emit, "");
        return;
    }
    writ_str_t type = writ_c_type(&g->c, fn->result->type, writ_c_is_text(fn->result->type) && !fn->result->own);
    if (!pointer) {
        export_env_t env = {{.print_ref = export_ref}};
        writ_emit_text(emit, "if (failed == 2 || (failed && !(0");
        for (size_t o = 0; o < plan->outcomes_count; o++) {
            if (plan->outcomes[o].kind == WRIT_OUTCOME_FAILURE && plan->outcomes[o].when_expr) {
                core_buffer_t *text = core_buffer_empty();
                writ_expr_print(g->model, plan->outcomes[o].when_expr, WRIT_LANGUAGE_C, &env.base, text);
                writ_str_t condition = writ_str_from_buffer(g->session, text);
                writ_emit_text(emit, " || (%.*s)", S(condition));
            }
        }
        writ_emit_line(emit, ")))");
        writ_emit_open(emit, "");
        writ_str_t fallback = panic.length ? panic : writ_cstr("-1");
        writ_emit_line(emit, "value = %.*s;", S(fallback));
        writ_emit_close(emit, nullptr);
        writ_emit_line(emit, "return (%.*s)value;", S(type));
    } else {
        writ_emit_line(emit, "return (%.*s)(uintptr_t)value;", S(type));
    }
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
}

static void
emit_export_release(const n00b_t *g, writ_emit_t *emit, const writ_handle_view_t *handle)
{
    writ_str_t base = writ_c_type(&g->c, handle->base, false);
    writ_str_t symbol = export_symbol(g, writ_str_format(g->session, "__export_release_%.*s", S(handle->name)));
    writ_emit_line(emit, "void %.*s(uint64_t handle);", S(symbol));
    writ_emit_open(emit, "void %.*s(%.*s%shandle)", S(handle->release), S(base), is_pointer(base) ? "" : " ");
    emit_export_entry_body(g, emit, writ_binding_panic_value(g->b), false, false);
    writ_emit_line(emit, "%.*s((uint64_t)(uintptr_t)handle);", S(symbol));
    writ_emit_line(emit, "n00b_ffi_leave(previous);");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
}

static writ_str_t
export_c_text(const n00b_t *g)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_line(&emit, "#include <stdint.h>");
    writ_emit_line(&emit, "#include <stddef.h>");
    writ_emit_line(&emit, "#include <string.h>");
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#include \"%.*s\"", S(writ_binding_header(g->b)));
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "uint64_t n00b_ffi_enter(uint64_t buffer);");
    writ_emit_line(&emit, "void n00b_ffi_leave(uint64_t previous);");
    writ_emit_line(&emit, "int32_t n00b_ffi_unwind_save(uint64_t *buffer) __attribute__((returns_twice));");
    writ_emit_line(&emit, "void *n00b_ffi_borrow(void *object, size_t *length, uint32_t access);");
    writ_emit_line(&emit, "void n00b_ffi_release(void *object);");
    writ_str_t alloc = export_allocation(g);
    if (alloc.length) {
        writ_emit_line(&emit, "void *%.*s(size_t size);", S(alloc));
    }
    writ_emit_line(&emit, "");
    emit_cstr_c_helpers(g, &emit);
    emit_export_c_helpers(g, &emit);
    emit_record_takers(g, &emit, emit_export_c_record_fill);
    emit_provided(g, &emit, emit_export_entry);
    for (size_t i = 0; i < writ_model_handle_count(g->model); i++) {
        const writ_handle_view_t *handle = writ_model_handle_at(g->model, i);
        if (handle->release.length && export_implementation(g, handle->name).length
            && !writ_model_fn_named(g->model, handle->release)) {
            emit_export_release(g, &emit, handle);
        }
    }
    return writ_emit_finish(&emit, g->session);
}

static void
map_release(void *object)
{
    core_buffer_free(object);
}

static writ_str_t
export_header_text(const n00b_t *g)
{
    const writ_model_t *model = g->model;
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_str_t guard = writ_str_format(g->session, "%.*s_N00B_EXPORT_H", S(g->c.upper));
    writ_emit_line(&emit, "#ifndef %.*s", S(guard));
    writ_emit_line(&emit, "#define %.*s", S(guard));
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#include <stdbool.h>");
    writ_emit_line(&emit, "#include <stddef.h>");
    writ_emit_line(&emit, "#include <stdint.h>");
    writ_emit_line(&emit, "");
    for (size_t i = 0; i < writ_model_handle_count(model); i++) {
        const writ_type_view_t *opaque = writ_model_handle_at(model, i)->base->target;
        if (opaque && opaque->kind == WRIT_TYPE_OPAQUE && opaque->c_spelling.length) {
            writ_emit_line(&emit, "typedef struct %.*s %.*s;", S(opaque->c_spelling), S(opaque->c_spelling));
        }
    }
    for (size_t i = 0; i < writ_model_enum_count(model); i++) {
        const writ_enum_view_t *view = writ_model_enum_at(model, i);
        writ_emit_open(&emit, "enum");
        for (size_t k = 0; k < view->items_count; k++) {
            writ_emit_line(&emit, "%.*s = %lld,", S(view->items[k].name), (long long)view->items[k].value);
        }
        writ_emit_close(&emit, ";");
    }
    for (size_t i = 0; i < writ_model_const_count(model); i++) {
        const writ_const_view_t *constant = writ_model_const_at(model, i);
        if (constant->text.length) {
            writ_emit_line(&emit, "#define %.*s \"%.*s\"", S(constant->name), S(constant->text));
        } else {
            writ_emit_line(&emit, "#define %.*s %lld", S(constant->name), (long long)constant->value);
        }
    }
    writ_c_emit_records(&g->c, &emit);
    writ_emit_line(&emit, "");
    for (size_t i = 0; i < writ_model_fn_count(model); i++) {
        const writ_fn_view_t *fn = writ_model_fn_at(model, i);
        if (bound_fn(g, i).provide.length) {
            writ_c_emit_real_prototype(&g->c, &emit, fn, fn->link_symbol, false, ";");
        }
    }
    for (size_t i = 0; i < writ_model_handle_count(model); i++) {
        const writ_handle_view_t *handle = writ_model_handle_at(model, i);
        if (handle->release.length && export_implementation(g, handle->name).length
            && !writ_model_fn_named(model, handle->release)) {
            writ_str_t base = writ_c_type(&g->c, handle->base, false);
            writ_emit_line(&emit, "void %.*s(%.*s%shandle);", S(handle->release), S(base), is_pointer(base) ? "" : " ");
        }
    }
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#endif");
    return writ_emit_finish(&emit, g->session);
}

static void
links_for(const n00b_t *g, writ_link_requirements_t *links)
{
    const writ_library_view_t *library = writ_model_library(g->model);
    size_t overrides = writ_binding_library_count(g->b);
    size_t total = overrides ? overrides : library->links_count;
    writ_str_t *libraries = writ_alloc(g->session, sizeof(writ_str_t) * (total + 1));
    writ_str_t *frameworks = writ_alloc(g->session, sizeof(writ_str_t) * (total + 1));
    size_t library_count = 0, framework_count = 0;
    for (size_t i = 0; i < total; i++) {
        writ_str_t link = overrides ? writ_binding_library_at(g->b, i) : library->links[i];
        const char *mark = "framework:";
        if (link.length > strlen(mark) && !memcmp(link.data, mark, strlen(mark))) {
            frameworks[framework_count++] = (writ_str_t){link.data + strlen(mark), link.length - strlen(mark)};
        } else {
            libraries[library_count++] = link;
        }
    }
    size_t path_count = writ_binding_search_path_count(g->b);
    writ_str_t *paths = writ_alloc(g->session, sizeof(writ_str_t) * (path_count + 1));
    for (size_t i = 0; i < path_count; i++) {
        paths[i] = writ_binding_search_path_at(g->b, i);
    }
    *links = (writ_link_requirements_t){
        .libraries_count    = library_count,
        .libraries          = libraries,
        .frameworks_count   = framework_count,
        .frameworks         = frameworks,
        .search_paths_count = path_count,
        .search_paths       = paths,
        .load_dynamic       = writ_str_eq(writ_binding_load(g->b), writ_cstr("dynamic")),
    };
}

static void
append_dispatch_exports(const n00b_t *g, writ_n00b_export_t *exports, size_t *count)
{
    for (size_t i = 0; i < writ_model_callback_count(g->model); i++) {
        const writ_fn_view_t *callback = writ_model_callback_at(g->model, i);
        if (userdata_receiver(callback) != WRIT_PLAN_NONE) {
            exports[(*count)++] = (writ_n00b_export_t){
                .function = callback_table(g, writ_binding_type(g->b, callback->contract_name), "dispatch"),
                .symbol = callback_dispatch_symbol(g, callback)};
        }
    }
}

static writ_n00b_export_t *
adapter_exports(const n00b_t *g, size_t *count)
{
    size_t capacity = writ_model_fn_count(g->model) + writ_model_handle_count(g->model)
                    + writ_model_callback_count(g->model) + 1;
    writ_n00b_export_t *exports = writ_alloc(g->session, sizeof(*exports) * capacity);
    *count = 0;
    for (size_t i = 0; i < writ_model_fn_count(g->model); i++) {
        writ_bound_fn_t bound = bound_fn(g, i);
        if (bound.provide.length) {
            writ_str_t adapter = export_adapter(g, writ_model_fn_at(g->model, i)->contract_name);
            exports[(*count)++] = (writ_n00b_export_t){.function = adapter, .symbol = export_symbol(g, adapter)};
        }
    }
    for (size_t i = 0; i < writ_model_handle_count(g->model); i++) {
        const writ_handle_view_t *handle = writ_model_handle_at(g->model, i);
        if (export_implementation(g, handle->name).length) {
            writ_str_t release = writ_str_format(g->session, "__export_release_%.*s", S(handle->name));
            exports[(*count)++] = (writ_n00b_export_t){.function = release, .symbol = export_symbol(g, release)};
        }
    }
    append_dispatch_exports(g, exports, count);
    return exports;
}

static writ_n00b_export_t *
provided_exports(const n00b_t *g, size_t *count)
{
    size_t capacity = 1 + writ_model_callback_count(g->model);
    for (size_t i = 0; i < writ_model_fn_count(g->model); i++) {
        capacity += writ_model_fn_at(g->model, i)->slots_count;
    }
    writ_n00b_export_t *exports = writ_alloc(g->session, sizeof(*exports) * capacity);
    *count = 0;
    for (size_t i = 0; i < writ_model_fn_count(g->model); i++) {
        const writ_fn_view_t *fn = writ_model_fn_at(g->model, i);
        for (size_t s = 0; bound_fn(g, i).bound && s < fn->slots_count; s++) {
            writ_str_t target = writ_binding_slot_provide(g->b, i, fn->slots[s].name);
            bool seen = !target.length;
            for (size_t e = 0; e < *count && !seen; e++) {
                seen = writ_str_eq(exports[e].function, target);
            }
            if (!seen) {
                exports[(*count)++] = (writ_n00b_export_t){.function = target, .symbol = provider(g, target)};
            }
        }
    }
    append_dispatch_exports(g, exports, count);
    return exports;
}

writ_status_t
writ_gen_n00b(const writ_gen_n00b_request_t *request, const writ_output_t *output, writ_link_requirements_t *links,
              writ_n00b_externs_t *externs)
{
    if (!request || !request->binding || !output || !output->write_file || !links || !externs) {
        return WRIT_INVALID_ARGUMENT;
    }
    *externs = (writ_n00b_externs_t){0};
    const writ_binding_t *b = request->binding;
    const writ_model_t *model = writ_binding_model(b);
    writ_session_t *session = model->session;
    writ_str_t prefix = request->shim_prefix.length ? request->shim_prefix : writ_c_default_prefix(model);
    writ_str_t module = request->module_name.length ? request->module_name : writ_model_library(model)->name;
    core_list_t(writ_n00b_extern_t) extern_list = core_list_new(writ_n00b_extern_t);
    n00b_t g = {.b = b, .model = model, .profile = writ_binding_profile(b), .session = session, .shim = prefix,
                .c = {model, prefix, writ_name_case(session, prefix, WRIT_CASE_UPPER), true},
                .map = core_buffer_empty(), .type_namespace = request->type_namespace, .externs = &extern_list};
    writ_on_session_free(session, map_release, g.map);
    core_list_t(plan_patch_t) patches = core_list_new(plan_patch_t);
    core_list_t(record_handle_t) record_handles = core_list_new(record_handle_t);
    g.patches = &patches;
    g.record_handles = &record_handles;
    find_record_handles(&g);
    size_t count = writ_model_fn_count(model);
    const writ_plan_view_t **plans = writ_alloc(session, sizeof(writ_plan_view_t *) * (count ? count : 1));
    for (size_t i = 0; i < count; i++) {
        writ_bound_fn_t bound = bound_fn(&g, i);
        plans[i] = bound.raw ? nullptr : patched_plan(&g, writ_model_fn_plan(model, writ_model_fn_at(model, i)));
        if (!plans[i] && bound.bound && !bound.raw) {
            core_list_free(patches);
            core_list_free(record_handles);
            return WRIT_HAS_ERRORS;
        }
    }
    links_for(&g, links);
    bool exporting = writ_binding_exports(b);
    writ_status_t status = WRIT_OK;
    if (exporting) {
        status = output->write_file(output->context, writ_binding_header(b), export_header_text(&g));
        if (status == WRIT_OK) {
            status = output->write_file(output->context, writ_str_format(session, "%.*s_n00b_export.c", S(prefix)),
                                        export_c_text(&g));
        }
    } else {
        writ_gen_c_request_t c_request = {.struct_size = sizeof(c_request), .prefix = prefix, .pointer_normalized = true};
        status = writ_gen_c(model, &c_request, output);
        if (status == WRIT_OK) {
            status = output->write_file(output->context, writ_str_format(session, "%.*s_n00b.c", S(prefix)),
                                        shim_c_text(&g, plans));
        }
    }
    if (status == WRIT_OK) {
        status = output->write_file(output->context, writ_str_format(session, "%.*s.n", S(module)),
                                    exporting ? export_n00b_text(&g) : n00b_text(&g));
    }
    if (status == WRIT_OK) {
        status = output->write_file(output->context, writ_str_format(session, "%.*s.n.map", S(module)),
                                    writ_str_copy(session, g.map->data ? g.map->data : "", g.map->byte_len));
    }
    writ_n00b_extern_t *items = writ_alloc(session, sizeof(*items) * (extern_list.len ? extern_list.len : 1));
    for (size_t i = 0; i < extern_list.len; i++) {
        items[i] = extern_list.data[i];
    }
    size_t export_count = 0;
    writ_n00b_export_t *exports = exporting ? adapter_exports(&g, &export_count) : provided_exports(&g, &export_count);
    *externs = (writ_n00b_externs_t){.count = extern_list.len, .items = items, .exports_count = export_count,
                                     .exports = exports, .header = exporting ? writ_binding_header(b) : (writ_str_t){0}};
    core_list_free(extern_list);
    core_list_free(patches);
    core_list_free(record_handles);
    return status;
}
