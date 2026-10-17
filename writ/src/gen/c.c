#include <ctype.h>

#include "gen.h"

bool
writ_c_is_text(const writ_type_view_t *type)
{
    return type && type->kind == WRIT_TYPE_POINTER && type->target && type->target->kind == WRIT_TYPE_CSTR;
}

writ_str_t
writ_c_type(const writ_c_context_t *c, const writ_type_view_t *type, bool constant)
{
    writ_session_t *session = c->model->session;
    if (!type) {
        return writ_cstr("void");
    }
    switch (type->kind) {
    case WRIT_TYPE_POINTER:
    case WRIT_TYPE_ARRAY: {
        writ_str_t target = writ_c_type(c, type->target, false);
        return writ_str_format(session, "%s%.*s *", constant ? "const " : "", S(target));
    }
    case WRIT_TYPE_HANDLE:
    case WRIT_TYPE_ENUM:
    case WRIT_TYPE_VIEW:
        return writ_c_type(c, type->target, constant);
    case WRIT_TYPE_CSTR:
        return writ_cstr("char");
    case WRIT_TYPE_OPAQUE:
        return type->c_spelling.length ? type->c_spelling : writ_cstr("void");
    default:
        return type->c_spelling.length ? type->c_spelling : type->name;
    }
}

writ_step_kind_t
writ_c_lift_kind(const writ_plan_view_t *plan, uint32_t item)
{
    for (size_t i = 0; i < plan->steps_count; i++) {
        writ_step_kind_t kind = plan->steps[i].kind;
        if (plan->steps[i].item == item
            && (kind == WRIT_STEP_LIFT_VALUE || kind == WRIT_STEP_LIFT_BUFFER || kind == WRIT_STEP_WRAP_HANDLE
                || kind == WRIT_STEP_COPY_BORROWED)) {
            return kind;
        }
    }
    return WRIT_STEP_LIFT_VALUE;
}

bool
writ_c_offset_result(const writ_plan_view_t *plan, uint32_t item)
{
    uint32_t with = plan->items[item].with_item;
    return plan->items[item].after_call == WRIT_AFTER_BORROWED_WITH && with != WRIT_PLAN_NONE
        && plan->items[with].shape == WRIT_SHAPE_TEXT;
}

static bool
is_lift(writ_step_kind_t kind)
{
    return kind == WRIT_STEP_LIFT_VALUE || kind == WRIT_STEP_WRAP_HANDLE;
}

bool
writ_c_returns_directly(const writ_c_context_t *c, const writ_plan_view_t *plan)
{
    size_t results = 0;
    for (size_t i = 0; i < plan->items_count; i++) {
        const writ_plan_item_view_t *item = &plan->items[i];
        if (item->role == WRIT_ROLE_ERROR_DETAIL || item->role == WRIT_ROLE_PACK) {
            return false;
        }
        if (item->role == WRIT_ROLE_RESULT) {
            if (item->contract_slot != WRIT_PLAN_NONE || !is_lift(writ_c_lift_kind(plan, (uint32_t)i))) {
                return false;
            }
            if (c->normalized && item->type && item->type->kind == WRIT_TYPE_RECORD) {
                return false;
            }
            results++;
        }
        if (item->shape == WRIT_SHAPE_BYTES || item->shape == WRIT_SHAPE_TEXT || item->shape == WRIT_SHAPE_ARRAY) {
            return false;
        }
    }
    for (size_t i = 0; i < plan->steps_count; i++) {
        writ_step_kind_t kind = plan->steps[i].kind;
        if (kind != WRIT_STEP_PASS && kind != WRIT_STEP_CALL && kind != WRIT_STEP_CLASSIFY && !is_lift(kind)) {
            return false;
        }
    }
    return plan->outcomes_count == 1 && results == 1 && !plan->reset_channels_count;
}

static writ_str_t
suffixed(const writ_c_context_t *c, writ_str_t name, const char *suffix)
{
    return writ_str_format(c->model->session, "%.*s%s", S(name), suffix);
}

static writ_str_t
text_list_type(const writ_c_context_t *c)
{
    return writ_str_format(c->model->session, "const %.*s_str_t *", S(c->prefix));
}

bool
writ_c_by_pointer(const writ_c_context_t *c, const writ_plan_item_view_t *item)
{
    return c->normalized && item->role == WRIT_ROLE_INPUT && item->type && item->type->kind == WRIT_TYPE_RECORD;
}

uint32_t
writ_c_userdata_slot(const writ_plan_view_t *plan, uint32_t item)
{
    for (size_t i = 0; i < plan->slots_count; i++) {
        if (plan->slots[i].item == item && plan->slots[i].part == WRIT_PART_USERDATA) {
            return (uint32_t)i;
        }
    }
    return WRIT_PLAN_NONE;
}

size_t
writ_c_item_params(const writ_c_context_t *c, const writ_plan_view_t *plan, uint32_t index, writ_c_param_t params[2])
{
    const writ_plan_item_view_t *item = &plan->items[index];
    writ_session_t *session = c->model->session;
    if (writ_c_by_pointer(c, item)) {
        params[0] = (writ_c_param_t){writ_str_format(session, "const %.*s *", S(writ_c_type(c, item->type, false))),
                                     item->name};
        return 1;
    }
    if (item->role == WRIT_ROLE_INPUT || item->role == WRIT_ROLE_INPUT_OUTPUT) {
        switch (item->shape) {
        case WRIT_SHAPE_BYTES:
        case WRIT_SHAPE_ARRAY:
        case WRIT_SHAPE_TEXT: {
            writ_str_t element = item->shape == WRIT_SHAPE_TEXT ? writ_cstr("char")
                                                                : writ_c_type(c, item->type->target, false);
            params[0] = (writ_c_param_t){writ_str_format(session, "const %.*s *", S(element)), item->name};
            params[1] = (writ_c_param_t){writ_cstr("size_t"), suffixed(c, item->name, "_length")};
            return 2;
        }
        case WRIT_SHAPE_CALLBACK:
            params[0] = (writ_c_param_t){writ_c_type(c, item->type, false), item->name};
            if (writ_c_userdata_slot(plan, index) == WRIT_PLAN_NONE) {
                return 1;
            }
            params[1] = (writ_c_param_t){writ_cstr("void *"), suffixed(c, item->name, "_userdata")};
            return 2;
        default:
            params[0] = (writ_c_param_t){writ_c_type(c, item->type, false), item->name};
            return 1;
        }
    }
    if (item->role == WRIT_ROLE_PACK) {
        params[0] = (writ_c_param_t){writ_c_is_text(item->type)
                                         ? text_list_type(c)
                                         : writ_str_format(session, "const %.*s *", S(writ_c_type(c, item->type, false))),
                                     item->name};
        params[1] = (writ_c_param_t){writ_cstr("size_t"), suffixed(c, item->name, "_count")};
        return 2;
    }
    writ_step_kind_t lift = writ_c_lift_kind(plan, index);
    writ_str_t type;
    if (lift == WRIT_STEP_LIFT_BUFFER && item->shape == WRIT_SHAPE_ARRAY) {
        params[0] = (writ_c_param_t){writ_str_format(session, "%.*s **", S(writ_c_type(c, item->type->target, false))),
                                     item->name};
        params[1] = (writ_c_param_t){writ_cstr("size_t *"), suffixed(c, item->name, "_length")};
        return 2;
    }
    if (lift == WRIT_STEP_LIFT_BUFFER || lift == WRIT_STEP_COPY_BORROWED
        || (item->role == WRIT_ROLE_ERROR_DETAIL && item->shape == WRIT_SHAPE_TEXT)) {
        type = writ_str_format(session, "%.*s_bytes_t", S(c->prefix));
    } else if (writ_c_offset_result(plan, index)) {
        type = writ_cstr("size_t");
    } else {
        type = writ_c_type(c, item->type, writ_c_is_text(item->type) && item->after_call != WRIT_AFTER_CALLER_OWNS);
    }
    bool pointer = type.length && type.data[type.length - 1] == '*';
    params[0] = (writ_c_param_t){writ_str_format(session, "%.*s%s*", S(type), pointer ? "" : " "), item->name};
    return 1;
}

