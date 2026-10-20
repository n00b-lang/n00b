#include "../model/model.h"
#include "model_names.h"

typedef struct {
    writ_expr_env_t         base;
    const writ_model_t     *model;
    const writ_plan_view_t *plan;
} render_env_t;

static void
render_ref(writ_expr_env_t *base, writ_ref_t ref, uint32_t expr, core_buffer_t *out)
{
    (void)expr;
    render_env_t *env = (render_env_t *)base;
    if (ref.kind == WRIT_REF_RESULT) {
        core_buffer_puts(out, "ret");
        return;
    }
    if (ref.kind == WRIT_REF_CHANNEL) {
        writ_str_t name = writ_sym_str(env->model->session, env->model->channels.data[ref.index].name);
        core_buffer_printf(out, "%.*s", (int)name.length, name.data);
        return;
    }
    uint32_t slot = writ_plan_contract_slot(env->model, env->plan, ref);
    uint32_t cell = ref.phase == WRIT_REF_VALUE ? WRIT_PLAN_NONE : writ_plan_cell_for_slot(env->plan, slot);
    if (cell != WRIT_PLAN_NONE) {
        core_buffer_printf(out, "L%u", cell);
    } else {
        core_buffer_printf(out, "%ss%u", ref.phase == WRIT_REF_VALUE ? "" : "*", slot);
    }
}

static void
put_expr(render_env_t *env, uint32_t expr, core_buffer_t *out)
{
    writ_expr_print(env->model, expr, WRIT_LANGUAGE_C, &env->base, out);
}

static void
put_item(const writ_plan_view_t *plan, uint32_t item, core_buffer_t *out)
{
    if (item == WRIT_PLAN_NONE) {
        core_buffer_putc(out, '-');
    } else {
        core_buffer_printf(out, "%.*s", (int)plan->items[item].name.length, plan->items[item].name.data);
    }
}

static void
put_target(const writ_plan_step_view_t *step, core_buffer_t *out)
{
    if (step->local != WRIT_PLAN_NONE) {
        core_buffer_printf(out, " -> L%u", step->local);
    } else if (step->slot != WRIT_PLAN_NONE) {
        core_buffer_printf(out, " -> s%u", step->slot);
    }
}

