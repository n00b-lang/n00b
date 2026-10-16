#include "../model/model.h"

core_list_decl(writ_plan_item_view_t);
core_list_decl(writ_plan_slot_view_t);
core_list_decl(writ_plan_local_view_t);
core_list_decl(writ_plan_step_view_t);
core_list_decl(writ_plan_outcome_view_t);
core_list_decl(writ_plan_assign_view_t);

typedef core_list_t(writ_plan_step_view_t) step_list_t;

typedef struct {
    writ_model_t                         *model;
    uint32_t                              fn;
    const writ_fn_rec_t                  *record;
    core_list_t(writ_plan_item_view_t)    items;
    core_list_t(writ_plan_slot_view_t)    slots;
    core_list_t(writ_plan_local_view_t)   locals;
    core_list_t(writ_plan_outcome_view_t) outcomes;
    core_list_t(writ_plan_assign_view_t)  query;
    core_list_t(uint32_t)                 resets;
    step_list_t                           prepare;
    step_list_t                           attempt;
    step_list_t                           finally;
    step_list_t                          *blocks;
    uint32_t                             *slot_item;
    bool                                 *hidden;
    uint32_t                              serialize_item;
    uint32_t                              retry_local;
    bool                                  variadic;
    bool                                  supported;
    uint32_t                              policy_slot;
    uint32_t                              arm;
    uint32_t                              target_index;
    writ_slot_rec_t                       target;
} builder_t;

static const writ_type_view_t *
type_view(const writ_model_t *model, writ_type_id id)
{
    return id ? &model->type_views[id] : nullptr;
}

static const writ_slot_rec_t *
contract_slot(const builder_t *b, uint32_t index)
{
    return index == b->target_index ? &b->target : writ_fn_slot(b->model, b->record, index);
}

static const writ_policy_arm_rec_t *
active_arm(const builder_t *b)
{
    if (b->policy_slot == WRIT_NONE) {
        return nullptr;
    }
    const writ_slot_rec_t *slot = writ_fn_slot(b->model, b->record, b->policy_slot);
    return &b->model->policy_arms.data[slot->first_arm + b->arm];
}

static void
apply_policy_arm(builder_t *b)
{
    for (uint32_t i = 0; b->policy_slot == WRIT_NONE && i < b->record->slot_count; i++) {
        if (writ_fn_slot(b->model, b->record, i)->arm_count) {
            b->policy_slot = i;
        }
    }
    const writ_policy_arm_rec_t *arm = active_arm(b);
    for (uint32_t i = 0; arm && i < b->record->slot_count; i++) {
        const writ_slot_rec_t *slot = writ_fn_slot(b->model, b->record, i);
        if (slot->name != arm->target) {
            continue;
        }
        b->target = *slot;
        b->target_index = i;
        if (arm->has_mode) {
            b->target.mode = arm->mode;
        }
        if (arm->span) {
            b->target.span = arm->span;
            b->target.span_node = arm->span_node;
            b->target.span_with = arm->span_with;
        }
        b->target.release = arm->release ? arm->release : b->target.release;
        b->target.own = b->target.own || arm->own;
    }
}

static writ_after_call_t
input_after_call(const writ_slot_rec_t *slot)
{
    return slot->mode == WRIT_MODE_CONSUME   ? WRIT_AFTER_CALLEE_OWNS
         : slot->span == WRIT_SPAN_UNTIL     ? WRIT_AFTER_BORROWED_UNTIL
         : slot->span == WRIT_SPAN_STATIC    ? WRIT_AFTER_STATIC
                                             : writ_default(WRIT_DEFAULT_INPUT_AFTER_CALL);
}

static const writ_type_rec_t *
physical(const builder_t *b, writ_type_id type)
{
    return writ_type(b->model, writ_type_physical(b->model, type));
}

static writ_sym_t
sym(builder_t *b, const char *text)
{
    return writ_intern(b->model->session, text, strlen(text));
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
emit_to(step_list_t *list, writ_plan_step_view_t value)
{
    core_list_push(*list, value);
}

static bool
unset_in(const builder_t *b, uint32_t item, writ_str_t outcome)
{
    uint32_t slot = item == WRIT_PLAN_NONE ? WRIT_PLAN_NONE : b->items.data[item].contract_slot;
    for (uint32_t i = 0; slot != WRIT_PLAN_NONE && i < b->record->on_count; i++) {
        const writ_on_rec_t *on = &b->model->ons.data[b->record->first_on + i];
        if (on->slot == slot && on->state == WRIT_ON_UNSET
            && writ_str_eq(writ_sym_str(b->model->session, on->outcome), outcome)) {
            return true;
        }
    }
    return false;
}

static void
emit_outcomes(builder_t *b, bool success, bool failure, writ_plan_step_view_t value)
{
    for (uint32_t i = 0; i < b->outcomes.len; i++) {
        bool is_success = b->outcomes.data[i].kind == WRIT_OUTCOME_SUCCESS;
        if (((is_success && success) || (!is_success && failure))
            && !unset_in(b, value.item, b->outcomes.data[i].name)) {
            core_list_push(b->blocks[i], value);
        }
    }
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
                                 .with_item     = WRIT_PLAN_NONE,
                             }));
    if (slot != WRIT_PLAN_NONE) {
        b->slot_item[slot] = (uint32_t)b->items.len - 1;
    }
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

static void
unsupported(builder_t *b, const writ_slot_rec_t *slot, const char *what)
{
    writ_report(b->model->session, WRIT_D_UNSUPPORTED, slot ? slot->loc : b->record->loc, "%s", what);
    b->supported = false;
}

