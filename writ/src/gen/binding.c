#include "gen.h"

typedef struct {
    writ_str_t text;
    bool       chosen;
} choice_t;

typedef struct {
    choice_t   name;
    choice_t   error;
    writ_str_t owner;
    writ_str_t provide;
    bool       is_static;
    bool       hidden;
    bool       raw;
    bool       internal;
} fn_decision_t;

typedef struct {
    uint32_t         fn;
    writ_str_t       slot;
    writ_repr_form_t form;
    choice_t         repr;
    choice_t         borrow;
} slot_decision_t;

typedef struct {
    writ_str_t contract;
    choice_t   name;
} type_decision_t;

typedef struct {
    writ_str_t owner;
    writ_str_t name;
    writ_str_t member;
} target_t;

core_list_decl(slot_decision_t);
core_list_decl(type_decision_t);

struct writ_binding {
    const writ_model_t           *model;
    const writ_profile_t         *profile;
    writ_session_t               *session;
    writ_unit_t                  *unit;
    uint32_t                      block;
    writ_loc_t                    anchor;
    bool                          export_side;
    writ_str_t                    language;
    choice_t                      module;
    choice_t                      load;
    choice_t                      header;
    core_list_t(writ_sym_t)       prefixes;
    core_list_t(writ_sym_t)       libraries;
    core_list_t(writ_sym_t)       search_paths;
    writ_str_t                    panic_value;
    writ_str_t                    allocator;
    fn_decision_t                *fns;
    core_list_t(slot_decision_t)  slots;
    core_list_t(type_decision_t)  types;
};

static const char *const form_names[] = {"cstr", "utf8", "bytes", "rest pack", "keyword pack"};

static const writ_node_t *
at(const writ_binding_t *b, uint32_t id)
{
    return writ_node(b->unit, id);
}

static writ_str_t
text_of(const writ_binding_t *b, writ_sym_t sym)
{
    return writ_sym_str(b->session, sym);
}

static writ_str_t
child_text(const writ_binding_t *b, uint32_t id, writ_node_kind_t kind)
{
    uint32_t found = writ_node_first_of(b->unit, id, kind);
    return found ? text_of(b, at(b, found)->name) : (writ_str_t){"", 0};
}

static target_t
split(writ_str_t text)
{
    target_t target = {.name = text};
    for (size_t i = 0; i + 1 < text.length; i++) {
        if (text.data[i] == ':' && text.data[i + 1] == ':') {
            target.owner = (writ_str_t){text.data, i};
            target.name = (writ_str_t){text.data + i + 2, text.length - i - 2};
            break;
        }
    }
    for (size_t i = 0; i < target.name.length; i++) {
        if (target.name.data[i] == '.') {
            target.member = (writ_str_t){target.name.data + i + 1, target.name.length - i - 1};
            target.name.length = i;
            break;
        }
    }
    return target;
}

static writ_str_t
stripped(const writ_binding_t *b, writ_str_t name)
{
    size_t best = 0;
    for (size_t i = 0; i < b->prefixes.len; i++) {
        writ_str_t prefix = text_of(b, b->prefixes.data[i]);
        if (prefix.length > best && prefix.length < name.length && !memcmp(name.data, prefix.data, prefix.length)) {
            best = prefix.length;
        }
    }
    return (writ_str_t){name.data + best, name.length - best};
}

static const writ_rust_fn_t *
rust_fn(const writ_binding_t *b, size_t index)
{
    const writ_rust_t *rust = b->model->rust;
    writ_str_t symbol = writ_model_fn_at(b->model, index)->link_symbol;
    for (size_t i = 0; rust && i < rust->fns.len; i++) {
        if (writ_str_eq(text_of(b, rust->fns.data[i].symbol), symbol)) {
            return &rust->fns.data[i];
        }
    }
    return nullptr;
}

static size_t
fn_named(const writ_binding_t *b, target_t target)
{
    size_t count = writ_model_fn_count(b->model);
    for (size_t i = 0; i < count; i++) {
        const writ_rust_fn_t *rust = target.owner.length ? rust_fn(b, i) : nullptr;
        bool match = target.owner.length
                       ? rust && writ_str_eq(text_of(b, rust->owner), target.owner)
                             && writ_str_eq(text_of(b, rust->name), target.name)
                       : writ_str_eq(writ_model_fn_at(b->model, i)->contract_name, target.name);
        if (match) {
            return i;
        }
    }
    return count;
}

