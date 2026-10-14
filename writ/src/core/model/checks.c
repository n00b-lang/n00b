#include <stdlib.h>

#include "model.h"

#define EACH_FN(model, f, fn)                                                                                          \
    for (uint32_t f = 0; f < (model)->fns.len; f++)                                                                     \
        for (const writ_fn_rec_t *fn = &(model)->fns.data[f]; fn; fn = nullptr)

#define EACH_PARAM(model, fn, s, slot)                                                                                 \
    for (uint32_t s = 0; s < (fn)->slot_count; s++)                                                                     \
        for (const writ_slot_rec_t *slot = writ_fn_slot(model, fn, s); slot; slot = nullptr)

static bool
is_out_array(const writ_model_t *model, const writ_slot_rec_t *slot)
{
    return slot->mode == WRIT_MODE_OUT && writ_type(model, writ_type_physical(model, slot->type))->key.kind == WRIT_TYPE_ARRAY;
}

static void
out_arrays_have_extents(writ_model_t *model)
{
    EACH_FN(model, f, fn) {
        EACH_PARAM(model, fn, s, slot) {
            if (is_out_array(model, slot) && slot->extent_kind != WRIT_EXTENT_BOUNDED
                && slot->extent_kind != WRIT_EXTENT_COUNT) {
                writ_report(model->session, WRIT_D_OUT_NEEDS_BOUND, slot->loc, nullptr);
            }
        }
    }
}

static void
bounded_extents_resolve(writ_model_t *model)
{
    EACH_FN(model, f, fn) {
        EACH_PARAM(model, fn, s, slot) {
            if (slot->extent_kind != WRIT_EXTENT_BOUNDED) {
                continue;
            }
            writ_ref_t cap = writ_expr_ref(model, slot->cap_expr);
            writ_ref_t len = writ_expr_ref(model, slot->len_expr);
            bool cell = cap.kind == WRIT_REF_SLOT && cap.phase == WRIT_REF_BEFORE;
            bool value = cap.kind == WRIT_REF_SLOT && cap.phase == WRIT_REF_VALUE
                      && writ_type_is_integer(model, model->slots.data[cap.index].type);
            bool same_cell = len.kind == WRIT_REF_SLOT && len.phase == WRIT_REF_AFTER && len.index == cap.index;
            if (!(cell && same_cell) && !(value && !(len.kind == WRIT_REF_SLOT && len.phase == WRIT_REF_BEFORE))) {
                writ_report(model->session, WRIT_D_BOUND_CELL, slot->loc, nullptr);
            }
        }
    }
}

static void
extents_integer(writ_model_t *model)
{
    for (uint32_t i = 0; i < model->slots.len; i++) {
        const writ_slot_rec_t *slot = &model->slots.data[i];
        writ_ref_t count = writ_expr_ref(model, slot->count_expr);
        if (slot->extent_kind == WRIT_EXTENT_COUNT && (count.kind == WRIT_REF_SLOT || count.kind == WRIT_REF_FIELD)
            && count.phase == WRIT_REF_VALUE && !writ_type_is_integer(model, model->slots.data[count.index].type)) {
            writ_report(model->session, WRIT_D_EXTENT_NOT_INTEGER, slot->loc, nullptr);
        }
    }
}

static void
sizing_has_output(writ_model_t *model)
{
    EACH_FN(model, f, fn) {
        if (!fn->sizing) {
            continue;
        }
        bool found = false;
        EACH_PARAM(model, fn, s, slot) {
            found = found || (is_out_array(model, slot) && slot->extent_kind == WRIT_EXTENT_BOUNDED);
        }
        if (!found) {
            writ_report(model->session, WRIT_D_SIZING_WITHOUT_OUTPUT, fn->loc, nullptr);
        }
    }
}

