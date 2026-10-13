#include "gen.h"

#define S(text) (int)(text).length, (text).data

enum {
    INPUT_LENGTH    = 16,
    FILL_LONG       = 8,
    FILL_SHORT      = 3,
    CAPACITY_INPUT  = 24,
    SCALAR_INPUT    = 3,
    PURE_OFFSET     = 16,
    SEEN_SLOTS      = 32,
};

typedef struct {
    writ_expr_env_t base;
    int64_t         candidate;
} outcome_env_t;

static bool
outcome_value(writ_expr_env_t *base, writ_ref_t ref, int64_t *out)
{
    if (ref.kind != WRIT_REF_RESULT) {
        return false;
    }
    *out = ((outcome_env_t *)base)->candidate;
    return true;
}

static uint32_t
outcome_for(const writ_model_t *model, const writ_plan_view_t *plan, int64_t candidate)
{
    outcome_env_t env = {{.value = outcome_value}, candidate};
    for (uint32_t i = 0; i < plan->outcomes_count; i++) {
        int64_t holds = 0;
        if (!plan->outcomes[i].when_expr
            || (writ_expr_eval(model, plan->outcomes[i].when_expr, &env.base, &holds) && holds)) {
            return i;
        }
    }
    return WRIT_PLAN_NONE;
}