static type_decision_t *
type_named(writ_binding_t *b, writ_str_t name)
{
    core_list_foreach(b->types, type) {
        if (writ_str_eq(type->contract, name)) {
            return type;
        }
    }
    return nullptr;
}

static void
add_type(writ_binding_t *b, writ_str_t name)
{
    if (!type_named(b, name)) {
        writ_str_t native = writ_profile_name(b->profile, WRIT_NAMING_TYPE, stripped(b, name));
        core_list_push(b->types, ((type_decision_t){name, {native, false}}));
    }
}

static writ_str_t
native_type(writ_binding_t *b, writ_str_t contract)
{
    type_decision_t *type = type_named(b, contract);
    return type ? type->name.text : contract;
}

bool
writ_fn_has_unknown_facts(const writ_fn_view_t *fn)
{
    for (size_t i = 0; i < fn->slots_count; i++) {
        const writ_slot_view_t *slot = &fn->slots[i];
        if (slot->extent_kind == WRIT_EXTENT_UNKNOWN || slot->span == WRIT_SPAN_UNKNOWN || slot->mode == WRIT_MODE_UNKNOWN) {
            return true;
        }
    }
    return false;
}

static void
default_fns(writ_binding_t *b)
{
    const writ_model_t *model = b->model;
    for (size_t i = 0; i < writ_model_handle_count(model); i++) {
        add_type(b, writ_model_handle_at(model, i)->name);
    }
    for (size_t i = 0; i < writ_model_record_count(model); i++) {
        add_type(b, writ_model_record_at(model, i)->name);
    }
    for (size_t i = 0; i < writ_model_enum_count(model); i++) {
        add_type(b, writ_model_enum_at(model, i)->name);
    }
    for (size_t i = 0; i < writ_model_fn_count(model); i++) {
        const writ_fn_view_t *fn = writ_model_fn_at(model, i);
        fn_decision_t *d = &b->fns[i];
        d->name.text = writ_profile_name(b->profile, WRIT_NAMING_FN, stripped(b, fn->contract_name));
        d->error.text = writ_cstr("Error");
        const writ_rust_fn_t *rust = rust_fn(b, i);
        if (rust) {
            d->owner = native_type(b, text_of(b, rust->owner));
            d->name.text = writ_profile_name(b->profile, WRIT_NAMING_FN, text_of(b, rust->name));
            d->is_static = rust->receiver == WRIT_RUST_FREE;
        }
        for (size_t h = 0; h < writ_model_handle_count(model); h++) {
            const writ_handle_view_t *handle = writ_model_handle_at(model, h);
            if (writ_str_eq(handle->release, fn->contract_name)) {
                d->owner = native_type(b, handle->name);
                d->name.text = b->profile->handle_close.length ? b->profile->handle_close : writ_cstr("close");
            }
        }
        for (size_t c = 0; c < model->channels.len; c++) {
            uint32_t source = model->channels.data[c].source_expr;
            writ_ref_t callee = writ_expr_ref(model, writ_node_child(model->unit, source, 0));
            d->internal = d->internal || (callee.kind == WRIT_REF_FN && callee.index == i);
        }
    }
}

static writ_repr_form_t
item_form(const writ_plan_item_view_t *item, bool *has_form)
{
    *has_form = true;
    switch (item->shape) {
    case WRIT_SHAPE_TEXT:
        return item->role == WRIT_ROLE_INPUT && writ_c_is_text(item->type) ? WRIT_REPR_CSTR : WRIT_REPR_UTF8;
    case WRIT_SHAPE_BYTES:
        return WRIT_REPR_BYTES;
    case WRIT_SHAPE_REST_PACK:
        return WRIT_REPR_REST;
    case WRIT_SHAPE_KEYWORD_PACK:
        return WRIT_REPR_KEYWORDS;
    default:
        *has_form = false;
        return WRIT_REPR_BYTES;
    }
}

