#include <ctype.h>

#include "gen.h"

writ_str_t
writ_c_type(const writ_c_context_t *c, const writ_type_view_t *type, bool constant)
{
    writ_session_t *session = c->model->session;
    if (!type) {
        return writ_cstr("void");
    }
    if (type->kind == WRIT_TYPE_POINTER || type->kind == WRIT_TYPE_ARRAY) {
        writ_str_t target = writ_c_type(c, type->target, false);
        return writ_str_format(session, "%s%.*s *", constant ? "const " : "", (int)target.length, target.data);
    }
    return type->c_spelling;
}

writ_str_t
writ_c_element(const writ_c_context_t *c, const writ_type_view_t *array)
{
    return writ_c_type(c, array->target, false);
}

static size_t
count_role(const writ_plan_view_t *plan, writ_plan_role_t role)
{
    size_t count = 0;
    for (size_t i = 0; i < plan->items_count; i++) {
        count += plan->items[i].role == role;
    }
    return count;
}

bool
writ_c_returns_directly(const writ_plan_view_t *plan)
{
    for (size_t i = 0; i < plan->steps_count; i++) {
        if (plan->steps[i].kind == WRIT_STEP_CHECK || plan->steps[i].kind == WRIT_STEP_ALLOC_OUT) {
            return false;
        }
    }
    return plan->outcomes_count == 1 && count_role(plan, WRIT_ROLE_RESULT) == 1
        && count_role(plan, WRIT_ROLE_ERROR_DETAIL) == 0;
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

static void
emit_param(writ_emit_t *emit, bool *first, writ_str_t type, writ_str_t name, const char *suffix)
{
    writ_emit_text(emit, "%s%.*s%s%.*s%s", *first ? "" : ", ", (int)type.length, type.data,
                   type.length && type.data[type.length - 1] == '*' ? "" : " ", (int)name.length, name.data, suffix);
    *first = false;
}

void
writ_c_emit_signature(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan)
{
    bool direct = writ_c_returns_directly(plan);
    writ_str_t ret = direct ? writ_c_type(c, direct_result(plan)->type, false) : writ_cstr("int32_t");
    writ_emit_text(emit, "%.*s %.*s_%.*s(", (int)ret.length, ret.data, (int)c->prefix.length, c->prefix.data,
                   (int)plan->fn->contract_name.length, plan->fn->contract_name.data);
    bool first = true;
    for (size_t i = 0; i < plan->items_count; i++) {
        const writ_plan_item_view_t *item = &plan->items[i];
        if (item->role != WRIT_ROLE_INPUT) {
            continue;
        }
        if (item->shape == WRIT_SHAPE_BYTES || item->shape == WRIT_SHAPE_ARRAY) {
            emit_param(emit, &first, writ_c_type(c, item->type, true), item->name, "");
            emit_param(emit, &first, writ_cstr("size_t"), item->name, "_length");
        } else {
            emit_param(emit, &first, writ_c_type(c, item->type, false), item->name, "");
        }
    }
    for (size_t i = 0; !direct && i < plan->items_count; i++) {
        const writ_plan_item_view_t *item = &plan->items[i];
        if (item->role == WRIT_ROLE_RESULT && item->shape == WRIT_SHAPE_BYTES) {
            emit_param(emit, &first,
                       writ_str_format(c->model->session, "%.*s_bytes_t *", (int)c->prefix.length, c->prefix.data),
                       item->name, "");
        } else if (item->role == WRIT_ROLE_RESULT || item->role == WRIT_ROLE_ERROR_DETAIL) {
            writ_str_t type = writ_c_type(c, item->type, false);
            emit_param(emit, &first, writ_str_format(c->model->session, "%.*s *", (int)type.length, type.data),
                       item->name, "");
        }
    }
    writ_emit_text(emit, "%s)", first ? "void" : "");
}

void
writ_c_emit_real_prototype(const writ_c_context_t *c, writ_emit_t *emit, const writ_fn_view_t *fn,
                           const char *terminator)
{
    writ_str_t ret = writ_c_type(c, fn->result, false);
    writ_emit_text(emit, "%.*s %.*s(", (int)ret.length, ret.data, (int)fn->contract_name.length,
                   fn->contract_name.data);
    bool first = true;
    for (size_t i = 0; i < fn->slots_count; i++) {
        const writ_slot_view_t *slot = &fn->slots[i];
        bool constant = slot->mode == WRIT_MODE_IN && slot->type->kind != WRIT_TYPE_INT
                     && slot->type->kind != WRIT_TYPE_BOOL && slot->type->kind != WRIT_TYPE_FLOAT;
        emit_param(emit, &first, writ_c_type(c, slot->type, constant), slot->name, "");
    }
    writ_emit_line(emit, "%s)%s", first ? "void" : "", terminator);
}

typedef struct {
    writ_expr_env_t         base;
    const writ_c_context_t *c;
    const writ_plan_view_t *plan;
} shim_env_t;

static void
shim_ref(writ_expr_env_t *base, writ_ref_t ref, core_buffer_t *out)
{
    shim_env_t *env = (shim_env_t *)base;
    if (ref.kind == WRIT_REF_RESULT) {
        core_buffer_puts(out, "ret");
        return;
    }
    uint32_t slot = writ_plan_contract_slot(env->c->model, env->plan, ref);
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

static writ_str_t
upper(writ_session_t *session, writ_str_t text)
{
    writ_str_t copy = writ_str_copy(session, text.data, text.length);
    for (size_t i = 0; i < copy.length; i++) {
        ((char *)copy.data)[i] = (char)toupper((unsigned char)copy.data[i]);
    }
    return copy;
}

#define S(text) (int)(text).length, (text).data

static void
emit_declarations(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan)
{
    for (size_t i = 0; i < plan->slots_count; i++) {
        const writ_plan_slot_view_t *slot = &plan->slots[i];
        writ_str_t type = writ_c_type(c, slot->c_type, slot->pass == WRIT_PASS_IN_POINTER);
        if (slot->contract_slot == WRIT_PLAN_NONE) {
            writ_emit_line(emit, "%.*s ret = 0;", S(type));
        } else {
            writ_emit_line(emit, "%.*s%ss%zu = 0;", S(type), type.data[type.length - 1] == '*' ? "" : " ", i);
        }
    }
    for (size_t i = 0; i < plan->locals_count; i++) {
        const writ_plan_local_view_t *local = &plan->locals[i];
        writ_str_t type = writ_c_type(c, local->type, false);
        if (local->kind == WRIT_LOCAL_BUFFER) {
            writ_emit_line(emit, "%.*s *l%zu = NULL;", S(type), i);
            writ_emit_line(emit, "size_t l%zu_capacity = 0;", i);
        } else {
            writ_emit_line(emit, "%.*s l%zu = 0;", S(type), i);
        }
    }
}

static void
emit_step(const writ_c_context_t *c, shim_env_t *env, writ_emit_t *emit, const writ_plan_step_view_t *step,
          writ_str_t upper_prefix)
{
    const writ_plan_view_t *plan = env->plan;
    const writ_plan_item_view_t *item = step->item != WRIT_PLAN_NONE ? &plan->items[step->item] : nullptr;
    switch (step->kind) {
    case WRIT_STEP_CHECK: {
        const writ_type_view_t *length = plan->slots[step->slot].c_type;
        writ_emit_open(emit, "if (!%.*s && %.*s_length)", S(item->name), S(item->name));
        writ_emit_line(emit, "outcome = %.*s_ARGUMENT_ERROR;", S(upper_prefix));
        writ_emit_line(emit, "goto finish;");
        writ_emit_close(emit, nullptr);
        if (length->size < 8 || length->is_signed) {
            uint64_t limit = length->is_signed ? (UINT64_C(1) << (8 * length->size - 1)) - 1
                                               : UINT64_MAX >> (64 - 8 * length->size);
            writ_emit_open(emit, "if ((uint64_t)%.*s_length > UINT64_C(%llu))", S(item->name),
                           (unsigned long long)limit);
            writ_emit_line(emit, "outcome = %.*s_ARGUMENT_ERROR;", S(upper_prefix));
            writ_emit_line(emit, "goto finish;");
            writ_emit_close(emit, nullptr);
        }
        return;
    }
    case WRIT_STEP_SPLIT: {
        writ_str_t pointer = writ_c_type(c, plan->slots[step->slot].c_type, true);
        writ_emit_line(emit, "s%u = %.*s_length ? %.*s : (%.*s)%.*s_empty;", step->slot, S(item->name), S(item->name),
                       S(pointer), S(c->prefix));
        writ_str_t length = writ_c_type(c, plan->slots[step->second_slot].c_type, false);
        writ_emit_line(emit, "s%u = (%.*s)%.*s_length;", step->second_slot, S(length), S(item->name));
        return;
    }
    case WRIT_STEP_PASS:
        if (step->source_local != WRIT_PLAN_NONE) {
            writ_str_t type = writ_c_type(c, plan->locals[step->local].type, false);
            writ_emit_line(emit, "l%u = (%.*s)l%u_capacity;", step->local, S(type), step->source_local);
        } else {
            writ_emit_line(emit, "s%u = %.*s;", step->slot, S(item->name));
        }
        return;
    case WRIT_STEP_ALLOC_OUT: {
        const writ_plan_local_view_t *local = &plan->locals[step->local];
        writ_str_t capacity = local->capacity_item != WRIT_PLAN_NONE ? plan->items[local->capacity_item].name
                                                                      : expr_text(env, local->capacity_expr);
        writ_str_t element = writ_c_type(c, local->type, false);
        writ_emit_line(emit, "l%u_capacity = (size_t)(%.*s);", step->local, S(capacity));
        writ_emit_line(emit, "l%u = malloc(l%u_capacity ? l%u_capacity * sizeof(%.*s) : 1);", step->local,
                       step->local, step->local, S(element));
        writ_emit_open(emit, "if (!l%u)", step->local);
        writ_emit_line(emit, "outcome = %.*s_OUT_OF_MEMORY;", S(upper_prefix));
        writ_emit_line(emit, "goto finish;");
        writ_emit_close(emit, nullptr);
        return;
    }
    case WRIT_STEP_BIND_OUT:
        writ_emit_line(emit, "s%u = %sl%u;", step->slot, plan->locals[step->local].kind == WRIT_LOCAL_CELL ? "&" : "",
                       step->local);
        return;
    case WRIT_STEP_CALL: {
        const writ_fn_view_t *fn = plan->fn;
        writ_emit_text(emit, "%s%.*s(", fn->result && fn->result->kind != WRIT_TYPE_VOID ? "ret = " : "",
                       S(fn->contract_name));
        for (size_t i = 0; i < fn->slots_count; i++) {
            writ_emit_text(emit, "%ss%zu", i ? ", " : "", i);
        }
        writ_emit_line(emit, ");");
        return;
    }
    case WRIT_STEP_FETCH_DETAIL: {
        writ_str_t value = expr_text(env, step->expr);
        writ_str_t type = writ_c_type(c, item->type, false);
        writ_emit_line(emit, "*%.*s = (%.*s)(%.*s);", S(item->name), S(type), S(value));
        return;
    }
    case WRIT_STEP_LIFT_BUFFER: {
        writ_str_t length = expr_text(env, step->expr);
        writ_emit_open(emit, "if ((size_t)(%.*s) > l%u_capacity)", S(length), step->local);
        writ_emit_line(emit, "outcome = %.*s_CONTRACT_VIOLATION;", S(upper_prefix));
        writ_emit_line(emit, "goto finish;");
        writ_emit_close(emit, nullptr);
        writ_emit_line(emit, "%.*s->data = (uint8_t *)l%u;", S(item->name), step->local);
        writ_emit_line(emit, "%.*s->length = (size_t)(%.*s);", S(item->name), S(length));
        writ_emit_line(emit, "l%u = NULL;", step->local);
        return;
    }
    case WRIT_STEP_LIFT_VALUE: {
        writ_str_t source = step->local != WRIT_PLAN_NONE ? writ_str_format(c->model->session, "l%u", step->local)
                                                           : writ_cstr("ret");
        if (writ_c_returns_directly(plan)) {
            writ_emit_line(emit, "return %.*s;", S(source));
        } else {
            writ_emit_line(emit, "*%.*s = %.*s;", S(item->name), S(source));
        }
        return;
    }
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
        writ_str_t when = outcome->when_expr ? expr_text(env, outcome->when_expr) : writ_cstr("");
        if (outcome->when_expr) {
            writ_emit_open(emit, "%sif (%.*s)", i ? "else " : "", S(when));
        } else if (i) {
            writ_emit_open(emit, "else");
        } else {
            writ_emit_open(emit, "");
        }
        writ_emit_line(emit, "outcome = %.*s_%.*s_%.*s;", S(c->prefix), S(plan->fn->contract_name), S(outcome->name));
        writ_emit_close(emit, nullptr);
    }
}

static bool
can_jump_to_finish(const writ_plan_view_t *plan)
{
    for (size_t i = 0; i < plan->steps_count; i++) {
        writ_step_kind_t kind = plan->steps[i].kind;
        if (kind == WRIT_STEP_CHECK || kind == WRIT_STEP_ALLOC_OUT || kind == WRIT_STEP_LIFT_BUFFER) {
            return true;
        }
    }
    return false;
}

static void
emit_finish_label(writ_emit_t *emit, bool *emitted, bool wanted)
{
    if (*emitted || !wanted) {
        return;
    }
    writ_emit_indent(emit, -1);
    writ_emit_line(emit, "finish:");
    writ_emit_indent(emit, 1);
    *emitted = true;
}

static void
emit_shim(const writ_c_context_t *c, writ_emit_t *emit, const writ_plan_view_t *plan, writ_str_t upper_prefix)
{
    shim_env_t env = {{.print_ref = shim_ref}, c, plan};
    bool direct = writ_c_returns_directly(plan);
    bool wants_label = !direct && can_jump_to_finish(plan);
    bool label_emitted = false;
    writ_c_emit_signature(c, emit, plan);
    writ_emit_open(emit, "");
    emit_declarations(c, emit, plan);
    if (!direct) {
        writ_emit_line(emit, "int32_t outcome = 0;");
    }
    uint32_t current = WRIT_PLAN_NONE;
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
                writ_emit_open(emit, "if (outcome == %.*s_%.*s_%.*s)", S(c->prefix), S(plan->fn->contract_name),
                               S(plan->outcomes[step->outcome].name));
            }
            current = step->outcome;
        }
        if (step->phase == WRIT_PHASE_FINALLY) {
            emit_finish_label(emit, &label_emitted, wants_label);
        }
        emit_step(c, &env, emit, step, upper_prefix);
    }
    if (current != WRIT_PLAN_NONE && !direct) {
        writ_emit_close(emit, nullptr);
    }
    emit_finish_label(emit, &label_emitted, wants_label);
    if (!direct) {
        writ_emit_line(emit, "return outcome;");
    }
    writ_emit_close(emit, nullptr);
}

