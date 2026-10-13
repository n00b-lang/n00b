#include "../model/model.h"
#include "model_names.h"

typedef struct {
    writ_expr_env_t         base;
    const writ_model_t     *model;
    const writ_plan_view_t *plan;
} render_env_t;

static void
render_ref(writ_expr_env_t *base, writ_ref_t ref, core_buffer_t *out)
{
    render_env_t *env = (render_env_t *)base;
    if (ref.kind == WRIT_REF_RESULT) {
        core_buffer_puts(out, "ret");
        return;
    }
    uint32_t slot = writ_plan_contract_slot(env->model, env->plan, ref);
    uint32_t cell = ref.phase == WRIT_REF_VALUE ? WRIT_PLAN_NONE : writ_plan_cell_for_slot(env->plan, slot);
    if (cell != WRIT_PLAN_NONE) {
        core_buffer_printf(out, "L%u", cell);
    } else {
        core_buffer_printf(out, "s%u", slot);
    }
}

static void
put_expr(render_env_t *env, uint32_t expr, core_buffer_t *out)
{
    writ_expr_print(env->model, expr, WRIT_LANGUAGE_C, &env->base, out);
}

static const char *
item_name(const writ_plan_view_t *plan, uint32_t item, char *scratch, size_t size)
{
    if (item == WRIT_PLAN_NONE) {
        return "-";
    }
    snprintf(scratch, size, "%.*s", (int)plan->items[item].name.length, plan->items[item].name.data);
    return scratch;
}

static void
render_operands(render_env_t *env, const writ_plan_step_view_t *step, core_buffer_t *out)
{
    const writ_plan_view_t *plan = env->plan;
    char name[128];
    switch (step->kind) {
    case WRIT_STEP_CHECK:
        core_buffer_printf(out, "%s fits s%u", item_name(plan, step->item, name, sizeof(name)), step->slot);
        return;
    case WRIT_STEP_SPLIT:
        core_buffer_printf(out, "%s -> s%u, s%u", item_name(plan, step->item, name, sizeof(name)), step->slot,
                           step->second_slot);
        return;
    case WRIT_STEP_PASS:
        if (step->source_local != WRIT_PLAN_NONE) {
            core_buffer_printf(out, "cap(L%u)", step->source_local);
        } else {
            core_buffer_puts(out, item_name(plan, step->item, name, sizeof(name)));
        }
        if (step->local != WRIT_PLAN_NONE) {
            core_buffer_printf(out, " -> L%u", step->local);
        } else {
            core_buffer_printf(out, " -> s%u", step->slot);
        }
        return;
    case WRIT_STEP_ALLOC_OUT: {
        const writ_plan_local_view_t *local = &plan->locals[step->local];
        core_buffer_printf(out, "%s -> L%u, cap ", item_name(plan, step->item, name, sizeof(name)), step->local);
        if (local->capacity_item != WRIT_PLAN_NONE) {
            core_buffer_puts(out, item_name(plan, local->capacity_item, name, sizeof(name)));
        } else {
            put_expr(env, local->capacity_expr, out);
        }
        return;
    }
    case WRIT_STEP_BIND_OUT:
        core_buffer_printf(out, "L%u -> s%u", step->local, step->slot);
        return;
    case WRIT_STEP_CALL:
        core_buffer_puts(out, "fixed");
        return;
    case WRIT_STEP_FETCH_DETAIL:
        put_expr(env, step->expr, out);
        core_buffer_printf(out, " -> %s", item_name(plan, step->item, name, sizeof(name)));
        return;
    case WRIT_STEP_LIFT_VALUE:
        if (step->local != WRIT_PLAN_NONE) {
            core_buffer_printf(out, "L%u", step->local);
        } else if (plan->slots[step->slot].contract_slot == WRIT_PLAN_NONE) {
            core_buffer_puts(out, "ret");
        } else {
            core_buffer_printf(out, "s%u", step->slot);
        }
        core_buffer_printf(out, " -> %s", item_name(plan, step->item, name, sizeof(name)));
        return;
    case WRIT_STEP_LIFT_BUFFER:
        core_buffer_printf(out, "L%u -> %s, len ", step->local, item_name(plan, step->item, name, sizeof(name)));
        put_expr(env, step->expr, out);
        return;
    case WRIT_STEP_RELEASE_LOCAL:
        core_buffer_printf(out, "L%u unless handed over", step->local);
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
    writ_emit_line(&emit, "plan %.*s", (int)plan->fn->contract_name.length, plan->fn->contract_name.data);
    for (size_t i = 0; i < plan->items_count; i++) {
        const writ_plan_item_view_t *item = &plan->items[i];
        writ_emit_line(&emit, "item %zu %.*s %s %s %s", i, (int)item->name.length, item->name.data,
                       writ_plan_role_names[item->role], writ_plan_shape_names[item->shape],
                       writ_after_call_names[item->after_call]);
    }
    for (size_t i = 0; i < plan->slots_count; i++) {
        const writ_plan_slot_view_t *slot = &plan->slots[i];
        char name[128];
        writ_emit_line(&emit, "slot %zu %s %s %s", i, writ_pass_names[slot->pass], writ_part_names[slot->part],
                       item_name(plan, slot->item, name, sizeof(name)));
    }
    for (size_t i = 0; i < plan->locals_count; i++) {
        const writ_plan_local_view_t *local = &plan->locals[i];
        char name[128];
        writ_emit_line(&emit, "local %zu %s %.*s %s", i, writ_local_kind_names[local->kind],
                       (int)local->type->name.length, local->type->name.data,
                       item_name(plan, local->item, name, sizeof(name)));
    }
    render_steps(&env, &emit);
    *rendered = writ_emit_finish(&emit, model->session);
    return WRIT_OK;
}