static const writ_plan_item_view_t *
direct_result(const writ_plan_view_t *plan)
{
    for (size_t i = 0; i < plan->items_count; i++) {
        if (plan->items[i].role == WRIT_ROLE_RESULT) {
            return &plan->items[i];
        }
    }
    return nullptr;
}

writ_str_t
writ_c_return_type(const writ_c_context_t *c, const writ_plan_view_t *plan)
{
    if (!writ_c_returns_directly(c, plan)) {
        return writ_cstr("int32_t");
    }
    const writ_plan_item_view_t *result = direct_result(plan);
    return writ_c_type(c, result->type, writ_c_is_text(result->type) && result->after_call != WRIT_AFTER_CALLER_OWNS);
}

static void
emit_param(writ_emit_t *emit, bool *first, writ_str_t type, writ_str_t name)
{
    bool pointer = type.length && type.data[type.length - 1] == '*';
    writ_emit_text(emit, "%s%.*s%s%.*s", *first ? "" : ", ", S(type), pointer ? "" : " ", S(name));
    *first = false;
}

bool
writ_c_is_input(const writ_plan_item_view_t *item)
{
    return item->role == WRIT_ROLE_INPUT || item->role == WRIT_ROLE_INPUT_OUTPUT || item->role == WRIT_ROLE_PACK;
}

size_t
writ_c_param_order(const writ_c_context_t *c, const writ_plan_view_t *plan, uint32_t *order)
{
    bool direct = writ_c_returns_directly(c, plan);
    size_t count = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < plan->items_count; i++) {
            bool input = writ_c_is_input(&plan->items[i]);
            if (input == (pass == 0) && (input || !direct)) {
                order[count++] = i;
            }
        }
    }
    return count;
}

writ_str_t
writ_c_plan_name(const writ_c_context_t *c, const writ_plan_view_t *plan)
{
    if (plan->policy_slot == WRIT_PLAN_NONE) {
        return plan->fn->contract_name;
    }
    const writ_slot_view_t *slot = &plan->fn->slots[plan->policy_slot];
    const writ_policy_arm_view_t *arm = &slot->policy_arms[plan->policy_arm];
    if (arm->is_else) {
        return writ_str_format(c->model->session, "%.*s_with_%.*s", S(plan->fn->contract_name), S(slot->name));
    }
    return writ_str_format(c->model->session, "%.*s_%.*s", S(plan->fn->contract_name),
                           S(writ_name_case(c->model->session, arm->constant, WRIT_CASE_SNAKE)));
}

void
writ_c_emit_signature(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan)
{
    writ_str_t ret = writ_c_return_type(c, plan);
    writ_emit_text(emit, "%.*s%s%.*s_%.*s(", S(ret), ret.data[ret.length - 1] == '*' ? "" : " ", S(c->prefix),
                   S(writ_c_plan_name(c, plan)));
    bool first = true;
    uint32_t *order = writ_alloc(c->model->session, sizeof(uint32_t) * (plan->items_count + 1));
    size_t ordered = writ_c_param_order(c, plan, order);
    for (size_t o = 0; o < ordered; o++) {
        writ_c_param_t params[2];
        size_t count = writ_c_item_params(c, plan, order[o], params);
        for (size_t p = 0; p < count; p++) {
            emit_param(emit, &first, params[p].type, params[p].name);
        }
    }
    writ_emit_text(emit, "%s)", first ? "void" : "");
}

writ_str_t
writ_c_real_param_type(const writ_c_context_t *c, const writ_slot_view_t *slot)
{
    const writ_type_view_t *type = slot->physical ? slot->physical : slot->type;
    bool constant = slot->mode == WRIT_MODE_IN && type->kind != WRIT_TYPE_HANDLE
                 && (type->kind == WRIT_TYPE_POINTER || type->kind == WRIT_TYPE_ARRAY);
    return writ_c_type(c, type, constant);
}

void
writ_c_emit_real_prototype(const writ_c_context_t *c, writ_emit_t *emit, const writ_fn_view_t *fn, writ_str_t name,
                           bool variadic_tail, const char *terminator)
{
    const writ_type_view_t *result = fn->result ? fn->result->type : nullptr;
    writ_str_t ret = writ_c_type(c, result, writ_c_is_text(result) && fn->result && !fn->result->own);
    writ_emit_text(emit, "%.*s %.*s(", S(ret), S(name));
    bool first = true;
    for (size_t i = 0; i < fn->slots_count; i++) {
        if (variadic_tail && fn->slots[i].type->kind == WRIT_TYPE_VA_LIST) {
            continue;
        }
        emit_param(emit, &first, writ_c_real_param_type(c, &fn->slots[i]), fn->slots[i].name);
    }
    if (variadic_tail || (fn->varargs != WRIT_VARARGS_NONE && fn->varargs != WRIT_VARARGS_VA_LIST)) {
        writ_emit_text(emit, "%s...", first ? "" : ", ");
        first = false;
    }
    writ_emit_line(emit, "%s)%s", first ? "void" : "", terminator);
}

writ_str_t
writ_c_callee(const writ_c_context_t *c, const writ_fn_view_t *fn)
{
    if (fn->varargs == WRIT_VARARGS_VA_LIST) {
        return writ_str_format(c->model->session, "%.*s_%.*s_forward", S(c->prefix), S(fn->contract_name));
    }
    return fn->expands_symbol.length ? fn->expands_symbol : fn->link_symbol;
}

typedef struct {
    writ_expr_env_t         base;
    const writ_c_context_t *c;
    const writ_plan_view_t *plan;
    const char             *result;
} shim_env_t;

static void print_shim_ref(shim_env_t *env, writ_ref_t ref, core_buffer_t *out);

static bool
empty_is_null(const writ_plan_view_t *plan, uint32_t index)
{
    const writ_plan_item_view_t *item = &plan->items[index];
    return item->nullable
        || (item->contract_slot != WRIT_PLAN_NONE && plan->fn->slots[item->contract_slot].null_if_empty);
}
static bool plans_use(const writ_plan_view_t *const *plans, size_t count, writ_step_kind_t kind);

static void
shim_ref(writ_expr_env_t *base, writ_ref_t ref, uint32_t expr, core_buffer_t *out)
{
    (void)expr;
    shim_env_t *env = (shim_env_t *)base;
    const writ_model_t *model = env->c->model;
    writ_type_id type = expr ? writ_expr_type(model, expr) : 0;
    bool integer = type && writ_type_is_integer(model, type);
    core_buffer_puts(out, integer ? "((long long)" : "");
    print_shim_ref(env, ref, out);
    core_buffer_puts(out, integer ? ")" : "");
}

static void
print_shim_ref(shim_env_t *env, writ_ref_t ref, core_buffer_t *out)
{
    const writ_model_t *model = env->c->model;
    if (ref.kind == WRIT_REF_RESULT) {
        core_buffer_puts(out, env->result);
        return;
    }
    if (ref.kind == WRIT_REF_CHANNEL) {
        uint32_t source = model->channels.data[ref.index].source_expr;
        writ_ref_t callee = writ_expr_ref(model, writ_node_child(model->unit, source, 0));
        writ_str_t symbol = writ_sym_str(model->session, model->fns.data[callee.index].link_symbol);
        core_buffer_printf(out, "(*%.*s())", S(symbol));
        return;
    }
    uint32_t slot = writ_plan_contract_slot(model, env->plan, ref);
    uint32_t cell = ref.phase == WRIT_REF_VALUE ? WRIT_PLAN_NONE : writ_plan_cell_for_slot(env->plan, slot);
    if (cell != WRIT_PLAN_NONE) {
        core_buffer_printf(out, "l%u", cell);
    } else if (ref.phase != WRIT_REF_VALUE) {
        core_buffer_printf(out, "(*s%u)", slot);
    } else {
        core_buffer_printf(out, "s%u", slot);
    }
}

static writ_str_t
expr_text(shim_env_t *env, uint32_t expr)
{
    core_buffer_t *buffer = core_buffer_empty();
    writ_expr_print(env->c->model, expr, WRIT_LANGUAGE_C, &env->base, buffer);
    return writ_str_from_buffer(env->c->model->session, buffer);
}