static bool
is_text(const builder_t *b, writ_type_id type)
{
    const writ_type_rec_t *pointer = physical(b, type);
    return pointer->key.kind == WRIT_TYPE_POINTER && writ_type(b->model, pointer->key.target)->key.kind == WRIT_TYPE_CSTR;
}

static writ_plan_shape_t
array_shape(const builder_t *b, writ_type_id array)
{
    const writ_type_rec_t *declared = writ_type(b->model, array);
    const writ_type_rec_t *element = physical(b, physical(b, array)->key.target);
    if (element->key.kind == WRIT_TYPE_INT && element->size == 1) {
        bool text = element->is_signed
                 || (declared->key.kind == WRIT_TYPE_VIEW && writ_is_text(b->model, declared->key.name, "utf8"));
        return text ? WRIT_SHAPE_TEXT : WRIT_SHAPE_BYTES;
    }
    return WRIT_SHAPE_ARRAY;
}

static writ_plan_shape_t
value_shape(const builder_t *b, writ_type_id type)
{
    writ_type_kind_t kind = writ_type(b->model, type)->key.kind;
    return kind == WRIT_TYPE_HANDLE                        ? WRIT_SHAPE_HANDLE
         : kind == WRIT_TYPE_VIEW || kind == WRIT_TYPE_ENUM ? WRIT_SHAPE_VIEW
         : kind == WRIT_TYPE_RECORD                        ? WRIT_SHAPE_RECORD
         : is_text(b, type)                                ? WRIT_SHAPE_TEXT
                                                           : WRIT_SHAPE_SCALAR;
}

static uint32_t
param_index(const builder_t *b, writ_ref_t ref)
{
    return ref.kind == WRIT_REF_SLOT ? ref.index - b->record->first_slot : WRIT_NONE;
}

static void
plan_count_array(builder_t *b, uint32_t index)
{
    const writ_slot_rec_t *slot = contract_slot(b, index);
    writ_ref_t count = writ_expr_ref(b->model, slot->count_expr);
    uint32_t length = param_index(b, count);
    if (length == WRIT_NONE || count.phase != WRIT_REF_VALUE) {
        unsupported(b, slot, "array extents that are not a single parameter");
        return;
    }
    b->hidden[length] = true;
    uint32_t item =
        add_item(b, slot->name, WRIT_ROLE_INPUT, array_shape(b, slot->type), input_after_call(slot), slot->type, index);
    b->items.data[item].nullable = physical(b, slot->type)->key.nullable;
    bind_slot(b, index, item, WRIT_PART_POINTER);
    bind_slot(b, length, item, WRIT_PART_LENGTH);
    writ_plan_step_view_t check = step(WRIT_STEP_CHECK);
    check.item = item;
    check.slot = length;
    emit_to(&b->prepare, check);
    writ_plan_step_view_t split = step(WRIT_STEP_SPLIT);
    split.item = item;
    split.slot = index;
    split.second_slot = length;
    emit_to(&b->prepare, split);
}

static void
plan_text_input(builder_t *b, uint32_t index)
{
    const writ_slot_rec_t *slot = contract_slot(b, index);
    uint32_t item = add_item(b, slot->name, WRIT_ROLE_INPUT, WRIT_SHAPE_TEXT, input_after_call(slot), slot->type, index);
    b->items.data[item].nullable = physical(b, slot->type)->key.nullable;
    bind_slot(b, index, item, WRIT_PART_WHOLE);
    uint32_t local = add_local(b, WRIT_LOCAL_TEXT, writ_type_builtin(b->model, "c_char"), item);
    writ_plan_step_view_t encode = step(WRIT_STEP_ENCODE_CSTR);
    encode.item = item;
    encode.local = local;
    encode.slot = index;
    emit_to(&b->prepare, encode);
    writ_plan_step_view_t release = step(WRIT_STEP_RELEASE_LOCAL);
    release.local = local;
    emit_to(&b->finally, release);
}

static writ_plan_step_view_t
plan_capacity(builder_t *b, uint32_t index, uint32_t item, uint32_t buffer, writ_type_id *cell_type)
{
    const writ_slot_rec_t *slot = contract_slot(b, index);
    writ_ref_t cap = writ_expr_ref(b->model, slot->cap_expr);
    uint32_t cap_slot = param_index(b, cap);
    const writ_slot_rec_t *capacity = contract_slot(b, cap_slot);
    bool cell = cap.phase == WRIT_REF_BEFORE;
    *cell_type = cell ? physical(b, capacity->type)->key.target : capacity->type;
    b->hidden[cap_slot] = true;
    bind_slot(b, cap_slot, item, cell ? WRIT_PART_CAPACITY_AND_LENGTH : WRIT_PART_CAPACITY);
    if (b->record->sizing == WRIT_SIZING_RETRY) {
        b->retry_local = buffer;
    }
    if (b->record->sizing == WRIT_SIZING_NEEDS || b->record->sizing == WRIT_SIZING_QUERY) {
        b->locals.data[buffer].capacity_expr = b->record->sizing_expr;
    } else {
        writ_str_t name = writ_sym_str(b->model->session, slot->name);
        writ_sym_t capacity_name = writ_intern_str(
            b->model->session, writ_str_format(b->model->session, "%.*s_capacity", (int)name.length, name.data));
        b->locals.data[buffer].capacity_item =
            add_item(b, capacity_name, WRIT_ROLE_INPUT, WRIT_SHAPE_SCALAR, WRIT_AFTER_NONE, *cell_type, WRIT_PLAN_NONE);
    }
    if (b->record->sizing == WRIT_SIZING_QUERY) {
        writ_plan_step_view_t query = step(WRIT_STEP_SIZE_QUERY);
        query.local = buffer;
        query.expr = b->record->sizing_expr;
        emit_to(&b->attempt, query);
        for (uint32_t i = 0; i < b->record->assign_count; i++) {
            const writ_assign_rec_t *assign = &b->model->assigns.data[b->record->first_assign + i];
            core_list_push(b->query, ((writ_plan_assign_view_t){sizeof(writ_plan_assign_view_t), assign->slot,
                                                               assign->expr}));
        }
    }
    writ_plan_step_view_t alloc = step(WRIT_STEP_ALLOC_OUT);
    alloc.item = item;
    alloc.local = buffer;
    alloc.expr = b->record->sizing == WRIT_SIZING_NEEDS ? b->record->sizing_expr : 0;
    emit_to(&b->attempt, alloc);
    writ_plan_step_view_t pass = step(WRIT_STEP_PASS);
    pass.source_local = buffer;
    if (!cell) {
        pass.slot = cap_slot;
        emit_to(&b->attempt, pass);
        return step(WRIT_STEP_BIND_OUT);
    }
    pass.local = add_local(b, WRIT_LOCAL_CELL, *cell_type, item);
    emit_to(&b->attempt, pass);
    writ_plan_step_view_t bind = step(WRIT_STEP_BIND_OUT);
    bind.local = pass.local;
    bind.slot = cap_slot;
    return bind;
}