static void
render_operands(render_env_t *env, const writ_plan_step_view_t *step, core_buffer_t *out)
{
    const writ_plan_view_t *plan = env->plan;
    switch (step->kind) {
    case WRIT_STEP_CHECK:
        put_item(plan, step->item, out);
        core_buffer_printf(out, " fits s%u", step->slot);
        return;
    case WRIT_STEP_SPLIT:
        put_item(plan, step->item, out);
        core_buffer_printf(out, " -> s%u, s%u", step->slot, step->second_slot);
        return;
    case WRIT_STEP_PASS:
        if (step->literal.length) {
            core_buffer_printf(out, "\"%.*s\"", (int)step->literal.length, step->literal.data);
        } else if (step->source_local != WRIT_PLAN_NONE) {
            core_buffer_printf(out, "cap(L%u)", step->source_local);
        } else if (plan->slots[step->slot].part == WRIT_PART_LENGTH) {
            core_buffer_puts(out, "count(");
            put_item(plan, step->item, out);
            core_buffer_putc(out, ')');
        } else {
            put_item(plan, step->item, out);
        }
        put_target(step, out);
        return;
    case WRIT_STEP_ENCODE_CSTR:
    case WRIT_STEP_COPY_BORROWED:
    case WRIT_STEP_WRAP_HANDLE:
    case WRIT_STEP_LIFT_VALUE:
        if (step->kind == WRIT_STEP_ENCODE_CSTR) {
            put_item(plan, step->item, out);
            put_target(step, out);
            return;
        }
        if (step->local != WRIT_PLAN_NONE) {
            core_buffer_printf(out, "L%u", step->local);
        } else {
            plan->slots[step->slot].contract_slot == WRIT_PLAN_NONE ? core_buffer_puts(out, "ret") : core_buffer_puts(out, "s");
        }
        core_buffer_puts(out, " -> ");
        put_item(plan, step->item, out);
        return;
    case WRIT_STEP_EXPAND:
        put_item(plan, step->item, out);
        core_buffer_printf(out, " -> s%u", step->slot);
        if (step->expr) {
            core_buffer_puts(out, ", until ");
            put_expr(env, step->expr, out);
        }
        return;
    case WRIT_STEP_SIZE_RETRY:
        core_buffer_printf(out, "L%u needs ", step->local);
        put_expr(env, step->expr, out);
        return;
    case WRIT_STEP_SIZE_QUERY:
        core_buffer_puts(out, "(");
        for (size_t i = 0; i < plan->query_count; i++) {
            core_buffer_printf(out, "%ss%u = ", i ? ", " : "", plan->query[i].slot);
            put_expr(env, plan->query[i].expr, out);
        }
        core_buffer_printf(out, ") -> cap(L%u) = ", step->local);
        put_expr(env, step->expr, out);
        return;
    case WRIT_STEP_ALLOC_OUT: {
        const writ_plan_local_view_t *local = &plan->locals[step->local];
        put_item(plan, step->item, out);
        core_buffer_printf(out, " -> L%u, cap ", step->local);
        if (local->capacity_item != WRIT_PLAN_NONE) {
            put_item(plan, local->capacity_item, out);
        } else if (step->expr) {
            put_expr(env, step->expr, out);
        } else {
            core_buffer_puts(out, "from query");
        }
        return;
    }
    case WRIT_STEP_BIND_OUT:
        core_buffer_printf(out, "L%u -> s%u", step->local, step->slot);
        return;
    case WRIT_STEP_CALL:
        plan->variadic ? core_buffer_puts(out, "variadic") : core_buffer_puts(out, "fixed");
        return;
    case WRIT_STEP_FETCH_DETAIL:
        put_expr(env, step->expr, out);
        core_buffer_puts(out, " -> ");
        put_item(plan, step->item, out);
        return;
    case WRIT_STEP_LIFT_BUFFER:
        core_buffer_printf(out, "L%u -> ", step->local);
        put_item(plan, step->item, out);
        core_buffer_puts(out, ", len ");
        put_expr(env, step->expr, out);
        return;
    case WRIT_STEP_MARK_CONSUMED:
        put_item(plan, step->item, out);
        return;
    case WRIT_STEP_RELEASE_LOCAL:
        core_buffer_printf(out, "L%u unless handed over", step->local);
        return;
    case WRIT_STEP_PASS_CONST:
        core_buffer_printf(out, "%.*s -> s%u", (int)step->literal.length, step->literal.data, step->slot);
        return;
    default:
        return;
    }
}

static void
render_classify(render_env_t *env, writ_emit_t *emit)
{
    for (size_t i = 0; i < env->plan->outcomes_count; i++) {
        const writ_plan_outcome_view_t *outcome = &env->plan->outcomes[i];
        core_buffer_t *line = core_buffer_empty();
        core_buffer_printf(line, "%-9s %.*s: %s", i ? "" : "classify", (int)outcome->name.length, outcome->name.data,
                           writ_outcome_kind_names[outcome->kind]);
        if (outcome->when_expr) {
            core_buffer_puts(line, " when ");
            put_expr(env, outcome->when_expr, line);
        }
        writ_emit_line(emit, "%.*s", (int)line->byte_len, line->data);
        core_buffer_free(line);
    }
}

