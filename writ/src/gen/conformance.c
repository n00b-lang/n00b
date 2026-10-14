#include "gen.h"

enum {
    INPUT_LENGTH   = 16,
    FILL_LONG      = 8,
    FILL_SHORT     = 3,
    CAPACITY_INPUT = 24,
    SCALAR_INPUT   = 3,
    OUT_VALUE      = 5,
    PURE_OFFSET    = 16,
    SEEN_SLOTS     = 32,
    SEEN_PACK      = 24,
    SEEN_COUNT     = 30,
    SEEN_FORMAT    = 31,
    TEXT_OFFSET    = 2,
};

typedef struct {
    const char *name;
    uint32_t    outcome;
    int64_t     ret;
    int64_t     channel;
    int         fill;
    bool        empty;
    int         arity;
} scenario_t;

typedef struct {
    writ_expr_env_t base;
    int64_t         ret;
    int64_t         channel;
} outcome_env_t;

static bool
outcome_value(writ_expr_env_t *base, writ_ref_t ref, uint32_t expr, int64_t *out)
{
    (void)expr;
    outcome_env_t *env = (outcome_env_t *)base;
    if (ref.kind == WRIT_REF_RESULT || ref.kind == WRIT_REF_CHANNEL) {
        *out = ref.kind == WRIT_REF_RESULT ? env->ret : env->channel;
        return true;
    }
    return false;
}

static uint32_t
outcome_for(const writ_model_t *model, const writ_plan_view_t *plan, int64_t ret, int64_t channel)
{
    outcome_env_t env = {{.value = outcome_value}, ret, channel};
    for (uint32_t i = 0; i < plan->outcomes_count; i++) {
        int64_t holds = 0;
        if (!plan->outcomes[i].when_expr) {
            return i;
        }
        if (!writ_expr_eval(model, plan->outcomes[i].when_expr, &env.base, &holds)) {
            return WRIT_PLAN_NONE;
        }
        if (holds) {
            return i;
        }
    }
    return WRIT_PLAN_NONE;
}

static bool
find_outcome(const writ_model_t *model, const writ_plan_view_t *plan, uint32_t wanted, int64_t *ret, int64_t *channel)
{
    static const int64_t channels[] = {0, 22, 34};
    int64_t candidates[96] = {1, 0, -1, 2, -3, 5, 22};
    size_t count = 7;
    for (size_t i = 0; i < writ_model_const_count(model) && count < 96; i++) {
        candidates[count++] = writ_model_const_at(model, i)->value;
    }
    for (size_t i = 0; i < writ_model_handle_count(model) && count < 96; i++) {
        candidates[count++] = writ_model_handle_at(model, i)->none_value;
    }
    for (size_t r = 0; r < count; r++) {
        for (size_t ch = 0; ch < 3; ch++) {
            if (outcome_for(model, plan, candidates[r], channels[ch]) == wanted) {
                *ret = candidates[r];
                *channel = channels[ch];
                return true;
            }
        }
    }
    return false;
}

static uint32_t
slot_with(const writ_plan_view_t *plan, uint32_t item, writ_part_t part)
{
    for (uint32_t i = 0; i < plan->slots_count; i++) {
        if (plan->slots[i].item == item && plan->slots[i].part == part) {
            return i;
        }
    }
    return WRIT_PLAN_NONE;
}

static const writ_type_view_t *
underlying(const writ_type_view_t *type)
{
    while (type->kind == WRIT_TYPE_HANDLE || type->kind == WRIT_TYPE_ENUM || type->kind == WRIT_TYPE_VIEW) {
        type = type->target;
    }
    return type;
}

static bool
is_value_type(const writ_type_view_t *type)
{
    writ_type_kind_t kind = underlying(type)->kind;
    return kind == WRIT_TYPE_INT || kind == WRIT_TYPE_BOOL || kind == WRIT_TYPE_FLOAT;
}

static bool
is_pointer_type(const writ_type_view_t *type)
{
    writ_type_kind_t kind = underlying(type)->kind;
    return kind == WRIT_TYPE_POINTER || kind == WRIT_TYPE_ARRAY;
}

static bool
is_channel_source(const writ_model_t *model, const writ_fn_view_t *fn)
{
    for (size_t i = 0; i < model->channels.len; i++) {
        uint32_t source = model->channels.data[i].source_expr;
        writ_ref_t callee = writ_expr_ref(model, writ_node_child(model->unit, source, 0));
        if (callee.kind == WRIT_REF_FN && &model->fn_views[callee.index] == fn) {
            return true;
        }
    }
    return false;
}

static bool
is_mocked(const writ_model_t *model, const writ_fn_view_t *fn)
{
    return !fn->expands_node && !is_channel_source(model, fn);
}

static writ_str_t
channel_lvalue(const writ_c_context_t *c, uint32_t channel)
{
    const writ_model_t *model = c->model;
    uint32_t source = model->channels.data[channel].source_expr;
    writ_ref_t callee = writ_expr_ref(model, writ_node_child(model->unit, source, 0));
    writ_str_t symbol = writ_sym_str(model->session, model->fns.data[callee.index].link_symbol);
    return writ_str_format(model->session, "(*%.*s())", S(symbol));
}