static bool
fully_wrapped(writ_str_t text)
{
    if (text.length < 2 || text.data[0] != '(' || text.data[text.length - 1] != ')') {
        return false;
    }
    int depth = 0;
    for (size_t i = 0; i < text.length; i++) {
        depth += text.data[i] == '(' ? 1 : text.data[i] == ')' ? -1 : 0;
        if (!depth && i + 1 < text.length) {
            return false;
        }
    }
    return true;
}

static writ_str_t
condition_text(shim_env_t *env, uint32_t expr)
{
    writ_str_t text = expr_text(env, expr);
    return fully_wrapped(text) ? text : writ_str_format(env->c->model->session, "(%.*s)", S(text));
}

static void
fail(const writ_c_context_t *c, writ_emit_t *emit, const char *code)
{
    writ_emit_line(emit, "outcome = %.*s_%s;", S(c->upper), code);
    writ_emit_line(emit, "goto finish;");
}

static writ_str_t
slot_type(const writ_c_context_t *c, const writ_plan_view_t *plan, uint32_t slot)
{
    const writ_plan_slot_view_t *physical = &plan->slots[slot];
    if (physical->contract_slot == WRIT_PLAN_NONE) {
        const writ_slot_view_t *result = plan->fn->result;
        return writ_c_type(c, result->type, writ_c_is_text(result->type) && !result->own);
    }
    return writ_c_real_param_type(c, &plan->fn->slots[physical->contract_slot]);
}

static void
emit_declarations(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan)
{
    for (uint32_t i = 0; i < plan->slots_count; i++) {
        const writ_plan_slot_view_t *slot = &plan->slots[i];
        if (slot->pass == WRIT_PASS_VARIADIC_TAIL) {
            continue;
        }
        writ_str_t type = slot_type(c, plan, i);
        bool pointer = type.data[type.length - 1] == '*';
        const char *zero = slot->c_type && slot->c_type->kind == WRIT_TYPE_RECORD ? "{0}" : "0";
        if (slot->contract_slot == WRIT_PLAN_NONE) {
            writ_emit_line(emit, "%.*s%sret = %s;", S(type), pointer ? "" : " ", zero);
            if (plan->query_count) {
                writ_emit_line(emit, "%.*s%squery_ret = %s;", S(type), pointer ? "" : " ", zero);
            }
        } else {
            writ_emit_line(emit, "%.*s%ss%u = %s;", S(type), pointer ? "" : " ", i, zero);
        }
    }
    for (size_t i = 0; i < plan->locals_count; i++) {
        const writ_plan_local_view_t *local = &plan->locals[i];
        writ_str_t type = writ_c_type(c, local->type, false);
        bool pointer = type.data[type.length - 1] == '*';
        if (local->kind == WRIT_LOCAL_CELL) {
            writ_emit_line(emit, "%.*s%sl%zu = %s;", S(type), pointer ? "" : " ", i,
                           local->type->kind == WRIT_TYPE_RECORD ? "{0}" : "0");
        } else {
            writ_emit_line(emit, "%.*s *l%zu = NULL;", S(type), i);
            if (local->kind == WRIT_LOCAL_BUFFER) {
                writ_emit_line(emit, "size_t l%zu_capacity = 0;", i);
            }
        }
    }
}

static const writ_plan_item_view_t *
pack_item(const writ_plan_view_t *plan)
{
    for (size_t i = 0; i < plan->items_count; i++) {
        if (plan->items[i].role == WRIT_ROLE_PACK) {
            return &plan->items[i];
        }
    }
    return nullptr;
}

static void
emit_fixed_args(shim_env_t *env, writ_emit_t *emit, const writ_fn_view_t *fn, bool *first)
{
    if (fn->expands_node) {
        const writ_model_t *model = env->c->model;
        const writ_node_t *expands = writ_node(model->unit, fn->expands_node);
        for (uint32_t i = 0; i < expands->child_count; i++) {
            writ_str_t arg = expr_text(env, writ_node_child(model->unit, fn->expands_node, i));
            writ_emit_text(emit, "%s%.*s", *first ? "" : ", ", S(arg));
            *first = false;
        }
        return;
    }
    for (size_t i = 0; i < fn->slots_count; i++) {
        if (env->plan->slots[i].pass == WRIT_PASS_VARIADIC_TAIL) {
            continue;
        }
        writ_emit_text(emit, "%ss%zu", *first ? "" : ", ", i);
        *first = false;
    }
}

static void
emit_call_with(shim_env_t *env, writ_emit_t *emit, const char *target, const char *tail)
{
    const writ_fn_view_t *fn = env->plan->fn;
    writ_str_t callee = writ_c_callee(env->c, fn);
    writ_emit_text(emit, "%s%s%.*s(", target ? target : "", target ? " = " : "", S(callee));
    bool first = true;
    emit_fixed_args(env, emit, fn, &first);
    if (tail) {
        writ_emit_text(emit, "%s%s", first ? "" : ", ", tail);
    }
    writ_emit_line(emit, ");");
}

static void
emit_pack_call(shim_env_t *env, writ_emit_t *emit, const char *target)
{
    const writ_c_context_t *c = env->c;
    const writ_plan_item_view_t *pack = pack_item(env->plan);
    const writ_fn_view_t *fn = env->plan->fn;
    bool text = writ_c_is_text(pack->type);
    writ_str_t end = fn->varargs == WRIT_VARARGS_UNTIL ? expr_text(env, fn->varargs_end_expr) : writ_cstr("");
    writ_str_t element = writ_c_type(c, pack->type, false);
    writ_emit_open(emit, "switch (%.*s_count)", S(pack->name));
    for (int arity = 0; arity <= WRIT_C_MAX_ARITY; arity++) {
        core_buffer_t *tail = core_buffer_empty();
        for (int k = 0; k < arity; k++) {
            if (text) {
                core_buffer_printf(tail, "%spack[%d]", k ? ", " : "", k);
            } else {
                core_buffer_printf(tail, "%s(%.*s)%.*s[%d]", k ? ", " : "", S(element), S(pack->name), k);
            }
        }
        if (end.length) {
            core_buffer_printf(tail, "%s(%.*s)%.*s", arity ? ", " : "", S(element), S(end));
        }
        writ_str_t tail_text = writ_str_from_buffer(c->model->session, tail);
        writ_emit_line(emit, "case %d:", arity);
        writ_emit_indent(emit, 1);
        emit_call_with(env, emit, target, tail_text.length ? tail_text.data : nullptr);
        writ_emit_line(emit, "break;");
        writ_emit_indent(emit, -1);
    }
    writ_emit_line(emit, "default:");
    writ_emit_indent(emit, 1);
    fail(c, emit, "ARGUMENT_ERROR");
    writ_emit_indent(emit, -1);
    writ_emit_close(emit, nullptr);
}

static void
emit_call(shim_env_t *env, writ_emit_t *emit, const char *target)
{
    const writ_plan_view_t *plan = env->plan;
    if (pack_item(plan)) {
        emit_pack_call(env, emit, target);
        return;
    }
    for (size_t i = 0; i < plan->steps_count; i++) {
        if (plan->steps[i].kind == WRIT_STEP_EXPAND) {
            writ_str_t tail = writ_str_format(env->c->model->session, "l%u", plan->steps[i].local);
            emit_call_with(env, emit, target, tail.data);
            return;
        }
    }
    emit_call_with(env, emit, target, nullptr);
}

static void
emit_query(shim_env_t *env, writ_emit_t *emit, const writ_plan_step_view_t *step)
{
    const writ_plan_view_t *plan = env->plan;
    writ_emit_open(emit, "");
    for (size_t i = 0; i < plan->query_count; i++) {
        writ_str_t value = expr_text(env, plan->query[i].expr);
        writ_str_t type = slot_type(env->c, plan, plan->query[i].slot);
        writ_emit_line(emit, "%.*s saved%zu = s%u;", S(type), i, plan->query[i].slot);
        writ_emit_line(emit, "s%u = (%.*s)(%.*s);", plan->query[i].slot, S(type), S(value));
    }
    emit_call(env, emit, "query_ret");
    for (size_t i = 0; i < plan->query_count; i++) {
        writ_emit_line(emit, "s%u = saved%zu;", plan->query[i].slot, i);
    }
    env->result = "query_ret";
    writ_str_t needs = expr_text(env, step->expr);
    env->result = "ret";
    writ_emit_open(emit, "if ((%.*s) < 0)", S(needs));
    fail(env->c, emit, "CONTRACT_VIOLATION");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "l%u_capacity = (size_t)(%.*s);", step->local, S(needs));
    writ_emit_close(emit, nullptr);
}