static void
outcomes_closed(writ_model_t *model)
{
    EACH_FN(model, f, fn) {
        const writ_node_t *node = writ_node(model->unit, fn->node);
        bool has_error = false;
        bool has_outcome = false;
        for (uint32_t i = 0; i < node->child_count; i++) {
            writ_node_kind_t kind = writ_node(model->unit, writ_node_child(model->unit, fn->node, i))->kind;
            has_error = has_error || kind == WRIT_N_ERROR;
            has_outcome = has_outcome || kind == WRIT_N_OUTCOME;
        }
        if (has_error && has_outcome) {
            writ_report(model->session, WRIT_D_ERROR_AND_OUTCOMES, fn->loc, nullptr);
        }
        if (fn->outcome_count && model->outcomes.data[fn->first_outcome + fn->outcome_count - 1].when_expr) {
            writ_report(model->session, WRIT_D_OUTCOMES_OPEN,
                        model->outcomes.data[fn->first_outcome + fn->outcome_count - 1].loc, nullptr);
        }
    }
}

static void
on_names_outcomes(writ_model_t *model)
{
    EACH_FN(model, f, fn) {
        for (uint32_t i = 0; i < fn->on_count; i++) {
            const writ_on_rec_t *on = &model->ons.data[fn->first_on + i];
            bool found = false;
            for (uint32_t o = 0; o < fn->outcome_count; o++) {
                const writ_outcome_rec_t *outcome = &model->outcomes.data[fn->first_outcome + o];
                found = found || outcome->name == on->outcome
                     || (writ_is_text(model, on->outcome, "error") && outcome->kind == WRIT_OUTCOME_FAILURE);
            }
            if (!found) {
                writ_report(model->session, WRIT_D_ON_UNKNOWN_OUTCOME, on->loc, nullptr);
            }
        }
    }
}

static void
returns_have_spans(writ_model_t *model)
{
    EACH_FN(model, f, fn) {
        if (!fn->has_result || fn->kind != WRIT_FN_FUNCTION) {
            continue;
        }
        const writ_slot_rec_t *result = writ_fn_slot(model, fn, fn->slot_count);
        writ_type_kind_t kind = writ_type(model, result->type)->key.kind;
        if ((kind == WRIT_TYPE_POINTER || kind == WRIT_TYPE_ARRAY) && !result->own && !result->span) {
            uint32_t diag = writ_report(model->session, WRIT_D_RETURN_NEEDS_SPAN, result->loc, nullptr);
            writ_loc_t end = {result->loc.source, result->loc.end, result->loc.end};
            writ_report_suggest(model->session, diag, end, writ_cstr(" unknown"),
                                writ_cstr("mark the lifetime unknown until it is known"));
        }
    }
}

static void
varargs_promoted(writ_model_t *model)
{
    EACH_FN(model, f, fn) {
        if (fn->varargs != WRIT_VARARGS_OF && fn->varargs != WRIT_VARARGS_UNTIL && fn->varargs != WRIT_VARARGS_COUNT) {
            continue;
        }
        const writ_type_rec_t *element = writ_type(model, writ_type_physical(model, fn->varargs_type));
        if ((element->key.kind == WRIT_TYPE_INT || element->key.kind == WRIT_TYPE_BOOL) && element->size < 4) {
            writ_report(model->session, WRIT_D_PROMOTED_VARARG, fn->loc, nullptr);
        }
        if (element->key.kind == WRIT_TYPE_FLOAT && element->size < 8) {
            writ_report(model->session, WRIT_D_PROMOTED_VARARG, fn->loc, nullptr);
        }
    }
}

static void
format_names_text(writ_model_t *model)
{
    EACH_FN(model, f, fn) {
        if (fn->format_slot == WRIT_NONE) {
            continue;
        }
        const writ_slot_rec_t *slot = writ_fn_slot(model, fn, fn->format_slot);
        const writ_type_rec_t *type = writ_type(model, slot->type);
        bool text = type->key.kind == WRIT_TYPE_POINTER && writ_type(model, type->key.target)->key.kind == WRIT_TYPE_CSTR;
        if (!text || (fn->varargs != WRIT_VARARGS_FORMAT && fn->varargs != WRIT_VARARGS_VA_LIST)) {
            writ_report(model->session, WRIT_D_FORMAT_NOT_TEXT, slot->loc, nullptr);
        }
    }
}