static writ_str_t
seen_at(const writ_c_context_t *c, uint32_t index, uint32_t slot)
{
    return writ_str_format(c->model->session, "%.*s_mock_seen[%u][%u]", S(c->prefix), index, slot);
}

static void
mock_record(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan, uint32_t index)
{
    const writ_fn_view_t *fn = plan->fn;
    for (uint32_t k = 0; k < fn->slots_count; k++) {
        const writ_slot_view_t *slot = &fn->slots[k];
        const writ_plan_slot_view_t *physical = &plan->slots[k];
        writ_str_t seen = seen_at(c, index, k);
        if (slot->type->kind == WRIT_TYPE_VA_LIST) {
            continue;
        }
        bool text = physical->item != WRIT_PLAN_NONE && plan->items[physical->item].shape == WRIT_SHAPE_TEXT
                 && plan->items[physical->item].role == WRIT_ROLE_INPUT && writ_c_is_text(slot->type);
        if (is_value_type(slot->type)) {
            writ_emit_line(emit, "%.*s = (long long)%.*s;", S(seen), S(slot->name));
        } else if (physical->part == WRIT_PART_CAPACITY_AND_LENGTH) {
            writ_emit_line(emit, "%.*s = %.*s ? (long long)*%.*s : -1;", S(seen), S(slot->name), S(slot->name));
        } else if (text) {
            writ_emit_line(emit, "%.*s = (%.*s != NULL) | ((%.*s && !strcmp(%.*s, \"text\")) << 1);", S(seen),
                           S(slot->name), S(slot->name), S(slot->name));
        } else {
            writ_emit_line(emit, "%.*s = %.*s != NULL;", S(seen), S(slot->name));
        }
        uint32_t length = physical->part == WRIT_PART_POINTER && physical->item != WRIT_PLAN_NONE
                              ? slot_with(plan, physical->item, WRIT_PART_LENGTH)
                              : WRIT_PLAN_NONE;
        if (length != WRIT_PLAN_NONE) {
            writ_emit_open(emit, "for (size_t i = 0; %.*s && i < (size_t)%.*s * sizeof *%.*s; i++)", S(slot->name),
                           S(fn->slots[length].name), S(slot->name));
            writ_emit_line(emit, "%.*s |= (((const uint8_t *)%.*s)[i] != (uint8_t)(i * 7 + 3)) << 1;", S(seen),
                           S(slot->name));
            writ_emit_close(emit, nullptr);
        }
    }
}

static writ_str_t
last_fixed(const writ_fn_view_t *fn)
{
    writ_str_t last = writ_cstr("");
    for (size_t i = 0; i < fn->slots_count; i++) {
        if (fn->slots[i].type->kind != WRIT_TYPE_VA_LIST) {
            last = fn->slots[i].name;
        }
    }
    return last;
}

static void
mock_pack(const writ_c_context_t *c, writ_emit_t *emit, const writ_fn_view_t *fn, uint32_t index)
{
    writ_str_t element = writ_c_type(c, fn->varargs_type, writ_c_is_text(fn->varargs_type));
    if (fn->varargs == WRIT_VARARGS_COUNT) {
        writ_emit_line(emit, "size_t limit = (size_t)%.*s;", S(fn->slots[fn->varargs_count_slot].name));
    } else {
        writ_emit_line(emit, "size_t limit = %d;", SEEN_COUNT - SEEN_PACK);
    }
    writ_emit_line(emit, "size_t count = 0;");
    writ_emit_open(emit, "while (count < limit)");
    writ_emit_line(emit, "%.*s value = va_arg(ap, %.*s);", S(element), S(element));
    if (fn->varargs == WRIT_VARARGS_UNTIL) {
        core_buffer_t *end = core_buffer_empty();
        writ_expr_print(c->model, fn->varargs_end_expr, WRIT_LANGUAGE_C, nullptr, end);
        writ_emit_open(emit, "if (value == (%.*s)%.*s)", S(element), (int)end->byte_len, end->data);
        writ_emit_line(emit, "break;");
        writ_emit_close(emit, nullptr);
        core_buffer_free(end);
    }
    if (writ_c_is_text(fn->varargs_type)) {
        writ_emit_line(emit, "%.*s_mock_seen[%u][%d + count] = value && strlen(value) == count + 1;", S(c->prefix),
                       index, SEEN_PACK);
    } else {
        writ_emit_line(emit, "%.*s_mock_seen[%u][%d + count] = (long long)value;", S(c->prefix), index, SEEN_PACK);
    }
    writ_emit_line(emit, "count++;");
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "%.*s_mock_seen[%u][%d] = (long long)count;", S(c->prefix), index, SEEN_COUNT);
}