static void
default_slots(writ_binding_t *b)
{
    for (size_t i = 0; i < writ_model_fn_count(b->model); i++) {
        const writ_fn_view_t *fn = writ_model_fn_at(b->model, i);
        const writ_plan_view_t *plan = writ_fn_has_unknown_facts(fn) ? nullptr : writ_model_fn_plan(b->model, fn);
        for (size_t k = 0; plan && k < plan->items_count; k++) {
            const writ_plan_item_view_t *item = &plan->items[k];
            bool has_form = false;
            writ_repr_form_t form = item_form(item, &has_form);
            bool borrowed = item->after_call == WRIT_AFTER_BORROWED_UNTIL || item->after_call == WRIT_AFTER_BORROWED_WITH;
            if ((!has_form && !borrowed) || item->role == WRIT_ROLE_ERROR_DETAIL) {
                continue;
            }
            const writ_repr_t *repr = has_form ? writ_profile_default(b->profile, form) : nullptr;
            slot_decision_t slot = {.fn = (uint32_t)i, .slot = item->name, .form = form};
            slot.repr.text = repr ? text_of(b, repr->id) : writ_cstr(has_form ? "native" : "");
            slot.borrow.text = writ_cstr(borrowed ? "copy" : "");
            core_list_push(b->slots, slot);
        }
    }
}

static slot_decision_t *
slot_named(writ_binding_t *b, size_t fn, writ_str_t name)
{
    name = writ_str_eq(name, writ_cstr("return")) ? writ_cstr("result") : name;
    core_list_foreach(b->slots, slot) {
        if (slot->fn == fn && writ_str_eq(slot->slot, name)) {
            return slot;
        }
    }
    return nullptr;
}

static bool
known_slot(const writ_binding_t *b, size_t fn, writ_str_t name)
{
    const writ_fn_view_t *view = writ_model_fn_at(b->model, fn);
    for (size_t i = 0; i < view->slots_count; i++) {
        if (writ_str_eq(view->slots[i].name, name)) {
            return true;
        }
    }
    return writ_str_eq(name, writ_cstr("return")) && view->result;
}

static size_t
resolve_fn(writ_binding_t *b, uint32_t item, writ_str_t text, bool slot)
{
    target_t target = split(text);
    size_t fn = fn_named(b, target);
    if (fn == writ_model_fn_count(b->model) || (slot && !known_slot(b, fn, target.member))
        || (!slot && target.member.length)) {
        writ_report(b->session, WRIT_D_BIND_TARGET, at(b, item)->loc, "`%.*s`", S(text));
        return writ_model_fn_count(b->model);
    }
    return fn;
}

static bool
receives(const writ_binding_t *b, size_t fn, writ_str_t type)
{
    const writ_rust_fn_t *rust = rust_fn(b, fn);
    if (rust) {
        return rust->receiver != WRIT_RUST_FREE && writ_str_eq(text_of(b, rust->owner), type);
    }
    const writ_fn_view_t *view = writ_model_fn_at(b->model, fn);
    for (size_t i = 0; i < view->slots_count; i++) {
        const writ_type_view_t *slot = view->slots[i].type;
        const writ_type_view_t *target = slot->kind == WRIT_TYPE_POINTER ? slot->target : slot;
        if (target && writ_str_eq(target->name, type)) {
            return true;
        }
    }
    return false;
}

static void
apply_method(writ_binding_t *b, uint32_t id)
{
    const writ_node_t *item = at(b, id);
    target_t owner = split(text_of(b, at(b, writ_node_child(b->unit, id, 0))->name));
    size_t fn = resolve_fn(b, id, text_of(b, at(b, writ_node_child(b->unit, id, 1))->name), false);
    if (fn == writ_model_fn_count(b->model)) {
        return;
    }
    if (!type_named(b, owner.name) || !owner.member.length) {
        writ_report(b->session, WRIT_D_BIND_TARGET, item->loc, "no type `%.*s`", S(owner.name));
        return;
    }
    bool is_static = writ_str_eq(text_of(b, item->payload), writ_cstr("static"));
    if (!is_static && !receives(b, fn, owner.name)) {
        writ_report(b->session, WRIT_D_BIND_RECEIVER, item->loc, "`%.*s` takes no `%.*s`",
                    S(writ_model_fn_at(b->model, fn)->contract_name), S(owner.name));
        return;
    }
    fn_decision_t *d = &b->fns[fn];
    d->owner = native_type(b, owner.name);
    d->name = (choice_t){owner.member, true};
    d->is_static = is_static;
}

