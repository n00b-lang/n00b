#include "../model/model.h"

core_list_decl(writ_plan_item_view_t);
core_list_decl(writ_plan_slot_view_t);
core_list_decl(writ_plan_local_view_t);
core_list_decl(writ_plan_step_view_t);
core_list_decl(writ_plan_outcome_view_t);

enum {
    BLOCK_PREPARE,
    BLOCK_ATTEMPT,
    BLOCK_SUCCESS,
    BLOCK_FAILURE,
    BLOCK_FINALLY,
    BLOCK_COUNT,
};

typedef struct {
    writ_model_t                          *model;
    uint32_t                               fn;
    const writ_fn_rec_t                   *record;
    core_list_t(writ_plan_item_view_t)     items;
    core_list_t(writ_plan_slot_view_t)     slots;
    core_list_t(writ_plan_local_view_t)    locals;
    core_list_t(writ_plan_step_view_t)     blocks[BLOCK_COUNT];
    bool                                  *consumed;
    bool                                   supported;
} builder_t;

static const writ_type_view_t *
type_view(const writ_model_t *model, writ_type_id id)
{
    return id ? &model->type_views[id] : nullptr;
}

static const writ_slot_rec_t *
contract_slot(const builder_t *b, uint32_t index)
{
    return &b->model->slots.data[b->record->first_slot + index];
}

static writ_plan_step_view_t
step(writ_step_kind_t kind)
{
    return (writ_plan_step_view_t){
        .struct_size  = sizeof(writ_plan_step_view_t),
        .kind         = kind,
        .outcome      = WRIT_PLAN_NONE,
        .item         = WRIT_PLAN_NONE,
        .slot         = WRIT_PLAN_NONE,
        .second_slot  = WRIT_PLAN_NONE,
        .local        = WRIT_PLAN_NONE,
        .source_local = WRIT_PLAN_NONE,
    };
}

static void
emit_step(builder_t *b, uint32_t block, writ_plan_step_view_t value)
{
    core_list_push(b->blocks[block], value);
}

static uint32_t
add_item(builder_t *b, writ_sym_t name, writ_plan_role_t role, writ_plan_shape_t shape, writ_after_call_t after,
         writ_type_id type, uint32_t slot)
{
    core_list_push(b->items, ((writ_plan_item_view_t){
                                 .struct_size   = sizeof(writ_plan_item_view_t),
                                 .name          = writ_sym_str(b->model->session, name),
                                 .role          = role,
                                 .shape         = shape,
                                 .after_call    = after,
                                 .type          = type_view(b->model, type),
                                 .contract_slot = slot,
                             }));
    return (uint32_t)b->items.len - 1;
}

static uint32_t
add_local(builder_t *b, writ_local_kind_t kind, writ_type_id type, uint32_t item)
{
    core_list_push(b->locals, ((writ_plan_local_view_t){
                                  .struct_size   = sizeof(writ_plan_local_view_t),
                                  .kind          = kind,
                                  .type          = type_view(b->model, type),
                                  .item          = item,
                                  .capacity_item = WRIT_PLAN_NONE,
                              }));
    return (uint32_t)b->locals.len - 1;
}

static void
bind_slot(builder_t *b, uint32_t slot, uint32_t item, writ_part_t part)
{
    b->slots.data[slot].item = item;
    b->slots.data[slot].part = part;
}

static writ_plan_shape_t
array_shape(const builder_t *b, writ_type_id array)
{
    const writ_type_rec_t *element = writ_type(b->model, writ_type(b->model, array)->key.target);
    return element->key.kind == WRIT_TYPE_INT && element->size == 1 && !element->is_signed ? WRIT_SHAPE_BYTES
                                                                                         : WRIT_SHAPE_ARRAY;
}

static void
unsupported(builder_t *b, const writ_slot_rec_t *slot, const char *what)
{
    writ_report(b->model->session, WRIT_D_UNSUPPORTED, slot ? slot->loc : b->record->loc, "%s", what);
    b->supported = false;
}