static void
mock_varargs(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan, uint32_t index)
{
    const writ_fn_view_t *fn = plan->fn;
    if (fn->varargs == WRIT_VARARGS_NONE || fn->varargs == WRIT_VARARGS_OPAQUE || fn->varargs == WRIT_VARARGS_OF) {
        return;
    }
    bool list = fn->varargs == WRIT_VARARGS_VA_LIST;
    writ_str_t ap = writ_cstr("ap");
    for (size_t i = 0; list && i < fn->slots_count; i++) {
        if (fn->slots[i].type->kind == WRIT_TYPE_VA_LIST) {
            ap = fn->slots[i].name;
        }
    }
    writ_emit_open(emit, "");
    if (!list) {
        writ_emit_line(emit, "va_list ap;");
        writ_emit_line(emit, "va_start(ap, %.*s);", S(last_fixed(fn)));
    }
    if (fn->format_slot != WRIT_PLAN_NONE) {
        writ_emit_line(emit, "const char *formatted = va_arg(%.*s, const char *);", S(ap));
        writ_emit_line(emit, "%.*s_mock_seen[%u][%d] = !strcmp(%.*s, \"%%s\") && !strcmp(formatted, \"text\");",
                       S(c->prefix), index, SEEN_FORMAT, S(fn->slots[fn->format_slot].name));
    } else {
        mock_pack(c, emit, fn, index);
    }
    if (!list) {
        writ_emit_line(emit, "va_end(ap);");
    }
    writ_emit_close(emit, nullptr);
}

static void
mock_buffer(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan, uint32_t k)
{
    const writ_fn_view_t *fn = plan->fn;
    uint32_t item = plan->slots[k].item;
    uint32_t cell = slot_with(plan, item, WRIT_PART_CAPACITY_AND_LENGTH);
    uint32_t capacity = slot_with(plan, item, WRIT_PART_CAPACITY);
    writ_str_t name = fn->slots[k].name;
    writ_str_t cap = cell != WRIT_PLAN_NONE ? writ_str_format(c->model->session, "(size_t)*%.*s", S(fn->slots[cell].name))
                                            : writ_str_format(c->model->session, "(size_t)%.*s", S(fn->slots[capacity].name));
    writ_emit_open(emit, "if (%.*s)", S(name));
    writ_emit_line(emit, "size_t written = %.*s_mock_fill < %.*s ? %.*s_mock_fill : %.*s;", S(c->prefix), S(cap),
                   S(c->prefix), S(cap));
    writ_emit_open(emit, "for (size_t i = 0; i < written; i++)");
    writ_emit_line(emit, "((uint8_t *)%.*s)[i] = (uint8_t)(i * 7 + 3);", S(name));
    writ_emit_close(emit, nullptr);
    writ_emit_open(emit, "if (written < %.*s)", S(cap));
    writ_emit_line(emit, "((uint8_t *)%.*s)[written] = 0;", S(name));
    writ_emit_close(emit, nullptr);
    if (cell != WRIT_PLAN_NONE) {
        writ_str_t cell_type = writ_c_type(c, fn->slots[cell].type->target, false);
        writ_emit_line(emit, "*%.*s = (%.*s)written;", S(fn->slots[cell].name), S(cell_type));
    }
    writ_emit_close(emit, nullptr);
}

static void
mock_outputs(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan)
{
    const writ_fn_view_t *fn = plan->fn;
    for (uint32_t k = 0; k < fn->slots_count; k++) {
        const writ_plan_slot_view_t *physical = &plan->slots[k];
        const writ_slot_view_t *slot = &fn->slots[k];
        if (physical->item == WRIT_PLAN_NONE || plan->items[physical->item].role != WRIT_ROLE_RESULT) {
            continue;
        }
        const writ_plan_item_view_t *item = &plan->items[physical->item];
        if (physical->part == WRIT_PART_POINTER) {
            mock_buffer(c, emit, plan, k);
        }
        if (physical->part != WRIT_PART_WHOLE) {
            continue;
        }
        writ_str_t target = writ_c_type(c, slot->type->target, false);
        writ_emit_open(emit, "if (%.*s)", S(slot->name));
        if (item->with_item != WRIT_PLAN_NONE) {
            writ_str_t with = fn->slots[plan->items[item->with_item].contract_slot].name;
            writ_emit_line(emit, "*%.*s = (%.*s)(%.*s + %d);", S(slot->name), S(target), S(with), TEXT_OFFSET);
        } else if (is_pointer_type(slot->type->target)) {
            writ_emit_line(emit, "*%.*s = (%.*s)(void *)%.*s_mock_object;", S(slot->name), S(target), S(c->prefix));
        } else {
            writ_emit_line(emit, "*%.*s = (%.*s)%d;", S(slot->name), S(target), OUT_VALUE);
        }
        writ_emit_close(emit, nullptr);
    }
}