static bool is_total(const builder_t *b, uint32_t index);

static void
plan_bounded_output(builder_t *b, uint32_t index)
{
    const writ_slot_rec_t *slot = contract_slot(b, index);
    uint32_t item = add_item(b, slot->name, WRIT_ROLE_RESULT, array_shape(b, slot->type),
                             writ_default(WRIT_DEFAULT_PROMOTED_AFTER_CALL), slot->type, index);
    bind_slot(b, index, item, WRIT_PART_POINTER);
    uint32_t buffer = add_local(b, WRIT_LOCAL_BUFFER, physical(b, slot->type)->key.target, item);
    writ_type_id cell_type = 0;
    writ_plan_step_view_t bind_cell = plan_capacity(b, index, item, buffer, &cell_type);
    writ_plan_step_view_t bind = step(WRIT_STEP_BIND_OUT);
    bind.local = buffer;
    bind.slot = index;
    emit_to(&b->attempt, bind);
    if (bind_cell.local != WRIT_PLAN_NONE) {
        emit_to(&b->attempt, bind_cell);
    }
    for (uint32_t i = 0; i < b->record->slot_count; i++) {
        if (i != index && is_total(b, i)
            && writ_expr_mentions(b->model, slot->len_expr, WRIT_REF_SLOT, b->record->first_slot + i)) {
            bind_slot(b, i, item, WRIT_PART_LENGTH);
            writ_plan_step_view_t total = step(WRIT_STEP_BIND_OUT);
            total.local = add_local(b, WRIT_LOCAL_CELL, physical(b, contract_slot(b, i)->type)->key.target, item);
            total.slot = i;
            emit_to(&b->attempt, total);
        }
    }
    writ_plan_step_view_t lift = step(WRIT_STEP_LIFT_BUFFER);
    lift.local = buffer;
    lift.item = item;
    lift.expr = slot->len_expr;
    emit_outcomes(b, true, false, lift);
    writ_plan_step_view_t release = step(WRIT_STEP_RELEASE_LOCAL);
    release.local = buffer;
    emit_to(&b->finally, release);
}

static bool
on_state(const builder_t *b, uint32_t slot, writ_on_state_t state)
{
    for (uint32_t i = 0; i < b->record->on_count; i++) {
        const writ_on_rec_t *on = &b->model->ons.data[b->record->first_on + i];
        if (on->slot == slot && on->state == state) {
            return true;
        }
    }
    return false;
}

static uint32_t
item_for_name(const builder_t *b, writ_sym_t name)
{
    uint32_t slot = writ_fn_slot_named(b->model, b->record, name);
    return slot == WRIT_NONE ? WRIT_PLAN_NONE : b->slot_item[slot];
}

static void
lift_pointer_result(builder_t *b, const writ_slot_rec_t *slot, uint32_t item, writ_plan_step_view_t source,
                    bool on_failure)
{
    writ_plan_item_view_t *view = &b->items.data[item];
    if (writ_type(b->model, slot->type)->key.kind == WRIT_TYPE_HANDLE) {
        const writ_type_rec_t *type = writ_type(b->model, slot->type);
        uint32_t handle = writ_type(b->model, writ_type_named(b->model, type->key.name))->decl;
        view->after_call = WRIT_AFTER_CALLER_OWNS;
        view->release = writ_sym_str(b->model->session, b->model->handles.data[handle].release);
        view->nullable = type->key.nullable;
        source.kind = WRIT_STEP_WRAP_HANDLE;
        emit_outcomes(b, true, on_failure, source);
        return;
    }
    if (slot->own) {
        view->after_call = WRIT_AFTER_CALLER_OWNS;
        view->release = writ_sym_str(b->model->session, slot->release);
        source.kind = WRIT_STEP_WRAP_HANDLE;
        emit_outcomes(b, true, on_failure, source);
        return;
    }
    view->nullable = physical(b, slot->type)->key.nullable;
    switch (slot->span) {
    case WRIT_SPAN_STATIC:
    case WRIT_SPAN_STATIC_PER_THREAD:
        view->after_call = WRIT_AFTER_STATIC;
        source.kind = WRIT_STEP_LIFT_VALUE;
        break;
    case WRIT_SPAN_WITH:
        view->after_call = WRIT_AFTER_BORROWED_WITH;
        view->with_item = item_for_name(b, slot->span_with);
        source.kind = view->with_item != WRIT_PLAN_NONE ? WRIT_STEP_LIFT_VALUE : WRIT_STEP_COPY_BORROWED;
        break;
    case WRIT_SPAN_UNTIL:
        view->after_call = WRIT_AFTER_BORROWED_UNTIL;
        source.kind = WRIT_STEP_COPY_BORROWED;
        break;
    default:
        unsupported(b, slot, "a binding for a pointer whose lifetime is `unknown`");
        return;
    }
    emit_outcomes(b, true, false, source);
}

