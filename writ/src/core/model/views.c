#include "model.h"

static writ_str_t
sym(const writ_model_t *model, writ_sym_t value)
{
    return writ_sym_str(model->session, value);
}

static const writ_type_view_t *
type_view(const writ_model_t *model, writ_type_id id)
{
    return id && id < model->types.len ? &model->type_views[id] : nullptr;
}

static writ_str_t *
sym_list(writ_model_t *model, const core_list_t(uint32_t) *syms)
{
    writ_str_t *list = writ_alloc(model->session, sizeof(writ_str_t) * syms->len);
    for (size_t i = 0; i < syms->len; i++) {
        list[i] = sym(model, syms->data[i]);
    }
    return list;
}

void
writ_model_build_views(writ_model_t *model)
{
    writ_session_t *session = model->session;
    model->type_views = writ_alloc(session, sizeof(writ_type_view_t) * model->types.len);
    for (size_t i = 1; i < model->types.len; i++) {
        const writ_type_rec_t *type = &model->types.data[i];
        model->type_views[i] = (writ_type_view_t){
            .struct_size = sizeof(writ_type_view_t),
            .kind        = type->key.kind,
            .name        = sym(model, type->key.name),
            .c_spelling  = sym(model, type->c_spelling),
            .size        = type->size,
            .align       = type->align,
            .is_signed   = type->is_signed,
            .nullable    = type->key.nullable,
            .target      = type_view(model, type->key.target),
        };
    }
    model->const_views = writ_alloc(session, sizeof(writ_const_view_t) * model->consts.len);
    for (size_t i = 0; i < model->consts.len; i++) {
        const writ_const_rec_t *record = &model->consts.data[i];
        model->const_views[i] = (writ_const_view_t){
            .struct_size = sizeof(writ_const_view_t),
            .name        = sym(model, record->name),
            .type        = type_view(model, record->type),
            .value       = record->value,
            .declared_at = writ_loc_span(session, record->loc),
        };
    }
    writ_slot_view_t *slots = writ_alloc(session, sizeof(writ_slot_view_t) * model->slots.len);
    for (size_t i = 0; i < model->slots.len; i++) {
        const writ_slot_rec_t *record = &model->slots.data[i];
        slots[i] = (writ_slot_view_t){
            .struct_size = sizeof(writ_slot_view_t),
            .name        = sym(model, record->name),
            .type        = type_view(model, record->type),
            .mode        = record->mode,
            .extent_kind = record->extent_kind,
            .count_expr  = record->count_expr,
            .cap_expr    = record->cap_expr,
            .len_expr    = record->len_expr,
            .declared_at = writ_loc_span(session, record->loc),
        };
    }
    model->fn_views = writ_alloc(session, sizeof(writ_fn_view_t) * model->fns.len);
    model->plan_views = writ_alloc(session, sizeof(writ_plan_view_t *) * model->fns.len);
    for (size_t i = 0; i < model->fns.len; i++) {
        const writ_fn_rec_t *record = &model->fns.data[i];
        model->fn_views[i] = (writ_fn_view_t){
            .struct_size   = sizeof(writ_fn_view_t),
            .contract_name = sym(model, record->name),
            .link_symbol   = sym(model, record->name),
            .slots_count   = record->slot_count,
            .slots         = &slots[record->first_slot],
            .result        = type_view(model, record->result),
            .is_pure       = record->pure,
            .sizing_expr   = record->sizing_expr,
            .error_expr    = record->error_expr,
            .via_expr      = record->via_expr,
            .declared_at   = writ_loc_span(session, record->loc),
        };
    }
    model->library_view = writ_alloc(session, sizeof(writ_library_view_t));
    *model->library_view = (writ_library_view_t){
        .struct_size   = sizeof(writ_library_view_t),
        .name          = sym(model, model->library),
        .links_count   = model->links.len,
        .links         = sym_list(model, &model->links),
        .headers_count = model->headers.len,
        .headers       = sym_list(model, &model->headers),
    };
}

const writ_library_view_t *
writ_model_library(const writ_model_t *model)
{
    return model ? model->library_view : nullptr;
}

size_t
writ_model_fn_count(const writ_model_t *model)
{
    return model ? model->fns.len : 0;
}

const writ_fn_view_t *
writ_model_fn_at(const writ_model_t *model, size_t index)
{
    return model && index < model->fns.len ? &model->fn_views[index] : nullptr;
}

const writ_fn_view_t *
writ_model_fn_named(const writ_model_t *model, writ_str_t name)
{
    for (size_t i = 0; model && i < model->fns.len; i++) {
        if (writ_str_eq(model->fn_views[i].contract_name, name)) {
            return &model->fn_views[i];
        }
    }
    return nullptr;
}

size_t
writ_model_const_count(const writ_model_t *model)
{
    return model ? model->consts.len : 0;
}

const writ_const_view_t *
writ_model_const_at(const writ_model_t *model, size_t index)
{
    return model && index < model->consts.len ? &model->const_views[index] : nullptr;
}

const writ_plan_view_t *
writ_model_fn_plan(const writ_model_t *model, const writ_fn_view_t *fn)
{
    if (!model || !fn || fn < model->fn_views || fn >= model->fn_views + model->fns.len) {
        return nullptr;
    }
    size_t index = (size_t)(fn - model->fn_views);
    writ_model_t *mutable_model = (writ_model_t *)model;
    if (!mutable_model->plan_views[index]) {
        mutable_model->plan_views[index] = writ_plan_build(mutable_model, (uint32_t)index);
    }
    return mutable_model->plan_views[index];
}