static void
mock_details(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan)
{
    const writ_model_t *model = c->model;
    for (size_t o = 0; o < plan->outcomes_count; o++) {
        uint32_t via = plan->outcomes[o].via_expr;
        writ_ref_t ref = writ_expr_ref(model, via);
        if (!via || ref.kind != WRIT_REF_FIELD) {
            continue;
        }
        writ_ref_t owner = writ_expr_ref(model, writ_node_child(model->unit, via, 0));
        writ_str_t base = writ_sym_str(model->session, model->slots.data[owner.index].name);
        writ_str_t field = writ_sym_str(model->session, model->slots.data[ref.index].name);
        writ_emit_open(emit, "if (%.*s)", S(base));
        writ_emit_line(emit, "%.*s->%.*s = \"mock detail\";", S(base), S(field));
        writ_emit_close(emit, nullptr);
    }
    for (size_t i = 0; i < plan->reset_channels_count; i++) {
        writ_emit_line(emit, "%.*s = (int)%.*s_mock_channel;", S(channel_lvalue(c, plan->reset_channels[i])),
                       S(c->prefix));
    }
}

static void
mock_return(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan)
{
    const writ_fn_view_t *fn = plan->fn;
    if (!fn->result) {
        return;
    }
    const writ_type_view_t *type = fn->result->type;
    writ_str_t spelled = writ_c_type(c, type, writ_c_is_text(type) && !fn->result->own);
    if (fn->is_pure && fn->slots_count) {
        writ_emit_line(emit, "return (%.*s)(%.*s + %d);", S(spelled), S(fn->slots[0].name), PURE_OFFSET);
    } else if (fn->sizing == WRIT_SIZING_QUERY) {
        writ_emit_line(emit, "return (%.*s)%.*s_mock_fill;", S(spelled), S(c->prefix));
    } else if (writ_c_is_text(type)) {
        writ_emit_line(emit, "return %.*s_mock_ret ? (%.*s)\"mock text\" : NULL;", S(c->prefix), S(spelled));
    } else if (is_pointer_type(type)) {
        writ_emit_line(emit, "return %.*s_mock_ret ? (%.*s)(void *)%.*s_mock_object : NULL;", S(c->prefix), S(spelled),
                       S(c->prefix));
    } else {
        writ_emit_line(emit, "return (%.*s)%.*s_mock_ret;", S(spelled), S(c->prefix));
    }
}

static void
emit_mock_fn(const writ_c_context_t *c, writ_emit_t *emit, uint32_t index)
{
    const writ_fn_view_t *fn = writ_model_fn_at(c->model, index);
    const writ_plan_view_t *plan = writ_model_fn_plan(c->model, fn);
    writ_emit_line(emit, "#undef %.*s", S(fn->link_symbol));
    writ_c_emit_real_prototype(c, emit, fn, fn->link_symbol, false, "");
    writ_emit_open(emit, "");
    mock_record(c, emit, plan, index);
    mock_varargs(c, emit, plan, index);
    mock_outputs(c, emit, plan);
    mock_details(c, emit, plan);
    mock_return(c, emit, plan);
    writ_emit_close(emit, nullptr);
}

static void
emit_mock_globals(const writ_c_context_t *c, writ_emit_t *emit, const char *storage)
{
    writ_emit_line(emit, "%slong long %.*s_mock_ret;", storage, S(c->prefix));
    writ_emit_line(emit, "%slong long %.*s_mock_channel;", storage, S(c->prefix));
    writ_emit_line(emit, "%ssize_t %.*s_mock_fill;", storage, S(c->prefix));
    writ_emit_line(emit, "%schar %.*s_mock_object[64];", storage, S(c->prefix));
    writ_emit_line(emit, "%slong long %.*s_mock_seen[%zu][%d];", storage, S(c->prefix),
                   writ_model_fn_count(c->model) ? writ_model_fn_count(c->model) : 1, SEEN_SLOTS);
}

static writ_str_t
mock_text(const writ_c_context_t *c)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_line(&emit, "#include \"%.*s_writ.h\"", S(c->prefix));
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#include <string.h>");
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#pragma clang diagnostic ignored \"-Wtautological-pointer-compare\"");
    writ_emit_line(&emit, "#pragma clang diagnostic ignored \"-Wpointer-bool-conversion\"");
    writ_emit_line(&emit, "");
    emit_mock_globals(c, &emit, "");
    for (uint32_t i = 0; i < writ_model_fn_count(c->model); i++) {
        if (is_mocked(c->model, writ_model_fn_at(c->model, i))) {
            writ_emit_line(&emit, "");
            emit_mock_fn(c, &emit, i);
        }
    }
    return writ_emit_finish(&emit, c->model->session);
}

static void
expect(writ_emit_t *emit, const writ_plan_view_t *plan, const char *scenario, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    core_buffer_t *condition = core_buffer_empty();
    core_buffer_vprintf(condition, format, args);
    va_end(args);
    core_buffer_t *quoted = core_buffer_empty();
    for (size_t i = 0; i < condition->byte_len; i++) {
        if (condition->data[i] == '"' || condition->data[i] == '\\') {
            core_buffer_putc(quoted, '\\');
        }
        core_buffer_putc(quoted, condition->data[i]);
    }
    writ_emit_line(emit, "expect(%.*s, \"%.*s\", \"%s\", \"%.*s\");", (int)condition->byte_len, condition->data,
                   S(plan->fn->contract_name), scenario, (int)quoted->byte_len, quoted->data);
    core_buffer_free(condition);
    core_buffer_free(quoted);
}