static void
emit_encode(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_step_view_t *step,
            const writ_plan_item_view_t *item)
{
    writ_emit_open(emit, "if (%.*s)", S(item->name));
    writ_emit_line(emit, "int32_t status = %.*s_encode(%.*s, %.*s_length, &l%u);", S(c->prefix), S(item->name),
                   S(item->name), step->local);
    writ_emit_open(emit, "if (status)");
    writ_emit_line(emit, "outcome = status;");
    writ_emit_line(emit, "goto finish;");
    writ_emit_close(emit, nullptr);
    writ_emit_close(emit, nullptr);
    if (!item->nullable) {
        writ_emit_open(emit, "else");
        fail(c, emit, "ARGUMENT_ERROR");
        writ_emit_close(emit, nullptr);
    }
    if (step->slot != WRIT_PLAN_NONE) {
        writ_emit_line(emit, "s%u = l%u;", step->slot, step->local);
    }
}

static void
emit_expand(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_step_view_t *step,
            const writ_plan_item_view_t *item)
{
    if (!item || item->role != WRIT_ROLE_PACK || !writ_c_is_text(item->type)) {
        return;
    }
    writ_emit_open(emit, "if (%.*s_count > %d)", S(item->name), WRIT_C_MAX_ARITY);
    fail(c, emit, "ARGUMENT_ERROR");
    writ_emit_close(emit, nullptr);
    writ_emit_open(emit, "for (size_t i = 0; i < %.*s_count; i++)", S(item->name));
    writ_emit_line(emit, "int32_t status = %.*s_encode(%.*s[i].data, %.*s[i].length, &pack[i]);", S(c->prefix),
                   S(item->name), S(item->name));
    writ_emit_open(emit, "if (status)");
    writ_emit_line(emit, "outcome = status;");
    writ_emit_line(emit, "goto finish;");
    writ_emit_close(emit, nullptr);
    writ_emit_close(emit, nullptr);
    (void)step;
}

writ_str_t
writ_c_count_field(const writ_model_t *model, const writ_slot_view_t *field)
{
    if (field->extent_kind != WRIT_EXTENT_COUNT || !field->count_expr) {
        return (writ_str_t){0};
    }
    writ_ref_t ref = writ_expr_ref(model, field->count_expr);
    return ref.kind == WRIT_REF_SLOT && ref.index < model->slots.len
             ? writ_sym_str(model->session, model->slots.data[ref.index].name)
             : (writ_str_t){0};
}

static void
emit_copy_record(const writ_c_context_t *c, writ_emit_t *emit, writ_str_t source, const writ_plan_item_view_t *item)
{
    const writ_model_t *model = c->model;
    const writ_type_view_t *target = item->type && item->type->target ? item->type->target : item->type;
    const writ_record_view_t *record = target ? writ_model_record_named(model, target->name) : nullptr;
    writ_str_t type = writ_c_type(c, target, false);
    writ_emit_open(emit, "if (!%.*s)", S(source));
    writ_emit_line(emit, "%.*s->data = NULL;", S(item->name));
    writ_emit_line(emit, "%.*s->length = 0;", S(item->name));
    writ_emit_line(emit, "goto finish;");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "size_t total = sizeof(%.*s);", S(type));
    for (size_t f = 0; record && f < record->fields_count; f++) {
        writ_str_t count = writ_c_count_field(model, &record->fields[f]);
        if (count.length) {
            writ_str_t element = writ_c_type(c, record->fields[f].type->target, false);
            writ_emit_line(emit, "total = (total + _Alignof(%.*s) - 1) & ~(_Alignof(%.*s) - 1);", S(element),
                           S(element));
            writ_emit_line(emit, "total += (size_t)%.*s->%.*s * sizeof(%.*s);", S(source), S(count), S(element));
        }
    }
    writ_emit_line(emit, "uint8_t *block = malloc(total);");
    writ_emit_open(emit, "if (!block)");
    fail(c, emit, "OUT_OF_MEMORY");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "%.*s *copy = (%.*s *)block;", S(type), S(type));
    writ_emit_line(emit, "*copy = *%.*s;", S(source));
    writ_emit_line(emit, "size_t at = sizeof(%.*s);", S(type));
    for (size_t f = 0; record && f < record->fields_count; f++) {
        writ_str_t count = writ_c_count_field(model, &record->fields[f]);
        if (!count.length) {
            continue;
        }
        writ_str_t name = record->fields[f].name;
        writ_str_t element = writ_c_type(c, record->fields[f].type->target, false);
        writ_emit_line(emit, "at = (at + _Alignof(%.*s) - 1) & ~(_Alignof(%.*s) - 1);", S(element), S(element));
        writ_emit_line(emit, "memcpy(block + at, %.*s->%.*s, (size_t)%.*s->%.*s * sizeof(%.*s));", S(source), S(name),
                       S(source), S(count), S(element));
        writ_emit_line(emit, "copy->%.*s = (void *)(block + at);", S(name));
        writ_emit_line(emit, "at += (size_t)%.*s->%.*s * sizeof(%.*s);", S(source), S(count), S(element));
    }
    writ_emit_line(emit, "%.*s->data = block;", S(item->name));
    writ_emit_line(emit, "%.*s->length = total;", S(item->name));
}

static void
emit_copy_sized(shim_env_t *env, writ_emit_t *emit, writ_str_t source, const writ_plan_step_view_t *step,
                const writ_plan_item_view_t *item)
{
    const writ_c_context_t *c = env->c;
    writ_str_t length = expr_text(env, step->expr);
    writ_emit_line(emit, "%.*s->data = NULL;", S(item->name));
    writ_emit_line(emit, "%.*s->length = 0;", S(item->name));
    if (!item->nullable) {
        writ_emit_open(emit, "if (!%.*s)", S(source));
        fail(c, emit, "CONTRACT_VIOLATION");
        writ_emit_close(emit, nullptr);
    }
    writ_emit_open(emit, "if (%.*s)", S(source));
    writ_emit_line(emit, "size_t length = (size_t)(%.*s);", S(length));
    writ_emit_line(emit, "%.*s->data = malloc(length + 1);", S(item->name));
    writ_emit_open(emit, "if (%.*s->data)", S(item->name));
    writ_emit_line(emit, "memcpy(%.*s->data, %.*s, length);", S(item->name), S(source));
    writ_emit_line(emit, "%.*s->length = length;", S(item->name));
    writ_emit_close(emit, nullptr);
    if (item->release.length) {
        writ_emit_line(emit, "%.*s((void *)%.*s);", S(item->release), S(source));
    }
    writ_emit_open(emit, "if (!%.*s->data)", S(item->name));
    fail(c, emit, "OUT_OF_MEMORY");
    writ_emit_close(emit, nullptr);
    writ_emit_close(emit, nullptr);
}

static void
emit_lift(shim_env_t *env, writ_emit_t *emit, const writ_plan_step_view_t *step, const writ_plan_item_view_t *item)
{
    const writ_c_context_t *c = env->c;
    const writ_plan_view_t *plan = env->plan;
    writ_str_t source = step->local != WRIT_PLAN_NONE ? writ_str_format(c->model->session, "l%u", step->local)
                                                       : writ_cstr("ret");
    uint32_t index = (uint32_t)(item - plan->items);
    if (writ_c_returns_directly(c, plan)) {
        writ_emit_line(emit, "return %.*s;", S(source));
        return;
    }
    if (step->kind == WRIT_STEP_COPY_BORROWED) {
        if (step->expr) {
            emit_copy_sized(env, emit, source, step, item);
            return;
        }
        if (!writ_c_is_text(item->type)) {
            emit_copy_record(c, emit, source, item);
            return;
        }
        writ_emit_open(emit, "if (%.*s_copy_text(%.*s, %.*s))", S(c->prefix), S(source), S(item->name));
        fail(c, emit, "OUT_OF_MEMORY");
        writ_emit_close(emit, nullptr);
        return;
    }
    if (writ_c_offset_result(plan, index)) {
        uint32_t with = item->with_item;
        uint32_t text_local = WRIT_PLAN_NONE;
        for (uint32_t l = 0; l < plan->locals_count; l++) {
            if (plan->locals[l].item == with && plan->locals[l].kind == WRIT_LOCAL_TEXT) {
                text_local = l;
            }
        }
        writ_emit_line(emit, "*%.*s = %.*s ? (size_t)(%.*s - l%u) : SIZE_MAX;", S(item->name), S(source), S(source),
                       text_local);
        return;
    }
    writ_emit_line(emit, "*%.*s = %.*s;", S(item->name), S(source));
}