static void
plan_out_sized(builder_t *b, uint32_t index)
{
    const writ_slot_rec_t *slot = contract_slot(b, index);
    writ_type_id target = physical(b, slot->type)->key.target;
    writ_ref_t count = writ_expr_ref(b->model, slot->count_expr);
    uint32_t length = param_index(b, count);
    if (length == WRIT_NONE || count.phase == WRIT_REF_VALUE || contract_slot(b, length)->mode != WRIT_MODE_OUT) {
        unsupported(b, slot, "output array extents that are not a single output parameter");
        return;
    }
    if (!slot->own) {
        unsupported(b, slot, "output arrays returned through a pointer that are not `own`");
        return;
    }
    uint32_t item = add_item(b, slot->name, WRIT_ROLE_RESULT, array_shape(b, target), WRIT_AFTER_CALLER_OWNS, target,
                             index);
    b->items.data[item].nullable = physical(b, target)->key.nullable;
    b->items.data[item].release = writ_sym_str(b->model->session, slot->release);
    bind_slot(b, index, item, WRIT_PART_POINTER);
    bind_slot(b, length, item, WRIT_PART_LENGTH);
    writ_plan_step_view_t bind = step(WRIT_STEP_BIND_OUT);
    bind.local = add_local(b, WRIT_LOCAL_CELL, target, item);
    bind.slot = index;
    emit_to(&b->attempt, bind);
    writ_plan_step_view_t total = step(WRIT_STEP_BIND_OUT);
    total.local = add_local(b, WRIT_LOCAL_CELL, physical(b, contract_slot(b, length)->type)->key.target, item);
    total.slot = length;
    emit_to(&b->attempt, total);
    writ_plan_step_view_t lift = step(WRIT_STEP_COPY_BORROWED);
    lift.local = bind.local;
    lift.item = item;
    lift.expr = slot->count_expr;
    emit_outcomes(b, true, false, lift);
}

static void
plan_out_pointer(builder_t *b, uint32_t index)
{
    const writ_slot_rec_t *slot = contract_slot(b, index);
    writ_type_id target = physical(b, slot->type)->key.target;
    if (physical(b, target)->key.kind == WRIT_TYPE_ARRAY && slot->extent_kind == WRIT_EXTENT_COUNT) {
        plan_out_sized(b, index);
        return;
    }
    uint32_t item = add_item(b, slot->name, WRIT_ROLE_RESULT, value_shape(b, target), WRIT_AFTER_NONE, target, index);
    bind_slot(b, index, item, WRIT_PART_WHOLE);
    uint32_t cell = add_local(b, WRIT_LOCAL_CELL, target, item);
    writ_plan_step_view_t bind = step(WRIT_STEP_BIND_OUT);
    bind.local = cell;
    bind.slot = index;
    emit_to(&b->attempt, bind);
    writ_plan_step_view_t lift = step(WRIT_STEP_LIFT_VALUE);
    lift.local = cell;
    lift.item = item;
    if (writ_type_is_pointer(b->model, target) || writ_type(b->model, target)->key.kind == WRIT_TYPE_HANDLE) {
        writ_slot_rec_t pointee = *slot;
        pointee.type = target;
        lift_pointer_result(b, &pointee, item, lift, on_state(b, index, WRIT_ON_OWN));
    } else {
        emit_outcomes(b, true, false, lift);
    }
}

static writ_plan_step_view_t
whole_input(builder_t *b, uint32_t index, writ_plan_role_t role, writ_plan_shape_t shape, writ_after_call_t after)
{
    const writ_slot_rec_t *slot = contract_slot(b, index);
    uint32_t item = add_item(b, slot->name, role, shape, after, slot->type, index);
    b->items.data[item].nullable = writ_type(b->model, slot->type)->key.nullable;
    bind_slot(b, index, item, WRIT_PART_WHOLE);
    writ_plan_step_view_t pass = step(WRIT_STEP_PASS);
    pass.item = item;
    pass.slot = index;
    return pass;
}

static void
plan_value_input(builder_t *b, uint32_t index)
{
    const writ_slot_rec_t *slot = contract_slot(b, index);
    writ_plan_shape_t shape = value_shape(b, slot->type);
    bool pointer = writ_type_is_pointer(b->model, slot->type);
    if (pointer) {
        const writ_type_rec_t *target = physical(b, physical(b, slot->type)->key.target);
        shape = target->key.kind == WRIT_TYPE_RECORD ? WRIT_SHAPE_RECORD : WRIT_SHAPE_SCALAR;
    }
    if (index == b->record->keywords_slot) {
        shape = WRIT_SHAPE_KEYWORD_PACK;
    }
    writ_plan_role_t role = slot->mode == WRIT_MODE_INOUT ? WRIT_ROLE_INPUT_OUTPUT : WRIT_ROLE_INPUT;
    writ_after_call_t after = slot->mode == WRIT_MODE_CONSUME ? WRIT_AFTER_CALLEE_OWNS
                            : pointer                         ? writ_default(WRIT_DEFAULT_INPUT_AFTER_CALL)
                                                              : WRIT_AFTER_NONE;
    writ_plan_step_view_t pass = whole_input(b, index, role, shape, after);
    uint32_t item = pass.item;
    emit_to(&b->prepare, pass);
    if (shape == WRIT_SHAPE_HANDLE && writ_type(b->model, slot->type)->key.kind == WRIT_TYPE_HANDLE) {
        const writ_type_rec_t *type = writ_type(b->model, slot->type);
        const writ_handle_rec_t *handle =
            &b->model->handles.data[writ_type(b->model, writ_type_named(b->model, type->key.name))->decl];
        if (handle->thread_rule == WRIT_THREAD_RULE_SERIALIZED && b->serialize_item == WRIT_PLAN_NONE) {
            b->serialize_item = item;
        }
    }
    if (slot->mode == WRIT_MODE_CONSUME) {
        writ_plan_step_view_t consumed = step(WRIT_STEP_MARK_CONSUMED);
        consumed.item = item;
        emit_outcomes(b, true, !on_state(b, index, WRIT_ON_NOT_CONSUMED), consumed);
    }
}