static void
apply_repr(writ_binding_t *b, uint32_t id)
{
    const writ_node_t *item = at(b, id);
    writ_str_t text = text_of(b, at(b, writ_node_child(b->unit, id, 0))->name);
    size_t fn = resolve_fn(b, id, text, true);
    if (fn == writ_model_fn_count(b->model)) {
        return;
    }
    target_t target = split(text);
    slot_decision_t *slot = slot_named(b, fn, target.member);
    const writ_repr_t *repr = slot ? writ_profile_repr(b->profile, item->name, slot->form) : nullptr;
    writ_str_t chosen = text_of(b, item->name);
    writ_str_t kind = text_of(b, item->payload);
    writ_node_kind_t wanted = writ_str_eq(kind, writ_cstr("string")) ? WRIT_N_STRING_ITEM
                            : writ_str_eq(kind, writ_cstr("buffer")) ? WRIT_N_BUFFER_ITEM
                                                                     : WRIT_N_PACK_NATIVE;
    bool fits = repr && repr->item == wanted;
    if (!fits) {
        writ_report(b->session, WRIT_D_BIND_REPR, item->loc, "`%.*s %.*s` for a %s slot", S(kind), S(chosen),
                    slot ? form_names[slot->form] : "plain");
        return;
    }
    slot->repr = (choice_t){chosen, true};
}

static void
apply_borrow(writ_binding_t *b, uint32_t id)
{
    writ_str_t text = text_of(b, at(b, writ_node_child(b->unit, id, 0))->name);
    size_t fn = resolve_fn(b, id, text, true);
    if (fn == writ_model_fn_count(b->model)) {
        return;
    }
    slot_decision_t *slot = slot_named(b, fn, split(text).member);
    writ_str_t mode = text_of(b, at(b, id)->payload);
    if (!slot || !slot->borrow.text.length) {
        writ_report(b->session, WRIT_D_BIND_NOT_OPEN, at(b, id)->loc, "`%.*s` is not borrowed from the library",
                    S(text));
    } else if (writ_str_eq(mode, writ_cstr("lifetime")) && !b->profile->lifetimes) {
        writ_report(b->session, WRIT_D_BIND_PROFILE, at(b, id)->loc, "`%.*s` has no lifetimes", S(b->language));
    } else {
        slot->borrow = (choice_t){mode, true};
    }
}

static void
apply_glob(writ_binding_t *b, uint32_t id, bool hide)
{
    writ_str_t pattern = text_of(b, at(b, id)->name);
    bool any = false;
    for (size_t i = 0; i < writ_model_fn_count(b->model); i++) {
        if (writ_glob_match(pattern, writ_model_fn_at(b->model, i)->contract_name)) {
            any = true;
            b->fns[i].hidden = b->fns[i].hidden || hide;
            b->fns[i].raw = b->fns[i].raw || !hide;
        }
    }
    if (!any) {
        writ_report(b->session, WRIT_D_BIND_TARGET, at(b, id)->loc, "`%.*s` matches no function", S(pattern));
    }
}

static void
apply_name(writ_binding_t *b, uint32_t id)
{
    writ_str_t text = text_of(b, at(b, writ_node_child(b->unit, id, 0))->name);
    writ_str_t name = child_text(b, id, WRIT_N_TEXT);
    type_decision_t *type = type_named(b, text);
    if (type) {
        type->name = (choice_t){name, true};
        return;
    }
    target_t target = split(text);
    size_t fn = resolve_fn(b, id, text, target.member.length != 0);
    if (fn != writ_model_fn_count(b->model) && !target.member.length) {
        b->fns[fn].name = (choice_t){name, true};
    }
}

static bool
for_side(writ_binding_t *b, uint32_t id, bool export_item)
{
    if (export_item != b->export_side) {
        writ_report(b->session, WRIT_D_BIND_DIRECTION, at(b, id)->loc, "`%s` belongs to an %s binding",
                    writ_node_kind_name(at(b, id)->kind), export_item ? "export" : "import");
        return false;
    }
    return true;
}