static void
plan_count_array(builder_t *b, uint32_t index)
{
    const writ_slot_rec_t *slot = contract_slot(b, index);
    writ_ref_t count = writ_expr_ref(b->model, slot->count_expr);
    if (count.kind != WRIT_REF_SLOT || count.phase != WRIT_REF_VALUE) {
        unsupported(b, slot, "array extents that are not a single parameter");
        return;
    }
    uint32_t length = count.index - b->record->first_slot;
    b->consumed[length] = true;
    uint32_t item = add_item(b, slot->name, WRIT_ROLE_INPUT, array_shape(b, slot->type),
                             writ_default(WRIT_DEFAULT_INPUT_AFTER_CALL), slot->type, index);
    bind_slot(b, index, item, WRIT_PART_POINTER);
    bind_slot(b, length, item, WRIT_PART_LENGTH);
    writ_plan_step_view_t check = step(WRIT_STEP_CHECK);
    check.item = item;
    check.slot = length;
    emit_step(b, BLOCK_PREPARE, check);
    writ_plan_step_view_t split = step(WRIT_STEP_SPLIT);
    split.item = item;
    split.slot = index;
    split.second_slot = length;
    emit_step(b, BLOCK_PREPARE, split);
}

static void
plan_bounded_output(builder_t *b, uint32_t index)
{
    const writ_slot_rec_t *slot = contract_slot(b, index);
    uint32_t cell_slot = writ_expr_ref(b->model, slot->cap_expr).index - b->record->first_slot;
    const writ_slot_rec_t *cell = contract_slot(b, cell_slot);
    writ_type_id cell_type = writ_type(b->model, cell->type)->key.target;
    b->consumed[cell_slot] = true;
    uint32_t item = add_item(b, slot->name, WRIT_ROLE_RESULT, array_shape(b, slot->type),
                             writ_default(WRIT_DEFAULT_PROMOTED_AFTER_CALL), slot->type, index);
    bind_slot(b, index, item, WRIT_PART_POINTER);
    bind_slot(b, cell_slot, item, WRIT_PART_CAPACITY_AND_LENGTH);
    uint32_t buffer = add_local(b, WRIT_LOCAL_BUFFER, writ_type(b->model, slot->type)->key.target, item);
    uint32_t cell_local = add_local(b, WRIT_LOCAL_CELL, cell_type, item);
    if (b->record->sizing_expr) {
        b->locals.data[buffer].capacity_expr = b->record->sizing_expr;
    } else {
        writ_str_t name = writ_sym_str(b->model->session, slot->name);
        writ_sym_t capacity_name =
            writ_intern_str(b->model->session,
                            writ_str_format(b->model->session, "%.*s_capacity", (int)name.length, name.data));
        b->locals.data[buffer].capacity_item =
            add_item(b, capacity_name, WRIT_ROLE_INPUT, WRIT_SHAPE_SCALAR, WRIT_AFTER_NONE, cell_type, cell_slot);
    }
    writ_plan_step_view_t alloc = step(WRIT_STEP_ALLOC_OUT);
    alloc.item = item;
    alloc.local = buffer;
    alloc.expr = b->record->sizing_expr;
    emit_step(b, BLOCK_ATTEMPT, alloc);
    writ_plan_step_view_t pass = step(WRIT_STEP_PASS);
    pass.source_local = buffer;
    pass.local = cell_local;
    emit_step(b, BLOCK_ATTEMPT, pass);
    writ_plan_step_view_t bind_buffer = step(WRIT_STEP_BIND_OUT);
    bind_buffer.local = buffer;
    bind_buffer.slot = index;
    emit_step(b, BLOCK_ATTEMPT, bind_buffer);
    writ_plan_step_view_t bind_cell = step(WRIT_STEP_BIND_OUT);
    bind_cell.local = cell_local;
    bind_cell.slot = cell_slot;
    emit_step(b, BLOCK_ATTEMPT, bind_cell);
    writ_plan_step_view_t lift = step(WRIT_STEP_LIFT_BUFFER);
    lift.local = buffer;
    lift.item = item;
    lift.expr = slot->len_expr;
    emit_step(b, BLOCK_SUCCESS, lift);
    writ_plan_step_view_t release = step(WRIT_STEP_RELEASE_LOCAL);
    release.local = buffer;
    emit_step(b, BLOCK_FINALLY, release);
}

