#include "gen.h"

typedef struct {
    const writ_binding_t *b;
    const writ_model_t   *model;
    const writ_profile_t *profile;
    writ_session_t       *session;
    writ_c_context_t      c;
    writ_str_t            shim;
    core_buffer_t        *map;
} n00b_t;

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
    PARAM_OUT,
} param_kind_t;

typedef struct {
    param_kind_t kind;
    uint32_t     item;
    writ_str_t   name;
    writ_str_t   c_type;
    writ_str_t   n00b_type;
    size_t       parts;
} shim_param_t;

core_list_decl(shim_param_t);

static const writ_type_view_t *record_target(const writ_plan_item_view_t *item);

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

static writ_str_t
item_type(const n00b_t *g, size_t fn, const writ_plan_view_t *plan, uint32_t index)
{
    const writ_plan_item_view_t *item = &plan->items[index];
    writ_str_t repr = writ_binding_slot_repr(g->b, fn, item->name);
    const writ_type_view_t *element = item->type && item->type->target ? item->type->target : item->type;
    switch (item->shape) {
    case WRIT_SHAPE_BYTES:
        return is_text_repr(repr) ? writ_cstr("string") : buffer_type(g);
    case WRIT_SHAPE_TEXT:
        return repr.length && !is_text_repr(repr) ? buffer_type(g) : writ_cstr("string");
    case WRIT_SHAPE_ARRAY:
        return writ_str_format(g->session, "Array[%.*s]", S(scalar_type(g, element)));
    case WRIT_SHAPE_REST_PACK:
        return writ_str_format(g->session, writ_str_eq(repr, writ_cstr("array")) ? "Array[%.*s]" : "List[%.*s]",
                               S(scalar_type(g, item->type)));
    case WRIT_SHAPE_OPTIONAL:
        return writ_str_format(g->session, "%.*s?", S(scalar_type(g, element)));
    default:
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

static void
shim_params(const n00b_t *g, size_t fn, const writ_plan_view_t *plan, core_list_t(shim_param_t) *params)
{
    uint32_t *order = writ_alloc(g->session, sizeof(uint32_t) * (plan->items_count + 1));
    size_t ordered = writ_c_param_order(plan, order);
    for (size_t o = 0; o < ordered; o++) {
        uint32_t index = order[o];
        const writ_plan_item_view_t *item = &plan->items[index];
        writ_c_param_t c_params[2];
        size_t count = writ_c_item_params(&g->c, plan, index, c_params);
        shim_param_t param = {.item = index, .name = item->name, .c_type = c_params[0].type, .parts = count};
        if (writ_c_is_input(item)) {
            if (count == 2) {
                param.kind = PARAM_VALUE;
                param.n00b_type = writ_str_format(g->session, "Ref[%.*s]", S(item_type(g, fn, plan, index)));
            } else if (writ_c_by_pointer(&g->c, item)) {
                param.kind = PARAM_RECORD;
                param.n00b_type = writ_str_format(g->session, "Ref[%.*s]", S(scalar_type(g, item->type)));
            } else if (record_target(item)) {
                param.kind = PARAM_RECORD;
                param.n00b_type = writ_str_format(g->session, "Ref[%.*s]", S(scalar_type(g, record_target(item))));
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
        param.n00b_type = bytes     ? writ_cstr("Ref[Ffi_Bytes]")
                          : address ? writ_cstr("Ref[uint]")
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
shim_symbol(const n00b_t *g, const writ_fn_view_t *fn)
{
    return writ_str_format(g->session, "%.*s_n00b_%.*s", S(g->shim), S(fn->contract_name));
}

static void
emit_c_shim(const n00b_t *g, writ_emit_t *emit, size_t fn, const writ_plan_view_t *plan)
{
    core_list_t(shim_param_t) params = core_list_new(shim_param_t);
    shim_params(g, fn, plan, &params);
    bool direct = writ_c_returns_directly(plan);
    writ_str_t ret = direct ? writ_c_return_type(&g->c, plan) : writ_cstr("int32_t");
    writ_str_t ret_type = is_pointer(ret) ? writ_cstr("uint64_t") : ret;
    writ_emit_text(emit, "%.*s %.*s(", S(ret_type), S(shim_symbol(g, plan->fn)));
    for (size_t i = 0; i < params.len; i++) {
        const shim_param_t *p = &params.data[i];
        writ_str_t type = p->kind == PARAM_SCALAR    ? p->c_type
                          : p->kind == PARAM_ADDRESS ? writ_cstr("uint64_t")
                          : p->kind == PARAM_OUT     ? writ_cstr("void *")
                                                     : writ_cstr("const void *");
        writ_emit_text(emit, "%s%.*s%s%.*s", i ? ", " : "", S(type), is_pointer(type) ? "" : " ", S(p->name));
    }
    writ_emit_text(emit, "%s)", params.len ? "" : "void");
    writ_emit_open(emit, "");
    for (size_t i = 0; i < params.len; i++) {
        const shim_param_t *p = &params.data[i];
        if (p->kind == PARAM_VALUE) {
            writ_emit_line(emit, "size_t %.*s_length = 0;", S(p->name));
            writ_emit_line(emit, "const void *%.*s_data = n00b_ffi_borrow((void *)%.*s, &%.*s_length, 0);",
                           S(p->name), S(p->name), S(p->name));
        }
    }
    writ_emit_text(emit, "%.*s writ_n00b_value = %.*s_%.*s(", S(ret), S(g->c.prefix), S(plan->fn->contract_name));
    bool first = true;
    for (size_t i = 0; i < params.len; i++) {
        const shim_param_t *p = &params.data[i];
        writ_emit_text(emit, "%s", first ? "" : ", ");
        first = false;
        switch (p->kind) {
        case PARAM_VALUE:
            writ_emit_text(emit, "(%.*s)%.*s_data, %.*s_length", S(p->c_type), S(p->name), S(p->name));
            break;
        case PARAM_ADDRESS:
            writ_emit_text(emit, "(%.*s)(uintptr_t)%.*s", S(p->c_type), S(p->name));
            break;
        case PARAM_SCALAR:
            writ_emit_text(emit, "%.*s", S(p->name));
            break;
        default:
            writ_emit_text(emit, "(%.*s)%.*s", S(p->c_type), S(p->name));
            break;
        }
    }
    writ_emit_line(emit, ");");
    for (size_t i = 0; i < params.len; i++) {
        if (params.data[i].kind == PARAM_VALUE) {
            writ_emit_line(emit, "n00b_ffi_release((void *)%.*s);", S(params.data[i].name));
        }
    }
    writ_emit_line(emit, "return %swrit_n00b_value;", is_pointer(ret) ? "(uint64_t)(uintptr_t)" : "");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    core_list_free(params);
}

static writ_str_t
shim_c_text(const n00b_t *g, const writ_plan_view_t *const *plans)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_line(&emit, "#include <stdint.h>");
    writ_emit_line(&emit, "#include <stddef.h>");
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#include \"%.*s_writ.h\"", S(g->c.prefix));
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "void *n00b_ffi_borrow(void *object, size_t *length, uint32_t access);");
    writ_emit_line(&emit, "void n00b_ffi_release(void *object);");
    writ_emit_line(&emit, "");
    for (size_t i = 0; i < writ_model_fn_count(g->model); i++) {
        writ_bound_fn_t bound = writ_binding_fn(g->b, i);
        if (bound.bound && !bound.raw && plans[i]) {
            emit_c_shim(g, &emit, i, plans[i]);
        }
    }
    return writ_emit_finish(&emit, g->session);
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
emit_types(const n00b_t *g, writ_emit_t *emit)
{
    const writ_model_t *model = g->model;
    writ_emit_open(emit, "type Error");
    writ_emit_line(emit, "code: %.*s", S(code_type(g)));
    writ_emit_line(emit, "text: string?");
    writ_emit_line(emit, "fn: string");
    writ_emit_close(emit, nullptr);
    for (size_t i = 0; i < writ_model_handle_count(model); i++) {
        const writ_handle_view_t *handle = writ_model_handle_at(model, i);
        writ_emit_line(emit, "");
        mark(g, emit, handle->declared_at, handle->name);
        writ_emit_open(emit, "type %.*s", S(writ_binding_type(g->b, handle->name)));
        writ_emit_line(emit, "raw: uint");
        writ_emit_close(emit, nullptr);
    }
    for (size_t i = 0; i < writ_model_record_count(model); i++) {
        const writ_record_view_t *record = writ_model_record_at(model, i);
        writ_emit_line(emit, "");
        mark(g, emit, record->declared_at, record->name);
        writ_emit_open(emit, "type %.*s layout(c)", S(writ_binding_type(g->b, record->name)));
        for (size_t f = 0; f < record->fields_count; f++) {
            const writ_slot_view_t *field = &record->fields[f];
            writ_str_t name = writ_profile_name(g->profile, WRIT_NAMING_FIELD, field->name);
            writ_emit_line(emit, "%.*s: %.*s", S(name), S(scalar_type(g, field->type)));
        }
        writ_emit_close(emit, nullptr);
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
    if (writ_model_const_count(model)) {
        writ_emit_line(emit, "");
    }
    for (size_t i = 0; i < writ_model_const_count(model); i++) {
        const writ_const_view_t *constant = writ_model_const_at(model, i);
        writ_str_t name = writ_profile_name(g->profile, WRIT_NAMING_CONST, constant->name);
        mark(g, emit, constant->declared_at, constant->name);
        if (constant->text.length) {
            writ_emit_line(emit, "const %.*s: string = \"%.*s\"", S(name), S(constant->text));
        } else {
            writ_emit_line(emit, "const %.*s: %.*s = %lld", S(name), S(scalar_type(g, constant->type)),
                           (long long)constant->value);
        }
    }
}

static void
emit_extern(const n00b_t *g, writ_emit_t *emit, size_t fn, const writ_plan_view_t *plan)
{
    core_list_t(shim_param_t) params = core_list_new(shim_param_t);
    shim_params(g, fn, plan, &params);
    mark(g, emit, plan->fn->declared_at, plan->fn->contract_name);
    writ_emit_text(emit, "extern<\"%.*s\"> __%.*s(", S(shim_symbol(g, plan->fn)), S(plan->fn->contract_name));
    for (size_t i = 0; i < params.len; i++) {
        writ_emit_text(emit, "%s%.*s: %.*s", i ? ", " : "", S(params.data[i].name), S(params.data[i].n00b_type));
    }
    writ_str_t ret = writ_c_returns_directly(plan) ? writ_c_return_type(&g->c, plan) : writ_cstr("int32_t");
    const writ_plan_item_view_t *direct = nullptr;
    for (size_t i = 0; i < plan->items_count; i++) {
        direct = plan->items[i].role == WRIT_ROLE_RESULT ? &plan->items[i] : direct;
    }
    writ_str_t n00b_ret = !writ_c_returns_directly(plan) ? prim(g, "i%u", 32)
                          : is_pointer(ret)               ? writ_cstr("uint")
                                                          : scalar_type(g, direct ? direct->type : nullptr);
    writ_emit_line(emit, ") -> %.*s", S(n00b_ret));
    core_list_free(params);
}

typedef struct {
    const n00b_t            *g;
    size_t                   fn;
    const writ_plan_view_t  *plan;
    writ_bound_fn_t          bound;
    uint32_t                 receiver;
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
wrapper_name(const wrapper_t *w)
{
    if (w->bound.owner.length && w->bound.is_static) {
        writ_str_t owner = writ_name_case(w->g->session, w->bound.owner, WRIT_CASE_SNAKE);
        return writ_str_format(w->g->session, "%.*s_%.*s", S(owner), S(w->bound.name));
    }
    return w->bound.name;
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
    return !writ_c_returns_directly(w->plan);
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
    if (success_count(w->plan) > 1) {
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
    if (count > 1) {
        writ_emit_line(emit, "");
        writ_emit_open(emit, "type %.*s", S(result_type_name(w)));
        for (size_t i = 0; i < count; i++) {
            writ_emit_line(emit, "%.*s: %.*s", S(local_name(w, outs[i])), S(item_type(w->g, w->fn, w->plan, outs[i])));
        }
        writ_emit_close(emit, nullptr);
    }
}

static void
emit_fail(const wrapper_t *w, writ_emit_t *emit, writ_str_t code, writ_str_t text)
{
    writ_str_t type = error_type(w);
    if (writ_str_eq(type, writ_cstr("Error"))) {
        writ_emit_line(emit, "let error: Error = (code = %.*s, text = %.*s, fn = \"%.*s\")", S(code), S(text),
                       S(w->plan->fn->contract_name));
        writ_emit_line(emit, "fail error");
        return;
    }
    writ_emit_line(emit, "fail %.*s.from_ffi(%.*s, %.*s, \"%.*s\")", S(type), S(code), S(text),
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
lifted(const wrapper_t *w, const shim_param_t *param)
{
    const writ_plan_item_view_t *item = &w->plan->items[param->item];
    writ_str_t name = local_name(w, param->item);
    writ_session_t *session = w->g->session;
    if (ends_with(param->n00b_type, "Ffi_Bytes]")) {
        return writ_str_format(session, item->shape == WRIT_SHAPE_TEXT && !ends_with(item_type(w->g, w->fn, w->plan,
                                                                                               param->item), "]")
                                            ? "__ffi_take_string(ref %.*s)"
                                            : "__ffi_take_bytes(ref %.*s)",
                               S(name));
    }
    if (item->shape == WRIT_SHAPE_HANDLE || (item->type && item->type->kind == WRIT_TYPE_HANDLE)) {
        return writ_str_format(session, "%.*s(raw = %.*s)", S(item_type(w->g, w->fn, w->plan, param->item)), S(name));
    }
    if (item->shape == WRIT_SHAPE_TEXT) {
        return writ_str_format(session, "__ffi_copy_cstr(%.*s)", S(name));
    }
    if (item->shape == WRIT_SHAPE_ARRAY) {
        return writ_str_format(session, "__ffi_take_array[%.*s](%.*s, %.*s_length)",
                               S(scalar_type(w->g, item->type->target)), S(name), S(name));
    }
    return name;
}

static void
emit_wrapper(const n00b_t *g, writ_emit_t *emit, size_t fn, const writ_plan_view_t *plan)
{
    wrapper_t w = {g, fn, plan, writ_binding_fn(g->b, fn), WRIT_PLAN_NONE};
    w.receiver = find_receiver(&w);
    uint32_t *outs = writ_alloc(g->session, sizeof(uint32_t) * (plan->items_count + 1));
    size_t out_count = results(&w, outs);
    emit_result_types(&w, emit, outs, out_count);
    writ_emit_line(emit, "");
    mark(g, emit, plan->fn->declared_at, plan->fn->contract_name);
    writ_emit_text(emit, "func %.*s(", S(wrapper_name(&w)));
    bool first = true;
    for (uint32_t i = 0; i < plan->items_count; i++) {
        if (!writ_c_is_input(&plan->items[i])) {
            continue;
        }
        bool modify = (i == w.receiver && consumes(&w, i))
                   || (plan->items[i].role == WRIT_ROLE_INPUT_OUTPUT && record_target(&plan->items[i]));
        writ_emit_text(emit, "%s%s%.*s: %.*s", first ? "" : ", ", modify ? "modify " : "", S(local_name(&w, i)),
                       S(receiver_type(&w, i)));
        first = false;
    }
    writ_emit_text(emit, ")");
    bool multiple_success = success_count(plan) > 1;
    if (multiple_success) {
        writ_emit_text(emit, " -> %.*s", S(outcome_type_name(&w)));
    } else if (out_count == 1) {
        writ_emit_text(emit, " -> %.*s", S(item_type(g, fn, plan, outs[0])));
    } else if (out_count > 1) {
        writ_emit_text(emit, " -> %.*s", S(result_type_name(&w)));
    }
    if (can_fail(&w)) {
        writ_emit_text(emit, " fail: %.*s", S(error_type(&w)));
    }
    writ_emit_open(emit, "");
    core_list_t(shim_param_t) params = core_list_new(shim_param_t);
    shim_params(g, fn, plan, &params);
    for (size_t i = 0; i < params.len; i++) {
        const shim_param_t *p = &params.data[i];
        if (p->kind == PARAM_VALUE && p->item != w.receiver) {
            writ_emit_line(emit, "var %.*s_value = %.*s", S(local_name(&w, p->item)), S(local_name(&w, p->item)));
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
    writ_emit_text(emit, "%s__%.*s(", can_fail(&w) ? "let outcome = " : out_count ? "let result = " : "",
                   S(plan->fn->contract_name));
    for (size_t i = 0; i < params.len; i++) {
        const shim_param_t *p = &params.data[i];
        writ_str_t name = local_name(&w, p->item);
        writ_emit_text(emit, "%s", i ? ", " : "");
        if (p->kind == PARAM_VALUE) {
            writ_emit_text(emit, p->item == w.receiver ? "ref %.*s" : "ref %.*s_value", S(name));
        } else if (p->kind == PARAM_ADDRESS && (is_handle(&plan->items[p->item]) || p->item == w.receiver)) {
            writ_emit_text(emit, "%.*s.raw", S(name));
        } else if (p->kind == PARAM_RECORD) {
            writ_emit_text(emit, "ref %.*s", S(name));
        } else if (p->kind == PARAM_OUT) {
            bool length = !writ_str_eq(p->name, plan->items[p->item].name);
            writ_emit_text(emit, length ? "ref %.*s_length" : "ref %.*s", S(name));
        } else {
            writ_emit_text(emit, "%.*s", S(name));
        }
    }
    writ_emit_line(emit, ")");
    if (can_fail(&w)) {
        writ_emit_open(emit, "if outcome < 0");
        emit_fail(&w, emit, writ_str_format(g->session, "cast[%.*s]outcome", S(code_type(g))), writ_cstr("nil"));
        writ_emit_close(emit, nullptr);
        for (size_t o = 0; o < plan->outcomes_count; o++) {
            if (plan->outcomes[o].kind == WRIT_OUTCOME_FAILURE) {
                writ_emit_open(emit, "if outcome == %zu", o);
                emit_fail(&w, emit, detail_text(&w, (uint32_t)o, false), detail_text(&w, (uint32_t)o, true));
                writ_emit_close(emit, nullptr);
            }
        }
    }
    if (w.receiver != WRIT_PLAN_NONE && consumes(&w, w.receiver)) {
        writ_emit_line(emit, "self.raw = 0");
    }
    if (multiple_success) {
        writ_emit_line(emit, "return cast[%.*s]outcome", S(outcome_type_name(&w)));
    } else if (!can_fail(&w) && out_count) {
        writ_emit_line(emit, "return result");
    } else if (out_count == 1) {
        for (size_t i = 0; i < params.len; i++) {
            if (params.data[i].kind == PARAM_OUT && params.data[i].item == outs[0]
                && writ_str_eq(params.data[i].name, plan->items[outs[0]].name)) {
                writ_emit_line(emit, "return %.*s", S(lifted(&w, &params.data[i])));
            }
        }
    } else if (out_count > 1) {
        writ_emit_text(emit, "let values: %.*s = (", S(result_type_name(&w)));
        for (size_t r = 0; r < out_count; r++) {
            for (size_t i = 0; i < params.len; i++) {
                if (params.data[i].kind == PARAM_OUT && params.data[i].item == outs[r]
                    && writ_str_eq(params.data[i].name, plan->items[outs[r]].name)) {
                    writ_emit_text(emit, "%s%.*s = %.*s", r ? ", " : "", S(local_name(&w, outs[r])),
                                   S(lifted(&w, &params.data[i])));
                }
            }
        }
        writ_emit_line(emit, ")");
        writ_emit_line(emit, "return values");
    }
    writ_emit_close(emit, nullptr);
    core_list_free(params);
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
    mark(g, emit, fn->declared_at, fn->contract_name);
    writ_emit_text(emit, "extern<\"%.*s\"> %.*s_raw(", S(fn->link_symbol), S(bound.name));
    for (size_t s = 0; s < fn->slots_count; s++) {
        writ_str_t name = writ_profile_name(g->profile, WRIT_NAMING_FIELD, fn->slots[s].name);
        writ_emit_text(emit, "%s%.*s: %.*s", s ? ", " : "", S(name), S(raw_type(g, fn->slots[s].type)));
    }
    writ_emit_line(emit, ") -> %.*s", S(raw_type(g, fn->result ? fn->result->type : nullptr)));
}

static writ_str_t
n00b_text(const n00b_t *g, const writ_plan_view_t *const *plans)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    emit_types(g, &emit);
    size_t count = writ_model_fn_count(g->model);
    writ_emit_line(&emit, "");
    for (size_t i = 0; i < count; i++) {
        writ_bound_fn_t bound = writ_binding_fn(g->b, i);
        if (bound.bound && bound.raw) {
            emit_raw(g, &emit, writ_model_fn_at(g->model, i), bound);
        } else if (bound.bound) {
            emit_extern(g, &emit, i, plans[i]);
        }
    }
    for (size_t i = 0; i < count; i++) {
        writ_bound_fn_t bound = writ_binding_fn(g->b, i);
        if (bound.bound && !bound.raw) {
            emit_wrapper(g, &emit, i, plans[i]);
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

static writ_str_t
export_c_text(const n00b_t *g)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_line(&emit, "#include <stdint.h>");
    writ_emit_line(&emit, "#include <stddef.h>");
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#include \"%.*s\"", S(writ_binding_header(g->b)));
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "int32_t n00b_ffi_enter(void);");
    writ_emit_line(&emit, "void n00b_ffi_leave(int32_t depth);");
    writ_emit_line(&emit, "int32_t n00b_ffi_failed(void);");
    writ_emit_line(&emit, "");
    writ_str_t panic = writ_binding_panic_value(g->b);
    for (size_t i = 0; i < writ_model_fn_count(g->model); i++) {
        const writ_fn_view_t *fn = writ_model_fn_at(g->model, i);
        writ_bound_fn_t bound = writ_binding_fn(g->b, i);
        if (!bound.provide.length) {
            continue;
        }
        writ_str_t target = provider(g, bound.provide);
        writ_c_emit_real_prototype(&g->c, &emit, fn, target, false, ";");
        writ_c_emit_real_prototype(&g->c, &emit, fn, fn->link_symbol, false, "");
        writ_emit_open(&emit, "");
        writ_emit_line(&emit, "int32_t depth = n00b_ffi_enter();");
        bool has_result = fn->result != nullptr;
        if (has_result) {
            writ_str_t type = writ_c_type(&g->c, fn->result->type,
                                          writ_c_is_text(fn->result->type) && !fn->result->own);
            writ_emit_text(&emit, "%.*s%sresult = ", S(type), is_pointer(type) ? "" : " ");
        }
        writ_emit_text(&emit, "%.*s(", S(target));
        for (size_t s = 0; s < fn->slots_count; s++) {
            writ_emit_text(&emit, "%s%.*s", s ? ", " : "", S(fn->slots[s].name));
        }
        writ_emit_line(&emit, ");");
        writ_emit_line(&emit, "int32_t failed = n00b_ffi_failed();");
        writ_emit_line(&emit, "n00b_ffi_leave(depth);");
        if (has_result) {
            writ_type_kind_t kind = fn->result->type->kind;
            bool integral = kind == WRIT_TYPE_INT || kind == WRIT_TYPE_ENUM || kind == WRIT_TYPE_BOOL;
            bool pointer = kind == WRIT_TYPE_POINTER || kind == WRIT_TYPE_HANDLE;
            writ_emit_open(&emit, "if (failed)");
            if (panic.length && integral) {
                writ_emit_line(&emit, "return %.*s;", S(panic));
            } else if (pointer) {
                writ_emit_line(&emit, "return NULL;");
            } else {
                writ_emit_line(&emit, "__builtin_trap();");
            }
            writ_emit_close(&emit, nullptr);
            writ_emit_line(&emit, "return result;");
        } else {
            writ_emit_open(&emit, "if (failed)");
            writ_emit_line(&emit, "__builtin_trap();");
            writ_emit_close(&emit, nullptr);
        }
        writ_emit_close(&emit, nullptr);
        writ_emit_line(&emit, "");
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
    for (size_t i = 0; i < writ_model_record_count(model); i++) {
        const writ_record_view_t *record = writ_model_record_at(model, i);
        writ_emit_open(&emit, "typedef struct %.*s", S(record->name));
        for (size_t f = 0; f < record->fields_count; f++) {
            writ_str_t type = writ_c_type(&g->c, record->fields[f].type, false);
            writ_emit_line(&emit, "%.*s%s%.*s;", S(type), is_pointer(type) ? "" : " ", S(record->fields[f].name));
        }
        writ_emit_close(&emit, writ_str_format(g->session, " %.*s;", S(record->name)).data);
    }
    writ_emit_line(&emit, "");
    for (size_t i = 0; i < writ_model_fn_count(model); i++) {
        const writ_fn_view_t *fn = writ_model_fn_at(model, i);
        if (writ_binding_fn(g->b, i).provide.length) {
            writ_c_emit_real_prototype(&g->c, &emit, fn, fn->link_symbol, false, ";");
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

writ_status_t
writ_gen_n00b(const writ_gen_n00b_request_t *request, const writ_output_t *output, writ_link_requirements_t *links)
{
    if (!request || !request->binding || !output || !output->write_file || !links) {
        return WRIT_INVALID_ARGUMENT;
    }
    const writ_binding_t *b = request->binding;
    const writ_model_t *model = writ_binding_model(b);
    writ_session_t *session = model->session;
    writ_str_t prefix = request->shim_prefix.length ? request->shim_prefix : writ_model_library(model)->name;
    writ_str_t module = request->module_name.length ? request->module_name : writ_model_library(model)->name;
    n00b_t g = {.b = b, .model = model, .profile = writ_binding_profile(b), .session = session, .shim = prefix,
                .c = {model, prefix, writ_name_case(session, prefix, WRIT_CASE_UPPER), true},
                .map = core_buffer_empty()};
    writ_on_session_free(session, map_release, g.map);
    size_t count = writ_model_fn_count(model);
    const writ_plan_view_t **plans = writ_alloc(session, sizeof(writ_plan_view_t *) * (count ? count : 1));
    for (size_t i = 0; i < count; i++) {
        plans[i] = writ_model_fn_plan(model, writ_model_fn_at(model, i));
        writ_bound_fn_t bound = writ_binding_fn(b, i);
        if (!plans[i] && bound.bound && !bound.raw) {
            return WRIT_HAS_ERRORS;
        }
    }
    links_for(&g, links);
    if (writ_binding_exports(b)) {
        writ_status_t status = output->write_file(output->context, writ_binding_header(b), export_header_text(&g));
        if (status == WRIT_OK) {
            status = output->write_file(output->context, writ_str_format(session, "%.*s_n00b_export.c", S(prefix)),
                                        export_c_text(&g));
        }
        return status;
    }
    writ_gen_c_request_t c_request = {.struct_size = sizeof(c_request), .prefix = prefix, .pointer_normalized = true};
    writ_status_t status = writ_gen_c(model, &c_request, output);
    if (status == WRIT_OK) {
        status = output->write_file(output->context, writ_str_format(session, "%.*s_n00b.c", S(prefix)),
                                    shim_c_text(&g, plans));
    }
    if (status == WRIT_OK) {
        status = output->write_file(output->context, writ_str_format(session, "%.*s.n", S(module)),
                                    n00b_text(&g, plans));
    }
    if (status == WRIT_OK) {
        status = output->write_file(output->context, writ_str_format(session, "%.*s.n.map", S(module)),
                                    writ_str_copy(session, g.map->data ? g.map->data : "", g.map->byte_len));
    }
    return status;
}