static void
apply_item(writ_binding_t *b, uint32_t id)
{
    const writ_node_t *item = at(b, id);
    switch (item->kind) {
    case WRIT_N_NAME_ITEM:
        apply_name(b, id);
        return;
    case WRIT_N_TYPE_AS: {
        type_decision_t *type = type_named(b, text_of(b, item->name));
        if (!type) {
            writ_report(b->session, WRIT_D_BIND_TARGET, item->loc, "no type `%.*s`", S(text_of(b, item->name)));
        } else {
            type->name = (choice_t){child_text(b, id, WRIT_N_TEXT), true};
        }
        return;
    }
    case WRIT_N_METHOD:
        if (for_side(b, id, false)) {
            apply_method(b, id);
        }
        return;
    case WRIT_N_REPR:
        apply_repr(b, id);
        return;
    case WRIT_N_BORROW:
        if (for_side(b, id, false)) {
            apply_borrow(b, id);
        }
        return;
    case WRIT_N_POLICY_USE:
        writ_report(b->session, WRIT_D_BIND_NOT_OPEN, item->loc, "the contract has no policy parameter here");
        return;
    case WRIT_N_HIDE:
    case WRIT_N_RAW_ITEM:
        if (for_side(b, id, false)) {
            apply_glob(b, id, item->kind == WRIT_N_HIDE);
        }
        return;
    case WRIT_N_ERROR_MAP: {
        writ_str_t pattern = child_text(b, id, WRIT_N_PATTERN);
        for (size_t i = 0; i < writ_model_fn_count(b->model); i++) {
            if (writ_glob_match(pattern, writ_model_fn_at(b->model, i)->contract_name)) {
                b->fns[i].error = (choice_t){child_text(b, id, WRIT_N_TEXT), true};
            }
        }
        return;
    }
    case WRIT_N_LOAD:
        if (for_side(b, id, false)) {
            b->load = (choice_t){text_of(b, item->payload), true};
        }
        return;
    case WRIT_N_LIBRARY_FILE:
        if (for_side(b, id, false)) {
            core_list_push(b->libraries, at(b, writ_node_first_of(b->unit, id, WRIT_N_TEXT))->name);
        }
        return;
    case WRIT_N_SEARCH_PATH:
        if (for_side(b, id, false)) {
            core_list_push(b->search_paths, at(b, writ_node_first_of(b->unit, id, WRIT_N_TEXT))->name);
        }
        return;
    case WRIT_N_HEADER_NAME:
        if (for_side(b, id, true)) {
            b->header = (choice_t){child_text(b, id, WRIT_N_TEXT), true};
        }
        return;
    case WRIT_N_PROVIDE:
        if (for_side(b, id, true)) {
            size_t fn = resolve_fn(b, id, text_of(b, at(b, writ_node_child(b->unit, id, 0))->name), false);
            if (fn != writ_model_fn_count(b->model)) {
                b->fns[fn].provide = child_text(b, id, WRIT_N_TEXT);
            }
        }
        return;
    case WRIT_N_ON_PANIC:
        if (for_side(b, id, true)) {
            uint32_t error = writ_node_first_of(b->unit, id, WRIT_N_PANIC_ERROR);
            uint32_t value = error ? writ_node_child(b->unit, error, 0) : 0;
            b->panic_value = value ? writ_loc_text(b->session, at(b, value)->loc) : (writ_str_t){0};
        }
        return;
    case WRIT_N_ALLOCATOR_OWN:
        if (for_side(b, id, true)) {
            b->allocator = text_of(b, item->name);
        }
        return;
    default:
        return;
    }
}

static bool
bound(const fn_decision_t *d)
{
    return !d->hidden && !d->internal;
}

static void
check_clashes(writ_binding_t *b)
{
    size_t count = writ_model_fn_count(b->model);
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count && bound(&b->fns[i]); j++) {
            const fn_decision_t *x = &b->fns[i], *y = &b->fns[j];
            if (bound(y) && writ_str_eq(x->owner, y->owner) && writ_str_eq(x->name.text, y->name.text)) {
                writ_report(b->session, WRIT_D_BIND_NAME_CLASH, b->anchor, "`%.*s` and `%.*s` are both `%.*s`",
                            S(writ_model_fn_at(b->model, i)->contract_name),
                            S(writ_model_fn_at(b->model, j)->contract_name), S(x->name.text));
            }
        }
    }
}