static bool
find_ret(const writ_model_t *model, const writ_plan_view_t *plan, writ_outcome_kind_t kind, int64_t *value)
{
    int64_t candidates[64] = {0, 1, -1, 2, -3, 5};
    size_t count = 6;
    for (size_t i = 0; i < writ_model_const_count(model) && count < 64; i++) {
        candidates[count++] = writ_model_const_at(model, i)->value;
    }
    for (size_t i = 0; i < count; i++) {
        uint32_t outcome = outcome_for(model, plan, candidates[i]);
        if (outcome != WRIT_PLAN_NONE && plan->outcomes[outcome].kind == kind) {
            *value = candidates[i];
            return true;
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

static bool
is_scalar_type(const writ_type_view_t *type)
{
    return type->kind == WRIT_TYPE_INT || type->kind == WRIT_TYPE_BOOL || type->kind == WRIT_TYPE_FLOAT;
}

static void
emit_mock_fn(const writ_c_context_t *c, writ_emit_t *emit, uint32_t index)
{
    const writ_fn_view_t *fn = writ_model_fn_at(c->model, index);
    const writ_plan_view_t *plan = writ_model_fn_plan(c->model, fn);
    writ_c_emit_real_prototype(c, emit, fn, "");
    writ_emit_open(emit, "");
    for (uint32_t k = 0; k < fn->slots_count; k++) {
        const writ_slot_view_t *slot = &fn->slots[k];
        const writ_plan_slot_view_t *physical = &plan->slots[k];
        if (is_scalar_type(slot->type)) {
            writ_emit_line(emit, "%.*s_mock_seen[%u][%u] = (long long)%.*s;", S(c->prefix), index, k, S(slot->name));
        } else if (physical->part == WRIT_PART_CAPACITY_AND_LENGTH) {
            writ_emit_line(emit, "%.*s_mock_seen[%u][%u] = (long long)*%.*s;", S(c->prefix), index, k, S(slot->name));
        } else {
            writ_emit_line(emit, "%.*s_mock_seen[%u][%u] = %.*s != NULL;", S(c->prefix), index, k, S(slot->name));
        }
    }
    for (uint32_t k = 0; k < fn->slots_count; k++) {
        const writ_plan_slot_view_t *physical = &plan->slots[k];
        if (physical->part != WRIT_PART_POINTER || physical->item == WRIT_PLAN_NONE) {
            continue;
        }
        const writ_slot_view_t *slot = &fn->slots[k];
        uint32_t length = slot_with(plan, physical->item, WRIT_PART_LENGTH);
        uint32_t cell = slot_with(plan, physical->item, WRIT_PART_CAPACITY_AND_LENGTH);
        if (length != WRIT_PLAN_NONE) {
            writ_emit_open(emit, "for (size_t i = 0; i < (size_t)%.*s; i++)", S(fn->slots[length].name));
            writ_emit_line(emit, "%.*s_mock_seen[%u][%u] |= (%.*s[i] != (uint8_t)(i * 7 + 3)) << 1;", S(c->prefix),
                           index, k, S(slot->name));
            writ_emit_close(emit, nullptr);
        } else if (cell != WRIT_PLAN_NONE) {
            writ_str_t cell_name = fn->slots[cell].name;
            writ_emit_line(emit, "size_t written = %.*s_mock_fill < (size_t)*%.*s ? %.*s_mock_fill : (size_t)*%.*s;",
                           S(c->prefix), S(cell_name), S(c->prefix), S(cell_name));
            writ_emit_open(emit, "for (size_t i = 0; i < written; i++)");
            writ_emit_line(emit, "%.*s[i] = (uint8_t)(i * 7 + 3);", S(slot->name));
            writ_emit_close(emit, nullptr);
            writ_str_t cell_type = writ_c_type(c, fn->slots[cell].type->target, false);
            writ_emit_line(emit, "*%.*s = (%.*s)written;", S(cell_name), S(cell_type));
        }
    }
    if (fn->result && fn->result->kind != WRIT_TYPE_VOID) {
        writ_str_t type = writ_c_type(c, fn->result, false);
        if (fn->is_pure && fn->slots_count) {
            writ_emit_line(emit, "return (%.*s)(%.*s + %d);", S(type), S(fn->slots[0].name), PURE_OFFSET);
        } else {
            writ_emit_line(emit, "return (%.*s)%.*s_mock_ret;", S(type), S(c->prefix));
        }
    }
    writ_emit_close(emit, nullptr);
}

static void
emit_mock_globals(const writ_c_context_t *c, writ_emit_t *emit, const char *storage)
{
    writ_emit_line(emit, "%slong long %.*s_mock_ret;", storage, S(c->prefix));
    writ_emit_line(emit, "%ssize_t %.*s_mock_fill;", storage, S(c->prefix));
    writ_emit_line(emit, "%slong long %.*s_mock_seen[%zu][%d];", storage, S(c->prefix),
                   writ_model_fn_count(c->model) ? writ_model_fn_count(c->model) : 1, SEEN_SLOTS);
}

static writ_str_t
mock_text(const writ_c_context_t *c)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_line(&emit, "#include <stdbool.h>");
    writ_emit_line(&emit, "#include <stddef.h>");
    writ_emit_line(&emit, "#include <stdint.h>");
    writ_emit_line(&emit, "");
    emit_mock_globals(c, &emit, "");
    for (uint32_t i = 0; i < writ_model_fn_count(c->model); i++) {
        writ_emit_line(&emit, "");
        emit_mock_fn(c, &emit, i);
    }
    return writ_emit_finish(&emit, c->model->session);
}

typedef struct {
    writ_expr_env_t         base;
    const writ_c_context_t *c;
    const writ_plan_view_t *plan;
} driver_env_t;

static void
driver_ref(writ_expr_env_t *base, writ_ref_t ref, core_buffer_t *out)
{
    driver_env_t *env = (driver_env_t *)base;
    const writ_plan_slot_view_t *physical = &env->plan->slots[writ_plan_contract_slot(env->c->model, env->plan, ref)];
    writ_str_t name = env->plan->items[physical->item].name;
    if (physical->part == WRIT_PART_LENGTH) {
        core_buffer_printf(out, "len_%.*s", S(name));
    } else {
        core_buffer_printf(out, "in_%.*s", S(name));
    }
}

static void
emit_inputs(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan, bool empty)
{
    for (size_t i = 0; i < plan->items_count; i++) {
        const writ_plan_item_view_t *item = &plan->items[i];
        if (item->role != WRIT_ROLE_INPUT) {
            continue;
        }
        if (item->shape == WRIT_SHAPE_BYTES) {
            writ_emit_line(emit, "uint8_t storage_%.*s[%d];", S(item->name), INPUT_LENGTH);
            writ_emit_open(emit, "for (size_t i = 0; i < %d; i++)", INPUT_LENGTH);
            writ_emit_line(emit, "storage_%.*s[i] = (uint8_t)(i * 7 + 3);", S(item->name));
            writ_emit_close(emit, nullptr);
            writ_emit_line(emit, "const uint8_t *in_%.*s = %s;", S(item->name),
                           empty ? "NULL" : writ_str_format(c->model->session, "storage_%.*s", S(item->name)).data);
            writ_emit_line(emit, "size_t len_%.*s = %d;", S(item->name), empty ? 0 : INPUT_LENGTH);
        } else {
            bool capacity = item->contract_slot != WRIT_PLAN_NONE && plan->slots[item->contract_slot].part
                                                                    == WRIT_PART_CAPACITY_AND_LENGTH;
            writ_str_t type = writ_c_type(c, item->type, false);
            writ_emit_line(emit, "%.*s in_%.*s = %d;", S(type), S(item->name), capacity ? CAPACITY_INPUT : SCALAR_INPUT);
        }
    }
}

static void
emit_call(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan)
{
    bool direct = writ_c_returns_directly(plan);
    for (size_t i = 0; !direct && i < plan->items_count; i++) {
        const writ_plan_item_view_t *item = &plan->items[i];
        if (item->role == WRIT_ROLE_RESULT && item->shape == WRIT_SHAPE_BYTES) {
            writ_emit_line(emit, "%.*s_bytes_t out_%.*s = {0};", S(c->prefix), S(item->name));
        } else if (item->role == WRIT_ROLE_RESULT || item->role == WRIT_ROLE_ERROR_DETAIL) {
            writ_str_t type = writ_c_type(c, item->type, false);
            writ_emit_line(emit, "%.*s out_%.*s = 0;", S(type), S(item->name));
        }
    }
    writ_str_t ret = direct ? writ_c_type(c, plan->items[plan->items_count - 1].type, false) : writ_cstr("int32_t");
    writ_emit_text(emit, "%.*s got = %.*s_%.*s(", S(ret), S(c->prefix), S(plan->fn->contract_name));
    bool first = true;
    for (size_t i = 0; i < plan->items_count; i++) {
        const writ_plan_item_view_t *item = &plan->items[i];
        if (item->role != WRIT_ROLE_INPUT) {
            continue;
        }
        writ_emit_text(emit, "%sin_%.*s", first ? "" : ", ", S(item->name));
        if (item->shape == WRIT_SHAPE_BYTES) {
            writ_emit_text(emit, ", len_%.*s", S(item->name));
        }
        first = false;
    }
    for (size_t i = 0; !direct && i < plan->items_count; i++) {
        const writ_plan_item_view_t *item = &plan->items[i];
        if (item->role == WRIT_ROLE_RESULT || item->role == WRIT_ROLE_ERROR_DETAIL) {
            writ_emit_text(emit, "%s&out_%.*s", first ? "" : ", ", S(item->name));
            first = false;
        }
    }
    writ_emit_line(emit, ");");
}

static void
expect(writ_emit_t *emit, const writ_plan_view_t *plan, const char *scenario, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    core_buffer_t *condition = core_buffer_empty();
    core_buffer_vprintf(condition, format, args);
    va_end(args);
    writ_emit_line(emit, "expect(%.*s, \"%.*s\", \"%s\", \"%.*s\");", (int)condition->byte_len, condition->data,
                   S(plan->fn->contract_name), scenario, (int)condition->byte_len, condition->data);
    core_buffer_free(condition);
}

static void
emit_seen_checks(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan, uint32_t index,
                 const char *scenario, bool empty)
{
    driver_env_t env = {{.print_ref = driver_ref}, c, plan};
    for (uint32_t k = 0; k < plan->slots_count; k++) {
        const writ_plan_slot_view_t *slot = &plan->slots[k];
        if (slot->item == WRIT_PLAN_NONE) {
            continue;
        }
        const writ_plan_item_view_t *item = &plan->items[slot->item];
        if (slot->part == WRIT_PART_POINTER) {
            expect(emit, plan, scenario, "%.*s_mock_seen[%u][%u] == 1", S(c->prefix), index, k);
        } else if (slot->part == WRIT_PART_LENGTH) {
            expect(emit, plan, scenario, "%.*s_mock_seen[%u][%u] == %d", S(c->prefix), index, k,
                   empty ? 0 : INPUT_LENGTH);
        } else if (slot->part == WRIT_PART_CAPACITY_AND_LENGTH) {
            const writ_plan_local_view_t *buffer = nullptr;
            for (size_t l = 0; l < plan->locals_count; l++) {
                if (plan->locals[l].item == slot->item && plan->locals[l].kind == WRIT_LOCAL_BUFFER) {
                    buffer = &plan->locals[l];
                }
            }
            core_buffer_t *capacity = core_buffer_empty();
            if (buffer->capacity_item != WRIT_PLAN_NONE) {
                core_buffer_printf(capacity, "in_%.*s", S(plan->items[buffer->capacity_item].name));
            } else {
                writ_expr_print(c->model, buffer->capacity_expr, WRIT_LANGUAGE_C, &env.base, capacity);
            }
            expect(emit, plan, scenario, "%.*s_mock_seen[%u][%u] == (long long)(%.*s)", S(c->prefix), index, k,
                   (int)capacity->byte_len, capacity->data);
            core_buffer_free(capacity);
        } else if (slot->part == WRIT_PART_WHOLE && item->role == WRIT_ROLE_INPUT) {
            expect(emit, plan, scenario, "%.*s_mock_seen[%u][%u] == (long long)in_%.*s", S(c->prefix), index, k,
                   S(item->name));
        }
    }
}

static void
emit_result_checks(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan, const char *scenario,
                   int fill)
{
    for (size_t i = 0; i < plan->items_count; i++) {
        const writ_plan_item_view_t *item = &plan->items[i];
        if (item->role != WRIT_ROLE_RESULT || item->shape != WRIT_SHAPE_BYTES) {
            continue;
        }
        expect(emit, plan, scenario, "out_%.*s.length == %d", S(item->name), fill);
        expect(emit, plan, scenario, "matches_pattern(out_%.*s.data, out_%.*s.length)", S(item->name), S(item->name));
        writ_emit_line(emit, "%.*s_bytes_free(&out_%.*s);", S(c->prefix), S(item->name));
    }
}

static void
emit_scenario(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan, uint32_t index,
              const char *scenario, writ_outcome_kind_t kind, int64_t ret, int fill, bool empty)
{
    writ_emit_open(emit, "");
    writ_emit_line(emit, "memset(%.*s_mock_seen[%u], 0, sizeof(%.*s_mock_seen[%u]));", S(c->prefix), index,
                   S(c->prefix), index);
    writ_emit_line(emit, "%.*s_mock_ret = %lld;", S(c->prefix), (long long)ret);
    writ_emit_line(emit, "%.*s_mock_fill = %d;", S(c->prefix), fill);
    emit_inputs(c, emit, plan, empty);
    emit_call(c, emit, plan);
    uint32_t expected = outcome_for(c->model, plan, ret);
    expect(emit, plan, scenario, "got == %.*s_%.*s_%.*s", S(c->prefix), S(plan->fn->contract_name),
           S(plan->outcomes[expected].name));
    emit_seen_checks(c, emit, plan, index, scenario, empty);
    if (kind == WRIT_OUTCOME_SUCCESS) {
        emit_result_checks(c, emit, plan, scenario, fill);
    }
    for (size_t i = 0; kind == WRIT_OUTCOME_FAILURE && i < plan->items_count; i++) {
        const writ_plan_item_view_t *item = &plan->items[i];
        if (item->role == WRIT_ROLE_ERROR_DETAIL) {
            expect(emit, plan, scenario, "out_%.*s == %lld", S(item->name), (long long)ret);
        } else if (item->role == WRIT_ROLE_RESULT && item->shape == WRIT_SHAPE_BYTES) {
            expect(emit, plan, scenario, "out_%.*s.data == NULL", S(item->name));
        }
    }
    writ_emit_close(emit, nullptr);
}

static void
emit_direct_test(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan)
{
    writ_emit_open(emit, "");
    writ_emit_line(emit, "%.*s_mock_ret = %d;", S(c->prefix), SCALAR_INPUT + PURE_OFFSET);
    emit_inputs(c, emit, plan, false);
    emit_call(c, emit, plan);
    expect(emit, plan, "direct", "got == %d", SCALAR_INPUT + PURE_OFFSET);
    writ_emit_close(emit, nullptr);
}

static void
emit_driver_fn(const writ_c_context_t *c, writ_emit_t *emit, uint32_t index)
{
    const writ_fn_view_t *fn = writ_model_fn_at(c->model, index);
    const writ_plan_view_t *plan = writ_model_fn_plan(c->model, fn);
    writ_emit_open(emit, "static void test_%.*s(void)", S(fn->contract_name));
    if (writ_c_returns_directly(plan)) {
        emit_direct_test(c, emit, plan);
        writ_emit_close(emit, nullptr);
        return;
    }
    int64_t ok = 0;
    int64_t failed = 0;
    if (find_ret(c->model, plan, WRIT_OUTCOME_SUCCESS, &ok)) {
        emit_scenario(c, emit, plan, index, "success", WRIT_OUTCOME_SUCCESS, ok, FILL_LONG, false);
        emit_scenario(c, emit, plan, index, "short", WRIT_OUTCOME_SUCCESS, ok, FILL_SHORT, false);
        emit_scenario(c, emit, plan, index, "empty", WRIT_OUTCOME_SUCCESS, ok, FILL_LONG, true);
    }
    if (find_ret(c->model, plan, WRIT_OUTCOME_FAILURE, &failed)) {
        emit_scenario(c, emit, plan, index, "failure", WRIT_OUTCOME_FAILURE, failed, FILL_LONG, false);
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
    for (size_t i = 0; i < writ_model_fn_count(c->model); i++) {
        if (writ_model_fn_at(c->model, i)->is_pure) {
            writ_c_emit_real_prototype(c, &emit, writ_model_fn_at(c->model, i), ";");
        }
    }
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