static void
emit_step(shim_env_t *env, writ_emit_t *emit, const writ_plan_step_view_t *step)
{
    const writ_c_context_t *c = env->c;
    const writ_plan_view_t *plan = env->plan;
    const writ_plan_item_view_t *item = step->item != WRIT_PLAN_NONE ? &plan->items[step->item] : nullptr;
    switch (step->kind) {
    case WRIT_STEP_CHECK: {
        const writ_type_view_t *length = plan->slots[step->slot].c_type;
        if (!item->nullable) {
            writ_emit_open(emit, "if (!%.*s && %.*s_length)", S(item->name), S(item->name));
            fail(c, emit, "ARGUMENT_ERROR");
            writ_emit_close(emit, nullptr);
        }
        if (length->size < 8 || length->is_signed) {
            uint64_t limit = length->is_signed ? (UINT64_C(1) << (8 * length->size - 1)) - 1
                                               : UINT64_MAX >> (64 - 8 * length->size);
            writ_emit_open(emit, "if ((uint64_t)%.*s_length > UINT64_C(%llu))", S(item->name),
                           (unsigned long long)limit);
            fail(c, emit, "ARGUMENT_ERROR");
            writ_emit_close(emit, nullptr);
        }
        return;
    }
    case WRIT_STEP_SPLIT: {
        writ_str_t pointer = slot_type(c, plan, step->slot);
        writ_str_t empty = empty_is_null(plan, step->item)
                             ? writ_cstr("NULL")
                             : writ_str_format(c->model->session, "(%.*s)%.*s_empty", S(pointer), S(c->prefix));
        writ_emit_line(emit, "s%u = %.*s_length ? (%.*s)%.*s : %.*s;", step->slot, S(item->name), S(pointer),
                       S(item->name), S(empty));
        writ_emit_line(emit, "s%u = (%.*s)%.*s_length;", step->second_slot, S(slot_type(c, plan, step->second_slot)),
                       S(item->name));
        return;
    }
    case WRIT_STEP_PASS:
        if (step->literal.length) {
            writ_emit_line(emit, "s%u = \"%.*s\";", step->slot, S(step->literal));
        } else if (step->source_local != WRIT_PLAN_NONE && step->local != WRIT_PLAN_NONE) {
            writ_str_t type = writ_c_type(c, plan->locals[step->local].type, false);
            writ_emit_line(emit, "l%u = (%.*s)l%u_capacity;", step->local, S(type), step->source_local);
        } else if (step->source_local != WRIT_PLAN_NONE) {
            writ_emit_line(emit, "s%u = (%.*s)l%u_capacity;", step->slot, S(slot_type(c, plan, step->slot)),
                           step->source_local);
        } else if (plan->slots[step->slot].part == WRIT_PART_LENGTH) {
            writ_emit_line(emit, "s%u = (%.*s)%.*s_count;", step->slot, S(slot_type(c, plan, step->slot)), S(item->name));
        } else {
            writ_emit_line(emit, "s%u = %s%.*s;", step->slot, writ_c_by_pointer(c, item) ? "*" : "", S(item->name));
        }
        if (item && item->shape == WRIT_SHAPE_CALLBACK && step->second_slot != WRIT_PLAN_NONE) {
            writ_emit_line(emit, "s%u = (%.*s)%.*s_userdata;", step->second_slot, S(slot_type(c, plan, step->second_slot)),
                           S(item->name));
        }
        return;
    case WRIT_STEP_ENCODE_CSTR:
        emit_encode(c, emit, step, item);
        return;
    case WRIT_STEP_PASS_CONST: {
        const writ_policy_arm_view_t *arm = &plan->fn->slots[plan->policy_slot].policy_arms[plan->policy_arm];
        writ_emit_line(emit, "s%u = (%.*s)(intptr_t)%lld;", step->slot, S(slot_type(c, plan, step->slot)),
                       (long long)arm->value);
        return;
    }
    case WRIT_STEP_EXPAND:
        emit_expand(c, emit, step, item);
        return;
    case WRIT_STEP_SIZE_QUERY:
        emit_query(env, emit, step);
        return;
    case WRIT_STEP_SIZE_RETRY: {
        writ_str_t needs = expr_text(env, step->expr);
        const writ_plan_local_view_t *local = &plan->locals[step->local];
        writ_emit_open(emit, "if ((size_t)(%.*s) > l%u_capacity)", S(needs), step->local);
        writ_emit_line(emit, "%.*s = (size_t)(%.*s);", S(plan->items[local->capacity_item].name), S(needs));
        writ_emit_line(emit, "free(l%u);", step->local);
        writ_emit_line(emit, "l%u = NULL;", step->local);
        writ_emit_line(emit, "continue;");
        writ_emit_close(emit, nullptr);
        writ_emit_line(emit, "break;");
        writ_emit_close(emit, nullptr);
        return;
    }
    case WRIT_STEP_ALLOC_OUT: {
        const writ_plan_local_view_t *local = &plan->locals[step->local];
        writ_str_t element = writ_c_type(c, local->type, false);
        if (local->capacity_item != WRIT_PLAN_NONE) {
            writ_emit_line(emit, "l%u_capacity = (size_t)%.*s;", step->local, S(plan->items[local->capacity_item].name));
        } else if (step->expr) {
            writ_emit_line(emit, "l%u_capacity = (size_t)(%.*s);", step->local, S(expr_text(env, step->expr)));
        }
        writ_emit_line(emit, "l%u = malloc(l%u_capacity ? l%u_capacity * sizeof(%.*s) : 1);", step->local, step->local,
                       step->local, S(element));
        writ_emit_open(emit, "if (!l%u)", step->local);
        fail(c, emit, "OUT_OF_MEMORY");
        writ_emit_close(emit, nullptr);
        return;
    }
    case WRIT_STEP_BIND_OUT:
        writ_emit_line(emit, "s%u = (%.*s)%sl%u;", step->slot, S(slot_type(c, plan, step->slot)),
                       plan->locals[step->local].kind == WRIT_LOCAL_CELL ? "&" : "", step->local);
        return;
    case WRIT_STEP_CALL:
        emit_call(env, emit, plan->fn->result ? "ret" : nullptr);
        return;
    case WRIT_STEP_FETCH_DETAIL: {
        if (item->shape == WRIT_SHAPE_TEXT && item->after_call == WRIT_AFTER_CALLER_OWNS) {
            writ_ref_t channel = writ_expr_ref(c->model, step->expr);
            uint32_t source = c->model->channels.data[channel.index].source_expr;
            writ_ref_t callee = writ_expr_ref(c->model, writ_node_child(c->model->unit, source, 0));
            const writ_fn_rec_t *source_fn = &c->model->fns.data[callee.index];
            writ_str_t name = writ_sym_str(c->model->session, source_fn->name);
            writ_emit_line(emit, "%.*s->data = NULL;", S(item->name));
            writ_emit_line(emit, "%.*s->length = 0;", S(item->name));
            if (source_fn->sizing == WRIT_SIZING_RETRY) {
                writ_emit_line(emit, "(void)%.*s_%.*s(%u, %.*s);", S(c->prefix), S(name),
                               writ_default(WRIT_DEFAULT_RETRY_CAPACITY), S(item->name));
            } else {
                writ_emit_line(emit, "(void)%.*s_%.*s(%.*s);", S(c->prefix), S(name), S(item->name));
            }
            return;
        }
        writ_str_t value = expr_text(env, step->expr);
        writ_str_t type = writ_c_type(c, item->type, writ_c_is_text(item->type));
        writ_emit_line(emit, "*%.*s = (%.*s)(%.*s);", S(item->name), S(type), S(value));
        return;
    }
    case WRIT_STEP_LIFT_BUFFER: {
        writ_str_t length = expr_text(env, step->expr);
        writ_emit_open(emit, "if ((%.*s) < 0 || (size_t)(%.*s) > l%u_capacity)", S(length), S(length), step->local);
        fail(c, emit, "CONTRACT_VIOLATION");
        writ_emit_close(emit, nullptr);
        if (item->shape == WRIT_SHAPE_ARRAY) {
            writ_emit_line(emit, "*%.*s = l%u;", S(item->name), step->local);
            writ_emit_line(emit, "*%.*s_length = (size_t)(%.*s);", S(item->name), S(length));
        } else {
            writ_emit_line(emit, "%.*s->data = (uint8_t *)l%u;", S(item->name), step->local);
            writ_emit_line(emit, "%.*s->length = (size_t)(%.*s);", S(item->name), S(length));
        }
        writ_emit_line(emit, "l%u = NULL;", step->local);
        return;
    }
    case WRIT_STEP_LIFT_VALUE:
    case WRIT_STEP_WRAP_HANDLE:
    case WRIT_STEP_COPY_BORROWED:
        emit_lift(env, emit, step, item);
        return;
    case WRIT_STEP_RELEASE_LOCAL:
        writ_emit_line(emit, "free(l%u);", step->local);
        return;
    default:
        return;
    }
}