static void
check_fns(writ_binding_t *b)
{
    check_clashes(b);
    for (size_t i = 0; i < writ_model_fn_count(b->model); i++) {
        const writ_fn_view_t *fn = writ_model_fn_at(b->model, i);
        const fn_decision_t *d = &b->fns[i];
        if (!bound(d)) {
            continue;
        }
        if (!d->raw && writ_fn_has_unknown_facts(fn)) {
            writ_loc_t block = b->anchor;
            uint32_t diag = writ_report(b->session, WRIT_D_BIND_UNKNOWN, block, "`%.*s`", S(fn->contract_name));
            if (b->block) {
                writ_loc_t brace = {block.source, block.end - 1, block.end - 1};
                writ_report_suggest(b->session, diag, brace,
                                    writ_str_format(b->session, "    raw \"%.*s\"\n", S(fn->contract_name)),
                                    writ_cstr("bind it without safety"));
            }
        }
        bool c_variadic = fn->varargs != WRIT_VARARGS_NONE && fn->varargs != WRIT_VARARGS_VA_LIST;
        if (c_variadic && !b->profile->variadic_call && !b->export_side) {
            writ_report(b->session, WRIT_D_BIND_PROFILE, b->anchor, "`%.*s` cannot call C variadic `%.*s`",
                        S(b->language), S(fn->contract_name));
        }
        if (b->export_side && !d->provide.length) {
            writ_report(b->session, WRIT_D_BIND_PROVIDE, b->anchor, "`%.*s`", S(fn->contract_name));
        }
    }
}

static bool
find_embedded_block(writ_binding_t *b, writ_str_t library)
{
    writ_unit_t *unit = b->unit;
    uint32_t first = 0, count = 0;
    for (uint32_t block = 0; (block = writ_unit_next_item(unit, WRIT_N_BINDING, block)); count++) {
        if (count == 1) {
            writ_report(b->session, WRIT_D_BIND_LANGUAGE, at(b, block)->loc, "`%.*s`",
                        S(text_of(b, unit->language)));
            return false;
        }
        first = block;
    }
    for (size_t i = 0; i < unit->roots.len && !first; i++) {
        if (unit->roots.data[i].kind == WRIT_SOURCE_EMBEDDED) {
            b->anchor = at(b, unit->roots.data[i].root)->loc;
        }
    }
    if (first) {
        b->anchor = at(b, first)->loc;
    }
    if (!library.length) {
        writ_report(b->session, WRIT_D_EMBEDDED_SHAPE, b->anchor, "no `library` in the contract");
        return false;
    }
    b->block    = first;
    b->language = text_of(b, unit->language);
    return true;
}

static bool
find_block(writ_binding_t *b, const writ_bind_request_t *request, writ_str_t library)
{
    if (request->binding->language) {
        return find_embedded_block(b, library);
    }
    uint32_t chosen = 0, count = 0;
    for (uint32_t block = 0; (block = writ_unit_next_item(request->binding, WRIT_N_BINDING, block)); count++) {
        if (!chosen || writ_str_eq(text_of(b, at(b, block)->name), library)) {
            chosen = block;
        }
    }
    writ_str_t language = chosen ? child_text(b, chosen, WRIT_N_REF) : (writ_str_t){"", 0};
    if (!chosen || (request->language.length && (count != 1 || !writ_str_eq(language, request->language)))) {
        writ_report(b->session, WRIT_D_BIND_LANGUAGE, chosen ? at(b, chosen)->loc : WRIT_NO_LOC, "`%.*s`",
                    S(request->language.length ? request->language : writ_cstr("any")));
        return false;
    }
    if (!writ_str_eq(text_of(b, at(b, chosen)->name), library)) {
        writ_report(b->session, WRIT_D_BIND_CONTRACT, at(b, chosen)->loc, "the contract is `%.*s`", S(library));
        return false;
    }
    b->language = language;
    b->block    = chosen;
    b->anchor   = at(b, chosen)->loc;
    return true;
}