static bool
is_capacity_input(const writ_plan_view_t *plan, uint32_t item)
{
    for (size_t i = 0; i < plan->locals_count; i++) {
        if (plan->locals[i].capacity_item == item) {
            return true;
        }
    }
    return false;
}

static void
input_array(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_item_view_t *item, const scenario_t *s)
{
    writ_str_t element = writ_c_type(c, item->type->target, false);
    writ_emit_line(emit, "uint8_t storage_%.*s[%d * sizeof(%.*s)];", S(item->name), INPUT_LENGTH, S(element));
    writ_emit_open(emit, "for (size_t i = 0; i < sizeof storage_%.*s; i++)", S(item->name));
    writ_emit_line(emit, "storage_%.*s[i] = (uint8_t)(i * 7 + 3);", S(item->name));
    writ_emit_close(emit, nullptr);
    writ_emit_line(emit, "const %.*s *in_%.*s = %s;", S(element), S(item->name),
                   s->empty ? "NULL"
                            : writ_str_format(c->model->session, "(const void *)storage_%.*s", S(item->name)).data);
    writ_emit_line(emit, "size_t len_%.*s = %d;", S(item->name), s->empty ? 0 : INPUT_LENGTH);
}

static void
emit_inputs(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan, const scenario_t *s)
{
    for (uint32_t i = 0; i < plan->items_count; i++) {
        const writ_plan_item_view_t *item = &plan->items[i];
        if (item->role == WRIT_ROLE_RESULT || item->role == WRIT_ROLE_ERROR_DETAIL) {
            continue;
        }
        switch (item->shape) {
        case WRIT_SHAPE_BYTES:
        case WRIT_SHAPE_ARRAY:
            input_array(c, emit, item, s);
            break;
        case WRIT_SHAPE_TEXT:
            writ_emit_line(emit, "const char *in_%.*s = \"text\";", S(item->name));
            writ_emit_line(emit, "size_t len_%.*s = 4;", S(item->name));
            break;
        case WRIT_SHAPE_REST_PACK:
            if (writ_c_is_text(item->type)) {
                writ_emit_line(emit, "%.*s_str_t in_%.*s[2] = {{\"a\", 1}, {\"bc\", 2}};", S(c->prefix), S(item->name));
            } else {
                writ_emit_line(emit, "%.*s in_%.*s[2] = {7, 9};", S(writ_c_type(c, item->type, false)), S(item->name));
            }
            writ_emit_line(emit, "size_t len_%.*s = %d;", S(item->name), s->arity);
            break;
        case WRIT_SHAPE_RECORD: {
            writ_str_t record = writ_c_type(c, item->type->target, false);
            writ_emit_line(emit, "%.*s record_%.*s;", S(record), S(item->name));
            writ_emit_line(emit, "memset(&record_%.*s, 0, sizeof record_%.*s);", S(item->name), S(item->name));
            writ_emit_line(emit, "%.*s *in_%.*s = &record_%.*s;", S(record), S(item->name), S(item->name));
            break;
        }
        default: {
            writ_str_t type = writ_c_type(c, item->type, false);
            if (is_pointer_type(item->type)) {
                writ_emit_line(emit, "%.*s in_%.*s = (%.*s)(void *)%.*s_mock_object;", S(type), S(item->name), S(type),
                               S(c->prefix));
            } else {
                writ_emit_line(emit, "%.*s in_%.*s = %d;", S(type), S(item->name),
                               is_capacity_input(plan, i) ? CAPACITY_INPUT : SCALAR_INPUT);
            }
            break;
        }
        }
    }
}

static void
emit_call(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan)
{
    bool direct = writ_c_returns_directly(plan);
    for (uint32_t i = 0; !direct && i < plan->items_count; i++) {
        const writ_plan_item_view_t *item = &plan->items[i];
        if (item->role != WRIT_ROLE_RESULT && item->role != WRIT_ROLE_ERROR_DETAIL) {
            continue;
        }
        writ_c_param_t param;
        writ_c_item_params(c, plan, i, &param);
        size_t pointee = param.type.length - 1;
        while (pointee && param.type.data[pointee - 1] == ' ') {
            pointee--;
        }
        writ_emit_line(emit, "%.*s out_%.*s;", (int)pointee, param.type.data, S(item->name));
        writ_emit_line(emit, "memset(&out_%.*s, 0, sizeof out_%.*s);", S(item->name), S(item->name));
    }
    writ_str_t ret = writ_c_return_type(c, plan);
    writ_emit_text(emit, "%.*s got = %.*s_%.*s(", S(ret), S(c->prefix), S(plan->fn->contract_name));
    uint32_t *order = writ_alloc(c->model->session, sizeof(uint32_t) * (plan->items_count + 1));
    size_t ordered = writ_c_param_order(plan, order);
    for (size_t o = 0; o < ordered; o++) {
        const writ_plan_item_view_t *item = &plan->items[order[o]];
        bool input = writ_c_is_input(item);
        writ_c_param_t params[2];
        size_t count = writ_c_item_params(c, plan, order[o], params);
        writ_emit_text(emit, "%s%s%s_%.*s", o ? ", " : "", input ? "" : "&", input ? "in" : "out", S(item->name));
        if (count == 2) {
            writ_emit_text(emit, ", len_%.*s", S(item->name));
        }
    }
    writ_emit_line(emit, ");");
}

