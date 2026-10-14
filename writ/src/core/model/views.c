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
    writ_str_t *list = writ_alloc(model->session, sizeof(writ_str_t) * (syms->len ? syms->len : 1));
    for (size_t i = 0; i < syms->len; i++) {
        list[i] = sym(model, syms->data[i]);
    }
    return list;
}

#define ALLOC_VIEWS(model, type, count) writ_alloc((model)->session, sizeof(type) * ((count) ? (count) : 1))

static void
build_types(writ_model_t *model)
{
    model->type_views = ALLOC_VIEWS(model, writ_type_view_t, model->types.len);
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
            .count       = type->key.count,
            .decl        = type->decl,
        };
    }
}

static void
build_slots(writ_model_t *model)
{
    writ_session_t *session = model->session;
    model->slot_views = ALLOC_VIEWS(model, writ_slot_view_t, model->slots.len);
    for (size_t i = 0; i < model->slots.len; i++) {
        const writ_slot_rec_t *record = &model->slots.data[i];
        model->slot_views[i] = (writ_slot_view_t){
            .struct_size   = sizeof(writ_slot_view_t),
            .name          = sym(model, record->name),
            .type          = type_view(model, record->type),
            .mode          = record->mode,
            .extent_kind   = record->extent_kind,
            .count_expr    = record->count_expr,
            .cap_expr      = record->cap_expr,
            .len_expr      = record->len_expr,
            .span          = record->span,
            .span_with     = sym(model, record->span_with),
            .own           = record->own,
            .release       = sym(model, record->release),
            .alloc         = sym(model, record->alloc),
            .null_if_empty = record->null_if_empty,
            .is_private    = record->is_private,
            .offset        = record->offset,
            .declared_at   = writ_loc_span(session, record->loc),
        };
    }
}

static void
build_fns(writ_model_t *model)
{
    writ_session_t *session = model->session;
    writ_outcome_view_t *outcomes = ALLOC_VIEWS(model, writ_outcome_view_t, model->outcomes.len);
    for (size_t i = 0; i < model->outcomes.len; i++) {
        const writ_outcome_rec_t *record = &model->outcomes.data[i];
        outcomes[i] = (writ_outcome_view_t){
            .struct_size = sizeof(writ_outcome_view_t),
            .name        = sym(model, record->name),
            .kind        = record->kind,
            .when_expr   = record->when_expr,
            .via_expr    = record->via_expr,
            .retry       = record->retry,
        };
    }
    model->fn_views = ALLOC_VIEWS(model, writ_fn_view_t, model->fns.len);
    model->plan_views = ALLOC_VIEWS(model, writ_plan_view_t *, model->fns.len);
    for (size_t i = 0; i < model->fns.len; i++) {
        const writ_fn_rec_t *record = &model->fns.data[i];
        model->fn_views[i] = (writ_fn_view_t){
            .struct_size        = sizeof(writ_fn_view_t),
            .contract_name      = sym(model, record->name),
            .source_name        = sym(model, record->source_name),
            .link_symbol        = sym(model, record->link_symbol),
            .kind               = record->kind,
            .slots_count        = record->slot_count,
            .slots              = &model->slot_views[record->first_slot],
            .result             = record->has_result ? &model->slot_views[record->first_slot + record->slot_count] : nullptr,
            .is_pure            = record->pure,
            .sizing             = record->sizing,
            .sizing_expr        = record->sizing_expr,
            .sizing_slot        = record->sizing_slot,
            .outcomes_count     = record->outcome_count,
            .outcomes           = &outcomes[record->first_outcome],
            .varargs            = record->varargs,
            .varargs_type       = type_view(model, record->varargs_type),
            .varargs_end_expr   = record->varargs_end,
            .varargs_count_slot = record->varargs_count_slot,
            .format_slot        = record->format_slot,
            .expands_symbol     = sym(model, record->expands_symbol),
            .expands_node       = record->expands_node,
            .thread             = record->thread,
            .declared_at        = writ_loc_span(session, record->loc),
        };
    }
}

static void
build_consts(writ_model_t *model)
{
    model->const_views = ALLOC_VIEWS(model, writ_const_view_t, model->consts.len);
    for (size_t i = 0; i < model->consts.len; i++) {
        const writ_const_rec_t *record = &model->consts.data[i];
        model->const_views[i] = (writ_const_view_t){
            .struct_size = sizeof(writ_const_view_t),
            .name        = sym(model, record->name),
            .type        = type_view(model, record->type),
            .value       = record->value,
            .text        = sym(model, record->text),
            .declared_at = writ_loc_span(model->session, record->loc),
        };
    }
}