static uint32_t
userdata_partner(const builder_t *b, uint32_t index)
{
    writ_sym_t name = contract_slot(b, index)->name;
    for (uint32_t i = 0; i < b->record->slot_count; i++) {
        const writ_slot_rec_t *slot = contract_slot(b, i);
        if (slot->userdata && slot->userdata_for == name) {
            return i;
        }
    }
    return WRIT_NONE;
}

static writ_after_call_t
callback_after_call(writ_span_kind_t span)
{
    switch (span) {
    case WRIT_SPAN_UNTIL:
        return WRIT_AFTER_BORROWED_UNTIL;
    case WRIT_SPAN_ONCE:
        return WRIT_AFTER_CALLEE_OWNS;
    case WRIT_SPAN_STATIC:
    case WRIT_SPAN_STATIC_PER_THREAD:
        return WRIT_AFTER_STATIC;
    default:
        return WRIT_AFTER_BORROWED;
    }
}

static void
plan_callback_input(builder_t *b, uint32_t index)
{
    writ_plan_step_view_t pass = whole_input(b, index, WRIT_ROLE_INPUT, WRIT_SHAPE_CALLBACK,
                                             callback_after_call(contract_slot(b, index)->span));
    pass.second_slot = userdata_partner(b, index);
    if (pass.second_slot != WRIT_NONE) {
        bind_slot(b, pass.second_slot, pass.item, WRIT_PART_USERDATA);
    }
    emit_to(&b->prepare, pass);
}

static bool
is_total(const builder_t *b, uint32_t index)
{
    const writ_slot_rec_t *slot = contract_slot(b, index);
    if (slot->mode != WRIT_MODE_OUT || physical(b, slot->type)->key.kind != WRIT_TYPE_POINTER) {
        return false;
    }
    for (uint32_t i = 0; i < b->record->slot_count; i++) {
        if (writ_expr_mentions(b->model, contract_slot(b, i)->len_expr, WRIT_REF_SLOT, b->record->first_slot + index)) {
            return true;
        }
    }
    return false;
}

static void
hide_dependent_slots(builder_t *b)
{
    for (uint32_t i = 0; i < b->record->slot_count; i++) {
        b->hidden[i] = b->hidden[i] || is_total(b, i);
        const writ_slot_rec_t *slot = contract_slot(b, i);
        uint32_t expr = slot->extent_kind == WRIT_EXTENT_COUNT ? slot->count_expr
                      : slot->extent_kind == WRIT_EXTENT_BOUNDED ? slot->cap_expr
                                                                 : 0;
        uint32_t index = param_index(b, writ_expr_ref(b->model, expr));
        if (index != WRIT_NONE) {
            b->hidden[index] = true;
        }
    }
    if (b->record->format_slot != WRIT_NONE) {
        b->hidden[b->record->format_slot] = true;
    }
    if (b->record->varargs_count_slot != WRIT_NONE) {
        b->hidden[b->record->varargs_count_slot] = true;
    }
    for (uint32_t i = 0; i < b->record->slot_count; i++) {
        b->hidden[i] = b->hidden[i] || contract_slot(b, i)->userdata;
    }
}

static void
plan_inputs(builder_t *b)
{
    hide_dependent_slots(b);
    for (uint32_t i = 0; i < b->record->slot_count; i++) {
        const writ_slot_rec_t *slot = contract_slot(b, i);
        const writ_type_rec_t *type = physical(b, slot->type);
        if (b->hidden[i] || type->key.kind == WRIT_TYPE_VA_LIST) {
            continue;
        }
        bool passes_in = slot->mode == WRIT_MODE_IN || slot->mode == WRIT_MODE_KEEP || slot->mode == WRIT_MODE_CONSUME;
        if (i == b->policy_slot && !active_arm(b)->is_else) {
            writ_plan_step_view_t pass = step(WRIT_STEP_PASS_CONST);
            pass.slot = i;
            pass.literal = writ_sym_str(b->model->session, active_arm(b)->constant);
            emit_to(&b->prepare, pass);
        } else if (type->key.kind == WRIT_TYPE_CALLBACK) {
            plan_callback_input(b, i);
        } else if (slot->extent_kind == WRIT_EXTENT_UNKNOWN || slot->span == WRIT_SPAN_UNKNOWN
                   || slot->mode == WRIT_MODE_UNKNOWN) {
            unsupported(b, slot, "a binding for a parameter with `unknown` facts");
        } else if (slot->mode == WRIT_MODE_OUT && type->key.kind == WRIT_TYPE_ARRAY) {
            if (slot->extent_kind == WRIT_EXTENT_BOUNDED) {
                plan_bounded_output(b, i);
            } else {
                unsupported(b, slot, "output arrays without `cap ..., len ...`");
            }
        } else if (slot->mode == WRIT_MODE_OUT && type->key.kind == WRIT_TYPE_POINTER) {
            plan_out_pointer(b, i);
        } else if (passes_in && type->key.kind == WRIT_TYPE_ARRAY && slot->extent_kind == WRIT_EXTENT_COUNT) {
            plan_count_array(b, i);
        } else if (slot->mode == WRIT_MODE_IN && is_text(b, slot->type)) {
            plan_text_input(b, i);
        } else if (type->key.kind == WRIT_TYPE_ARRAY || slot->mode == WRIT_MODE_OUT || slot->mode == WRIT_MODE_KEEP) {
            unsupported(b, slot, "this parameter shape");
        } else {
            plan_value_input(b, i);
        }
    }
}