static void
check_slot(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan, uint32_t index, uint32_t k,
           const scenario_t *s)
{
    const writ_plan_slot_view_t *slot = &plan->slots[k];
    const writ_plan_item_view_t *item = &plan->items[slot->item];
    writ_str_t seen = seen_at(c, index, k);
    if (item->role == WRIT_ROLE_RESULT || item->role == WRIT_ROLE_ERROR_DETAIL) {
        return;
    }
    if (item->shape == WRIT_SHAPE_TEXT && slot->part == WRIT_PART_WHOLE) {
        expect(emit, plan, s->name, "%.*s == 3", S(seen));
    } else if (slot->part == WRIT_PART_POINTER) {
        expect(emit, plan, s->name, "%.*s == %d", S(seen), s->empty && item->nullable ? 0 : 1);
    } else if (slot->part == WRIT_PART_LENGTH) {
        expect(emit, plan, s->name, "%.*s == %d", S(seen),
               item->role == WRIT_ROLE_PACK ? s->arity : s->empty ? 0 : INPUT_LENGTH);
    } else if (slot->part == WRIT_PART_WHOLE && is_value_type(item->type)) {
        expect(emit, plan, s->name, "%.*s == (long long)in_%.*s", S(seen), S(item->name));
    } else if (slot->part == WRIT_PART_WHOLE) {
        expect(emit, plan, s->name, "%.*s == 1", S(seen));
    }
}

static void
emit_seen_checks(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan, uint32_t index,
                 const scenario_t *s)
{
    const writ_fn_view_t *fn = plan->fn;
    if (!is_mocked(c->model, fn)) {
        return;
    }
    for (uint32_t k = 0; k < fn->slots_count; k++) {
        if (plan->slots[k].item != WRIT_PLAN_NONE && plan->slots[k].pass != WRIT_PASS_VARIADIC_TAIL) {
            check_slot(c, emit, plan, index, k, s);
        }
    }
    if (fn->format_slot != WRIT_PLAN_NONE) {
        expect(emit, plan, s->name, "%.*s_mock_seen[%u][%d] == 1", S(c->prefix), index, SEEN_FORMAT);
        return;
    }
    if (fn->varargs != WRIT_VARARGS_UNTIL && fn->varargs != WRIT_VARARGS_COUNT) {
        return;
    }
    expect(emit, plan, s->name, "%.*s_mock_seen[%u][%d] == %d", S(c->prefix), index, SEEN_COUNT, s->arity);
    for (int k = 0; k < s->arity; k++) {
        if (writ_c_is_text(fn->varargs_type)) {
            expect(emit, plan, s->name, "%.*s_mock_seen[%u][%d] == 1", S(c->prefix), index, SEEN_PACK + k);
        } else {
            expect(emit, plan, s->name, "%.*s_mock_seen[%u][%d] == (long long)in_args[%d]", S(c->prefix), index,
                   SEEN_PACK + k, k);
        }
    }
}

static int
expected_capacity(const writ_plan_view_t *plan, uint32_t item, int fill)
{
    for (size_t l = 0; l < plan->locals_count; l++) {
        const writ_plan_local_view_t *local = &plan->locals[l];
        if (local->item == item && local->kind == WRIT_LOCAL_BUFFER) {
            if (local->capacity_item != WRIT_PLAN_NONE) {
                return CAPACITY_INPUT;
            }
            return plan->query_count ? fill + 1 : INPUT_LENGTH + PURE_OFFSET;
        }
    }
    return 0;
}

static void
check_detail(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan, uint32_t i,
             const scenario_t *s)
{
    const writ_plan_item_view_t *item = &plan->items[i];
    bool fetched = false;
    for (size_t k = 0; k < plan->steps_count; k++) {
        fetched = fetched || (plan->steps[k].item == i && plan->steps[k].outcome == s->outcome);
    }
    if (!fetched) {
        return;
    }
    writ_ref_t via = writ_expr_ref(c->model, plan->outcomes[s->outcome].via_expr);
    if (via.kind == WRIT_REF_FIELD) {
        expect(emit, plan, s->name, "out_%.*s && !strcmp(out_%.*s, \"mock detail\")", S(item->name), S(item->name));
    } else if (via.kind == WRIT_REF_CHANNEL) {
        expect(emit, plan, s->name, "out_%.*s == %lld", S(item->name), (long long)s->channel);
    } else if (via.kind == WRIT_REF_RESULT) {
        expect(emit, plan, s->name, "out_%.*s == %lld", S(item->name), (long long)s->ret);
    }
}