static void
binding_release(void *object)
{
    writ_binding_t *b = object;
    core_list_free(b->prefixes);
    core_list_free(b->libraries);
    core_list_free(b->search_paths);
    core_list_free(b->slots);
    core_list_free(b->types);
}

writ_status_t
writ_bind(const writ_model_t *model, const writ_bind_request_t *request, const writ_binding_t **out)
{
    if (!model || !request || !request->binding || !request->profiles || !out) {
        return WRIT_INVALID_ARGUMENT;
    }
    writ_session_t *session = model->session;
    size_t first_diag = session->diags.len;
    writ_binding_t *b = writ_alloc(session, sizeof(writ_binding_t));
    *b = (writ_binding_t){.model = model, .session = session, .unit = request->binding,
                          .prefixes = core_list_new(writ_sym_t), .libraries = core_list_new(writ_sym_t),
                          .search_paths = core_list_new(writ_sym_t), .slots = core_list_new(slot_decision_t),
                          .types = core_list_new(type_decision_t)};
    writ_on_session_free(session, binding_release, b);
    writ_str_t library = writ_model_library(model)->name;
    if (find_block(b, request, library)) {
        b->profile = writ_profile_read(request->profiles, writ_intern_str(session, b->language));
    }
    if (b->profile) {
        b->export_side = writ_str_eq(text_of(b, at(b, b->block)->payload), writ_cstr("export"));
        b->module = (choice_t){writ_profile_name(b->profile, WRIT_NAMING_MODULE, library), false};
        b->load = (choice_t){writ_cstr("static"), false};
        b->header = (choice_t){writ_str_format(session, "%.*s.h", S(library)), false};
        b->fns = writ_alloc(session, sizeof(fn_decision_t) * (writ_model_fn_count(model) + 1));
        memset(b->fns, 0, sizeof(fn_decision_t) * (writ_model_fn_count(model) + 1));
        for (uint32_t i = 0; i < at(b, b->block)->child_count; i++) {
            uint32_t item = writ_node_child(b->unit, b->block, i);
            if (at(b, item)->kind == WRIT_N_STRIP_PREFIX) {
                core_list_push(b->prefixes, at(b, item)->name);
            } else if (at(b, item)->kind == WRIT_N_MODULE && b->unit->language) {
                writ_report(session, WRIT_D_EMBEDDED_SHAPE, at(b, item)->loc,
                            "the host decides where the bound API lives");
            } else if (at(b, item)->kind == WRIT_N_MODULE) {
                b->module = (choice_t){text_of(b, at(b, item)->name), true};
            }
        }
        default_fns(b);
        default_slots(b);
        for (uint32_t i = 0; i < at(b, b->block)->child_count; i++) {
            apply_item(b, writ_node_child(b->unit, b->block, i));
        }
        check_fns(b);
    }
    bool failed = session->diags.len != first_diag;
    writ_diag_flush(session, first_diag);
    *out = failed ? nullptr : b;
    return failed ? WRIT_HAS_ERRORS : WRIT_OK;
}

static const char *
how(const choice_t *choice)
{
    return choice->chosen ? "chosen" : "default";
}