static bool
result_consumed(const builder_t *b)
{
    for (uint32_t i = 0; i < b->record->outcome_count; i++) {
        const writ_outcome_rec_t *outcome = &b->model->outcomes.data[b->record->first_outcome + i];
        if (writ_expr_mentions(b->model, outcome->when_expr, WRIT_REF_RESULT, b->fn)) {
            return true;
        }
    }
    for (uint32_t i = 0; i < b->record->slot_count; i++) {
        if (writ_expr_mentions(b->model, contract_slot(b, i)->len_expr, WRIT_REF_RESULT, b->fn)) {
            return true;
        }
    }
    return writ_expr_mentions(b->model, b->record->sizing_expr, WRIT_REF_RESULT, b->fn);
}

static void
plan_result(builder_t *b)
{
    if (!b->record->has_result) {
        return;
    }
    uint32_t ret_slot = b->record->slot_count;
    const writ_slot_rec_t *slot = contract_slot(b, ret_slot);
    bool pointer = writ_type_is_pointer(b->model, slot->type) || writ_type(b->model, slot->type)->key.kind == WRIT_TYPE_HANDLE;
    if (!pointer && result_consumed(b)) {
        return;
    }
    uint32_t item = add_item(b, sym(b, "result"), WRIT_ROLE_RESULT, value_shape(b, slot->type), WRIT_AFTER_NONE,
                             slot->type, WRIT_PLAN_NONE);
    bind_slot(b, ret_slot, item, WRIT_PART_WHOLE);
    writ_plan_step_view_t lift = step(WRIT_STEP_LIFT_VALUE);
    lift.slot = ret_slot;
    lift.item = item;
    if (pointer) {
        lift_pointer_result(b, slot, item, lift, false);
    } else {
        emit_outcomes(b, true, false, lift);
    }
}

static void
note_channels(builder_t *b, uint32_t expr)
{
    for (uint32_t c = 0; expr && c < b->model->channels.len; c++) {
        if (!b->model->channels.data[c].reset || !writ_expr_mentions(b->model, expr, WRIT_REF_CHANNEL, c)) {
            continue;
        }
        bool seen = false;
        core_list_foreach(b->resets, existing) {
            seen = seen || *existing == c;
        }
        if (!seen) {
            core_list_push(b->resets, c);
        }
    }
}

static void
plan_details(builder_t *b)
{
    uint32_t failures = 0;
    for (uint32_t i = 0; i < b->outcomes.len; i++) {
        failures += b->outcomes.data[i].kind == WRIT_OUTCOME_FAILURE;
    }
    for (uint32_t i = 0; i < b->record->outcome_count; i++) {
        const writ_outcome_rec_t *outcome = &b->model->outcomes.data[b->record->first_outcome + i];
        note_channels(b, outcome->when_expr);
        note_channels(b, outcome->via_expr);
        if (!outcome->via_expr) {
            continue;
        }
        writ_ref_t via = writ_expr_ref(b->model, outcome->via_expr);
        writ_type_id type = writ_expr_type(b->model, outcome->via_expr);
        if (!type) {
            type = writ_type_builtin(b->model, "i64");
        }
        writ_str_t outcome_name = writ_sym_str(b->model->session, outcome->name);
        const char *base = via.kind == WRIT_REF_RESULT ? "status" : "detail";
        writ_sym_t name = failures > 1 ? writ_intern_str(b->model->session,
                                                         writ_str_format(b->model->session, "%.*s_%s",
                                                                         (int)outcome_name.length, outcome_name.data, base))
                                       : sym(b, base);
        bool text = via.kind == WRIT_REF_CHANNEL && b->model->channels.data[via.index].kind == WRIT_CHANNEL_TEXT;
        uint32_t item = add_item(b, name, WRIT_ROLE_ERROR_DETAIL, text ? WRIT_SHAPE_TEXT : value_shape(b, type),
                                 text ? WRIT_AFTER_CALLER_OWNS : WRIT_AFTER_NONE, type, WRIT_PLAN_NONE);
        writ_plan_step_view_t fetch = step(WRIT_STEP_FETCH_DETAIL);
        fetch.item = item;
        fetch.expr = outcome->via_expr;
        core_list_push(b->blocks[i], fetch);
    }
}