static void
check_result(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan, uint32_t i,
             const scenario_t *s)
{
    const writ_plan_item_view_t *item = &plan->items[i];
    writ_step_kind_t lift = writ_c_lift_kind(plan, i);
    if (lift == WRIT_STEP_LIFT_BUFFER) {
        int capacity = expected_capacity(plan, i, s->fill);
        expect(emit, plan, s->name, "out_%.*s.length == %d", S(item->name), s->fill < capacity ? s->fill : capacity);
        expect(emit, plan, s->name, "matches_pattern(out_%.*s.data, out_%.*s.length)", S(item->name), S(item->name));
        writ_emit_line(emit, "%.*s_bytes_free(&out_%.*s);", S(c->prefix), S(item->name));
    } else if (lift == WRIT_STEP_COPY_BORROWED) {
        expect(emit, plan, s->name, "out_%.*s.data && !strcmp((const char *)out_%.*s.data, \"mock text\")",
               S(item->name), S(item->name));
        writ_emit_line(emit, "%.*s_bytes_free(&out_%.*s);", S(c->prefix), S(item->name));
    } else if (writ_c_offset_result(plan, i)) {
        expect(emit, plan, s->name, "out_%.*s == %d", S(item->name), TEXT_OFFSET);
    } else if (writ_c_is_text(item->type)) {
        expect(emit, plan, s->name, "out_%.*s && !strcmp(out_%.*s, \"mock text\")", S(item->name), S(item->name));
    } else if (is_pointer_type(item->type)) {
        expect(emit, plan, s->name, "(void *)out_%.*s == (void *)%.*s_mock_object", S(item->name), S(c->prefix));
    } else if (item->contract_slot == WRIT_PLAN_NONE) {
        long long expected = plan->fn->is_pure ? SCALAR_INPUT + PURE_OFFSET : (long long)s->ret;
        expect(emit, plan, s->name, "out_%.*s == (%.*s)%lld", S(item->name), S(writ_c_type(c, item->type, false)),
               expected);
    } else {
        expect(emit, plan, s->name, "out_%.*s == %d", S(item->name), OUT_VALUE);
    }
}

static void
emit_direct_check(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan, const scenario_t *s)
{
    const writ_fn_view_t *fn = plan->fn;
    if (fn->is_pure) {
        expect(emit, plan, s->name, "got == %d", SCALAR_INPUT + PURE_OFFSET);
    } else if (writ_c_is_text(fn->result->type)) {
        expect(emit, plan, s->name, "got && !strcmp(got, \"mock text\")");
    } else if (is_pointer_type(fn->result->type)) {
        expect(emit, plan, s->name, "(void *)got == (void *)%.*s_mock_object", S(c->prefix));
    } else {
        expect(emit, plan, s->name, "got == %lld", (long long)s->ret);
    }
}

static void
emit_scenario(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan, uint32_t index,
              const scenario_t *s)
{
    writ_emit_open(emit, "");
    writ_emit_line(emit, "memset(%.*s_mock_seen[%u], 0, sizeof %.*s_mock_seen[%u]);", S(c->prefix), index,
                   S(c->prefix), index);
    writ_emit_line(emit, "%.*s_mock_ret = %lld;", S(c->prefix), (long long)s->ret);
    writ_emit_line(emit, "%.*s_mock_channel = %lld;", S(c->prefix), (long long)s->channel);
    writ_emit_line(emit, "%.*s_mock_fill = %d;", S(c->prefix), s->fill);
    emit_inputs(c, emit, plan, s);
    emit_call(c, emit, plan);
    if (writ_c_returns_directly(plan)) {
        emit_direct_check(c, emit, plan, s);
    } else {
        expect(emit, plan, s->name, "got == %u", s->outcome);
        bool success = plan->outcomes[s->outcome].kind == WRIT_OUTCOME_SUCCESS;
        for (uint32_t i = 0; i < plan->items_count; i++) {
            if (plan->items[i].role == WRIT_ROLE_ERROR_DETAIL) {
                check_detail(c, emit, plan, i, s);
            } else if (plan->items[i].role == WRIT_ROLE_RESULT && success) {
                check_result(c, emit, plan, i, s);
            }
        }
    }
    emit_seen_checks(c, emit, plan, index, s);
    writ_emit_close(emit, nullptr);
}

static bool
has_shape(const writ_plan_view_t *plan, writ_plan_shape_t shape, writ_plan_role_t role)
{
    for (size_t i = 0; i < plan->items_count; i++) {
        if (plan->items[i].shape == shape && plan->items[i].role == role) {
            return true;
        }
    }
    return false;
}

