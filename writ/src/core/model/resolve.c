#include "model.h"

static void
model_release(void *object)
{
    writ_model_t *model = object;
    core_list_free(model->links);
    core_list_free(model->headers);
    core_list_free(model->types);
    core_indexed_table_free(model->type_index);
    core_dict_free(&model->type_names);
    core_list_free(model->consts);
    core_list_free(model->slots);
    core_list_free(model->fns);
    core_dict_free(&model->scope);
    core_list_free(model->refs);
}

static writ_model_t *
model_new(writ_unit_t *unit)
{
    writ_session_t *session = unit->session;
    writ_model_t *model = writ_alloc(session, sizeof(writ_model_t));
    *model = (writ_model_t){
        .session = session,
        .unit    = unit,
        .links   = core_list_new(uint32_t),
        .headers = core_list_new(uint32_t),
        .consts  = core_list_new(writ_const_rec_t),
        .slots   = core_list_new(writ_slot_rec_t),
        .fns     = core_list_new(writ_fn_rec_t),
        .refs    = core_list_new(writ_ref_t),
    };
    core_dict_init(&model->scope, nullptr, nullptr);
    writ_types_init(model);
    writ_on_session_free(session, model_release, model);
    return model;
}

static bool
declare(writ_model_t *model, writ_sym_t name, uint32_t kind, uint32_t index, writ_loc_t loc)
{
    if (!core_dict_add(&model->scope, name, ((uintptr_t)kind << 32) | index)) {
        writ_str_t text = writ_sym_str(model->session, name);
        writ_report(model->session, WRIT_D_DUPLICATE, loc, "`%.*s`", (int)text.length, text.data);
        return false;
    }
    return true;
}

static writ_mode_t
mode_of(const writ_model_t *model, writ_sym_t lexeme)
{
    writ_str_t text = writ_sym_str(model->session, lexeme);
    if (writ_str_eq(text, writ_cstr("out"))) {
        return WRIT_MODE_OUT;
    }
    if (writ_str_eq(text, writ_cstr("inout"))) {
        return WRIT_MODE_INOUT;
    }
    return text.length ? WRIT_MODE_IN : writ_default(WRIT_DEFAULT_PARAM_MODE);
}

static void
take_extent(writ_model_t *model, writ_slot_rec_t *slot, uint32_t extent)
{
    const writ_node_t *node = writ_node(model->unit, extent);
    if (node->kind == WRIT_N_EXTENT_COUNT) {
        slot->extent_kind = WRIT_EXTENT_COUNT;
        slot->count_expr = writ_node_child(model->unit, extent, 0);
    } else if (node->kind == WRIT_N_EXTENT_BOUNDED) {
        slot->extent_kind = WRIT_EXTENT_BOUNDED;
        slot->cap_expr = writ_node_child(model->unit, extent, 0);
        slot->len_expr = writ_node_child(model->unit, extent, 1);
    }
}

static writ_type_id
resolve_type(writ_model_t *model, uint32_t id, writ_slot_rec_t *slot)
{
    const writ_node_t *node = writ_node(model->unit, id);
    switch (node->kind) {
    case WRIT_N_TYPE_NAMED: {
        writ_type_id type = writ_type_named(model, node->name);
        if (!type) {
            writ_str_t text = writ_sym_str(model->session, node->name);
            writ_report(model->session, WRIT_D_UNKNOWN_TYPE, node->loc, "`%.*s`", (int)text.length, text.data);
        }
        return type;
    }
    case WRIT_N_TYPE_POINTER:
    case WRIT_N_TYPE_ARRAY: {
        writ_type_id target = resolve_type(model, writ_node_child(model->unit, id, 0), nullptr);
        if (!target) {
            return 0;
        }
        bool nullable = writ_str_eq(writ_sym_str(model->session, node->payload), writ_cstr("?"));
        if (node->kind == WRIT_N_TYPE_ARRAY && slot) {
            take_extent(model, slot, writ_node_child(model->unit, id, 1));
        }
        return writ_type_intern(model, (writ_type_key_t){
            .kind     = node->kind == WRIT_N_TYPE_ARRAY ? WRIT_TYPE_ARRAY : WRIT_TYPE_POINTER,
            .nullable = nullable,
            .target   = target,
        });
    }
    case WRIT_N_TYPE_BYTES:
        if (slot) {
            take_extent(model, slot, writ_node_child(model->unit, id, 0));
        }
        return writ_type_intern(model, (writ_type_key_t){
            .kind   = WRIT_TYPE_ARRAY,
            .target = writ_type_named(model, writ_intern(model->session, "u8", 2)),
        });
    default:
        return 0;
    }
}