writ_status_t
writ_binding_render(const writ_binding_t *b, writ_str_t *rendered)
{
    if (!b || !rendered) {
        return WRIT_INVALID_ARGUMENT;
    }
    writ_emit_t e;
    writ_emit_init(&e);
    writ_emit_line(&e, "binding %.*s for %.*s %s", S(writ_model_library(b->model)->name), S(b->language),
                   b->export_side ? "export" : "import");
    writ_emit_line(&e, "module %.*s (%s)", S(b->module.text), how(&b->module));
    if (b->export_side) {
        writ_emit_line(&e, "header %.*s (%s)", S(b->header.text), how(&b->header));
    } else {
        writ_emit_line(&e, "load %.*s (%s)", S(b->load.text), how(&b->load));
    }
    for (size_t i = 0; i < b->types.len; i++) {
        const type_decision_t *type = &b->types.data[i];
        writ_emit_line(&e, "type %.*s: %.*s (%s)", S(type->contract), S(type->name.text), how(&type->name));
    }
    for (size_t i = 0; i < writ_model_fn_count(b->model); i++) {
        const fn_decision_t *d = &b->fns[i];
        writ_str_t contract = writ_model_fn_at(b->model, i)->contract_name;
        if (!bound(d)) {
            writ_emit_line(&e, "fn %.*s: %s", S(contract), d->hidden ? "hidden" : "internal");
            continue;
        }
        writ_str_t place = d->owner.length ? writ_str_format(b->session, "%.*s%s", S(d->owner), d->is_static ? "::" : ".")
                                           : writ_cstr("");
        writ_emit_line(&e, "fn %.*s: %.*s%.*s (%s)%s", S(contract), S(place), S(d->name.text), how(&d->name),
                       d->raw ? " raw" : "");
        writ_emit_indent(&e, 1);
        if (b->export_side) {
            writ_emit_line(&e, "provided by %.*s", S(d->provide));
        }
        if (writ_model_fn_at(b->model, i)->outcomes_count > 1) {
            writ_emit_line(&e, "error %.*s (%s)", S(d->error.text), how(&d->error));
        }
        for (size_t s = 0; s < b->slots.len; s++) {
            const slot_decision_t *slot = &b->slots.data[s];
            if (slot->fn != i) {
                continue;
            }
            if (slot->repr.text.length) {
                writ_emit_line(&e, "%.*s: %s as %.*s (%s)", S(slot->slot), form_names[slot->form], S(slot->repr.text),
                               how(&slot->repr));
            }
            if (slot->borrow.text.length) {
                writ_emit_line(&e, "%.*s: borrowed, %.*s (%s)", S(slot->slot), S(slot->borrow.text), how(&slot->borrow));
            }
        }
        writ_emit_indent(&e, -1);
    }
    *rendered = writ_emit_finish(&e, b->session);
    return WRIT_OK;
}

const writ_model_t *
writ_binding_model(const writ_binding_t *b)
{
    return b->model;
}

const writ_profile_t *
writ_binding_profile(const writ_binding_t *b)
{
    return b->profile;
}

bool
writ_binding_exports(const writ_binding_t *b)
{
    return b->export_side;
}

writ_str_t
writ_binding_load(const writ_binding_t *b)
{
    return b->load.text;
}

writ_str_t
writ_binding_header(const writ_binding_t *b)
{
    return b->header.text;
}

writ_str_t
writ_binding_panic_value(const writ_binding_t *b)
{
    return b->panic_value;
}

writ_str_t
writ_binding_allocator(const writ_binding_t *b)
{
    return b->allocator;
}

writ_bound_fn_t
writ_binding_fn(const writ_binding_t *b, size_t fn)
{
    const fn_decision_t *d = &b->fns[fn];
    return (writ_bound_fn_t){.bound = bound(d), .raw = d->raw, .is_static = d->is_static, .name = d->name.text,
                             .owner = d->owner, .error = d->error.text, .provide = d->provide};
}

writ_str_t
writ_binding_slot_repr(const writ_binding_t *b, size_t fn, writ_str_t slot)
{
    for (size_t i = 0; i < b->slots.len; i++) {
        if (b->slots.data[i].fn == fn && writ_str_eq(b->slots.data[i].slot, slot)) {
            return b->slots.data[i].repr.text;
        }
    }
    return (writ_str_t){0};
}

writ_str_t
writ_binding_type(const writ_binding_t *b, writ_str_t contract)
{
    for (size_t i = 0; i < b->types.len; i++) {
        if (writ_str_eq(b->types.data[i].contract, contract)) {
            return b->types.data[i].name.text;
        }
    }
    return writ_profile_name(b->profile, WRIT_NAMING_TYPE, contract);
}

size_t
writ_binding_library_count(const writ_binding_t *b)
{
    return b->libraries.len;
}

writ_str_t
writ_binding_library_at(const writ_binding_t *b, size_t index)
{
    return text_of(b, b->libraries.data[index]);
}

size_t
writ_binding_search_path_count(const writ_binding_t *b)
{
    return b->search_paths.len;
}

writ_str_t
writ_binding_search_path_at(const writ_binding_t *b, size_t index)
{
    return text_of(b, b->search_paths.data[index]);
}