static void
render_steps(render_env_t *env, writ_emit_t *emit)
{
    const writ_plan_view_t *plan = env->plan;
    char previous[64] = "";
    char block[64];
    for (size_t i = 0; i < plan->steps_count; i++) {
        const writ_plan_step_view_t *step = &plan->steps[i];
        if (step->kind == WRIT_STEP_CLASSIFY) {
            render_classify(env, emit);
            snprintf(previous, sizeof(previous), "classify");
            continue;
        }
        if (step->phase == WRIT_PHASE_OUTCOME) {
            snprintf(block, sizeof(block), "%.*s", (int)plan->outcomes[step->outcome].name.length,
                     plan->outcomes[step->outcome].name.data);
        } else {
            snprintf(block, sizeof(block), "%s", writ_phase_names[step->phase]);
        }
        bool starts_block = strcmp(block, previous) != 0;
        core_buffer_t *line = core_buffer_empty();
        core_buffer_printf(line, "%-9s %-14s ", starts_block ? block : "", writ_step_kind_names[step->kind]);
        render_operands(env, step, line);
        writ_emit_line(emit, "%.*s", (int)line->byte_len, line->data);
        core_buffer_free(line);
        snprintf(previous, sizeof(previous), "%s", block);
    }
}

writ_status_t
writ_model_plan_render(const writ_model_t *model, const writ_plan_view_t *plan, writ_str_t *rendered)
{
    if (!model || !plan || !rendered) {
        return WRIT_INVALID_ARGUMENT;
    }
    render_env_t env = {{.print_ref = render_ref}, model, plan};
    writ_emit_t emit;
    writ_emit_init(&emit);
    writ_emit_text(&emit, "plan %.*s", (int)plan->fn->contract_name.length, plan->fn->contract_name.data);
    if (plan->policy_slot != WRIT_PLAN_NONE) {
        const writ_policy_arm_view_t *arm = &plan->fn->slots[plan->policy_slot].policy_arms[plan->policy_arm];
        writ_str_t name = arm->is_else ? writ_cstr("else") : arm->constant;
        writ_emit_text(&emit, " arm %.*s", (int)name.length, name.data);
    }
    writ_emit_line(&emit, "");
    if (plan->serialize_item != WRIT_PLAN_NONE) {
        core_buffer_t *line = core_buffer_empty();
        put_item(plan, plan->serialize_item, line);
        writ_emit_line(&emit, "serialize on %.*s", (int)line->byte_len, line->data);
        core_buffer_free(line);
    }
    for (size_t i = 0; i < plan->reset_channels_count; i++) {
        writ_str_t name = writ_sym_str(model->session, model->channels.data[plan->reset_channels[i]].name);
        writ_emit_line(&emit, "reset %.*s", (int)name.length, name.data);
    }
    for (size_t i = 0; i < plan->items_count; i++) {
        const writ_plan_item_view_t *item = &plan->items[i];
        writ_emit_line(&emit, "item %zu %.*s %s %s %s%s%.*s", i, (int)item->name.length, item->name.data,
                       writ_plan_role_names[item->role], writ_plan_shape_names[item->shape],
                       writ_after_call_names[item->after_call], item->release.length ? " release " : "",
                       (int)item->release.length, item->release.data);
    }
    for (size_t i = 0; i < plan->slots_count; i++) {
        const writ_plan_slot_view_t *slot = &plan->slots[i];
        core_buffer_t *line = core_buffer_empty();
        put_item(plan, slot->item, line);
        writ_emit_line(&emit, "slot %zu %s %s %.*s", i, writ_pass_names[slot->pass], writ_part_names[slot->part],
                       (int)line->byte_len, line->data);
        core_buffer_free(line);
    }
    for (size_t i = 0; i < plan->locals_count; i++) {
        const writ_plan_local_view_t *local = &plan->locals[i];
        core_buffer_t *line = core_buffer_empty();
        put_item(plan, local->item, line);
        writ_emit_line(&emit, "local %zu %s %.*s %.*s", i, writ_local_kind_names[local->kind],
                       (int)local->type->name.length, local->type->name.data, (int)line->byte_len, line->data);
        core_buffer_free(line);
    }
    render_steps(&env, &emit);
    *rendered = writ_emit_finish(&emit, model->session);
    return WRIT_OK;
}