static void
plan_out_scalar(builder_t *b, uint32_t index)
{
    const writ_slot_rec_t *slot = contract_slot(b, index);
    writ_type_id target = writ_type(b->model, slot->type)->key.target;
    uint32_t item = add_item(b, slot->name, WRIT_ROLE_RESULT, WRIT_SHAPE_SCALAR, WRIT_AFTER_NONE, target, index);
    bind_slot(b, index, item, WRIT_PART_WHOLE);
    uint32_t cell = add_local(b, WRIT_LOCAL_CELL, target, item);
    writ_plan_step_view_t bind = step(WRIT_STEP_BIND_OUT);
    bind.local = cell;
    bind.slot = index;
    emit_step(b, BLOCK_ATTEMPT, bind);
    writ_plan_step_view_t lift = step(WRIT_STEP_LIFT_VALUE);
    lift.local = cell;
    lift.item = item;
    emit_step(b, BLOCK_SUCCESS, lift);
}

static void
mark_consumed_parts(builder_t *b)
{
    for (uint32_t i = 0; i < b->record->slot_count; i++) {
        const writ_slot_rec_t *slot = contract_slot(b, i);
        writ_ref_t part = slot->extent_kind == WRIT_EXTENT_COUNT     ? writ_expr_ref(b->model, slot->count_expr)
                        : slot->extent_kind == WRIT_EXTENT_BOUNDED   ? writ_expr_ref(b->model, slot->cap_expr)
                                                                     : (writ_ref_t){0};
        if (part.kind == WRIT_REF_SLOT) {
            b->consumed[part.index - b->record->first_slot] = true;
        }
    }
}

static void
plan_inputs(builder_t *b)
{
    mark_consumed_parts(b);
    for (uint32_t i = 0; i < b->record->slot_count; i++) {
        const writ_slot_rec_t *slot = contract_slot(b, i);
        writ_type_kind_t kind = writ_type(b->model, slot->type)->key.kind;
        if (b->consumed[i]) {
            continue;
        }
        if (slot->mode == WRIT_MODE_OUT && kind == WRIT_TYPE_ARRAY) {
            plan_bounded_output(b, i);
        } else if (slot->mode == WRIT_MODE_IN && kind == WRIT_TYPE_ARRAY && slot->extent_kind == WRIT_EXTENT_COUNT) {
            plan_count_array(b, i);
        } else if (slot->mode == WRIT_MODE_OUT && kind == WRIT_TYPE_POINTER) {
            plan_out_scalar(b, i);
        } else if (slot->mode == WRIT_MODE_IN && kind != WRIT_TYPE_POINTER && kind != WRIT_TYPE_ARRAY) {
            uint32_t item = add_item(b, slot->name, WRIT_ROLE_INPUT, WRIT_SHAPE_SCALAR, WRIT_AFTER_NONE, slot->type, i);
            bind_slot(b, i, item, WRIT_PART_WHOLE);
            writ_plan_step_view_t pass = step(WRIT_STEP_PASS);
            pass.item = item;
            pass.slot = i;
            emit_step(b, BLOCK_PREPARE, pass);
        } else {
            unsupported(b, slot, "this parameter shape");
        }
    }
}