static writ_str_t
header_text(const writ_c_context_t *c, writ_str_t upper_prefix, const writ_plan_view_t *const *plans, size_t count)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_line(&emit, "#ifndef %.*s_WRIT_H", S(upper_prefix));
    writ_emit_line(&emit, "#define %.*s_WRIT_H", S(upper_prefix));
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#include <stdbool.h>");
    writ_emit_line(&emit, "#include <stddef.h>");
    writ_emit_line(&emit, "#include <stdint.h>");
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#ifdef __cplusplus");
    writ_emit_line(&emit, "extern \"C\" {");
    writ_emit_line(&emit, "#endif");
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#define %.*s_ARGUMENT_ERROR (-1)", S(upper_prefix));
    writ_emit_line(&emit, "#define %.*s_OUT_OF_MEMORY (-2)", S(upper_prefix));
    writ_emit_line(&emit, "#define %.*s_CONTRACT_VIOLATION (-3)", S(upper_prefix));
    writ_emit_line(&emit, "");
    writ_emit_open(&emit, "typedef struct");
    writ_emit_line(&emit, "uint8_t *data;");
    writ_emit_line(&emit, "size_t length;");
    writ_emit_close(&emit, writ_str_format(c->model->session, " %.*s_bytes_t;", S(c->prefix)).data);
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "void %.*s_bytes_free(%.*s_bytes_t *bytes);", S(c->prefix), S(c->prefix));
    for (size_t i = 0; i < count; i++) {
        const writ_plan_view_t *plan = plans[i];
        writ_emit_line(&emit, "");
        if (!writ_c_returns_directly(plan)) {
            writ_emit_open(&emit, "enum");
            for (size_t o = 0; o < plan->outcomes_count; o++) {
                writ_emit_line(&emit, "%.*s_%.*s_%.*s = %zu,", S(c->prefix), S(plan->fn->contract_name),
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

static writ_str_t
shim_text(const writ_c_context_t *c, writ_str_t upper_prefix, const writ_plan_view_t *const *plans, size_t count)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_line(&emit, "#include \"%.*s_writ.h\"", S(c->prefix));
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "#include <stdlib.h>");
    writ_emit_line(&emit, "");
    for (size_t i = 0; i < writ_model_fn_count(c->model); i++) {
        writ_c_emit_real_prototype(c, &emit, writ_model_fn_at(c->model, i), ";");
    }
    writ_emit_line(&emit, "");
    writ_emit_line(&emit, "static const uint8_t %.*s_empty[1];", S(c->prefix));
    writ_emit_line(&emit, "");
    writ_emit_open(&emit, "void %.*s_bytes_free(%.*s_bytes_t *bytes)", S(c->prefix), S(c->prefix));
    writ_emit_line(&emit, "free(bytes->data);");
    writ_emit_line(&emit, "bytes->data = NULL;");
    writ_emit_line(&emit, "bytes->length = 0;");
    writ_emit_close(&emit, nullptr);
    for (size_t i = 0; i < count; i++) {
        writ_emit_line(&emit, "");
        emit_shim(c, &emit, plans[i], upper_prefix);
    }
    return writ_emit_finish(&emit, c->model->session);
}

writ_status_t
writ_gen_c(const writ_model_t *model, const writ_gen_c_request_t *request, const writ_output_t *output)
{
    if (!model || !request || !output || !output->write_file) {
        return WRIT_INVALID_ARGUMENT;
    }
    writ_session_t *session = model->session;
    writ_str_t prefix = request->prefix.length ? request->prefix : writ_model_library(model)->name;
    writ_c_context_t c = {model, writ_str_copy(session, prefix.data, prefix.length)};
    writ_str_t upper_prefix = upper(session, c.prefix);
    size_t count = writ_model_fn_count(model);
    const writ_plan_view_t **plans = writ_alloc(session, sizeof(writ_plan_view_t *) * (count ? count : 1));
    for (size_t i = 0; i < count; i++) {
        plans[i] = writ_model_fn_plan(model, writ_model_fn_at(model, i));
        if (!plans[i]) {
            return WRIT_HAS_ERRORS;
        }
    }
    writ_status_t status = output->write_file(output->context, writ_str_format(session, "%.*s_writ.h", S(c.prefix)),
                                              header_text(&c, upper_prefix, plans, count));
    if (status == WRIT_OK) {
        status = output->write_file(output->context, writ_str_format(session, "%.*s_writ.c", S(c.prefix)),
                                    shim_text(&c, upper_prefix, plans, count));
    }
    if (status == WRIT_OK && request->conformance) {
        status = writ_gen_conformance(&c, output);
    }
    return status;
}