static void
plan_varargs(builder_t *b)
{
    const writ_fn_rec_t *record = b->record;
    if (record->varargs == WRIT_VARARGS_NONE) {
        return;
    }
    uint32_t tail = (uint32_t)b->slots.len;
    for (uint32_t i = 0; i < b->record->slot_count; i++) {
        if (b->slots.data[i].pass == WRIT_PASS_VARIADIC_TAIL) {
            tail = i;
        }
    }
    if (tail == b->slots.len) {
        core_list_push(b->slots, ((writ_plan_slot_view_t){
                                     .struct_size   = sizeof(writ_plan_slot_view_t),
                                     .c_type        = type_view(b->model, record->varargs_type),
                                     .pass          = WRIT_PASS_VARIADIC_TAIL,
                                     .item          = WRIT_PLAN_NONE,
                                     .contract_slot = WRIT_PLAN_NONE,
                                 }));
    }
    b->variadic = true;
    writ_plan_step_view_t expand = step(WRIT_STEP_EXPAND);
    expand.slot = tail;
    if (record->format_slot != WRIT_NONE) {
        writ_plan_step_view_t literal = step(WRIT_STEP_PASS);
        literal.slot = record->format_slot;
        literal.literal = writ_cstr("%s");
        emit_to(&b->prepare, literal);
        uint32_t item = add_item(b, sym(b, "text"), WRIT_ROLE_INPUT, WRIT_SHAPE_TEXT, WRIT_AFTER_BORROWED,
                                 contract_slot(b, record->format_slot)->type, WRIT_PLAN_NONE);
        bind_slot(b, tail, item, WRIT_PART_ELEMENTS);
        uint32_t local = add_local(b, WRIT_LOCAL_TEXT, writ_type_builtin(b->model, "c_char"), item);
        writ_plan_step_view_t encode = step(WRIT_STEP_ENCODE_CSTR);
        encode.item = item;
        encode.local = local;
        emit_to(&b->prepare, encode);
        expand.item = item;
        expand.local = local;
        emit_to(&b->attempt, expand);
        writ_plan_step_view_t release = step(WRIT_STEP_RELEASE_LOCAL);
        release.local = local;
        emit_to(&b->finally, release);
        return;
    }
    if (record->varargs != WRIT_VARARGS_OF && record->varargs != WRIT_VARARGS_UNTIL && record->varargs != WRIT_VARARGS_COUNT) {
        unsupported(b, nullptr, "a binding for `...` with no element rule");
        return;
    }
    uint32_t item = add_item(b, sym(b, "args"), WRIT_ROLE_PACK, WRIT_SHAPE_REST_PACK, WRIT_AFTER_BORROWED,
                             record->varargs_type, WRIT_PLAN_NONE);
    bind_slot(b, tail, item, WRIT_PART_ELEMENTS);
    if (record->varargs_count_slot != WRIT_NONE) {
        bind_slot(b, record->varargs_count_slot, item, WRIT_PART_LENGTH);
        writ_plan_step_view_t count = step(WRIT_STEP_PASS);
        count.item = item;
        count.slot = record->varargs_count_slot;
        emit_to(&b->prepare, count);
    }
    expand.item = item;
    expand.expr = record->varargs_end;
    emit_to(&b->attempt, expand);
}

static void
plan_slots(builder_t *b)
{
    uint32_t count = b->record->slot_count + (b->record->has_result ? 1 : 0);
    for (uint32_t i = 0; i < count; i++) {
        bool is_ret = i == b->record->slot_count;
        const writ_slot_rec_t *slot = contract_slot(b, i);
        const writ_type_rec_t *type = physical(b, slot->type);
        bool indirect = type->key.kind == WRIT_TYPE_POINTER || type->key.kind == WRIT_TYPE_ARRAY;
        writ_pass_t pass = type->key.kind == WRIT_TYPE_VA_LIST     ? WRIT_PASS_VARIADIC_TAIL
                         : is_ret || !indirect                     ? WRIT_PASS_VALUE
                         : slot->mode == WRIT_MODE_OUT             ? WRIT_PASS_OUT_POINTER
                         : slot->mode == WRIT_MODE_INOUT           ? WRIT_PASS_INOUT_POINTER
                                                                   : WRIT_PASS_IN_POINTER;
        core_list_push(b->slots, ((writ_plan_slot_view_t){
                                     .struct_size   = sizeof(writ_plan_slot_view_t),
                                     .c_type        = type_view(b->model, slot->physical ? slot->physical : slot->type),
                                     .pass          = pass,
                                     .part          = WRIT_PART_WHOLE,
                                     .item          = WRIT_PLAN_NONE,
                                     .contract_slot = is_ret ? WRIT_PLAN_NONE : i,
                                 }));
    }
}

static void
plan_outcomes(builder_t *b)
{
    for (uint32_t i = 0; i < b->record->outcome_count; i++) {
        const writ_outcome_rec_t *outcome = &b->model->outcomes.data[b->record->first_outcome + i];
        core_list_push(b->outcomes, ((writ_plan_outcome_view_t){
                                        .struct_size = sizeof(writ_plan_outcome_view_t),
                                        .name        = writ_sym_str(b->model->session, outcome->name),
                                        .kind        = outcome->kind,
                                        .when_expr   = outcome->when_expr,
                                        .via_expr    = outcome->via_expr,
                                        .retry       = outcome->retry,
                                    }));
    }
    if (!b->outcomes.len) {
        core_list_push(b->outcomes, ((writ_plan_outcome_view_t){.struct_size = sizeof(writ_plan_outcome_view_t),
                                                               .name        = writ_cstr("ok"),
                                                               .kind        = WRIT_OUTCOME_SUCCESS}));
    }
    b->blocks = writ_alloc(b->model->session, sizeof(step_list_t) * b->outcomes.len);
    for (uint32_t i = 0; i < b->outcomes.len; i++) {
        b->blocks[i] = core_list_new(writ_plan_step_view_t);
    }
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
    return ref.index - model->fns.data[writ_model_fn_index(model, plan->fn)].first_slot;
}

static void *
arena_copy(writ_session_t *session, const void *data, size_t size)
{
    void *copy = writ_alloc(session, size ? size : 1);
    if (size) {
        memcpy(copy, data, size);
    }
    return copy;
}

