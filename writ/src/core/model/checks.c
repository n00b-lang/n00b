#include <stdlib.h>

#include "model.h"

#define SYM(model, sym) S(writ_sym_str((model)->session, (sym)))

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
                writ_report(model->session, WRIT_D_OUT_NEEDS_BOUND, slot->loc, "`%.*s`", SYM(model, slot->name));
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
                writ_report(model->session, WRIT_D_BOUND_CELL, slot->loc, "`%.*s`", SYM(model, slot->name));
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
            writ_report(model->session, WRIT_D_EXTENT_NOT_INTEGER, slot->loc, "the count of `%.*s` is not an integer",
                        SYM(model, slot->name));
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
            writ_report(model->session, WRIT_D_SIZING_WITHOUT_OUTPUT, fn->loc, "`%.*s` has none", SYM(model, fn->name));
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
            writ_report(model->session, WRIT_D_ERROR_AND_OUTCOMES, fn->loc, "`%.*s` states both", SYM(model, fn->name));
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
                writ_report(model->session, WRIT_D_ON_UNKNOWN_OUTCOME, on->loc, "`%.*s` has no outcome `%.*s`",
                            SYM(model, fn->name), SYM(model, on->outcome));
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
            uint32_t diag = writ_report(model->session, WRIT_D_RETURN_NEEDS_SPAN, result->loc, "the result of `%.*s`",
                                        SYM(model, fn->name));
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
            writ_report(model->session, WRIT_D_PROMOTED_VARARG, fn->loc, "`%.*s` arrives as `c_int`; declare it `c_int`",
                        SYM(model, element->key.name));
        }
        if (element->key.kind == WRIT_TYPE_FLOAT && element->size < 8) {
            writ_report(model->session, WRIT_D_PROMOTED_VARARG, fn->loc, "`%.*s` arrives as `f64`; declare it `f64`",
                        SYM(model, element->key.name));
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
        if (!text) {
            writ_report(model->session, WRIT_D_FORMAT_NOT_TEXT, slot->loc, "`%.*s` is not a `*cstr`",
                        SYM(model, slot->name));
        } else if (fn->varargs != WRIT_VARARGS_FORMAT && fn->varargs != WRIT_VARARGS_VA_LIST) {
            writ_report(model->session, WRIT_D_FORMAT_NOT_TEXT, slot->loc,
                        "`%.*s` takes neither `...` nor a `va_list`", SYM(model, fn->name));
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
        writ_str_t name = writ_sym_str(model->session, slot->name);
        if (!unit || !unit->size || !element || element->key.kind != WRIT_TYPE_RECORD) {
            writ_report(model->session, WRIT_D_REINTERPRET, slot->loc,
                        "`%.*s` must be an array of records over a pointer to a sized element", S(name));
        } else if (element->size % unit->size) {
            writ_report(model->session, WRIT_D_REINTERPRET, slot->loc,
                        "`%.*s`: a %u-byte record is not a whole number of %u-byte physical elements", S(name),
                        element->size, unit->size);
        } else if (packed != element->size) {
            writ_report(model->session, WRIT_D_REINTERPRET, slot->loc,
                        "`%.*s`: the record's fields fill %u of its %u bytes; padding cannot be reinterpreted", S(name),
                        packed, element->size);
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
record_nones_fit(writ_model_t *model)
{
    for (uint32_t r = 0; r < model->records.len; r++) {
        const writ_record_rec_t *record = &model->records.data[r];
        for (uint32_t i = 0; record->none_node && i < writ_node(model->unit, record->none_node)->child_count; i++) {
            uint32_t value = writ_node_child(model->unit, record->none_node, i);
            const writ_node_t *node = writ_node(model->unit, value);
            const writ_slot_rec_t *field = nullptr;
            for (uint32_t f = 0; f < record->field_count; f++) {
                const writ_slot_rec_t *candidate = &model->slots.data[record->first_field + f];
                field = candidate->name == node->name ? candidate : field;
            }
            uint32_t expr = writ_node_child(model->unit, value, 0);
            int64_t evaluated = 0;
            if (!field) {
                writ_str_t name = writ_sym_str(model->session, node->name);
                writ_report(model->session, WRIT_D_UNKNOWN_NAME, node->loc, "no field `%.*s`", S(name));
            } else if (!writ_type_is_integer(model, field->type)
                       || !writ_expr_check_in(model, WRIT_EXPR_SCOPE_CONSTANT, WRIT_NONE, expr)
                       || !writ_expr_eval(model, expr, nullptr, &evaluated)) {
                writ_report(model->session, WRIT_D_BAD_NONE_VALUE, node->loc, "field `%.*s`", SYM(model, field->name));
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
        if (buffers != 1) {
            writ_report(model->session, WRIT_D_TEXT_CHANNEL, channel->loc,
                        "`%.*s` fills %u out arrays with `cap ..., len ...`; a text channel needs exactly one",
                        SYM(model, fn->name), buffers);
        } else if (fn->sizing != WRIT_SIZING_RETRY && fn->sizing != WRIT_SIZING_QUERY) {
            writ_report(model->session, WRIT_D_TEXT_CHANNEL, channel->loc,
                        "`%.*s` needs `sizing retry` or `sizing query`", SYM(model, fn->name));
        }
    }
}

static bool
is_callback_slot(const writ_model_t *model, const writ_slot_rec_t *slot)
{
    const writ_type_rec_t *type = writ_type(model, writ_type_physical(model, slot->type));
    return type->key.kind == WRIT_TYPE_CALLBACK;
}

static void
userdata_pairs(writ_model_t *model)
{
    EACH_FN(model, f, fn) {
        uint32_t receivers = 0;
        EACH_PARAM(model, fn, s, slot) {
            if (!slot->userdata) {
                continue;
            }
            if (fn->kind == WRIT_FN_CALLBACK) {
                if (slot->userdata_for) {
                    writ_report(model->session, WRIT_D_BAD_USERDATA, slot->userdata_loc,
                                "inside callback `%.*s`, `userdata` names no parameter; it marks where the context "
                                "arrives", SYM(model, fn->name));
                } else if (++receivers > 1) {
                    writ_report(model->session, WRIT_D_BAD_USERDATA, slot->userdata_loc,
                                "callback `%.*s` already has a `userdata` parameter", SYM(model, fn->name));
                }
                continue;
            }
            uint32_t target = WRIT_NONE;
            for (uint32_t t = 0; slot->userdata_for && t < fn->slot_count; t++) {
                if (writ_fn_slot(model, fn, t)->name == slot->userdata_for) {
                    target = t;
                }
            }
            if (slot->userdata_for && target == WRIT_NONE) {
                writ_report(model->session, WRIT_D_UNKNOWN_SLOT, slot->userdata_loc, "`%.*s`",
                            SYM(model, slot->userdata_for));
                continue;
            }
            bool paired = false;
            EACH_PARAM(model, fn, o, other) {
                paired = paired || (other != slot && other->userdata && other->userdata_for == slot->userdata_for);
            }
            if (target == WRIT_NONE) {
                writ_report(model->session, WRIT_D_BAD_USERDATA, slot->userdata_loc,
                            "`%.*s` must name the callback parameter it travels with", SYM(model, slot->name));
            } else if (!is_callback_slot(model, writ_fn_slot(model, fn, target))) {
                writ_report(model->session, WRIT_D_BAD_USERDATA, slot->userdata_loc, "`%.*s` is not a callback",
                            SYM(model, slot->userdata_for));
            } else if (paired) {
                writ_report(model->session, WRIT_D_BAD_USERDATA, slot->userdata_loc,
                            "callback `%.*s` has more than one `userdata` parameter", SYM(model, slot->userdata_for));
            }
        }
    }
}

static const writ_record_rec_t *
keywords_record(const writ_model_t *model, writ_type_id type)
{
    const writ_type_rec_t *pointer = writ_type(model, writ_type_physical(model, type));
    if (pointer->key.kind != WRIT_TYPE_POINTER) {
        return nullptr;
    }
    const writ_type_rec_t *target = writ_type(model, pointer->key.target);
    if (target->key.kind != WRIT_TYPE_RECORD || target->decl >= model->records.len) {
        return nullptr;
    }
    const writ_record_rec_t *record = &model->records.data[target->decl];
    return record->presence == WRIT_PRESENCE_NONE ? nullptr : record;
}

static void
check_presence(writ_model_t *model, const writ_record_rec_t *record)
{
    if (record->presence == WRIT_PRESENCE_SENTINEL) {
        for (uint32_t f = 0; f < record->field_count; f++) {
            const writ_slot_rec_t *field = &model->slots.data[record->first_field + f];
            if (!field->has_none && !writ_type_is_pointer(model, field->type)) {
                writ_report(model->session, WRIT_D_PRESENCE, record->loc,
                            "under `sentinel`, keyword `%.*s` needs a `none` value or a pointer type",
                            SYM(model, field->name));
                return;
            }
        }
        return;
    }
    if ((record->presence != WRIT_PRESENCE_MASK && record->presence != WRIT_PRESENCE_SIZE)
        || record->presence_field == WRIT_NONE) {
        return;
    }
    const writ_slot_rec_t *field = &model->slots.data[record->first_field + record->presence_field];
    if (!writ_type_is_integer(model, field->type)) {
        writ_report(model->session, WRIT_D_PRESENCE, record->loc, "presence field `%.*s` is not an integer",
                    SYM(model, field->name));
        return;
    }
    uint32_t bits = 8 * writ_type(model, writ_type_physical(model, field->type))->size;
    if (record->presence == WRIT_PRESENCE_MASK && record->field_count - 1 > bits) {
        writ_report(model->session, WRIT_D_PRESENCE, record->loc,
                    "mask field `%.*s` has %u bits for %u keywords", SYM(model, field->name), bits,
                    record->field_count - 1);
    }
}

static void
keyword_packs(writ_model_t *model)
{
    EACH_FN(model, f, fn) {
        if (fn->keywords_slot != WRIT_NONE && !keywords_record(model, writ_fn_slot(model, fn, fn->keywords_slot)->type)) {
            const writ_slot_rec_t *slot = writ_fn_slot(model, fn, fn->keywords_slot);
            writ_report(model->session, WRIT_D_PACK_KEYWORDS, slot->loc, "`%.*s` does not point to one",
                        SYM(model, slot->name));
        }
    }
    for (uint32_t r = 0; r < model->records.len; r++) {
        check_presence(model, &model->records.data[r]);
    }
}

static void
check_arm(writ_model_t *model, const writ_fn_rec_t *fn, const writ_slot_rec_t *slot, const writ_policy_arm_rec_t *arm,
          uint32_t elses)
{
    bool target = false;
    for (uint32_t t = 0; t < fn->slot_count; t++) {
        target = target || writ_fn_slot(model, fn, t)->name == arm->target;
    }
    if (!target) {
        writ_report(model->session, WRIT_D_POLICY, arm->loc, "`%.*s` has no parameter `%.*s`", SYM(model, fn->name),
                    SYM(model, arm->target));
        return;
    }
    if (arm->is_else) {
        if (elses > 1) {
            writ_report(model->session, WRIT_D_POLICY, arm->loc, "`%.*s` has more than one `else` arm",
                        SYM(model, slot->name));
        }
        return;
    }
    for (size_t c = 0; c < model->consts.len; c++) {
        if (model->consts.data[c].name == arm->constant) {
            if (writ_type_physical(model, model->consts.data[c].type) != writ_type_physical(model, slot->type)) {
                writ_report(model->session, WRIT_D_POLICY, arm->loc, "`%.*s` is not of the type of `%.*s`",
                            SYM(model, arm->constant), SYM(model, slot->name));
            }
            return;
        }
    }
    writ_report(model->session, WRIT_D_POLICY, arm->loc, "`%.*s` is not a constant", SYM(model, arm->constant));
}

static void
policy_arms(writ_model_t *model)
{
    EACH_FN(model, f, fn) {
        uint32_t policies = 0;
        EACH_PARAM(model, fn, s, slot) {
            uint32_t elses = 0;
            policies += slot->arm_count != 0;
            for (uint32_t a = 0; a < slot->arm_count; a++) {
                const writ_policy_arm_rec_t *arm = &model->policy_arms.data[slot->first_arm + a];
                elses += arm->is_else;
                check_arm(model, fn, slot, arm, elses);
            }
            if (policies > 1) {
                writ_report(model->session, WRIT_D_UNSUPPORTED, slot->loc, "more than one policy parameter");
            }
            if (!slot->span_by) {
                continue;
            }
            bool sets = false;
            bool named = false;
            EACH_PARAM(model, fn, p, policy) {
                named = named || (policy->name == slot->span_by && policy->arm_count);
                for (uint32_t a = 0; policy->name == slot->span_by && a < policy->arm_count; a++) {
                    sets = sets || model->policy_arms.data[policy->first_arm + a].target == slot->name;
                }
            }
            if (!named) {
                writ_report(model->session, WRIT_D_POLICY, slot->loc, "`span by %.*s`: `%.*s` is not a policy parameter",
                            SYM(model, slot->span_by), SYM(model, slot->span_by));
            } else if (!sets) {
                writ_report(model->session, WRIT_D_POLICY, slot->loc,
                            "`span by %.*s`: no arm of `%.*s` sets `%.*s`", SYM(model, slot->span_by),
                            SYM(model, slot->span_by), SYM(model, slot->name));
            }
        }
    }
}

static const writ_slot_rec_t *
event_arg_slot(const writ_model_t *model, const writ_fn_rec_t *fn, const writ_event_arg_rec_t *arg)
{
    if (arg->kind == WRIT_EVENT_ARG_RESULT) {
        return fn->has_result ? writ_fn_slot(model, fn, fn->slot_count) : nullptr;
    }
    for (uint32_t s = 0; arg->kind == WRIT_EVENT_ARG_SLOT && s < fn->slot_count; s++) {
        if (writ_fn_slot(model, fn, s)->name == arg->name) {
            return writ_fn_slot(model, fn, s);
        }
    }
    return nullptr;
}

static const writ_fn_rec_t *
event_target(const writ_model_t *model, writ_sym_t name)
{
    EACH_FN(model, f, fn) {
        if (fn->kind == WRIT_FN_FUNCTION && fn->name == name) {
            return fn;
        }
    }
    return nullptr;
}

static void
check_event(writ_model_t *model, const writ_fn_rec_t *fn, const writ_event_rec_t *event)
{
    for (uint32_t a = 0; a < event->arg_count; a++) {
        const writ_event_arg_rec_t *arg = &model->event_args.data[event->first_arg + a];
        if (arg->kind == WRIT_EVENT_ARG_RESULT && !event_arg_slot(model, fn, arg)) {
            writ_report(model->session, WRIT_D_EVENT, event->loc, "`%.*s` has no result for `ret`",
                        SYM(model, fn->name));
            return;
        }
        if (arg->kind == WRIT_EVENT_ARG_SLOT && !event_arg_slot(model, fn, arg)) {
            writ_report(model->session, WRIT_D_EVENT, event->loc, "`%.*s` has no parameter `%.*s`",
                        SYM(model, fn->name), SYM(model, arg->name));
            return;
        }
    }
    if (event->kind != WRIT_EVENT_CALL) {
        const char *word = event->kind == WRIT_EVENT_RELEASE ? "release" : "any";
        const writ_slot_rec_t *subject = event->arg_count == 1
            ? event_arg_slot(model, fn, &model->event_args.data[event->first_arg]) : nullptr;
        if (!subject) {
            writ_report(model->session, WRIT_D_EVENT, event->loc, "`%s` takes one handle parameter", word);
        } else if (writ_type(model, subject->type)->key.kind != WRIT_TYPE_HANDLE) {
            writ_report(model->session, WRIT_D_EVENT, event->loc, "`%s(%.*s)`: `%.*s` is not a handle", word,
                        SYM(model, subject->name), SYM(model, subject->name));
        }
        return;
    }
    const writ_fn_rec_t *target = event_target(model, event->function);
    if (!target) {
        writ_report(model->session, WRIT_D_EVENT, event->loc, "`%.*s` is not a function of the contract",
                    SYM(model, event->function));
    } else if (event->arg_count > target->slot_count) {
        writ_report(model->session, WRIT_D_EVENT, event->loc, "`%.*s` takes %u parameters, not %u",
                    SYM(model, event->function), target->slot_count, event->arg_count);
    }
}

static void
check_events(writ_model_t *model, const writ_fn_rec_t *fn, uint32_t first, uint32_t count)
{
    for (uint32_t e = 0; e < count; e++) {
        check_event(model, fn, &model->events.data[first + e]);
    }
}

static void
events_resolve(writ_model_t *model)
{
    EACH_FN(model, f, fn) {
        for (uint32_t s = 0; s < fn->slot_count + fn->has_result; s++) {
            const writ_slot_rec_t *slot = writ_fn_slot(model, fn, s);
            check_events(model, fn, slot->first_event, slot->event_count);
            for (uint32_t a = 0; a < slot->arm_count; a++) {
                const writ_policy_arm_rec_t *arm = &model->policy_arms.data[slot->first_arm + a];
                check_events(model, fn, arm->first_event, arm->event_count);
            }
        }
    }
}

static uint32_t
tag_enum(const writ_model_t *model, writ_type_id type)
{
    const writ_type_rec_t *tag = writ_type(model, type);
    if (tag->key.kind == WRIT_TYPE_VIEW) {
        tag = writ_type(model, writ_type_named(model, tag->key.name));
    }
    return tag->key.kind == WRIT_TYPE_ENUM ? tag->decl : WRIT_NONE;
}

static bool
arm_in_enum(const writ_model_t *model, const writ_union_arm_rec_t *arm, uint32_t enumeration)
{
    uint32_t index = 0;
    return enumeration == WRIT_NONE || !arm->constant
        || (writ_scope_lookup(model, arm->constant, WRIT_SCOPE_CONST, &index)
            && model->consts.data[index].owner == enumeration);
}

static void
union_tags(writ_model_t *model)
{
    for (uint32_t r = 0; r < model->records.len; r++) {
        const writ_record_rec_t *tagged = &model->records.data[r];
        if (tagged->owner == WRIT_NONE) {
            continue;
        }
        const writ_record_rec_t *owner = &model->records.data[tagged->owner];
        const writ_slot_rec_t *field = &model->slots.data[owner->first_field + tagged->owner_field];
        const writ_slot_rec_t *tag = field->tag_field == WRIT_NONE ? nullptr
                                                                   : &model->slots.data[owner->first_field + field->tag_field];
        if (!tag || !writ_type_is_integer(model, tag->type)) {
            writ_report(model->session, WRIT_D_UNION_TAG, field->loc, "`%.*s` is not an earlier integer field",
                        S(writ_sym_str(model->session, field->tag_name)));
            continue;
        }
        uint32_t enumeration = tag_enum(model, tag->type);
        for (uint32_t a = 0; a < tagged->arm_count; a++) {
            const writ_union_arm_rec_t *arm = &model->union_arms.data[tagged->first_arm + a];
            bool repeated = false;
            for (uint32_t b = 0; b < a; b++) {
                repeated = repeated || model->union_arms.data[tagged->first_arm + b].value == arm->value;
            }
            if (repeated) {
                writ_report(model->session, WRIT_D_UNION_TAG, arm->loc, "value %lld names two arms",
                            (long long)arm->value);
            } else if (!arm_in_enum(model, arm, enumeration)) {
                writ_report(model->session, WRIT_D_UNION_TAG, arm->loc, "`%.*s` is not an item of `%.*s`",
                            S(writ_sym_str(model->session, arm->constant)),
                            S(writ_sym_str(model->session, model->enums.data[enumeration].name)));
            }
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