static void
build_decls(writ_model_t *model)
{
    writ_session_t *session = model->session;
    model->record_views = ALLOC_VIEWS(model, writ_record_view_t, model->records.len);
    for (size_t i = 0; i < model->records.len; i++) {
        const writ_record_rec_t *record = &model->records.data[i];
        model->record_views[i] = (writ_record_view_t){
            .struct_size  = sizeof(writ_record_view_t),
            .name         = sym(model, record->name),
            .type         = type_view(model, record->type),
            .fields_count = record->field_count,
            .fields       = &model->slot_views[record->first_field],
            .declared_at  = writ_loc_span(session, record->loc),
        };
    }
    model->handle_views = ALLOC_VIEWS(model, writ_handle_view_t, model->handles.len);
    for (size_t i = 0; i < model->handles.len; i++) {
        const writ_handle_rec_t *record = &model->handles.data[i];
        model->handle_views[i] = (writ_handle_view_t){
            .struct_size = sizeof(writ_handle_view_t),
            .name        = sym(model, record->name),
            .type        = type_view(model, record->type),
            .base        = type_view(model, record->base),
            .has_none    = record->has_none,
            .none_value  = record->none_value,
            .release     = sym(model, record->release),
            .thread_rule = record->thread_rule,
            .declared_at = writ_loc_span(session, record->loc),
        };
    }
    model->enum_views = ALLOC_VIEWS(model, writ_enum_view_t, model->enums.len);
    for (size_t i = 0; i < model->enums.len; i++) {
        const writ_enum_rec_t *record = &model->enums.data[i];
        model->enum_views[i] = (writ_enum_view_t){
            .struct_size = sizeof(writ_enum_view_t),
            .name        = sym(model, record->name),
            .type        = type_view(model, record->type),
            .base        = type_view(model, record->base),
            .items_count = record->item_count,
            .items       = &model->const_views[record->first_item],
            .closed      = record->closed,
            .is_flags    = record->flags,
            .declared_at = writ_loc_span(session, record->loc),
        };
    }
    model->channel_views = ALLOC_VIEWS(model, writ_channel_view_t, model->channels.len);
    for (size_t i = 0; i < model->channels.len; i++) {
        const writ_channel_rec_t *record = &model->channels.data[i];
        model->channel_views[i] = (writ_channel_view_t){
            .struct_size       = sizeof(writ_channel_view_t),
            .name              = sym(model, record->name),
            .per_handle        = record->per_handle,
            .handle            = type_view(model, record->handle),
            .reset_before_call = record->reset,
            .kind              = record->kind,
            .source_expr       = record->source_expr,
            .declared_at       = writ_loc_span(session, record->loc),
        };
    }
}

void
writ_model_build_views(writ_model_t *model)
{
    build_types(model);
    build_slots(model);
    build_consts(model);
    build_fns(model);
    build_decls(model);
    model->library_view = writ_alloc(model->session, sizeof(writ_library_view_t));
    *model->library_view = (writ_library_view_t){
        .struct_size         = sizeof(writ_library_view_t),
        .name                = sym(model, model->library),
        .platform            = sym(model, model->platform->triple),
        .encoding            = sym(model, model->encoding),
        .links_count         = model->links.len,
        .links               = sym_list(model, &model->links),
        .headers_count       = model->headers.len,
        .headers             = sym_list(model, &model->headers),
        .defines_count       = model->defines.len,
        .defines             = sym_list(model, &model->defines),
        .include_paths_count = model->include_paths.len,
        .include_paths       = sym_list(model, &model->include_paths),
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
    size_t count = 0;
    for (size_t i = 0; model && i < model->fns.len; i++) {
        count += model->fns.data[i].kind == WRIT_FN_FUNCTION;
    }
    return count;
}

const writ_fn_view_t *
writ_model_fn_at(const writ_model_t *model, size_t index)
{
    for (size_t i = 0; model && i < model->fns.len; i++) {
        if (model->fns.data[i].kind == WRIT_FN_FUNCTION && !index--) {
            return &model->fn_views[i];
        }
    }
    return nullptr;
}

const writ_fn_view_t *
writ_model_fn_named(const writ_model_t *model, writ_str_t name)
{
    for (size_t i = 0; model && i < model->fns.len; i++) {
        if (model->fns.data[i].kind == WRIT_FN_FUNCTION && writ_str_eq(model->fn_views[i].contract_name, name)) {
            return &model->fn_views[i];
        }
    }
    return nullptr;
}

uint32_t
writ_model_fn_index(const writ_model_t *model, const writ_fn_view_t *fn)
{
    return model && fn >= model->fn_views && fn < model->fn_views + model->fns.len ? (uint32_t)(fn - model->fn_views)
                                                                                  : WRIT_NONE;
}

#define VIEW_ACCESSORS(kind, list, views)                                                                              \
    size_t writ_model_##kind##_count(const writ_model_t *model)                                                         \
    {                                                                                                                  \
        return model ? model->list.len : 0;                                                                            \
    }                                                                                                                  \
    const writ_##kind##_view_t *writ_model_##kind##_at(const writ_model_t *model, size_t index)                        \
    {                                                                                                                  \
        return model && index < model->list.len ? &model->views[index] : nullptr;                                      \
    }                                                                                                                  \
    const writ_##kind##_view_t *writ_model_##kind##_named(const writ_model_t *model, writ_str_t name)                  \
    {                                                                                                                  \
        for (size_t i = 0; model && i < model->list.len; i++) {                                                        \
            if (writ_str_eq(model->views[i].name, name)) {                                                             \
                return &model->views[i];                                                                               \
            }                                                                                                          \
        }                                                                                                              \
        return nullptr;                                                                                                \
    }

VIEW_ACCESSORS(const, consts, const_views)
VIEW_ACCESSORS(record, records, record_views)
VIEW_ACCESSORS(handle, handles, handle_views)
VIEW_ACCESSORS(enum, enums, enum_views)
VIEW_ACCESSORS(channel, channels, channel_views)

const writ_plan_view_t *
writ_model_fn_plan(const writ_model_t *model, const writ_fn_view_t *fn)
{
    uint32_t index = writ_model_fn_index(model, fn);
    if (index == WRIT_NONE) {
        return nullptr;
    }
    writ_model_t *mutable_model = (writ_model_t *)model;
    if (!mutable_model->plan_views[index]) {
        mutable_model->plan_views[index] = writ_plan_build(mutable_model, index);
    }
    return mutable_model->plan_views[index];
}