static void
emit_classify(shim_env_t *env, writ_emit_t *emit)
{
    const writ_plan_view_t *plan = env->plan;
    const writ_c_context_t *c = env->c;
    for (size_t i = 0; i < plan->outcomes_count; i++) {
        const writ_plan_outcome_view_t *outcome = &plan->outcomes[i];
        if (outcome->when_expr) {
            writ_emit_open(emit, "%sif %.*s", i ? "else " : "", S(condition_text(env, outcome->when_expr)));
        } else {
            writ_emit_open(emit, i ? "else" : "");
        }
        writ_emit_line(emit, "outcome = %.*s_%.*s_%.*s;", S(c->prefix), S(writ_c_plan_name(c, plan)), S(outcome->name));
        writ_emit_close(emit, nullptr);
    }
}

static bool
can_fail(const writ_plan_view_t *plan)
{
    for (size_t i = 0; i < plan->steps_count; i++) {
        switch (plan->steps[i].kind) {
        case WRIT_STEP_CHECK:
        case WRIT_STEP_ENCODE_CSTR:
        case WRIT_STEP_SIZE_QUERY:
        case WRIT_STEP_ALLOC_OUT:
        case WRIT_STEP_LIFT_BUFFER:
        case WRIT_STEP_COPY_BORROWED:
            return true;
        default:
            break;
        }
    }
    return pack_item(plan) != nullptr;
}

static bool
has_text_pack(const writ_plan_view_t *plan)
{
    const writ_plan_item_view_t *pack = pack_item(plan);
    return pack && writ_c_is_text(pack->type);
}

static void
emit_reset(shim_env_t *env, writ_emit_t *emit)
{
    const writ_model_t *model = env->c->model;
    for (size_t i = 0; i < env->plan->reset_channels_count; i++) {
        writ_ref_t ref = {WRIT_REF_CHANNEL, WRIT_REF_VALUE, env->plan->reset_channels[i]};
        core_buffer_t *buffer = core_buffer_empty();
        shim_ref(&env->base, ref, 0, buffer);
        writ_emit_line(emit, "%.*s = 0;", (int)buffer->byte_len, buffer->data);
        core_buffer_free(buffer);
    }
    (void)model;
}

static void
emit_shim(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan)
{
    shim_env_t env = {{.print_ref = shim_ref}, c, plan, "ret"};
    bool direct = writ_c_returns_directly(c, plan);
    writ_c_emit_signature(c, emit, plan);
    writ_emit_open(emit, "");
    emit_declarations(c, emit, plan);
    if (has_text_pack(plan)) {
        writ_emit_line(emit, "char *pack[%d] = {0};", WRIT_C_MAX_ARITY);
    }
    if (!direct) {
        writ_emit_line(emit, "int32_t outcome = 0;");
    }
    emit_reset(&env, emit);
    uint32_t current = WRIT_PLAN_NONE;
    bool finished = false;
    bool retrying = false;
    for (size_t i = 0; i < plan->steps_count; i++) {
        const writ_plan_step_view_t *step = &plan->steps[i];
        if (step->kind == WRIT_STEP_CLASSIFY) {
            if (!direct) {
                emit_classify(&env, emit);
            }
            continue;
        }
        if (step->outcome != current && !direct) {
            if (current != WRIT_PLAN_NONE) {
                writ_emit_close(emit, nullptr);
            }
            if (step->outcome != WRIT_PLAN_NONE) {
                writ_emit_open(emit, "if (outcome == %.*s_%.*s_%.*s)", S(c->prefix), S(writ_c_plan_name(c, plan)),
                               S(plan->outcomes[step->outcome].name));
            }
            current = step->outcome;
        }
        if (step->phase == WRIT_PHASE_FINALLY && !finished && !direct && can_fail(plan)) {
            writ_emit_indent(emit, -1);
            writ_emit_line(emit, "finish:");
            writ_emit_indent(emit, 1);
            finished = true;
        }
        if (step->phase == WRIT_PHASE_ATTEMPT && !retrying && plans_use(&plan, 1, WRIT_STEP_SIZE_RETRY)) {
            writ_emit_open(emit, "for (;;)");
            retrying = true;
        }
        emit_step(&env, emit, step);
    }
    if (current != WRIT_PLAN_NONE && !direct) {
        writ_emit_close(emit, nullptr);
    }
    if (!direct) {
        if (!finished && can_fail(plan)) {
            writ_emit_indent(emit, -1);
            writ_emit_line(emit, "finish:");
            writ_emit_indent(emit, 1);
        }
        if (has_text_pack(plan)) {
            writ_emit_open(emit, "for (size_t i = 0; i < %d; i++)", WRIT_C_MAX_ARITY);
            writ_emit_line(emit, "free(pack[i]);");
            writ_emit_close(emit, nullptr);
        }
        writ_emit_line(emit, "return outcome;");
    }
    writ_emit_close(emit, nullptr);
}

static void
emit_forwarder(const writ_c_context_t *c, writ_emit_t *emit, const writ_fn_view_t *fn)
{
    writ_str_t name = writ_c_callee(c, fn);
    writ_emit_text(emit, "static ");
    writ_c_emit_real_prototype(c, emit, fn, name, true, "");
    writ_emit_open(emit, "");
    writ_str_t last = writ_cstr("");
    for (size_t i = 0; i < fn->slots_count; i++) {
        if (fn->slots[i].type->kind != WRIT_TYPE_VA_LIST) {
            last = fn->slots[i].name;
        }
    }
    writ_emit_line(emit, "va_list ap;");
    writ_emit_line(emit, "va_start(ap, %.*s);", S(last));
    bool has_result = fn->result != nullptr;
    if (has_result) {
        writ_str_t type = writ_c_type(c, fn->result->type, writ_c_is_text(fn->result->type) && !fn->result->own);
        writ_emit_text(emit, "%.*s result = ", S(type));
    }
    writ_emit_text(emit, "%.*s(", S(fn->link_symbol));
    for (size_t i = 0; i < fn->slots_count; i++) {
        bool is_list = fn->slots[i].type->kind == WRIT_TYPE_VA_LIST;
        writ_emit_text(emit, "%s%.*s", i ? ", " : "", S(is_list ? writ_cstr("ap") : fn->slots[i].name));
    }
    writ_emit_line(emit, ");");
    writ_emit_line(emit, "va_end(ap);");
    if (has_result) {
        writ_emit_line(emit, "return result;");
    }
    writ_emit_close(emit, nullptr);
}

static void
emit_includes(const writ_c_context_t *c, writ_emit_t *emit)
{
    const writ_library_view_t *library = writ_model_library(c->model);
    for (size_t i = 0; i < library->headers_count; i++) {
        writ_emit_line(emit, "#include <%.*s>", S(library->headers[i]));
    }
}