static void
append_block(step_list_t *steps, step_list_t *block, writ_phase_t phase, uint32_t outcome_index)
{
    for (size_t i = 0; i < block->len; i++) {
        writ_plan_step_view_t value = block->data[i];
        value.phase = phase;
        value.outcome = outcome_index;
        core_list_push(*steps, value);
    }
}

#define COPY(session, list) arena_copy(session, (list).data, sizeof((list).data[0]) * (list).len)

static void
builder_free(builder_t *b)
{
    core_list_free(b->items);
    core_list_free(b->slots);
    core_list_free(b->locals);
    core_list_free(b->outcomes);
    core_list_free(b->query);
    core_list_free(b->resets);
    core_list_free(b->prepare);
    core_list_free(b->attempt);
    core_list_free(b->finally);
}

static void
mark_unset_results(builder_t *b)
{
    for (uint32_t item = 0; item < b->items.len; item++) {
        writ_plan_item_view_t *view = &b->items.data[item];
        for (uint32_t i = 0; view->role == WRIT_ROLE_RESULT && i < b->outcomes.len; i++) {
            if (b->outcomes.data[i].kind == WRIT_OUTCOME_SUCCESS && unset_in(b, item, b->outcomes.data[i].name)) {
                view->optional = true;
            }
        }
    }
}

const writ_plan_view_t *
writ_plan_build(writ_model_t *model, uint32_t fn, uint32_t arm)
{
    writ_session_t *session = model->session;
    builder_t b = {
        .model          = model,
        .fn             = fn,
        .record         = &model->fns.data[fn],
        .items          = core_list_new(writ_plan_item_view_t),
        .slots          = core_list_new(writ_plan_slot_view_t),
        .locals         = core_list_new(writ_plan_local_view_t),
        .outcomes       = core_list_new(writ_plan_outcome_view_t),
        .query          = core_list_new(writ_plan_assign_view_t),
        .resets         = core_list_new(uint32_t),
        .prepare        = core_list_new(writ_plan_step_view_t),
        .attempt        = core_list_new(writ_plan_step_view_t),
        .finally        = core_list_new(writ_plan_step_view_t),
        .serialize_item = WRIT_PLAN_NONE,
        .retry_local    = WRIT_PLAN_NONE,
        .supported      = true,
        .policy_slot    = WRIT_NONE,
        .arm            = arm,
        .target_index   = WRIT_NONE,
    };
    uint32_t slots = b.record->slot_count + 2;
    b.hidden = writ_alloc(session, sizeof(bool) * slots);
    b.slot_item = writ_alloc(session, sizeof(uint32_t) * slots);
    memset(b.hidden, 0, sizeof(bool) * slots);
    memset(b.slot_item, 0xff, sizeof(uint32_t) * slots);
    apply_policy_arm(&b);
    plan_slots(&b);
    plan_outcomes(&b);
    plan_inputs(&b);
    plan_varargs(&b);
    plan_result(&b);
    plan_details(&b);
    mark_unset_results(&b);

    step_list_t steps = core_list_new(writ_plan_step_view_t);
    append_block(&steps, &b.prepare, WRIT_PHASE_PREPARE, WRIT_PLAN_NONE);
    append_block(&steps, &b.attempt, WRIT_PHASE_ATTEMPT, WRIT_PLAN_NONE);
    writ_plan_step_view_t call = step(WRIT_STEP_CALL);
    call.phase = WRIT_PHASE_ATTEMPT;
    core_list_push(steps, call);
    if (b.retry_local != WRIT_PLAN_NONE) {
        writ_plan_step_view_t retry = step(WRIT_STEP_SIZE_RETRY);
        retry.phase = WRIT_PHASE_ATTEMPT;
        retry.local = b.retry_local;
        retry.expr = b.record->sizing_expr;
        core_list_push(steps, retry);
    }
    writ_plan_step_view_t classify = step(WRIT_STEP_CLASSIFY);
    classify.phase = WRIT_PHASE_CLASSIFY;
    core_list_push(steps, classify);
    for (uint32_t i = 0; i < b.outcomes.len; i++) {
        append_block(&steps, &b.blocks[i], WRIT_PHASE_OUTCOME, i);
        core_list_free(b.blocks[i]);
    }
    append_block(&steps, &b.finally, WRIT_PHASE_FINALLY, WRIT_PLAN_NONE);

    writ_plan_view_t *plan = nullptr;
    if (b.supported) {
        plan = writ_alloc(session, sizeof(writ_plan_view_t));
        *plan = (writ_plan_view_t){
            .struct_size          = sizeof(writ_plan_view_t),
            .fn                   = &model->fn_views[fn],
            .query_count          = b.query.len,
            .query                = COPY(session, b.query),
            .items_count          = b.items.len,
            .items                = COPY(session, b.items),
            .slots_count          = b.slots.len,
            .slots                = COPY(session, b.slots),
            .locals_count         = b.locals.len,
            .locals               = COPY(session, b.locals),
            .steps_count          = steps.len,
            .steps                = COPY(session, steps),
            .outcomes_count       = b.outcomes.len,
            .outcomes             = COPY(session, b.outcomes),
            .serialize_item       = b.serialize_item,
            .reset_channels_count = b.resets.len,
            .reset_channels       = COPY(session, b.resets),
            .variadic             = b.variadic,
            .policy_slot          = b.policy_slot == WRIT_NONE ? WRIT_PLAN_NONE : b.policy_slot,
            .policy_arm           = b.policy_slot == WRIT_NONE ? WRIT_PLAN_NONE : b.arm,
        };
    }
    builder_free(&b);
    core_list_free(steps);
    return plan;
}