static void
collect_fn(writ_model_t *model, uint32_t id)
{
    const writ_node_t *node = writ_node(model->unit, id);
    writ_fn_rec_t record = {
        .name       = node->name,
        .node       = id,
        .first_slot = (uint32_t)model->slots.len,
        .loc        = node->loc,
    };
    for (uint32_t i = 0; i < node->child_count; i++) {
        uint32_t child = writ_node_child(model->unit, id, i);
        const writ_node_t *part = writ_node(model->unit, child);
        switch (part->kind) {
        case WRIT_N_PARAM: {
            writ_slot_rec_t slot = {
                .name = part->name,
                .fn   = (uint32_t)model->fns.len,
                .mode = mode_of(model, part->payload),
                .loc  = part->loc,
            };
            slot.type = resolve_type(model, writ_node_child(model->unit, child, 0), &slot);
            core_list_push(model->slots, slot);
            record.slot_count++;
            break;
        }
        case WRIT_N_RESULT:
            record.result = resolve_type(model, writ_node_child(model->unit, child, 0), nullptr);
            break;
        case WRIT_N_PURE:
            record.pure = true;
            break;
        case WRIT_N_SIZING:
            record.sizing_expr = writ_node_child(model->unit, child, 0);
            break;
        case WRIT_N_ERROR:
            record.error_expr = writ_node_child(model->unit, child, 0);
            record.via_expr = writ_node_child(model->unit, writ_node_first_of(model->unit, child, WRIT_N_VIA), 0);
            break;
        default:
            break;
        }
    }
    if (declare(model, record.name, WRIT_SCOPE_FN, (uint32_t)model->fns.len, record.loc)) {
        core_list_push(model->fns, record);
    } else {
        model->slots.len = record.first_slot;
    }
}

static void
collect(writ_model_t *model, uint32_t id)
{
    const writ_node_t *node = writ_node(model->unit, id);
    switch (node->kind) {
    case WRIT_N_LIBRARY:
        if (model->library) {
            writ_report(model->session, WRIT_D_DUPLICATE, node->loc, "`library`");
        }
        model->library = node->name;
        break;
    case WRIT_N_LINK:
        core_list_push(model->links, node->name);
        break;
    case WRIT_N_HEADER:
        core_list_push(model->headers, node->name);
        break;
    case WRIT_N_CONST: {
        writ_const_rec_t record = {
            .name = node->name,
            .node = id,
            .type = resolve_type(model, writ_node_child(model->unit, id, 0), nullptr),
            .loc  = node->loc,
        };
        if (declare(model, record.name, WRIT_SCOPE_CONST, (uint32_t)model->consts.len, record.loc)) {
            core_list_push(model->consts, record);
        }
        break;
    }
    case WRIT_N_FN:
        collect_fn(model, id);
        break;
    default:
        break;
    }
}

static void
check_expressions(writ_model_t *model)
{
    for (uint32_t i = 0; i < model->consts.len; i++) {
        writ_const_rec_t *record = &model->consts.data[i];
        uint32_t value = writ_node_child(model->unit, record->node, 1);
        if (!writ_expr_check(model, UINT32_MAX, value) || !writ_expr_eval(model, value, nullptr, &record->value)
            || !writ_type_is_integer(model, record->type)) {
            writ_report(model->session, WRIT_D_NOT_CONSTANT, record->loc, nullptr);
        }
    }
    for (uint32_t f = 0; f < model->fns.len; f++) {
        const writ_fn_rec_t *fn = &model->fns.data[f];
        for (uint32_t s = 0; s < fn->slot_count; s++) {
            const writ_slot_rec_t *slot = &model->slots.data[fn->first_slot + s];
            uint32_t exprs[] = {slot->count_expr, slot->cap_expr, slot->len_expr};
            for (size_t e = 0; e < 3; e++) {
                if (exprs[e]) {
                    writ_expr_check(model, f, exprs[e]);
                }
            }
        }
        uint32_t clauses[] = {fn->sizing_expr, fn->error_expr, fn->via_expr};
        for (size_t c = 0; c < 3; c++) {
            if (clauses[c]) {
                writ_expr_check(model, f, clauses[c]);
            }
        }
    }
}

writ_status_t
writ_resolve(writ_unit_t *unit, const writ_model_t **out)
{
    if (!unit || !out) {
        return WRIT_INVALID_ARGUMENT;
    }
    size_t first_diag = unit->session->diags.len;
    writ_model_t *model = model_new(unit);
    for (size_t r = 0; r < unit->roots.len; r++) {
        const writ_node_t *root = writ_node(unit, unit->roots.data[r].root);
        for (uint32_t i = 0; i < root->child_count; i++) {
            collect(model, writ_node_child(unit, unit->roots.data[r].root, i));
        }
    }
    check_expressions(model);
    if (unit->session->diags.len == first_diag) {
        writ_model_check(model);
    }
    bool failed = unit->session->diags.len != first_diag;
    if (!failed) {
        writ_model_build_views(model);
    }
    writ_diag_flush(unit->session, first_diag);
    *out = failed ? nullptr : model;
    return failed ? WRIT_HAS_ERRORS : WRIT_OK;
}