static void
emit_record_fields(const writ_c_context_t *c, writ_emit_t *emit, const writ_record_view_t *record)
{
    const writ_model_t *model = c->model;
    for (size_t f = 0; f < record->fields_count; f++) {
        const writ_slot_view_t *field = &record->fields[f];
        const writ_record_view_t *tagged = field->type && field->type->kind == WRIT_TYPE_UNION
                                             ? writ_model_record_at(model, field->type->decl)
                                             : nullptr;
        if (tagged && tagged->arms_count) {
            writ_emit_open(emit, "union");
            emit_record_fields(c, emit, tagged);
            writ_emit_close(emit, writ_str_format(model->session, " %.*s;", S(field->name)).data);
            continue;
        }
        writ_str_t type = writ_c_type(c, field->type, false);
        bool pointer = type.length && type.data[type.length - 1] == '*';
        writ_emit_line(emit, "%.*s%s%.*s;", S(type), pointer ? "" : " ", S(field->name));
    }
}

void
writ_c_emit_records(const writ_c_context_t *c, writ_emit_t *emit)
{
    const writ_model_t *model = c->model;
    for (size_t i = 0; i < writ_model_record_count(model); i++) {
        const writ_record_view_t *record = writ_model_record_at(model, i);
        if (record->arms_count) {
            continue;
        }
        writ_emit_open(emit, "typedef %s %.*s", record->type->kind == WRIT_TYPE_UNION ? "union" : "struct",
                       S(record->name));
        emit_record_fields(c, emit, record);
        writ_emit_close(emit, writ_str_format(model->session, " %.*s;", S(record->name)).data);
    }
}

static void
emit_declared_types(const writ_c_context_t *c, writ_emit_t *emit)
{
    const writ_model_t *model = c->model;
    if (writ_model_library(model)->headers_count) {
        return;
    }
    for (size_t i = 0; i < writ_model_handle_count(model); i++) {
        const writ_type_view_t *opaque = writ_model_handle_at(model, i)->base->target;
        if (opaque && opaque->kind == WRIT_TYPE_OPAQUE && opaque->c_spelling.length) {
            writ_emit_line(emit, "typedef struct %.*s %.*s;", S(opaque->c_spelling), S(opaque->c_spelling));
        }
    }
    for (size_t i = 0; i < writ_model_enum_count(model); i++) {
        const writ_enum_view_t *view = writ_model_enum_at(model, i);
        writ_emit_open(emit, "enum");
        for (size_t k = 0; k < view->items_count; k++) {
            writ_str_t item = writ_name_case(model->session, view->items[k].name, WRIT_CASE_UPPER);
            writ_emit_line(emit, "%.*s_%.*s = %lld,", S(c->upper), S(item), (long long)view->items[k].value);
        }
        writ_emit_close(emit, ";");
    }
    writ_c_emit_records(c, emit);
    writ_emit_line(emit, "");
}

static void
emit_callback_types(const writ_c_context_t *c, writ_emit_t *emit)
{
    if (writ_model_library(c->model)->headers_count) {
        return;
    }
    for (size_t i = 0; i < writ_model_callback_count(c->model); i++) {
        const writ_fn_view_t *fn = writ_model_callback_at(c->model, i);
        writ_emit_text(emit, "typedef ");
        writ_c_emit_real_prototype(c, emit, fn, writ_str_format(c->model->session, "(*%.*s)", S(fn->contract_name)),
                                   false, ";");
    }
    if (writ_model_callback_count(c->model)) {
        writ_emit_line(emit, "");
    }
}

static writ_str_t
header_text(const writ_c_context_t *c, const writ_plan_view_t *const *plans, size_t count)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_line(&emit, "#ifndef %.*s_WRIT_H", S(c->upper));
    writ_emit_line(&emit, "#define %.*s_WRIT_H", S(c->upper));
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#include <stdarg.h>");
    writ_emit_line(&emit, "#include <stdbool.h>");
    writ_emit_line(&emit, "#include <stddef.h>");
    writ_emit_line(&emit, "#include <stdint.h>");
    emit_includes(c, &emit);
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#ifdef __cplusplus");
    writ_emit_line(&emit, "extern \"C\" {");
    writ_emit_line(&emit, "#endif");
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#define %.*s_ARGUMENT_ERROR (%d)", S(c->upper), WRIT_C_ARGUMENT_ERROR);
    writ_emit_line(&emit, "#define %.*s_OUT_OF_MEMORY (%d)", S(c->upper), WRIT_C_OUT_OF_MEMORY);
    writ_emit_line(&emit, "#define %.*s_CONTRACT_VIOLATION (%d)", S(c->upper), WRIT_C_CONTRACT_VIOLATION);
    writ_emit_line(&emit, "");
    writ_emit_open(&emit, "typedef struct");
    writ_emit_line(&emit, "uint8_t *data;");
    writ_emit_line(&emit, "size_t length;");
    writ_emit_close(&emit, writ_str_format(c->model->session, " %.*s_bytes_t;", S(c->prefix)).data);
    writ_emit_line(&emit, "");
    writ_emit_open(&emit, "typedef struct");
    writ_emit_line(&emit, "const char *data;");
    writ_emit_line(&emit, "size_t length;");
    writ_emit_close(&emit, writ_str_format(c->model->session, " %.*s_str_t;", S(c->prefix)).data);
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "void %.*s_bytes_free(%.*s_bytes_t *bytes);", S(c->prefix), S(c->prefix));
    writ_emit_line(&emit, "void %.*s_free(void *data);", S(c->prefix));
    writ_emit_line(&emit, "");
    emit_declared_types(c, &emit);
    emit_callback_types(c, &emit);
    for (size_t i = 0; i < count; i++) {
        const writ_plan_view_t *plan = plans[i];
        writ_emit_line(&emit, "");
        if (!writ_c_returns_directly(c, plan)) {
            writ_emit_open(&emit, "enum");
            for (size_t o = 0; o < plan->outcomes_count; o++) {
                writ_emit_line(&emit, "%.*s_%.*s_%.*s = %zu,", S(c->prefix), S(writ_c_plan_name(c, plan)),
                               S(plan->outcomes[o].name), o);
            }
            writ_emit_close(&emit, ";");
        }
        writ_c_emit_signature(c, &emit, plan);
        writ_emit_line(&emit, ";");
    }
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#ifdef __cplusplus");
    writ_emit_line(&emit, "}");
    writ_emit_line(&emit, "#endif");
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#endif");
    return writ_emit_finish(&emit, c->model->session);
}

static void emit_encode_helper(const writ_c_context_t *c, writ_emit_t *emit);
static void emit_copy_helper(const writ_c_context_t *c, writ_emit_t *emit);

static bool
plans_use(const writ_plan_view_t *const *plans, size_t count, writ_step_kind_t kind)
{
    for (size_t i = 0; i < count; i++) {
        for (size_t s = 0; s < plans[i]->steps_count; s++) {
            const writ_plan_step_view_t *step = &plans[i]->steps[s];
            bool text_pack = kind == WRIT_STEP_ENCODE_CSTR && step->kind == WRIT_STEP_EXPAND && has_text_pack(plans[i]);
            bool empty = kind == WRIT_STEP_SPLIT && step->kind == kind && !empty_is_null(plans[i], step->item);
            if ((step->kind == kind && kind != WRIT_STEP_SPLIT) || text_pack || empty) {
                return true;
            }
        }
    }
    return false;
}

static void
emit_helpers(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *const *plans, size_t count)
{
    if (plans_use(plans, count, WRIT_STEP_SPLIT)) {
        writ_emit_line(emit, "static const uint8_t %.*s_empty[1];", S(c->prefix));
        writ_emit_line(emit, "");
    }
    writ_emit_open(emit, "void %.*s_bytes_free(%.*s_bytes_t *bytes)", S(c->prefix), S(c->prefix));
    writ_emit_line(emit, "free(bytes->data);");
    writ_emit_line(emit, "bytes->data = NULL;");
    writ_emit_line(emit, "bytes->length = 0;");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "");
    writ_emit_open(emit, "void %.*s_free(void *data)", S(c->prefix));
    writ_emit_line(emit, "free(data);");
    writ_emit_close(emit, nullptr);
    if (plans_use(plans, count, WRIT_STEP_ENCODE_CSTR)) {
        writ_emit_line(emit, "");
        emit_encode_helper(c, emit);
    }
    if (plans_use(plans, count, WRIT_STEP_COPY_BORROWED)) {
        writ_emit_line(emit, "");
        emit_copy_helper(c, emit);
    }
}