static void
plan_result(builder_t *b)
{
    const writ_fn_rec_t *record = b->record;
    uint32_t ret_slot = record->slot_count;
    bool has_result = record->result && writ_type(b->model, record->result)->key.kind != WRIT_TYPE_VOID;
    bool consumed = writ_expr_mentions(b->model, record->error_expr, WRIT_REF_RESULT, b->fn);
    if (has_result && !consumed) {
        uint32_t item = add_item(b, writ_intern(b->model->session, "result", 6), WRIT_ROLE_RESULT, WRIT_SHAPE_SCALAR,
                                 WRIT_AFTER_NONE, record->result, WRIT_PLAN_NONE);
        bind_slot(b, ret_slot, item, WRIT_PART_WHOLE);
        writ_plan_step_view_t lift = step(WRIT_STEP_LIFT_VALUE);
        lift.slot = ret_slot;
        lift.item = item;
        emit_step(b, BLOCK_SUCCESS, lift);
    }
    if (!record->via_expr) {
        return;
    }
    writ_ref_t via = writ_expr_ref(b->model, record->via_expr);
    writ_type_id detail_type = via.kind == WRIT_REF_RESULT ? record->result
                             : via.kind == WRIT_REF_SLOT  ? b->model->slots.data[via.index].type
                                                          : writ_type_named(b->model, writ_intern(b->model->session, "i64", 3));
    const char *name = via.kind == WRIT_REF_RESULT ? "status" : "detail";
    uint32_t detail = add_item(b, writ_intern(b->model->session, name, strlen(name)), WRIT_ROLE_ERROR_DETAIL,
                               WRIT_SHAPE_SCALAR, WRIT_AFTER_NONE, detail_type, WRIT_PLAN_NONE);
    writ_plan_step_view_t fetch = step(WRIT_STEP_FETCH_DETAIL);
    fetch.item = detail;
    fetch.expr = record->via_expr;
    emit_step(b, BLOCK_FAILURE, fetch);
}

static void
plan_slots(builder_t *b)
{
    for (uint32_t i = 0; i <= b->record->slot_count; i++) {
        bool is_ret = i == b->record->slot_count;
        writ_type_id type = is_ret ? b->record->result : contract_slot(b, i)->type;
        if (is_ret && (!type || writ_type(b->model, type)->key.kind == WRIT_TYPE_VOID)) {
            break;
        }
        writ_type_kind_t kind = writ_type(b->model, type)->key.kind;
        writ_mode_t mode = is_ret ? WRIT_MODE_IN : contract_slot(b, i)->mode;
        bool indirect = kind == WRIT_TYPE_POINTER || kind == WRIT_TYPE_ARRAY;
        writ_pass_t pass = !indirect                ? WRIT_PASS_VALUE
                         : mode == WRIT_MODE_OUT    ? WRIT_PASS_OUT_POINTER
                         : mode == WRIT_MODE_INOUT  ? WRIT_PASS_INOUT_POINTER
                                                    : WRIT_PASS_IN_POINTER;
        core_list_push(b->slots, ((writ_plan_slot_view_t){
                                     .struct_size   = sizeof(writ_plan_slot_view_t),
                                     .c_type        = type_view(b->model, type),
                                     .pass          = pass,
                                     .part          = WRIT_PART_WHOLE,
                                     .item          = WRIT_PLAN_NONE,
                                     .contract_slot = is_ret ? WRIT_PLAN_NONE : i,
                                 }));
    }
}

static writ_plan_outcome_view_t
outcome(const char *name, writ_outcome_kind_t kind, uint32_t when, uint32_t via)
{
    return (writ_plan_outcome_view_t){
        .struct_size = sizeof(writ_plan_outcome_view_t),
        .name        = writ_cstr(name),
        .kind        = kind,
        .when_expr   = when,
        .via_expr    = via,
    };
}

uint32_t
writ_plan_cell_for_slot(const writ_plan_view_t *plan, uint32_t slot)
{
    for (size_t i = 0; i < plan->steps_count; i++) {
        const writ_plan_step_view_t *bind = &plan->steps[i];
        if (bind->kind == WRIT_STEP_BIND_OUT && bind->slot == slot && plan->locals[bind->local].kind == WRIT_LOCAL_CELL) {
            return bind->local;
        }
    }
    return WRIT_PLAN_NONE;
}

uint32_t
writ_plan_contract_slot(const writ_model_t *model, const writ_plan_view_t *plan, writ_ref_t ref)
{
    return ref.index - model->fns.data[plan->fn - model->fn_views].first_slot;
}

static void *
arena_copy(writ_session_t *session, const void *data, size_t size)
{
    void *copy = writ_alloc(session, size);
    if (size) {
        memcpy(copy, data, size);
    }
    return copy;
}