static void
emit_driver_fn(const writ_c_context_t *c, writ_emit_t *emit, uint32_t index)
{
    const writ_fn_view_t *fn = writ_model_fn_at(c->model, index);
    const writ_plan_view_t *plan = writ_model_fn_plan(c->model, fn);
    writ_emit_open(emit, "static void test_%.*s(void)", S(fn->contract_name));
    bool pack = has_shape(plan, WRIT_SHAPE_REST_PACK, WRIT_ROLE_PACK);
    for (uint32_t o = 0; is_mocked(c->model, fn) && o < plan->outcomes_count; o++) {
        int64_t ret = SCALAR_INPUT + PURE_OFFSET;
        int64_t channel = 0;
        if (!writ_c_returns_directly(plan) && !find_outcome(c->model, plan, o, &ret, &channel)) {
            continue;
        }
        if (fn->sizing == WRIT_SIZING_QUERY) {
            ret = FILL_LONG;
        }
        const char *name = writ_str_format(c->model->session, "%.*s", S(plan->outcomes[o].name)).data;
        scenario_t full = {name, o, ret, channel, FILL_LONG, false, pack ? 2 : 0};
        emit_scenario(c, emit, plan, index, &full);
        if (plan->outcomes[o].kind != WRIT_OUTCOME_SUCCESS) {
            continue;
        }
        if (has_shape(plan, WRIT_SHAPE_BYTES, WRIT_ROLE_RESULT) || has_shape(plan, WRIT_SHAPE_TEXT, WRIT_ROLE_RESULT)) {
            scenario_t shorter = {"short", o, fn->sizing == WRIT_SIZING_QUERY ? FILL_SHORT : ret, channel, FILL_SHORT,
                                  false, full.arity};
            emit_scenario(c, emit, plan, index, &shorter);
        }
        if (has_shape(plan, WRIT_SHAPE_BYTES, WRIT_ROLE_INPUT) || has_shape(plan, WRIT_SHAPE_ARRAY, WRIT_ROLE_INPUT)) {
            scenario_t empty = {"empty", o, ret, channel, FILL_LONG, true, full.arity};
            emit_scenario(c, emit, plan, index, &empty);
        }
        if (pack) {
            scenario_t none = {"no arguments", o, ret, channel, FILL_LONG, false, 0};
            emit_scenario(c, emit, plan, index, &none);
        }
    }
    writ_emit_close(emit, nullptr);
}

static writ_str_t
driver_text(const writ_c_context_t *c)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_line(&emit, "#include \"%.*s_writ.h\"", S(c->prefix));
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#include <stdio.h>");
    writ_emit_line(&emit, "#include <string.h>");
    writ_emit_line(&emit, "");
    emit_mock_globals(c, &emit, "extern ");
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "static int failures;");
    writ_emit_line(&emit, "");
    writ_emit_open(&emit, "static void expect(bool ok, const char *fn, const char *scenario, const char *what)");
    writ_emit_open(&emit, "if (!ok)");
    writ_emit_line(&emit, "fprintf(stderr, \"FAIL %%s %%s: %%s\\n\", fn, scenario, what);");
    writ_emit_line(&emit, "failures++;");
    writ_emit_close(&emit, nullptr);
    writ_emit_close(&emit, nullptr);
    writ_emit_line(&emit, "");
    writ_emit_open(&emit, "static bool matches_pattern(const uint8_t *data, size_t length)");
    writ_emit_open(&emit, "for (size_t i = 0; i < length; i++)");
    writ_emit_open(&emit, "if (data[i] != (uint8_t)(i * 7 + 3))");
    writ_emit_line(&emit, "return false;");
    writ_emit_close(&emit, nullptr);
    writ_emit_close(&emit, nullptr);
    writ_emit_line(&emit, "return true;");
    writ_emit_close(&emit, nullptr);
    for (uint32_t i = 0; i < writ_model_fn_count(c->model); i++) {
        writ_emit_line(&emit, "");
        emit_driver_fn(c, &emit, i);
    }
    writ_emit_line(&emit, "");
    writ_emit_open(&emit, "int main(void)");
    for (size_t i = 0; i < writ_model_fn_count(c->model); i++) {
        writ_emit_line(&emit, "test_%.*s();", S(writ_model_fn_at(c->model, i)->contract_name));
    }
    writ_emit_open(&emit, "if (!failures)");
    writ_emit_line(&emit, "printf(\"conformance ok\\n\");");
    writ_emit_close(&emit, nullptr);
    writ_emit_line(&emit, "return failures != 0;");
    writ_emit_close(&emit, nullptr);
    return writ_emit_finish(&emit, c->model->session);
}

writ_status_t
writ_gen_conformance(const writ_c_context_t *c, const writ_output_t *output)
{
    writ_session_t *session = c->model->session;
    writ_status_t status = output->write_file(output->context, writ_str_format(session, "%.*s_writ_mock.c", S(c->prefix)),
                                              mock_text(c));
    if (status == WRIT_OK) {
        status = output->write_file(output->context,
                                    writ_str_format(session, "%.*s_writ_conformance.c", S(c->prefix)), driver_text(c));
    }
    return status;
}