static void
emit_encode_helper(const writ_c_context_t *c, writ_emit_t *emit)
{
    writ_emit_open(emit, "static int32_t %.*s_encode(const char *text, size_t length, char **out)", S(c->prefix));
    writ_emit_open(emit, "if (memchr(text, 0, length))");
    writ_emit_line(emit, "return %.*s_ARGUMENT_ERROR;", S(c->upper));
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "*out = malloc(length + 1);");
    writ_emit_open(emit, "if (!*out)");
    writ_emit_line(emit, "return %.*s_OUT_OF_MEMORY;", S(c->upper));
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "memcpy(*out, text, length);");
    writ_emit_line(emit, "(*out)[length] = 0;");
    writ_emit_line(emit, "return 0;");
    writ_emit_close(emit, nullptr);
}

static void
emit_copy_helper(const writ_c_context_t *c, writ_emit_t *emit)
{
    writ_emit_open(emit, "static int %.*s_copy_text(const char *text, %.*s_bytes_t *out)", S(c->prefix), S(c->prefix));
    writ_emit_line(emit, "size_t length = text ? strlen(text) : 0;");
    writ_emit_line(emit, "out->data = malloc(length + 1);");
    writ_emit_open(emit, "if (!out->data)");
    writ_emit_line(emit, "return -1;");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "memcpy(out->data, text ? text : \"\", length + 1);");
    writ_emit_line(emit, "out->length = length;");
    writ_emit_line(emit, "return 0;");
    writ_emit_close(emit, nullptr);
}

static void
emit_release_prototypes(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *const *plans,
                        size_t count)
{
    core_list_t(writ_sym_t) declared = core_list_new(writ_sym_t);
    for (size_t i = 0; i < count; i++) {
        for (size_t t = 0; t < plans[i]->items_count; t++) {
            const writ_plan_item_view_t *item = &plans[i]->items[t];
            if (!item->release.length || writ_c_lift_kind(plans[i], (uint32_t)t) != WRIT_STEP_COPY_BORROWED
                || writ_model_fn_named(c->model, item->release)) {
                continue;
            }
            writ_sym_t release = writ_intern_str(c->model->session, item->release);
            bool seen = false;
            for (size_t d = 0; !seen && d < declared.len; d++) {
                seen = declared.data[d] == release;
            }
            if (!seen) {
                writ_emit_line(emit, "void %.*s(void *data);", S(item->release));
                core_list_push(declared, release);
            }
        }
    }
    core_list_free(declared);
}

static writ_str_t
shim_text(const writ_c_context_t *c, const writ_plan_view_t *const *plans, size_t count)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_line(&emit, "#include \"%.*s_writ.h\"", S(c->prefix));
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#include <stdlib.h>");
    writ_emit_line(&emit, "#include <string.h>");
    writ_emit_line(&emit, "");
    if (!writ_model_library(c->model)->headers_count) {
        for (size_t i = 0; i < writ_model_fn_count(c->model); i++) {
            const writ_fn_view_t *fn = writ_model_fn_at(c->model, i);
            writ_c_emit_real_prototype(c, &emit, fn, fn->link_symbol, false, ";");
        }
        emit_release_prototypes(c, &emit, plans, count);
        writ_emit_line(&emit, "");
    }
    emit_helpers(c, &emit, plans, count);
    for (size_t i = 0; i < count; i++) {
        if (plans[i]->fn->varargs == WRIT_VARARGS_VA_LIST) {
            writ_emit_line(&emit, "");
            emit_forwarder(c, &emit, plans[i]->fn);
        }
    }
    for (size_t i = 0; i < count; i++) {
        writ_emit_line(&emit, "");
        emit_shim(c, &emit, plans[i]);
    }
    return writ_emit_finish(&emit, c->model->session);
}

static writ_str_t
upper(writ_session_t *session, writ_str_t text)
{
    writ_str_t copy = writ_str_copy(session, text.data, text.length);
    for (size_t i = 0; i < copy.length; i++) {
        ((char *)copy.data)[i] = (char)toupper((unsigned char)copy.data[i]);
    }
    return copy;
}

static bool
claims_prefix(writ_str_t name, writ_str_t prefix)
{
    return name.length > prefix.length && !memcmp(name.data, prefix.data, prefix.length) && name.data[prefix.length] == '_';
}

static bool
prefix_taken(const writ_model_t *model, writ_str_t prefix)
{
    for (size_t i = 0; i < writ_model_fn_count(model); i++) {
        const writ_fn_view_t *fn = writ_model_fn_at(model, i);
        if (claims_prefix(fn->contract_name, prefix) || claims_prefix(fn->link_symbol, prefix)
            || claims_prefix(fn->source_name, prefix)) {
            return true;
        }
    }
    for (size_t i = 0; i < writ_model_callback_count(model); i++) {
        if (claims_prefix(writ_model_callback_at(model, i)->contract_name, prefix)) {
            return true;
        }
    }
    for (size_t i = 0; i < writ_model_const_count(model); i++) {
        if (claims_prefix(writ_model_const_at(model, i)->name, prefix)) {
            return true;
        }
    }
    for (size_t i = 0; i < writ_model_record_count(model); i++) {
        const writ_record_view_t *record = writ_model_record_at(model, i);
        if (claims_prefix(record->name, prefix) || claims_prefix(record->type->c_spelling, prefix)) {
            return true;
        }
    }
    for (size_t i = 0; i < writ_model_enum_count(model); i++) {
        if (claims_prefix(writ_model_enum_at(model, i)->name, prefix)) {
            return true;
        }
    }
    for (size_t i = 0; i < writ_model_handle_count(model); i++) {
        if (claims_prefix(writ_model_handle_at(model, i)->name, prefix)) {
            return true;
        }
    }
    return false;
}

writ_str_t
writ_c_default_prefix(const writ_model_t *model)
{
    writ_str_t prefix = writ_model_library(model)->name;
    while (prefix_taken(model, prefix)) {
        prefix = writ_str_format(model->session, "writ_%.*s", S(prefix));
    }
    return prefix;
}

writ_status_t
writ_gen_c(const writ_model_t *model, const writ_gen_c_request_t *request, const writ_output_t *output)
{
    if (!model || !request || !output || !output->write_file) {
        return WRIT_INVALID_ARGUMENT;
    }
    writ_session_t *session = model->session;
    writ_str_t prefix = request->prefix.length ? request->prefix : writ_c_default_prefix(model);
    writ_c_context_t c = {model, writ_str_copy(session, prefix.data, prefix.length), upper(session, prefix),
                          request->struct_size >= sizeof(writ_gen_c_request_t) && request->pointer_normalized};
    size_t total = writ_model_fn_count(model);
    size_t capacity = 0;
    for (size_t i = 0; i < total; i++) {
        const writ_fn_view_t *fn = writ_model_fn_at(model, i);
        size_t arms = 1;
        for (size_t s = 0; s < fn->slots_count; s++) {
            arms = fn->slots[s].policy_arms_count ? fn->slots[s].policy_arms_count : arms;
        }
        capacity += arms;
    }
    const writ_plan_view_t **plans = writ_alloc(session, sizeof(writ_plan_view_t *) * (capacity ? capacity : 1));
    size_t count = 0;
    for (size_t i = 0; i < total; i++) {
        const writ_fn_view_t *fn = writ_model_fn_at(model, i);
        if (c.normalized && writ_fn_has_unknown_facts(fn)) {
            continue;
        }
        const writ_plan_view_t *plan = writ_model_fn_plan(model, fn);
        if (!plan && !c.normalized) {
            return WRIT_HAS_ERRORS;
        }
        for (size_t arm = 0; plan; plan = writ_model_fn_arm_plan(model, fn, ++arm)) {
            plans[count++] = plan;
        }
    }
    writ_status_t status = output->write_file(output->context, writ_str_format(session, "%.*s_writ.h", S(c.prefix)),
                                              header_text(&c, plans, count));
    if (status == WRIT_OK) {
        status = output->write_file(output->context, writ_str_format(session, "%.*s_writ.c", S(c.prefix)),
                                    shim_text(&c, plans, count));
    }
    if (status == WRIT_OK && request->conformance) {
        status = writ_gen_conformance(&c, output);
    }
    return status;
}