static void
append_block(core_list_t(writ_plan_step_view_t) *steps, core_list_t(writ_plan_step_view_t) *block,
             writ_phase_t phase, uint32_t outcome_index)
{
    for (size_t i = 0; i < block->len; i++) {
        writ_plan_step_view_t value = block->data[i];
        value.phase = phase;
        value.outcome = outcome_index;
        core_list_push(*steps, value);
    }
}

const writ_plan_view_t *
writ_plan_build(writ_model_t *model, uint32_t fn)
{
    writ_session_t *session = model->session;
    builder_t b = {
        .model     = model,
        .fn        = fn,
        .record    = &model->fns.data[fn],
        .items     = core_list_new(writ_plan_item_view_t),
        .slots     = core_list_new(writ_plan_slot_view_t),
        .locals    = core_list_new(writ_plan_local_view_t),
        .supported = true,
    };
    for (int i = 0; i < BLOCK_COUNT; i++) {
        b.blocks[i] = core_list_new(writ_plan_step_view_t);
    }
    b.consumed = writ_alloc(session, sizeof(bool) * (b.record->slot_count + 1));
    plan_slots(&b);
    plan_inputs(&b);
    plan_result(&b);

    core_list_t(writ_plan_outcome_view_t) outcomes = core_list_new(writ_plan_outcome_view_t);
    core_list_t(writ_plan_step_view_t) steps = core_list_new(writ_plan_step_view_t);
    append_block(&steps, &b.blocks[BLOCK_PREPARE], WRIT_PHASE_PREPARE, WRIT_PLAN_NONE);
    append_block(&steps, &b.blocks[BLOCK_ATTEMPT], WRIT_PHASE_ATTEMPT, WRIT_PLAN_NONE);
    writ_plan_step_view_t call = step(WRIT_STEP_CALL);
    call.phase = WRIT_PHASE_ATTEMPT;
    core_list_push(steps, call);
    if (b.record->error_expr) {
        core_list_push(outcomes, outcome("error", WRIT_OUTCOME_FAILURE, b.record->error_expr, b.record->via_expr));
    }
    core_list_push(outcomes, outcome("ok", WRIT_OUTCOME_SUCCESS, 0, 0));
    writ_plan_step_view_t classify = step(WRIT_STEP_CLASSIFY);
    classify.phase = WRIT_PHASE_CLASSIFY;
    core_list_push(steps, classify);
    for (uint32_t i = 0; i < outcomes.len; i++) {
        bool failure = outcomes.data[i].kind == WRIT_OUTCOME_FAILURE;
        append_block(&steps, &b.blocks[failure ? BLOCK_FAILURE : BLOCK_SUCCESS], WRIT_PHASE_OUTCOME, i);
    }
    append_block(&steps, &b.blocks[BLOCK_FINALLY], WRIT_PHASE_FINALLY, WRIT_PLAN_NONE);

    writ_plan_view_t *plan = nullptr;
    if (b.supported) {
        plan = writ_alloc(session, sizeof(writ_plan_view_t));
        *plan = (writ_plan_view_t){
            .struct_size    = sizeof(writ_plan_view_t),
            .fn             = &model->fn_views[fn],
            .items_count    = b.items.len,
            .items          = arena_copy(session, b.items.data, sizeof(writ_plan_item_view_t) * b.items.len),
            .slots_count    = b.slots.len,
            .slots          = arena_copy(session, b.slots.data, sizeof(writ_plan_slot_view_t) * b.slots.len),
            .locals_count   = b.locals.len,
            .locals         = arena_copy(session, b.locals.data, sizeof(writ_plan_local_view_t) * b.locals.len),
            .steps_count    = steps.len,
            .steps          = arena_copy(session, steps.data, sizeof(writ_plan_step_view_t) * steps.len),
            .outcomes_count = outcomes.len,
            .outcomes       = arena_copy(session, outcomes.data, sizeof(writ_plan_outcome_view_t) * outcomes.len),
        };
    }
    core_list_free(b.items);
    core_list_free(b.slots);
    core_list_free(b.locals);
    for (int i = 0; i < BLOCK_COUNT; i++) {
        core_list_free(b.blocks[i]);
    }
    core_list_free(outcomes);
    core_list_free(steps);
    return plan;
}