static uint32_t
asserted_size(writ_model_t *model, uint32_t size_attr, bool *found)
{
    const writ_node_t *node = writ_node(model->unit, size_attr);
    *found = false;
    uint32_t chosen = 0;
    for (uint32_t i = 0; i < node->child_count; i++) {
        uint32_t entry = writ_node_child(model->unit, size_attr, i);
        uint32_t qualifier = writ_node_first_of(model->unit, entry, WRIT_N_REF);
        writ_sym_t on = qualifier ? writ_node(model->unit, qualifier)->name : 0;
        const writ_platform_t *platform = model->platform;
        if (!on || on == platform->data_model || on == platform->os || on == platform->arch || on == platform->triple) {
            chosen = (uint32_t)strtoul(writ_sym_str(model->session, writ_node(model->unit, entry)->name).data, nullptr, 0);
            *found = true;
        }
    }
    return chosen;
}

static void
layout_assertions(writ_model_t *model)
{
    for (uint32_t r = 0; r < model->records.len; r++) {
        const writ_record_rec_t *record = &model->records.data[r];
        const writ_type_rec_t *type = writ_type(model, record->type);
        bool found = false;
        uint32_t size = record->size_assert ? asserted_size(model, record->size_assert, &found) : 0;
        if (found && size != type->size) {
            writ_report(model->session, WRIT_D_LAYOUT_MISMATCH, record->loc, "size %u asserted, %u computed", size,
                        type->size);
        }
        if (record->align_assert && record->align_assert != type->align) {
            writ_report(model->session, WRIT_D_LAYOUT_MISMATCH, record->loc, "alignment %u asserted, %u computed",
                        record->align_assert, type->align);
        }
        for (uint32_t i = 0; i < record->field_count; i++) {
            const writ_slot_rec_t *field = &model->slots.data[record->first_field + i];
            if (field->offset_assert != WRIT_NONE && field->offset_assert != field->offset) {
                writ_report(model->session, WRIT_D_LAYOUT_MISMATCH, field->loc, "offset %u asserted, %u computed",
                            field->offset_assert, field->offset);
            }
        }
    }
}

static void
reinterpreted_elements(writ_model_t *model)
{
    for (uint32_t i = 0; i < model->slots.len; i++) {
        const writ_slot_rec_t *slot = &model->slots.data[i];
        if (!slot->physical) {
            continue;
        }
        const writ_type_rec_t *array = writ_type(model, writ_type_physical(model, slot->type));
        const writ_type_rec_t *pointer = writ_type(model, slot->physical);
        const writ_type_rec_t *unit = pointer->key.kind == WRIT_TYPE_POINTER ? writ_type(model, pointer->key.target)
                                                                             : nullptr;
        const writ_type_rec_t *element = array->key.kind == WRIT_TYPE_ARRAY ? writ_type(model, array->key.target)
                                                                            : nullptr;
        uint32_t packed = 0;
        if (element && element->key.kind == WRIT_TYPE_RECORD) {
            const writ_record_rec_t *record = &model->records.data[element->decl];
            for (uint32_t f = 0; f < record->field_count; f++) {
                packed += writ_type(model, model->slots.data[record->first_field + f].type)->size;
            }
        }
        if (!unit || !element || !unit->size || element->size % unit->size || packed != element->size) {
            writ_report(model->session, WRIT_D_REINTERPRET, slot->loc, nullptr);
        }
    }
}

typedef struct {
    writ_expr_env_t base;
    int64_t         ret;
} domain_env_t;

