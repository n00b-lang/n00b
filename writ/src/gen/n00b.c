#include "gen.h"

core_list_decl(writ_n00b_extern_t);

enum { N00B_FFI_WRITE = 1, N00B_FFI_STRING = 2 };

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
add_extern(const n00b_t *g, writ_str_t name, writ_str_t symbol, const writ_str_t *params, size_t count,
           writ_str_t result)
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
                                }));
    core_buffer_release(&signature);
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
} shim_param_t;

core_list_decl(shim_param_t);

static const writ_type_view_t *record_target(const writ_plan_item_view_t *item);
static bool expressible(const writ_record_view_t *record);

static bool
opaque_record(const n00b_t *g, const writ_type_view_t *type)
{
    const writ_record_view_t *record = type ? writ_model_record_named(g->model, type->name) : nullptr;
    return record && !expressible(record);
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
    case WRIT_SHAPE_CALLBACK:
        return writ_binding_type(g->b, item->type->name);
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
            param.kind = owned ? PARAM_OWNED : PARAM_CALLBACK;
            param.c_type = writ_cstr("uint64_t");
            param.n00b_type = writ_cstr("u64");
            core_list_push(*params, param);
            continue;
        }
        if (writ_c_is_input(item)) {
            if (count == 2) {
                param.kind = PARAM_VALUE;
                param.n00b_type = writ_str_format(g->session, "Ref[%.*s]", S(item_type(g, fn, plan, index)));
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

static const writ_plan_view_t *
bound_plan(const n00b_t *g, size_t fn, size_t k)
{
    writ_bound_fn_t bound = writ_binding_fn(g->b, fn);
    const writ_fn_view_t *view = writ_model_fn_at(g->model, fn);
    if (bound.policy_arm != WRIT_PLAN_NONE) {
        return k ? nullptr : writ_model_fn_arm_plan(g->model, view, bound.policy_arm);
    }
    return writ_model_fn_arm_plan(g->model, view, k);
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

static const shim_param_t *
handle_param(const writ_plan_view_t *plan, const core_list_t(shim_param_t) *params)
{
    for (size_t i = 0; i < params->len; i++) {
        if (params->data[i].kind == PARAM_ADDRESS && is_handle(&plan->items[params->data[i].item])) {
            return &params->data[i];
        }
    }
    return nullptr;
}

static bool
keeps_until(const writ_plan_view_t *plan, const shim_param_t *param)
{
    return param->kind == PARAM_VALUE && plan->items[param->item].after_call == WRIT_AFTER_BORROWED_UNTIL;
}

static bool
releases_handle(const writ_plan_view_t *plan, const shim_param_t *param)
{
    return param->kind == PARAM_ADDRESS && is_handle(&plan->items[param->item])
        && plan->items[param->item].after_call == WRIT_AFTER_CALLEE_OWNS;
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
        writ_bound_fn_t bound = writ_binding_fn(g->b, i);
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
    return keeps_until(plan, param) && handle_param(plan, params);
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
emit_keep_table(const n00b_t *g, writ_emit_t *emit)
{
    writ_str_t s = g->shim;
    writ_emit_line(emit, "typedef struct {");
    writ_emit_line(emit, "    uint64_t owner;");
    writ_emit_line(emit, "    void *data;");
    writ_emit_line(emit, "} %.*s_kept_t;", S(s));
    writ_emit_line(emit, "");
    writ_emit_line(emit, "static %.*s_kept_t *%.*s_kept;", S(s), S(s));
    writ_emit_line(emit, "static size_t %.*s_kept_count;", S(s));
    writ_emit_line(emit, "static size_t %.*s_kept_capacity;", S(s));
    writ_emit_line(emit, "static atomic_flag %.*s_kept_lock = ATOMIC_FLAG_INIT;", S(s));
    writ_emit_line(emit, "");
    writ_emit_open(emit, "static void %.*s_keep(uint64_t owner, void *data)", S(s));
    writ_emit_line(emit, "while (atomic_flag_test_and_set_explicit(&%.*s_kept_lock, memory_order_acquire)) {", S(s));
    writ_emit_line(emit, "}");
    writ_emit_open(emit, "if (%.*s_kept_count == %.*s_kept_capacity)", S(s), S(s));
    writ_emit_line(emit, "size_t capacity = %.*s_kept_capacity ? %.*s_kept_capacity * 2 : 8;", S(s), S(s));
    writ_emit_line(emit, "%.*s_kept_t *grown = realloc(%.*s_kept, capacity * sizeof *grown);", S(s), S(s));
    writ_emit_open(emit, "if (!grown)");
    writ_emit_line(emit, "atomic_flag_clear_explicit(&%.*s_kept_lock, memory_order_release);", S(s));
    writ_emit_line(emit, "n00bb_runtime_trap(\"out of memory\", 13);");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "%.*s_kept = grown;", S(s));
    writ_emit_line(emit, "%.*s_kept_capacity = capacity;", S(s));
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "%.*s_kept[%.*s_kept_count++] = (%.*s_kept_t){owner, data};", S(s), S(s), S(s));
    writ_emit_line(emit, "atomic_flag_clear_explicit(&%.*s_kept_lock, memory_order_release);", S(s));
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_emit_open(emit, "static void %.*s_drop(uint64_t owner)", S(s));
    writ_emit_line(emit, "while (atomic_flag_test_and_set_explicit(&%.*s_kept_lock, memory_order_acquire)) {", S(s));
    writ_emit_line(emit, "}");
    writ_emit_open(emit, "for (size_t i = 0; i < %.*s_kept_count;)", S(s));
    writ_emit_open(emit, "if (%.*s_kept[i].owner == owner)", S(s));
    writ_emit_line(emit, "free(%.*s_kept[i].data);", S(s));
    writ_emit_line(emit, "%.*s_kept[i] = %.*s_kept[--%.*s_kept_count];", S(s), S(s), S(s));
    writ_emit_close(emit, nullptr);
    writ_emit_open(emit, "else");
    writ_emit_line(emit, "i++;");
    writ_emit_close(emit, nullptr);
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "atomic_flag_clear_explicit(&%.*s_kept_lock, memory_order_release);", S(s));
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
    writ_emit_text(emit, "%.*s %.*s(", S(ret_type), S(shim_symbol(g, plan)));
    bool any = false;
    for (size_t i = 0; i < params.len; i++) {
        const shim_param_t *p = &params.data[i];
        if (p->kind == PARAM_OWNED) {
            continue;
        }
        writ_str_t type = p->kind == PARAM_SCALAR || p->kind == PARAM_CALLBACK ? p->c_type
                          : p->kind == PARAM_ADDRESS                            ? writ_cstr("uint64_t")
                          : p->kind == PARAM_OUT     ? writ_cstr("void *")
                                                     : writ_cstr("const void *");
        writ_emit_text(emit, "%s%.*s%s%.*s", any ? ", " : "", S(type), is_pointer(type) ? "" : " ", S(p->name));
        any = true;
    }
    writ_emit_text(emit, "%s)", any ? "" : "void");
    writ_emit_open(emit, "");
    size_t copies = 0;
    for (size_t i = 0; i < params.len; i++) {
        const shim_param_t *p = &params.data[i];
        if (p->kind == PARAM_STORAGE || (p->kind == PARAM_VALUE && p->borrow)) {
            writ_emit_line(emit, "size_t %.*s_length = 0;", S(p->name));
            writ_emit_line(emit, "const void *%.*s_data = n00b_ffi_borrow((void *)%.*s, &%.*s_length, %d);",
                           S(p->name), S(p->name), S(p->name),
                           writ_str_eq(p->n00b_type, writ_cstr("Ref[string]")) ? N00B_FFI_STRING : 0);
        } else if (p->kind == PARAM_VALUE) {
            writ_emit_line(emit, "size_t %.*s_length = 0;", S(p->name));
            writ_emit_line(emit, "const void *%.*s_borrowed = n00b_ffi_borrow((void *)%.*s, &%.*s_length, %d);",
                           S(p->name), S(p->name), S(p->name),
                           writ_str_eq(p->n00b_type, writ_cstr("Ref[string]")) ? N00B_FFI_STRING : 0);
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
        case PARAM_OWNED:
            writ_emit_text(emit, "%.*s_free", S(shim_symbol(g, plan)));
            break;
        default:
            writ_emit_text(emit, "(%.*s)%.*s", S(p->c_type), S(p->name));
            break;
        }
    }
    writ_emit_line(emit, ");");
    const shim_param_t *owner = handle_param(plan, &params);
    bool keeps = shim_keeps(g);
    for (size_t i = 0; i < params.len; i++) {
        writ_after_call_t after = plan->items[params.data[i].item].after_call;
        bool handed_over = (owned_arm(plan) && after == WRIT_AFTER_CALLEE_OWNS) || after == WRIT_AFTER_BORROWED_UNTIL;
        if (keeps_until(plan, &params.data[i]) && owner) {
            writ_emit_line(emit, "%.*s_keep(%.*s, %.*s_data);", S(g->shim), S(owner->name), S(params.data[i].name));
        } else if (params.data[i].kind == PARAM_VALUE && !params.data[i].borrow && !handed_over) {
            writ_emit_line(emit, "free(%.*s_data);", S(params.data[i].name));
        } else if (params.data[i].kind == PARAM_STORAGE || params.data[i].borrow) {
            writ_emit_line(emit, "n00b_ffi_release((void *)%.*s);", S(params.data[i].name));
        }
        if (keeps && releases_handle(plan, &params.data[i])) {
            writ_emit_line(emit, "%.*s_drop(%.*s);", S(g->shim), S(params.data[i].name));
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
    writ_emit_line(emit, "n00b_ffi_dispatch((uint64_t)(uintptr_t)%.*s, %s, %s);", S(callback->slots[receiver].name),
                   packed ? "&arguments" : "NULL", returns ? "&result" : "NULL");
    if (returns) {
        writ_emit_line(emit, "return result;");
    }
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
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
        writ_bound_fn_t bound = writ_binding_fn(g->b, i);
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

static writ_str_t
shim_c_text(const n00b_t *g, const writ_plan_view_t *const *plans)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_line(&emit, "#include <stdint.h>");
    writ_emit_line(&emit, "#include <stddef.h>");
    writ_emit_line(&emit, "#include <stdlib.h>");
    writ_emit_line(&emit, "#include <string.h>");
    bool keeps = shim_keeps(g);
    if (keeps) {
        writ_emit_line(&emit, "#include <stdatomic.h>");
    }
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#include \"%.*s_writ.h\"", S(g->c.prefix));
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "void *n00b_ffi_borrow(void *object, size_t *length, uint32_t access);");
    writ_emit_line(&emit, "void n00b_ffi_release(void *object);");
    writ_emit_line(&emit, "void n00bb_runtime_trap(const char *reason, uint64_t length);");
    writ_emit_line(&emit, "");
    if (keeps) {
        emit_keep_table(g, &emit);
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
    if (copies_cstr(g)) {
        writ_emit_open(&emit, "uint64_t %.*s_cstr_length(uint64_t address)", S(g->shim));
        writ_emit_line(&emit, "return address ? strlen((const char *)(uintptr_t)address) : 0;");
        writ_emit_close(&emit, nullptr);
        writ_emit_line(&emit, "");
        writ_emit_open(&emit, "void %.*s_cstr_copy(uint64_t address, void *out)", S(g->shim));
        writ_emit_line(&emit, "size_t length = 0;");
        writ_emit_line(&emit, "uint8_t *data = n00b_ffi_borrow(out, &length, %d);", N00B_FFI_WRITE);
        writ_emit_open(&emit, "if (data && address && length)");
        writ_emit_line(&emit, "memcpy(data, (const void *)(uintptr_t)address, length);");
        writ_emit_close(&emit, nullptr);
        writ_emit_line(&emit, "n00b_ffi_release(out);");
        writ_emit_close(&emit, nullptr);
        writ_emit_line(&emit, "");
    }
    bool dispatches = false;
    for (size_t i = 0; i < writ_model_callback_count(g->model); i++) {
        dispatches = dispatches || userdata_receiver(writ_model_callback_at(g->model, i)) != WRIT_PLAN_NONE;
    }
    if (dispatches) {
        writ_emit_line(&emit, "int32_t n00b_ffi_dispatch(uint64_t handle, const void *arguments, void *result);");
        writ_emit_line(&emit, "");
    }
    for (size_t i = 0; i < writ_model_callback_count(g->model); i++) {
        emit_trampoline(g, &emit, writ_model_callback_at(g->model, i));
    }
    for (size_t i = 0; i < writ_model_fn_count(g->model); i++) {
        writ_bound_fn_t bound = writ_binding_fn(g->b, i);
        for (size_t k = 0; bound.bound && !bound.raw && plans[i] && bound_plan(g, i, k); k++) {
            emit_c_shim(g, &emit, i, bound_plan(g, i, k));
        }
    }
    return writ_emit_finish(&emit, g->session);
}

static bool
expressible_field(const writ_type_view_t *type)
{
    switch (type ? type->kind : WRIT_TYPE_VOID) {
    case WRIT_TYPE_BOOL:
    case WRIT_TYPE_INT:
    case WRIT_TYPE_POINTER:
    case WRIT_TYPE_ARRAY:
    case WRIT_TYPE_CALLBACK:
    case WRIT_TYPE_ENUM:
    case WRIT_TYPE_HANDLE:
    case WRIT_TYPE_RECORD:
    case WRIT_TYPE_CSTR:
        return true;
    case WRIT_TYPE_FLOAT:
        return type->size == 8;
    case WRIT_TYPE_VIEW:
        return expressible_field(type->target);
    default:
        return false;
    }
}

static bool
expressible(const writ_record_view_t *record)
{
    for (size_t f = 0; f < record->fields_count; f++) {
        if (!expressible_field(record->fields[f].type)) {
            return false;
        }
    }
    return true;
}

static void
emit_opaque_record(const n00b_t *g, writ_emit_t *emit, const writ_record_view_t *record)
{
    writ_str_t type = writ_binding_type(g->b, record->name);
    writ_emit_open(emit, "type %.*s", S(type));
    writ_emit_line(emit, "storage: %.*s", S(buffer_type(g)));
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
    if (callback->slots_count < 2) {
        return;
    }
    writ_emit_line(emit, "");
    writ_emit_open(emit, "type %.*s_Arguments layout(c)", S(name));
    for (size_t i = 0; i < callback->slots_count; i++) {
        if (i != receiver) {
            writ_emit_line(emit, "public %.*s: %.*s",
                           S(writ_profile_name(g->profile, WRIT_NAMING_FIELD, callback->slots[i].name)),
                           S(scalar_type(g, callback->slots[i].type)));
        }
    }
    writ_emit_close(emit, nullptr);
}

static writ_str_t
cell_constructor(const n00b_t *g, writ_str_t type)
{
    return writ_str_format(g->session, "__%.*s_cell", S(writ_name_case(g->session, type, WRIT_CASE_SNAKE)));
}

static const writ_plan_view_t *
release_plan(const n00b_t *g, const writ_handle_view_t *handle)
{
    for (size_t i = 0; i < writ_model_fn_count(g->model); i++) {
        writ_bound_fn_t bound = writ_binding_fn(g->b, i);
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
        if (!writ_binding_fn(g->b, i).bound || !source || !writ_str_eq(fn->result->type->name, handle->name)) {
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
                      const writ_str_t *fields, size_t count, size_t kept)
{
    writ_emit_open(emit, "func %.*s(raw: uint%.*s) -> %.*s", S(name), S(keep_param), S(type));
    writ_emit_text(emit, "var cell: %.*s_Cell = (raw = raw", S(type));
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
    writ_emit_open(emit, "type %.*s_Cell", S(type));
    writ_emit_line(emit, "public raw: uint");
    for (size_t s = 0; s < source_count; s++) {
        fields[s] = keep_field(g, sources[s]);
        writ_emit_line(emit, "public %.*s: %.*s?", S(fields[s]), S(sources[s]));
    }
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_emit_line(emit, "alias %.*s = Ref[%.*s_Cell]", S(type), S(type));
    writ_emit_line(emit, "");
    emit_cell_constructor(emit, type, cell_constructor(g, type), writ_cstr(""), fields, source_count, source_count);
    for (size_t s = 0; s < source_count; s++) {
        writ_emit_line(emit, "");
        emit_cell_constructor(emit, type, with_constructor(g, type, sources[s]),
                              writ_str_format(g->session, ", keep: %.*s", S(sources[s])), fields, source_count, s);
    }
    const writ_plan_view_t *release = release_plan(g, handle);
    if (!release) {
        return;
    }
    writ_emit_line(emit, "");
    writ_emit_open(emit, "func __finalize(consume self: Ref[%.*s_Cell])", S(type));
    writ_emit_open(emit, "if self.raw != 0 and debug_enabled()");
    writ_emit_line(emit, "__fd_write(2, \"%.*s collected while open; releasing it\\n\")", S(type));
    writ_emit_line(emit, "let released = __%.*s(self.raw)", S(writ_c_plan_name(&g->c, release)));
    writ_emit_close(emit, nullptr);
    writ_emit_close(emit, nullptr);
}

static void
emit_types(const n00b_t *g, writ_emit_t *emit)
{
    const writ_model_t *model = g->model;
    writ_emit_open(emit, "type Error");
    writ_emit_line(emit, "public code: %.*s", S(code_type(g)));
    writ_emit_line(emit, "public text: string?");
    writ_emit_line(emit, "public fn: string");
    writ_emit_close(emit, nullptr);
    for (size_t i = 0; i < writ_model_handle_count(model); i++) {
        emit_handle_type(g, emit, writ_model_handle_at(model, i));
    }
    for (size_t i = 0; i < writ_model_record_count(model); i++) {
        const writ_record_view_t *record = writ_model_record_at(model, i);
        writ_emit_line(emit, "");
        mark(g, emit, record->declared_at, record->name);
        if (!expressible(record)) {
            emit_opaque_record(g, emit, record);
            continue;
        }
        writ_emit_open(emit, "type %.*s layout(c)", S(writ_binding_type(g->b, record->name)));
        for (size_t f = 0; f < record->fields_count; f++) {
            const writ_slot_view_t *field = &record->fields[f];
            writ_str_t name = writ_profile_name(g->profile, WRIT_NAMING_FIELD, field->name);
            writ_emit_line(emit, "public %.*s: %.*s", S(name), S(scalar_type(g, field->type)));
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
    writ_str_t *types = writ_alloc(g->session, sizeof(writ_str_t) * (params.len ? params.len : 1));
    size_t typed = 0;
    for (size_t i = 0; i < params.len; i++) {
        if (params.data[i].kind != PARAM_OWNED) {
            types[typed++] = params.data[i].n00b_type;
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
    add_extern(g, writ_str_format(g->session, "__%.*s", S(writ_c_plan_name(&g->c, plan))), shim_symbol(g, plan), types,
               typed, n00b_ret);
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
    bool multiple_success = success_count(w->plan) > 1;
    if (count > 1 || (count && multiple_success)) {
        writ_emit_line(emit, "");
        writ_emit_open(emit, "type %.*s", S(result_type_name(w)));
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
        return writ_str_format(session,
                               text_result(w->g, w->fn, w->plan, param) ? "%.*s_text" : "__ffi_take_bytes(ref %.*s)",
                               S(name));
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
emit_closed_checks(const wrapper_t *w, writ_emit_t *emit, const core_list_t(shim_param_t) *params)
{
    for (size_t i = 0; i < params->len; i++) {
        const shim_param_t *p = &params->data[i];
        const writ_plan_item_view_t *item = &w->plan->items[p->item];
        if (p->kind != PARAM_ADDRESS || !is_handle(item) || (item->type && item->type->nullable)) {
            continue;
        }
        writ_emit_open(emit, "if %.*s.raw == 0", S(local_name(w, p->item)));
        if (can_fail(w)) {
            emit_fail(w, emit, writ_str_format(w->g->session, "%d", WRIT_C_ARGUMENT_ERROR),
                      writ_cstr("\"handle is closed\""));
        } else {
            writ_emit_line(emit, "panic(\"%.*s: handle is closed\")", S(w->plan->fn->contract_name));
        }
        writ_emit_close(emit, nullptr);
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
    writ_emit_open(emit, "type %.*s", S(options_type_name(w)));
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
exposed_input(const writ_plan_view_t *plan, uint32_t item)
{
    return writ_c_is_input(&plan->items[item]) && !policy_item(plan, item) && !retry_capacity(plan, item);
}

static void
emit_wrapper_params(const wrapper_t *w, writ_emit_t *emit, uint32_t keywords, bool options_record, bool *first)
{
    const writ_plan_view_t *plan = w->plan;
    for (uint32_t i = 0; i < plan->items_count; i++) {
        if (!exposed_input(plan, i) || (i == keywords && !options_record)) {
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
        if (exposed_input(plan, i) && (i != keywords || options_record)) {
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

static void
emit_wrapper(const n00b_t *g, writ_emit_t *emit, size_t fn, const writ_plan_view_t *plan)
{
    wrapper_t w = {.g = g, .fn = fn, .plan = plan, .bound = writ_binding_fn(g->b, fn), .receiver = WRIT_PLAN_NONE};
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
    bool multiple_success = success_count(plan) > 1;
    if (multiple_success && out_count) {
        writ_emit_text(emit, " -> %.*s", S(result_type_name(&w)));
    } else if (multiple_success) {
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
    if (keyword_view) {
        emit_keyword_record(&w, emit, keywords, keyword_view);
    }
    for (size_t i = 0; i < params.len; i++) {
        const shim_param_t *p = &params.data[i];
        if (p->kind == PARAM_VALUE && p->item != w.receiver) {
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
    writ_emit_text(emit, "%s__%.*s(", can_fail(&w) ? "let outcome = " : out_count ? "let result = " : "",
                   S(writ_c_plan_name(&g->c, plan)));
    bool passed = false;
    for (size_t i = 0; i < params.len; i++) {
        const shim_param_t *p = &params.data[i];
        writ_str_t name = local_name(&w, p->item);
        if (p->kind == PARAM_OWNED) {
            continue;
        }
        writ_emit_text(emit, "%s", passed ? ", " : "");
        passed = true;
        if (p->kind == PARAM_VALUE) {
            writ_emit_text(emit, p->item == w.receiver ? "ref %.*s" : "ref %.*s_value", S(name));
        } else if (p->kind == PARAM_ADDRESS && (is_handle(&plan->items[p->item]) || p->item == w.receiver)) {
            writ_emit_text(emit, "%.*s.raw", S(name));
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
            writ_emit_text(emit, "__ffi_callback_register(%.*s)", S(name));
        } else if (retry_capacity(plan, p->item)) {
            writ_emit_text(emit, "%u", writ_default(WRIT_DEFAULT_RETRY_CAPACITY));
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
    add_extern(g, writ_str_format(g->session, "__%.*s_raw", S(bound.name)), fn->link_symbol, types, fn->slots_count,
               result);
    writ_emit_line(emit, "");
    mark(g, emit, fn->declared_at, fn->contract_name);
    writ_emit_text(emit, "public func %.*s_raw(", S(bound.name));
    for (size_t s = 0; s < fn->slots_count; s++) {
        writ_str_t name = writ_profile_name(g->profile, WRIT_NAMING_FIELD, fn->slots[s].name);
        writ_emit_text(emit, "%s%.*s: %.*s", s ? ", " : "", S(name), S(types[s]));
    }
    writ_emit_line(emit, ") -> %.*s {", S(result));
    writ_emit_text(emit, "    return __%.*s_raw(", S(bound.name));
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
    wrapper_t w = {.g = g, .fn = fn, .plan = plan, .bound = writ_binding_fn(g->b, fn), .receiver = WRIT_PLAN_NONE};
    w.receiver = find_receiver(&w);
    uint32_t *outs = writ_alloc(g->session, sizeof(uint32_t) * (plan->items_count + 1));
    size_t out_count = results(&w, outs);
    if (out_count > 1 || success_count(plan) > 1) {
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
    emit_types(g, &emit);
    size_t count = writ_model_fn_count(g->model);
    for (size_t i = 0; i < count; i++) {
        writ_bound_fn_t bound = writ_binding_fn(g->b, i);
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
    if (takes_bytes) {
        writ_str_t take_params[] = {writ_cstr("Ref[Ffi_Bytes]"), writ_cstr("Ref[Array[u8]]")};
        add_extern(g, writ_cstr("__ffi_bytes_take"), writ_str_format(g->session, "%.*s_take_bytes", S(g->shim)),
                   take_params, 2, writ_cstr("nil"));
        writ_emit_line(&emit, "");
        writ_emit_line(&emit, "func __ffi_take_bytes(bytes: Ref[Ffi_Bytes]) -> Array[u8] {");
        writ_emit_line(&emit, "    let value: Ffi_Bytes = ref_read(bytes)");
        writ_emit_line(&emit, "    var copied: Array[u8] = new[Array[u8]](cast[int]value.length)");
        writ_emit_line(&emit, "    __ffi_bytes_take(bytes, ref copied)");
        writ_emit_line(&emit, "    return copied");
        writ_emit_line(&emit, "}");
    }
    if (takes_bytes && takes_text(g)) {
        writ_emit_line(&emit, "");
        writ_emit_open(&emit, "func __ffi_take_text(bytes: Ref[Ffi_Bytes], text: Ref[string]) -> bool");
        writ_emit_line(&emit, "let copied = __ffi_take_bytes(bytes)");
        writ_emit_line(&emit, "return __string_from_utf8(copied, text)");
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
        writ_str_t length_params[] = {writ_cstr("uint")};
        writ_str_t copy_params[] = {writ_cstr("uint"), writ_cstr("Ref[Array[u8]]")};
        add_extern(g, writ_cstr("__ffi_cstr_length"), writ_str_format(g->session, "%.*s_cstr_length", S(g->shim)),
                   length_params, 1, writ_cstr("uint"));
        add_extern(g, writ_cstr("__ffi_cstr_copy"), writ_str_format(g->session, "%.*s_cstr_copy", S(g->shim)),
                   copy_params, 2, writ_cstr("nil"));
        writ_emit_line(&emit, "");
        writ_emit_open(&emit, "func __ffi_copy_cstr(address: uint, text: Ref[string]) -> bool");
        writ_emit_line(&emit, "var bytes: Array[u8] = new[Array[u8]](cast[int]__ffi_cstr_length(address))");
        writ_emit_line(&emit, "__ffi_cstr_copy(address, ref bytes)");
        writ_emit_line(&emit, "return __string_from_utf8(bytes, text)");
        writ_emit_close(&emit, nullptr);
    }
    emit_n00b_array_takers(g, &emit);
    for (size_t i = 0; i < count; i++) {
        writ_bound_fn_t bound = writ_binding_fn(g->b, i);
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
    writ_str_t prefix = request->shim_prefix.length ? request->shim_prefix : writ_model_library(model)->name;
    writ_str_t module = request->module_name.length ? request->module_name : writ_model_library(model)->name;
    core_list_t(writ_n00b_extern_t) extern_list = core_list_new(writ_n00b_extern_t);
    n00b_t g = {.b = b, .model = model, .profile = writ_binding_profile(b), .session = session, .shim = prefix,
                .c = {model, prefix, writ_name_case(session, prefix, WRIT_CASE_UPPER), true},
                .map = core_buffer_empty(), .type_namespace = request->type_namespace, .externs = &extern_list};
    writ_on_session_free(session, map_release, g.map);
    size_t count = writ_model_fn_count(model);
    const writ_plan_view_t **plans = writ_alloc(session, sizeof(writ_plan_view_t *) * (count ? count : 1));
    for (size_t i = 0; i < count; i++) {
        writ_bound_fn_t bound = writ_binding_fn(b, i);
        plans[i] = bound.raw ? nullptr : writ_model_fn_plan(model, writ_model_fn_at(model, i));
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
                                    n00b_text(&g));
    }
    if (status == WRIT_OK) {
        status = output->write_file(output->context, writ_str_format(session, "%.*s.n.map", S(module)),
                                    writ_str_copy(session, g.map->data ? g.map->data : "", g.map->byte_len));
    }
    writ_n00b_extern_t *items = writ_alloc(session, sizeof(*items) * (extern_list.len ? extern_list.len : 1));
    for (size_t i = 0; i < extern_list.len; i++) {
        items[i] = extern_list.data[i];
    }
    *externs = (writ_n00b_externs_t){.count = extern_list.len, .items = items};
    core_list_free(extern_list);
    return status;
}