static bool
domain_value(writ_expr_env_t *base, writ_ref_t ref, uint32_t expr, int64_t *out)
{
    (void)expr;
    *out = ((domain_env_t *)base)->ret;
    return ref.kind == WRIT_REF_RESULT;
}

static bool
succeeds(writ_model_t *model, const writ_fn_rec_t *fn, int64_t value)
{
    for (uint32_t o = 0; o < fn->outcome_count; o++) {
        const writ_outcome_rec_t *outcome = &model->outcomes.data[fn->first_outcome + o];
        domain_env_t env = {{.value = domain_value}, value};
        int64_t holds = 1;
        if (!outcome->when_expr || (writ_expr_eval(model, outcome->when_expr, &env.base, &holds) && holds)) {
            return outcome->kind == WRIT_OUTCOME_SUCCESS;
        }
    }
    return true;
}

static void
views_fit_success_domain(writ_model_t *model)
{
    EACH_FN(model, f, fn) {
        const writ_slot_rec_t *result = fn->has_result ? writ_fn_slot(model, fn, fn->slot_count) : nullptr;
        const writ_type_rec_t *view = result ? writ_type(model, result->type) : nullptr;
        writ_type_id named = view && view->key.kind == WRIT_TYPE_VIEW ? writ_type_named(model, view->key.name) : 0;
        const writ_type_rec_t *target = named ? writ_type(model, named) : nullptr;
        if (!target || target->key.kind != WRIT_TYPE_ENUM || !model->enums.data[target->decl].closed) {
            continue;
        }
        const writ_enum_rec_t *e = &model->enums.data[target->decl];
        int64_t low = INT64_MAX, high = INT64_MIN;
        for (uint32_t k = 0; k < e->item_count; k++) {
            int64_t value = model->consts.data[e->first_item + k].value;
            low = value < low ? value : low;
            high = value > high ? value : high;
        }
        int64_t outside[] = {-1, low - 1, high + 1};
        for (size_t i = 0; e->item_count && i < sizeof(outside) / sizeof(outside[0]); i++) {
            bool member = false;
            for (uint32_t k = 0; k < e->item_count; k++) {
                member = member || model->consts.data[e->first_item + k].value == outside[i];
            }
            if (!member && succeeds(model, fn, outside[i])) {
                writ_str_t name = writ_sym_str(model->session, e->name);
                writ_report(model->session, WRIT_D_VIEW_DOMAIN, result->loc, "%lld succeeds but is not a `%.*s`",
                            (long long)outside[i], S(name));
                break;
            }
        }
    }
}

static void
text_channels_have_buffers(writ_model_t *model)
{
    for (uint32_t c = 0; c < model->channels.len; c++) {
        const writ_channel_rec_t *channel = &model->channels.data[c];
        writ_ref_t callee = writ_expr_ref(model, writ_node_child(model->unit, channel->source_expr, 0));
        if (channel->kind != WRIT_CHANNEL_TEXT || callee.kind != WRIT_REF_FN) {
            continue;
        }
        const writ_fn_rec_t *fn = &model->fns.data[callee.index];
        uint32_t buffers = 0;
        EACH_PARAM(model, fn, s, slot) {
            buffers += is_out_array(model, slot) && slot->extent_kind == WRIT_EXTENT_BOUNDED;
        }
        if (buffers != 1 || (fn->sizing != WRIT_SIZING_RETRY && fn->sizing != WRIT_SIZING_QUERY)) {
            writ_report(model->session, WRIT_D_TEXT_CHANNEL, channel->loc, nullptr);
        }
    }
}

static void (*const checks[])(writ_model_t *model) = {
#define WRIT_CHECK(name, section) name,
#include "checks.def"
#undef WRIT_CHECK
};

void
writ_model_check(writ_model_t *model)
{
    for (size_t c = 0; c < sizeof(checks) / sizeof(checks[0]); c++) {
        checks[c](model);
    }
}
