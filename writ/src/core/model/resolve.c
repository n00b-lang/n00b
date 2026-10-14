#include "model.h"

typedef struct {
    writ_model_t         *model;
    core_list_t(uint32_t) decls;
    core_dict_t           aliases;
    core_dict_t           alias_state;
} resolver_t;

static void
model_release(void *object)
{
    writ_model_t *model = object;
    core_list_free(model->links);
    core_list_free(model->headers);
    core_list_free(model->defines);
    core_list_free(model->include_paths);
    core_list_free(model->platforms);
    core_list_free(model->types);
    core_indexed_table_free(model->type_index);
    core_dict_free(&model->type_names);
    core_list_free(model->consts);
    core_list_free(model->slots);
    core_list_free(model->outcomes);
    core_list_free(model->ons);
    core_list_free(model->assigns);
    core_list_free(model->fns);
    core_list_free(model->records);
    core_list_free(model->handles);
    core_list_free(model->enums);
    core_list_free(model->channels);
    core_list_free(model->allocators);
    core_list_free(model->vars);
    core_list_free(model->conventions);
    core_list_free(model->refines);
    core_dict_free(&model->scope);
    core_list_free(model->refs);
    core_list_free(model->uses);
}

static writ_model_t *
model_new(writ_unit_t *unit, const writ_platform_t *platform)
{
    writ_session_t *session = unit->session;
    writ_model_t *model = writ_alloc(session, sizeof(writ_model_t));
    *model = (writ_model_t){
        .session       = session,
        .unit          = unit,
        .platform      = platform,
        .encoding      = writ_intern(session, "utf8", 4),
        .links         = core_list_new(uint32_t),
        .headers       = core_list_new(uint32_t),
        .defines       = core_list_new(uint32_t),
        .include_paths = core_list_new(uint32_t),
        .platforms     = core_list_new(uint32_t),
        .consts        = core_list_new(writ_const_rec_t),
        .slots         = core_list_new(writ_slot_rec_t),
        .outcomes      = core_list_new(writ_outcome_rec_t),
        .ons           = core_list_new(writ_on_rec_t),
        .assigns       = core_list_new(writ_assign_rec_t),
        .fns           = core_list_new(writ_fn_rec_t),
        .records       = core_list_new(writ_record_rec_t),
        .handles       = core_list_new(writ_handle_rec_t),
        .enums         = core_list_new(writ_enum_rec_t),
        .channels      = core_list_new(writ_channel_rec_t),
        .allocators    = core_list_new(writ_allocator_rec_t),
        .vars          = core_list_new(writ_var_rec_t),
        .conventions   = core_list_new(writ_convention_rec_t),
        .refines       = core_list_new(uint32_t),
        .refs          = core_list_new(writ_ref_t),
        .uses          = core_list_new(uint32_t),
    };
    core_dict_init(&model->scope, nullptr, nullptr);
    writ_types_init(model);
    writ_on_session_free(session, model_release, model);
    return model;
}

static const writ_node_t *
node_at(const writ_model_t *model, uint32_t id)
{
    return writ_node(model->unit, id);
}

static uint32_t
child(const writ_model_t *model, uint32_t id, uint32_t index)
{
    return writ_node_child(model->unit, id, index);
}

static uint32_t
first_of(const writ_model_t *model, uint32_t id, writ_node_kind_t kind)
{
    return writ_node_first_of(model->unit, id, kind);
}

bool
writ_is_text(const writ_model_t *model, writ_sym_t sym, const char *text)
{
    return writ_str_eq(writ_sym_str(model->session, sym), writ_cstr(text));
}

static void
unsupported(writ_model_t *model, uint32_t node, const char *what)
{
    writ_report(model->session, WRIT_D_UNSUPPORTED, node_at(model, node)->loc, "%s", what);
}

static bool
declare(writ_model_t *model, writ_sym_t name, writ_scope_kind_t kind, uint32_t index, writ_loc_t loc)
{
    if (!core_dict_add(&model->scope, name, ((uintptr_t)kind << 32) | index)) {
        writ_str_t text = writ_sym_str(model->session, name);
        writ_report(model->session, WRIT_D_DUPLICATE, loc, "`%.*s`", (int)text.length, text.data);
        return false;
    }
    return true;
}

bool
writ_scope_lookup(const writ_model_t *model, writ_sym_t name, writ_scope_kind_t want, uint32_t *index)
{
    bool found = false;
    uintptr_t entry = (uintptr_t)core_dict_get((core_dict_t *)&model->scope, name, &found);
    if (!found || (entry >> 32) != want) {
        return false;
    }
    *index = (uint32_t)entry;
    return true;
}

const writ_slot_rec_t *
writ_fn_slot(const writ_model_t *model, const writ_fn_rec_t *fn, uint32_t index)
{
    return &model->slots.data[fn->first_slot + index];
}

uint32_t
writ_fn_slot_named(const writ_model_t *model, const writ_fn_rec_t *fn, writ_sym_t name)
{
    for (uint32_t i = 0; i < fn->slot_count; i++) {
        if (model->slots.data[fn->first_slot + i].name == name) {
            return i;
        }
    }
    return WRIT_NONE;
}

static bool
is_type_node(writ_node_kind_t kind)
{
    switch (kind) {
    case WRIT_N_TYPE_VIEW:
    case WRIT_N_TYPE_NAMED:
    case WRIT_N_TYPE_PATH:
    case WRIT_N_TYPE_POINTER:
    case WRIT_N_TYPE_ARRAY:
    case WRIT_N_TYPE_INLINE:
    case WRIT_N_TYPE_BYTES:
    case WRIT_N_TYPE_REF:
    case WRIT_N_TYPE_SLICE:
    case WRIT_N_TYPE_TUPLE:
        return true;
    default:
        return false;
    }
}

static uint32_t
type_child(const writ_model_t *model, uint32_t id)
{
    const writ_node_t *node = node_at(model, id);
    for (uint32_t i = 0; i < node->child_count; i++) {
        uint32_t candidate = child(model, id, i);
        if (is_type_node(node_at(model, candidate)->kind)) {
            return candidate;
        }
    }
    return 0;
}

static writ_sym_t
sym(writ_model_t *model, const char *text)
{
    return writ_intern(model->session, text, strlen(text));
}

static writ_type_id
declare_named_type(writ_model_t *model, writ_type_kind_t kind, writ_sym_t name, uint32_t decl, writ_loc_t loc)
{
    writ_type_id id = writ_type_intern(model, (writ_type_key_t){.kind = kind, .name = name});
    writ_type_rec_t *record = &model->types.data[id];
    record->decl = decl;
    record->loc = loc;
    record->c_spelling = name;
    record->align = record->align ? record->align : 1;
    writ_type_declare(model, name, id, loc);
    return id;
}

static void gather(resolver_t *r, uint32_t root);

static void
gather_use(resolver_t *r, uint32_t id)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    writ_str_t relative = writ_sym_str(model->session, node->name);
    writ_str_t base = writ_source_at(model->session, node->loc.source)->path;
    size_t cut = base.length;
    while (cut > 0 && base.data[cut - 1] != '/') {
        cut--;
    }
    writ_str_t path = relative.length && relative.data[0] == '/'
                        ? relative
                        : writ_str_format(model->session, "%.*s%.*s", (int)cut, base.data, (int)relative.length,
                                          relative.data);
    writ_sym_t key = writ_intern_str(model->session, path);
    core_list_foreach(model->uses, seen) {
        if (*seen == key) {
            return;
        }
    }
    core_list_push(model->uses, key);
    size_t roots = model->unit->roots.len;
    writ_status_t status = writ_unit_read_file(model->unit, path, nullptr);
    if (status == WRIT_IO_FAILED) {
        writ_report(model->session, WRIT_D_UNREADABLE, node->loc, "`%.*s`", (int)path.length, path.data);
        return;
    }
    for (size_t i = roots; i < model->unit->roots.len; i++) {
        gather(r, model->unit->roots.data[i].root);
    }
}

static void
gather_directive(resolver_t *r, uint32_t id)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    switch (node->kind) {
    case WRIT_N_LIBRARY:
        if (model->library) {
            writ_report(model->session, WRIT_D_DUPLICATE, node->loc, "`library`");
        }
        model->library = node->name;
        return;
    case WRIT_N_LINK:
        core_list_push(model->links, node->name);
        return;
    case WRIT_N_HEADER:
        core_list_push(model->headers, node->name);
        return;
    case WRIT_N_DEFINE: {
        uint32_t value = child(model, id, 0);
        writ_str_t name = writ_sym_str(model->session, node->name);
        writ_str_t text = value ? writ_loc_text(model->session, node_at(model, value)->loc) : writ_cstr("");
        core_list_push(model->defines,
                       writ_intern_str(model->session, value ? writ_str_format(model->session, "%.*s=%.*s",
                                                                               (int)name.length, name.data,
                                                                               (int)text.length, text.data)
                                                             : name));
        return;
    }
    case WRIT_N_INCLUDE_PATH:
        core_list_push(model->include_paths, node->name);
        return;
    case WRIT_N_ENCODING:
        model->encoding = node->name;
        return;
    case WRIT_N_PLATFORMS:
        for (uint32_t i = 0; i < node->child_count; i++) {
            core_list_push(model->platforms, node_at(model, child(model, id, i))->name);
        }
        return;
    case WRIT_N_USE:
        gather_use(r, id);
        return;
    case WRIT_N_ABI:
        if (!writ_is_text(model, node->name, "c")) {
            unsupported(model, id, "ABI families other than `c` (dsl.md §9.4)");
        }
        return;
    case WRIT_N_LANG:
        if (!writ_is_text(model, node->payload, "c")) {
            unsupported(model, id, "contracts for languages other than C");
        }
        return;
    case WRIT_N_IMPORT:
        unsupported(model, id, "`import` inside a contract; run `writ import` and `use` the draft");
        return;
    case WRIT_N_CRATE:
        unsupported(model, id, "`crate` outside a Rust contract");
        return;
    case WRIT_N_FEATURE:
        return;
    case WRIT_N_VERSION_QUERY:
        unsupported(model, id, "library version queries");
        return;
    default:
        break;
    }
}

static void gather_items(resolver_t *r, uint32_t parent, uint32_t from);

static void
take_c_spelling(writ_model_t *model, uint32_t id, writ_type_id type)
{
    uint32_t source = first_of(model, id, WRIT_N_SOURCE);
    if (source) {
        model->types.data[type].c_spelling = node_at(model, source)->name;
    }
}

static void
gather_decl(resolver_t *r, uint32_t id)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    switch (node->kind) {
    case WRIT_N_WHEN: {
        uint32_t condition = child(model, id, 0);
        if (writ_condition_holds(model, condition)) {
            gather_items(r, id, 1);
        } else {
            uint32_t otherwise = first_of(model, id, WRIT_N_ELSE);
            if (otherwise) {
                gather_items(r, otherwise, 0);
            }
        }
        return;
    }
    case WRIT_N_STRUCT:
        core_list_push(model->records, ((writ_record_rec_t){.name = node->name, .loc = node->loc}));
        model->records.data[model->records.len - 1].type =
            declare_named_type(model, WRIT_TYPE_RECORD, node->name, (uint32_t)model->records.len - 1, node->loc);
        take_c_spelling(model, id, model->records.data[model->records.len - 1].type);
        break;
    case WRIT_N_OPAQUE:
        take_c_spelling(model, id, declare_named_type(model, WRIT_TYPE_OPAQUE, node->name, WRIT_NONE, node->loc));
        break;
    case WRIT_N_HANDLE:
        core_list_push(model->handles, ((writ_handle_rec_t){.name = node->name, .loc = node->loc}));
        model->handles.data[model->handles.len - 1].type =
            declare_named_type(model, WRIT_TYPE_HANDLE, node->name, (uint32_t)model->handles.len - 1, node->loc);
        break;
    case WRIT_N_ENUM:
    case WRIT_N_FLAGS:
        core_list_push(model->enums, ((writ_enum_rec_t){.name = node->name, .loc = node->loc,
                                                         .flags = node->kind == WRIT_N_FLAGS}));
        model->enums.data[model->enums.len - 1].type =
            declare_named_type(model, WRIT_TYPE_ENUM, node->name, (uint32_t)model->enums.len - 1, node->loc);
        break;
    case WRIT_N_CALLBACK:
        declare_named_type(model, WRIT_TYPE_CALLBACK, node->name, WRIT_NONE, node->loc);
        break;
    case WRIT_N_ALIAS:
        if (writ_type_named(model, node->name) || !core_dict_add(&r->aliases, node->name, id)) {
            writ_str_t text = writ_sym_str(model->session, node->name);
            writ_report(model->session, WRIT_D_DUPLICATE, node->loc, "type `%.*s`", (int)text.length, text.data);
        }
        return;
    case WRIT_N_CONVENTIONS: {
        uint32_t pattern = first_of(model, id, WRIT_N_PATTERN);
        if (!writ_is_text(model, node->payload, "fn")) {
            unsupported(model, id, "conventions for handles and structs");
            return;
        }
        core_list_push(model->conventions,
                       ((writ_convention_rec_t){node_at(model, pattern)->name, id, node->loc}));
        return;
    }
    case WRIT_N_REFINE:
        core_list_push(model->refines, id);
        return;
    case WRIT_N_INTERFACE:
        unsupported(model, id, "interfaces (dsl.md §9.1)");
        return;
    case WRIT_N_SYMBOL_DECL:
        unsupported(model, id, "symbol declarations (dsl.md §7.4)");
        return;
    case WRIT_N_IMPL:
    case WRIT_N_RUST_ERROR:
        unsupported(model, id, "Rust declarations (dsl.md §8.9)");
        return;
    case WRIT_N_PROFILE:
    case WRIT_N_BINDING:
        unsupported(model, id, "language profiles and bindings (dsl.md §9.2, §9.3)");
        return;
    default:
        break;
    }
    core_list_push(r->decls, id);
}

static void
gather_items(resolver_t *r, uint32_t parent, uint32_t from)
{
    uint32_t count = node_at(r->model, parent)->child_count;
    for (uint32_t i = from; i < count; i++) {
        uint32_t item = child(r->model, parent, i);
        writ_node_kind_t kind = node_at(r->model, item)->kind;
        if (kind == WRIT_N_ELSE) {
            continue;
        }
        gather_directive(r, item);
        gather_decl(r, item);
    }
}

static void
gather(resolver_t *r, uint32_t root)
{
    gather_items(r, root, 0);
}

static writ_type_id resolve_type(resolver_t *r, uint32_t id, writ_slot_rec_t *slot);

static writ_type_id
resolve_alias(resolver_t *r, writ_sym_t name)
{
    writ_model_t *model = r->model;
    bool found = false;
    uint32_t node = (uint32_t)(uintptr_t)core_dict_get(&r->aliases, name, &found);
    if (!found) {
        return 0;
    }
    uintptr_t state = (uintptr_t)core_dict_get(&r->alias_state, name, &found);
    if (found && state == 2) {
        return writ_type_named(model, name);
    }
    if (found && state == 1) {
        writ_report(model->session, WRIT_D_UNKNOWN_TYPE, node_at(model, node)->loc, "alias refers to itself");
        return 0;
    }
    core_dict_put(&r->alias_state, name, (void *)1);
    writ_type_id target = resolve_type(r, child(model, node, 0), nullptr);
    core_dict_put(&r->alias_state, name, (void *)2);
    if (target) {
        core_dict_put(&model->type_names, name, (void *)(uintptr_t)target);
    }
    return target;
}

static writ_type_id
named_type(resolver_t *r, uint32_t id, bool handle_base)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    if (first_of(model, id, WRIT_N_GENERIC_ARGS)) {
        unsupported(model, id, "generic Rust types (dsl.md §8.9)");
        return 0;
    }
    uint32_t encoding = first_of(model, id, WRIT_N_ENCODING_ARG);
    writ_type_id type = writ_type_named(model, node->name);
    if (!type) {
        type = resolve_alias(r, node->name);
    }
    if (!type && handle_base && writ_is_text(model, node->name, "opaque")) {
        return writ_type_intern(model, (writ_type_key_t){.kind = WRIT_TYPE_OPAQUE});
    }
    if (!type) {
        writ_str_t text = writ_sym_str(model->session, node->name);
        writ_report(model->session, WRIT_D_UNKNOWN_TYPE, node->loc, "`%.*s`", (int)text.length, text.data);
        return 0;
    }
    if (encoding) {
        if (writ_type(model, type)->key.kind != WRIT_TYPE_CSTR) {
            writ_report(model->session, WRIT_D_UNKNOWN_TYPE, node->loc, "only `cstr` takes an encoding");
            return 0;
        }
        unsupported(model, encoding, "text encodings other than the library's");
        return 0;
    }
    return type;
}

static uint32_t
constant_count(resolver_t *r, uint32_t expr, bool *constant)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, expr);
    *constant = false;
    if (node->kind == WRIT_N_EXPR_NAME) {
        uint32_t index;
        if (!writ_scope_lookup(model, node->name, WRIT_SCOPE_CONST, &index)) {
            return 0;
        }
    }
    int64_t value = 0;
    size_t first_diag = model->session->diags.len;
    if (writ_expr_check_in(model, WRIT_EXPR_SCOPE_CONSTANT, WRIT_NONE, expr)
        && writ_expr_eval(model, expr, nullptr, &value) && value >= 0) {
        *constant = true;
        return (uint32_t)value;
    }
    model->session->diags.len = first_diag;
    return 0;
}

static void
take_extent(resolver_t *r, writ_slot_rec_t *slot, uint32_t extent)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, extent);
    switch (node->kind) {
    case WRIT_N_EXTENT_COUNT: {
        uint32_t expr = child(model, extent, 0);
        const writ_node_t *count = node_at(model, expr);
        if (count->kind == WRIT_N_EXPR_NAME && writ_is_text(model, count->name, "unknown")) {
            slot->extent_kind = WRIT_EXTENT_UNKNOWN;
        } else {
            slot->extent_kind = WRIT_EXTENT_COUNT;
            slot->count_expr = expr;
        }
        return;
    }
    case WRIT_N_EXTENT_UNTIL:
        slot->extent_kind = WRIT_EXTENT_UNTIL;
        slot->count_expr = child(model, extent, 0);
        return;
    case WRIT_N_EXTENT_BOUNDED:
    case WRIT_N_EXTENT_CAP_LEN:
        slot->extent_kind = WRIT_EXTENT_BOUNDED;
        slot->cap_expr = child(model, extent, 0);
        slot->len_expr = child(model, extent, 1);
        return;
    default:
        unsupported(model, extent, "extents chosen by a condition");
        return;
    }
}

static writ_type_id
pointer_to(resolver_t *r, uint32_t id, writ_type_id target, bool nullable)
{
    writ_model_t *model = r->model;
    const writ_type_rec_t *type = writ_type(model, target);
    if (nullable && type->key.kind == WRIT_TYPE_HANDLE) {
        return writ_type_intern(model, (writ_type_key_t){
            .kind = WRIT_TYPE_HANDLE, .nullable = 1, .name = type->key.name, .target = type->key.target});
    }
    (void)id;
    return writ_type_intern(model, (writ_type_key_t){.kind = WRIT_TYPE_POINTER, .nullable = nullable,
                                                     .target = target});
}

static writ_type_id
resolve_type_in(resolver_t *r, uint32_t id, writ_slot_rec_t *slot, bool handle_base)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    switch (node->kind) {
    case WRIT_N_TYPE_NAMED:
        return named_type(r, id, handle_base);
    case WRIT_N_TYPE_POINTER: {
        writ_type_id target = resolve_type_in(r, child(model, id, 0), nullptr, handle_base);
        bool nullable = writ_is_text(model, node->payload, "?");
        return target ? pointer_to(r, id, target, nullable) : 0;
    }
    case WRIT_N_TYPE_ARRAY: {
        uint32_t element = child(model, id, 0);
        if (node_at(model, element)->child_count != 1) {
            unsupported(model, element, "ownership and spans on array elements");
            return 0;
        }
        writ_type_id target = resolve_type(r, child(model, element, 0), nullptr);
        if (!target) {
            return 0;
        }
        if (slot) {
            take_extent(r, slot, child(model, id, 1));
        }
        return writ_type_intern(model, (writ_type_key_t){
            .kind = WRIT_TYPE_ARRAY, .nullable = writ_is_text(model, node->payload, "?"), .target = target});
    }
    case WRIT_N_TYPE_INLINE: {
        writ_type_id target = resolve_type(r, child(model, id, 0), nullptr);
        uint32_t extent = child(model, id, 1);
        bool constant = false;
        uint32_t count = 0;
        if (node_at(model, extent)->kind == WRIT_N_EXTENT_COUNT) {
            count = constant_count(r, child(model, extent, 0), &constant);
        }
        if (!constant && slot) {
            take_extent(r, slot, extent);
        }
        return target ? writ_type_intern(model, (writ_type_key_t){.kind = WRIT_TYPE_INLINE, .target = target,
                                                                  .count = count})
                      : 0;
    }
    case WRIT_N_TYPE_BYTES: {
        if (slot) {
            take_extent(r, slot, child(model, id, 0));
        }
        writ_type_id bytes = writ_type_intern(model, (writ_type_key_t){.kind = WRIT_TYPE_ARRAY,
                                                                        .target = writ_type_builtin(model, "u8")});
        return writ_is_text(model, node->payload, "utf8")
                 ? writ_type_intern(model, (writ_type_key_t){.kind = WRIT_TYPE_VIEW, .target = bytes,
                                                             .name = node->payload})
                 : bytes;
    }
    case WRIT_N_TYPE_VIEW: {
        writ_type_id base = resolve_type_in(r, child(model, id, 0), slot, handle_base);
        uint32_t target = child(model, id, 1);
        if (node_at(model, target)->kind != WRIT_N_TYPE_NAMED) {
            unsupported(model, target, "reinterpreted element types (`as *T`)");
            return 0;
        }
        return base ? writ_type_intern(model, (writ_type_key_t){.kind = WRIT_TYPE_VIEW, .target = base,
                                                                .name = node_at(model, target)->name})
                    : 0;
    }
    default:
        unsupported(model, id, "Rust types (dsl.md §8.9)");
        return 0;
    }
}

static writ_type_id
resolve_type(resolver_t *r, uint32_t id, writ_slot_rec_t *slot)
{
    return resolve_type_in(r, id, slot, false);
}

static void
take_span(writ_model_t *model, uint32_t id, writ_span_kind_t *span, uint32_t *span_node, writ_sym_t *with)
{
    const writ_node_t *node = node_at(model, id);
    *span_node = id;
    switch (node->kind) {
    case WRIT_N_SPAN_CALL:
        *span = WRIT_SPAN_CALL;
        return;
    case WRIT_N_SPAN_STATIC:
        *span = first_of(model, id, WRIT_N_PER_THREAD) ? WRIT_SPAN_STATIC_PER_THREAD : WRIT_SPAN_STATIC;
        return;
    case WRIT_N_SPAN_WITH:
        *span = WRIT_SPAN_WITH;
        *with = node->name;
        return;
    case WRIT_N_SPAN_UNTIL:
        *span = WRIT_SPAN_UNTIL;
        return;
    case WRIT_N_SPAN_ONCE:
        *span = WRIT_SPAN_ONCE;
        return;
    case WRIT_N_SPAN_UNKNOWN:
        *span = WRIT_SPAN_UNKNOWN;
        return;
    default:
        return;
    }
}

static writ_mode_t
mode_of(const writ_model_t *model, writ_sym_t lexeme)
{
    static const char *const names[] = {"in", "out", "inout", "consume", "keep", "moved", "unknown"};
    writ_str_t text = writ_sym_str(model->session, lexeme);
    for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (writ_str_eq(text, writ_cstr(names[i]))) {
            return i;
        }
    }
    return writ_default(WRIT_DEFAULT_PARAM_MODE);
}

static writ_slot_rec_t
resolve_slot(resolver_t *r, uint32_t id, uint32_t owner)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    writ_slot_rec_t slot = {
        .name          = node->name,
        .owner         = owner,
        .mode          = node->kind == WRIT_N_PARAM ? mode_of(model, node->payload) : WRIT_MODE_IN,
        .offset_assert = WRIT_NONE,
        .loc           = node->loc,
    };
    uint32_t type = type_child(model, id);
    slot.type = type ? resolve_type(r, type, &slot) : 0;
    for (uint32_t i = 0; i < node->child_count; i++) {
        uint32_t attr = child(model, id, i);
        const writ_node_t *part = node_at(model, attr);
        switch (part->kind) {
        case WRIT_N_OWN:
            slot.own = true;
            break;
        case WRIT_N_RELEASE:
            slot.release = part->name;
            break;
        case WRIT_N_ALLOC:
            slot.alloc = part->name;
            break;
        case WRIT_N_PRIVATE:
            slot.is_private = true;
            break;
        case WRIT_N_NULL_IF_EMPTY:
            slot.null_if_empty = true;
            break;
        case WRIT_N_OFFSET:
            slot.offset_assert = (uint32_t)strtoul(writ_sym_str(model->session, part->name).data, nullptr, 0);
            break;
        case WRIT_N_USERDATA:
            unsupported(model, attr, "callback user data (dsl.md §6.3)");
            break;
        case WRIT_N_ALIGN_ATTR:
            unsupported(model, attr, "field alignment");
            break;
        case WRIT_N_SPAN_CALL:
        case WRIT_N_SPAN_STATIC:
        case WRIT_N_SPAN_WITH:
        case WRIT_N_SPAN_UNTIL:
        case WRIT_N_SPAN_ONCE:
        case WRIT_N_SPAN_UNKNOWN:
            take_span(model, attr, &slot.span, &slot.span_node, &slot.span_with);
            break;
        default:
            break;
        }
    }
    return slot;
}

static writ_fn_rec_t
fn_record(writ_sym_t name, uint32_t node, writ_fn_kind_t kind, uint32_t first_slot, writ_loc_t loc)
{
    return (writ_fn_rec_t){
        .name               = name,
        .node               = node,
        .kind               = kind,
        .first_slot         = first_slot,
        .sizing_slot        = WRIT_NONE,
        .varargs_count_slot = WRIT_NONE,
        .format_slot        = WRIT_NONE,
        .source_name        = name,
        .link_symbol        = name,
        .loc                = loc,
    };
}

static uint32_t
slot_ref(writ_model_t *model, const writ_fn_rec_t *fn, uint32_t ref_node)
{
    const writ_node_t *ref = node_at(model, ref_node);
    uint32_t index = writ_fn_slot_named(model, fn, ref->name);
    if (index == WRIT_NONE) {
        writ_str_t text = writ_sym_str(model->session, ref->name);
        writ_report(model->session, WRIT_D_UNKNOWN_SLOT, ref->loc, "`%.*s`", (int)text.length, text.data);
    }
    return index;
}

static void
add_outcome(writ_model_t *model, writ_fn_rec_t *fn, writ_outcome_rec_t outcome)
{
    if (fn->first_outcome + fn->outcome_count != model->outcomes.len) {
        size_t start = model->outcomes.len;
        for (uint32_t i = 0; i < fn->outcome_count; i++) {
            core_list_push(model->outcomes, model->outcomes.data[fn->first_outcome + i]);
        }
        fn->first_outcome = (uint32_t)start;
    }
    core_list_push(model->outcomes, outcome);
    fn->outcome_count++;
}

static void
take_via(writ_model_t *model, uint32_t via, writ_outcome_rec_t *outcome)
{
    if (!via) {
        return;
    }
    outcome->via_expr = child(model, via, 0);
    outcome->via_own = first_of(model, via, WRIT_N_OWN) != 0;
    uint32_t release = first_of(model, via, WRIT_N_RELEASE);
    outcome->via_release = release ? node_at(model, release)->name : 0;
}

typedef struct {
    bool outcomes;
    bool sizing;
    bool thread;
} fn_facts_t;

static void
apply_clause(resolver_t *r, writ_fn_rec_t *fn, uint32_t id, fn_facts_t *stated)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    switch (node->kind) {
    case WRIT_N_PURE:
        fn->pure = true;
        return;
    case WRIT_N_SIZING: {
        if (stated && !stated->sizing) {
            fn->assign_count = 0;
            stated->sizing = true;
        }
        fn->sizing = WRIT_SIZING_NEEDS;
        fn->sizing_expr = child(model, id, node->child_count - 1);
        uint32_t query = first_of(model, id, WRIT_N_SIZING_QUERY);
        if (query) {
            fn->sizing = WRIT_SIZING_QUERY;
            fn->first_assign = (uint32_t)model->assigns.len;
            fn->assign_count = node_at(model, query)->child_count;
            for (uint32_t i = 0; i < fn->assign_count; i++) {
                uint32_t assign = child(model, query, i);
                uint32_t slot = writ_fn_slot_named(model, fn, node_at(model, assign)->name);
                if (slot == WRIT_NONE) {
                    writ_str_t text = writ_sym_str(model->session, node_at(model, assign)->name);
                    writ_report(model->session, WRIT_D_UNKNOWN_SLOT, node_at(model, assign)->loc, "`%.*s`",
                                (int)text.length, text.data);
                }
                core_list_push(model->assigns, ((writ_assign_rec_t){slot, child(model, assign, 0)}));
            }
        }
        if (first_of(model, id, WRIT_N_SIZING_RETRY)) {
            fn->sizing = WRIT_SIZING_RETRY;
        }
        uint32_t target = first_of(model, id, WRIT_N_SIZING_FOR);
        if (target) {
            fn->sizing_slot = writ_fn_slot_named(model, fn, node_at(model, target)->name);
            if (fn->sizing_slot == WRIT_NONE) {
                writ_report(model->session, WRIT_D_UNKNOWN_SLOT, node_at(model, target)->loc, nullptr);
            }
        }
        return;
    }
    case WRIT_N_ERROR:
    case WRIT_N_OUTCOME: {
        if (stated && !stated->outcomes) {
            fn->outcome_count = 0;
            stated->outcomes = true;
        }
        writ_outcome_rec_t outcome = {
            .name  = node->kind == WRIT_N_ERROR ? sym(model, "error") : node->name,
            .kind  = node->kind == WRIT_N_ERROR || writ_is_text(model, node->payload, "failure")
                       ? WRIT_OUTCOME_FAILURE
                       : WRIT_OUTCOME_SUCCESS,
            .retry = first_of(model, id, WRIT_N_RETRY) != 0,
            .loc   = node->loc,
        };
        if (node->kind == WRIT_N_ERROR) {
            outcome.when_expr = child(model, id, 0);
        } else {
            uint32_t when = first_of(model, id, WRIT_N_WHEN_PART);
            outcome.when_expr = when ? child(model, when, 0) : 0;
        }
        take_via(model, first_of(model, id, WRIT_N_VIA), &outcome);
        add_outcome(model, fn, outcome);
        if (node->kind == WRIT_N_ERROR) {
            add_outcome(model, fn, (writ_outcome_rec_t){.name = sym(model, "ok"), .kind = WRIT_OUTCOME_SUCCESS,
                                                        .loc = node->loc});
        }
        return;
    }
    case WRIT_N_ON: {
        uint32_t slot = slot_ref(model, fn, first_of(model, id, WRIT_N_REF));
        writ_on_rec_t on = {.outcome = node->name, .slot = slot, .loc = node->loc};
        static const char *const words[] = {"own", "unset", "partial"};
        for (uint32_t i = 0; i < 3; i++) {
            if (writ_is_text(model, node->payload, words[i])) {
                on.state = i;
            }
        }
        if (first_of(model, id, WRIT_N_NOT_CONSUMED)) {
            on.state = WRIT_ON_NOT_CONSUMED;
        }
        for (uint32_t i = 0; i < node->child_count; i++) {
            writ_sym_t unused = 0;
            uint32_t part = child(model, id, i);
            writ_node_kind_t kind = node_at(model, part)->kind;
            if (kind >= WRIT_N_SPAN_CALL && kind <= WRIT_N_SPAN_UNKNOWN && kind != WRIT_N_PER_THREAD) {
                on.state = WRIT_ON_SPAN;
                take_span(model, part, &on.span, &on.span_node, &unused);
            }
        }
        if (fn->first_on + fn->on_count != model->ons.len) {
            size_t start = model->ons.len;
            for (uint32_t i = 0; i < fn->on_count; i++) {
                core_list_push(model->ons, model->ons.data[fn->first_on + i]);
            }
            fn->first_on = (uint32_t)start;
        }
        core_list_push(model->ons, on);
        fn->on_count++;
        return;
    }
    case WRIT_N_EXPANDS:
        fn->expands_node = id;
        fn->expands_symbol = node->name;
        return;
    case WRIT_N_THREAD: {
        static const char *const words[] = {"", "any", "main", "same"};
        fn->thread = first_of(model, id, WRIT_N_SAME_AS) ? WRIT_THREAD_SAME_AS : WRIT_THREAD_UNSTATED;
        for (uint32_t i = 1; i < 4; i++) {
            if (writ_is_text(model, node->payload, words[i])) {
                fn->thread = i;
            }
        }
        return;
    }
    case WRIT_N_NOTE:
        fn->note = node->name;
        return;
    case WRIT_N_SOURCE:
        fn->source_name = node->name;
        fn->source_macro = first_of(model, id, WRIT_N_MACRO) != 0;
        return;
    case WRIT_N_SYMBOL: {
        uint32_t when = first_of(model, id, WRIT_N_WHEN_PART);
        if (when && !writ_condition_holds(model, child(model, when, 0))) {
            return;
        }
        uint32_t text = first_of(model, id, WRIT_N_TEXT);
        fn->no_symbol = !text;
        fn->link_symbol = text ? node_at(model, text)->name : 0;
        if (first_of(model, id, WRIT_N_SYMBOL_VERSION)) {
            unsupported(model, id, "versioned symbols");
        }
        return;
    }
    case WRIT_N_FORMAT:
        fn->format_slot = slot_ref(model, fn, first_of(model, id, WRIT_N_REF));
        return;
    case WRIT_N_PACK_REST:
    case WRIT_N_REENTRANT:
    case WRIT_N_UNWIND:
    case WRIT_N_SIGNAL_SAFE:
        return;
    case WRIT_N_SINCE:
    case WRIT_N_REMOVED:
        unsupported(model, id, "`since` and `removed` (they need a version query)");
        return;
    default:
        return;
    }
}

static bool
is_clause(writ_node_kind_t kind)
{
    switch (kind) {
    case WRIT_N_PURE:
    case WRIT_N_SIZING:
    case WRIT_N_ERROR:
    case WRIT_N_OUTCOME:
    case WRIT_N_ON:
    case WRIT_N_EXPANDS:
    case WRIT_N_THREAD:
    case WRIT_N_NOTE:
    case WRIT_N_SOURCE:
    case WRIT_N_SYMBOL:
    case WRIT_N_FORMAT:
    case WRIT_N_PACK_REST:
    case WRIT_N_REENTRANT:
    case WRIT_N_UNWIND:
    case WRIT_N_SIGNAL_SAFE:
    case WRIT_N_SINCE:
    case WRIT_N_REMOVED:
        return true;
    default:
        return false;
    }
}

static void
apply_clauses(resolver_t *r, writ_fn_rec_t *fn, uint32_t id, fn_facts_t *stated)
{
    const writ_node_t *node = node_at(r->model, id);
    for (uint32_t i = 0; i < node->child_count; i++) {
        uint32_t clause = child(r->model, id, i);
        if (is_clause(node_at(r->model, clause)->kind)) {
            apply_clause(r, fn, clause, stated);
        }
    }
}

static uint32_t
convention_for(resolver_t *r, writ_sym_t name, writ_loc_t loc)
{
    writ_model_t *model = r->model;
    writ_str_t text = writ_sym_str(model->session, name);
    uint32_t best = WRIT_NONE;
    size_t best_prefix = 0;
    bool tie = false;
    for (uint32_t i = 0; i < model->conventions.len; i++) {
        writ_str_t pattern = writ_sym_str(model->session, model->conventions.data[i].pattern);
        if (!writ_glob_match(pattern, text)) {
            continue;
        }
        size_t prefix = writ_glob_literal_prefix(pattern);
        if (best == WRIT_NONE || prefix > best_prefix) {
            best = i;
            best_prefix = prefix;
            tie = false;
        } else if (prefix == best_prefix) {
            tie = true;
        }
    }
    if (tie) {
        writ_report(model->session, WRIT_D_CONVENTION_TIE, loc, "`%.*s`", (int)text.length, text.data);
    }
    return best;
}

static void
resolve_varargs(resolver_t *r, writ_fn_rec_t *fn, uint32_t id)
{
    writ_model_t *model = r->model;
    uint32_t type = type_child(model, id);
    fn->varargs = WRIT_VARARGS_OPAQUE;
    if (!type) {
        return;
    }
    fn->varargs = WRIT_VARARGS_OF;
    fn->varargs_type = resolve_type(r, type, nullptr);
    uint32_t until = first_of(model, id, WRIT_N_VARARGS_UNTIL);
    uint32_t count = first_of(model, id, WRIT_N_VARARGS_COUNT);
    if (until) {
        fn->varargs = WRIT_VARARGS_UNTIL;
        fn->varargs_end = child(model, until, 0);
    } else if (count) {
        fn->varargs = WRIT_VARARGS_COUNT;
        fn->varargs_count_slot = writ_fn_slot_named(model, fn, node_at(model, count)->name);
        if (fn->varargs_count_slot == WRIT_NONE) {
            writ_report(model->session, WRIT_D_UNKNOWN_SLOT, node_at(model, count)->loc, nullptr);
        }
    }
}

static void
resolve_fn(resolver_t *r, uint32_t id, writ_fn_kind_t kind)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    writ_fn_rec_t fn = fn_record(node->name, id, kind, (uint32_t)model->slots.len, node->loc);
    uint32_t index = (uint32_t)model->fns.len;
    uint32_t varargs = 0;
    for (uint32_t i = 0; i < node->child_count; i++) {
        uint32_t part = child(model, id, i);
        switch (node_at(model, part)->kind) {
        case WRIT_N_PARAM: {
            writ_slot_rec_t slot = resolve_slot(r, part, index);
            if (writ_type(model, slot.type)->key.kind == WRIT_TYPE_VA_LIST) {
                fn.varargs = WRIT_VARARGS_VA_LIST;
            }
            core_list_push(model->slots, slot);
            fn.slot_count++;
            break;
        }
        case WRIT_N_VARARGS:
            varargs = part;
            break;
        case WRIT_N_SELF_PARAM:
            unsupported(model, part, "`self` parameters (dsl.md §8.9)");
            break;
        default:
            break;
        }
    }
    uint32_t result = first_of(model, id, WRIT_N_RESULT);
    if (result) {
        writ_slot_rec_t slot = resolve_slot(r, result, index);
        slot.name = sym(model, "ret");
        fn.result = slot.type;
        fn.has_result = slot.type && writ_type(model, slot.type)->key.kind != WRIT_TYPE_VOID;
        if (fn.has_result) {
            core_list_push(model->slots, slot);
        }
    }
    if (varargs) {
        resolve_varargs(r, &fn, varargs);
    }
    if (kind == WRIT_FN_FUNCTION) {
        uint32_t convention = convention_for(r, fn.name, fn.loc);
        if (convention != WRIT_NONE) {
            apply_clauses(r, &fn, model->conventions.data[convention].node, nullptr);
        }
    }
    fn_facts_t stated = {0};
    apply_clauses(r, &fn, id, &stated);
    if (fn.format_slot != WRIT_NONE && fn.varargs == WRIT_VARARGS_OPAQUE) {
        fn.varargs = WRIT_VARARGS_FORMAT;
    }
    if (kind == WRIT_FN_CALLBACK) {
        writ_type_id type = writ_type_named(model, fn.name);
        model->types.data[type].decl = index;
        core_list_push(model->fns, fn);
        return;
    }
    if (declare(model, fn.name, WRIT_SCOPE_FN, index, fn.loc)) {
        core_list_push(model->fns, fn);
    } else {
        model->slots.len = fn.first_slot;
    }
}

static void
resolve_record(resolver_t *r, uint32_t id)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    writ_type_id type = writ_type_named(model, node->name);
    uint32_t index = writ_type(model, type)->decl;
    writ_record_rec_t *record = &model->records.data[index];
    record->first_field = (uint32_t)model->slots.len;
    for (uint32_t i = 0; i < node->child_count; i++) {
        uint32_t part = child(model, id, i);
        const writ_node_t *piece = node_at(model, part);
        switch (piece->kind) {
        case WRIT_N_FIELD: {
            writ_slot_rec_t field = resolve_slot(r, part, index);
            if (!field.span && writ_type_is_pointer(model, field.type)) {
                field.span = WRIT_SPAN_WITH;
                field.span_with = sym(model, "self");
            }
            core_list_push(model->slots, field);
            model->records.data[index].field_count++;
            break;
        }
        case WRIT_N_LAYOUT:
            model->records.data[index].explicit_layout = writ_is_text(model, piece->payload, "explicit");
            if (model->records.data[index].explicit_layout) {
                unsupported(model, part, "explicit layout");
            }
            break;
        case WRIT_N_SIZE_ATTR:
            model->records.data[index].size_assert = part;
            break;
        case WRIT_N_ALIGN_ATTR:
            model->records.data[index].align_assert =
                (uint32_t)strtoul(writ_sym_str(model->session, piece->name).data, nullptr, 0);
            break;
        case WRIT_N_PACK_ATTR:
            model->records.data[index].pack =
                (uint32_t)strtoul(writ_sym_str(model->session, piece->name).data, nullptr, 0);
            break;
        case WRIT_N_STRUCT_NONE:
            model->records.data[index].none_node = part;
            break;
        default:
            break;
        }
    }
}

static void
resolve_handle(resolver_t *r, uint32_t id)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    writ_type_id type = writ_type_named(model, node->name);
    writ_handle_rec_t *handle = &model->handles.data[writ_type(model, type)->decl];
    handle->base = resolve_type_in(r, child(model, id, 0), nullptr, true);
    handle = &model->handles.data[writ_type(model, type)->decl];
    handle->release = node_at(model, first_of(model, id, WRIT_N_RELEASE))->name;
    handle->thread_rule = writ_is_text(model, node->payload, "shared")          ? WRIT_THREAD_RULE_SHARED
                        : writ_is_text(model, node->payload, "thread_affine")   ? WRIT_THREAD_RULE_AFFINE
                                                                                : WRIT_THREAD_RULE_SERIALIZED;
    writ_type_rec_t *record = &model->types.data[type];
    const writ_type_rec_t *base = writ_type(model, handle->base);
    record->key.target = handle->base;
    record->size = base->size;
    record->align = base->align;
    record->is_signed = base->is_signed;
}

static void
resolve_enum(resolver_t *r, uint32_t id)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    writ_type_id type = writ_type_named(model, node->name);
    uint32_t index = writ_type(model, type)->decl;
    uint32_t base_node = type_child(model, id);
    writ_type_id base = base_node ? resolve_type(r, base_node, nullptr) : writ_type_builtin(model, "c_int");
    writ_enum_rec_t *record = &model->enums.data[index];
    record->base = base;
    record->closed = first_of(model, id, WRIT_N_CLOSED) != 0;
    record->first_item = (uint32_t)model->consts.len;
    writ_type_rec_t *self = &model->types.data[type];
    self->key.target = base;
    self->size = writ_type(model, base)->size;
    self->align = writ_type(model, base)->align;
    self->is_signed = writ_type(model, base)->is_signed;
    int64_t next = 0;
    for (uint32_t i = 0; i < node->child_count; i++) {
        uint32_t item = child(model, id, i);
        const writ_node_t *piece = node_at(model, item);
        if (piece->kind != WRIT_N_ENUM_ITEM) {
            continue;
        }
        uint32_t value = child(model, item, 0);
        writ_const_rec_t constant = {.name = piece->name, .node = item, .type = type, .value = next, .owner = index,
                                     .loc = piece->loc};
        if (value && !(writ_expr_check_in(model, WRIT_EXPR_SCOPE_CONSTANT, WRIT_NONE, value)
                       && writ_expr_eval(model, value, nullptr, &constant.value))) {
            writ_report(model->session, WRIT_D_NOT_CONSTANT, piece->loc, nullptr);
        }
        next = constant.value + 1;
        if (declare(model, constant.name, WRIT_SCOPE_CONST, (uint32_t)model->consts.len, constant.loc)) {
            core_list_push(model->consts, constant);
            model->enums.data[index].item_count++;
        }
    }
}

static void
resolve_const(resolver_t *r, uint32_t id)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    writ_const_rec_t record = {
        .name  = node->name,
        .node  = id,
        .type  = resolve_type(r, child(model, id, 0), nullptr),
        .owner = WRIT_NONE,
        .loc   = node->loc,
    };
    uint32_t value = child(model, id, 1);
    const writ_node_t *value_node = node_at(model, value);
    if (value_node->kind == WRIT_N_EXPR_STRING) {
        record.text = value_node->name;
    } else if (!writ_expr_check_in(model, WRIT_EXPR_SCOPE_CONSTANT, WRIT_NONE, value)
               || !writ_expr_eval(model, value, nullptr, &record.value) || !writ_type_is_integer(model, record.type)) {
        writ_report(model->session, WRIT_D_NOT_CONSTANT, record.loc, nullptr);
    }
    if (declare(model, record.name, WRIT_SCOPE_CONST, (uint32_t)model->consts.len, record.loc)) {
        core_list_push(model->consts, record);
    }
}

static void
resolve_channel(resolver_t *r, uint32_t id)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    writ_channel_rec_t channel = {.name = node->name, .loc = node->loc};
    uint32_t per_handle = first_of(model, id, WRIT_N_PER_HANDLE);
    if (per_handle) {
        channel.per_handle = true;
        channel.handle = writ_type_named(model, node_at(model, per_handle)->name);
        if (writ_type(model, channel.handle)->key.kind != WRIT_TYPE_HANDLE) {
            writ_report(model->session, WRIT_D_UNKNOWN_TYPE, node_at(model, per_handle)->loc, "not a handle");
        }
    }
    channel.reset = first_of(model, id, WRIT_N_RESET) != 0;
    uint32_t source = first_of(model, id, WRIT_N_CHANNEL_SOURCE);
    channel.kind = writ_is_text(model, node_at(model, source)->payload, "text") ? WRIT_CHANNEL_TEXT : WRIT_CHANNEL_VALUE;
    channel.source_expr = child(model, source, 0);
    channel.valid_until = first_of(model, id, WRIT_N_VALID_UNTIL);
    if (declare(model, channel.name, WRIT_SCOPE_CHANNEL, (uint32_t)model->channels.len, channel.loc)) {
        core_list_push(model->channels, channel);
    }
}

static void
resolve_var(resolver_t *r, uint32_t id)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    writ_var_rec_t var = {
        .name        = node->name,
        .type        = resolve_type(r, type_child(model, id), nullptr),
        .is_mutable  = writ_is_text(model, node->payload, "mut"),
        .link_symbol = node->name,
        .loc         = node->loc,
    };
    for (uint32_t i = 0; i < node->child_count; i++) {
        uint32_t part = child(model, id, i);
        if (node_at(model, part)->kind == WRIT_N_SYMBOL) {
            uint32_t when = first_of(model, part, WRIT_N_WHEN_PART);
            uint32_t text = first_of(model, part, WRIT_N_TEXT);
            if (text && (!when || writ_condition_holds(model, child(model, when, 0)))) {
                var.link_symbol = node_at(model, text)->name;
            }
        }
    }
    if (declare(model, var.name, WRIT_SCOPE_VAR, (uint32_t)model->vars.len, var.loc)) {
        core_list_push(model->vars, var);
    }
}

static void
resolve_decl(resolver_t *r, uint32_t id)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    switch (node->kind) {
    case WRIT_N_FN:
        resolve_fn(r, id, WRIT_FN_FUNCTION);
        return;
    case WRIT_N_CALLBACK:
        resolve_fn(r, id, WRIT_FN_CALLBACK);
        return;
    case WRIT_N_STRUCT:
        resolve_record(r, id);
        return;
    case WRIT_N_HANDLE:
        resolve_handle(r, id);
        return;
    case WRIT_N_CHANNEL:
        resolve_channel(r, id);
        return;
    case WRIT_N_VAR:
        resolve_var(r, id);
        return;
    case WRIT_N_ALLOCATOR:
        core_list_push(model->allocators,
                       ((writ_allocator_rec_t){node->name, node_at(model, first_of(model, id, WRIT_N_ALLOC))->name,
                                               node_at(model, first_of(model, id, WRIT_N_RELEASE))->name,
                                               node->loc}));
        return;
    default:
        return;
    }
}

static bool
physical_narrowing(const writ_model_t *model, writ_type_id from, writ_type_id to)
{
    from = writ_type_physical(model, from);
    to = writ_type_physical(model, to);
    if (from == to) {
        return true;
    }
    const writ_type_rec_t *a = writ_type(model, from);
    const writ_type_rec_t *b = writ_type(model, to);
    bool a_pointer = a->key.kind == WRIT_TYPE_POINTER || a->key.kind == WRIT_TYPE_ARRAY;
    bool b_pointer = b->key.kind == WRIT_TYPE_POINTER || b->key.kind == WRIT_TYPE_ARRAY;
    if (a_pointer && b_pointer) {
        if (b->key.nullable && !a->key.nullable) {
            return false;
        }
        const writ_type_rec_t *a_target = writ_type(model, writ_type_physical(model, a->key.target));
        const writ_type_rec_t *b_target = writ_type(model, writ_type_physical(model, b->key.target));
        bool a_char = a_target->key.kind == WRIT_TYPE_CSTR || (a_target->key.kind == WRIT_TYPE_INT && a_target->size == 1);
        bool b_char = b_target->key.kind == WRIT_TYPE_CSTR || (b_target->key.kind == WRIT_TYPE_INT && b_target->size == 1);
        return (a_char && b_char) || a_target->key.kind == WRIT_TYPE_VOID
            || physical_narrowing(model, a->key.target, b->key.target);
    }
    return a->key.kind == b->key.kind && a->size == b->size && a->is_signed == b->is_signed;
}

static void
apply_refine(resolver_t *r, uint32_t id)
{
    writ_model_t *model = r->model;
    const writ_node_t *node = node_at(model, id);
    uint32_t index = WRIT_NONE;
    if (!writ_scope_lookup(model, node->name, WRIT_SCOPE_FN, &index)) {
        writ_type_id callback = writ_type_named(model, node->name);
        if (writ_type(model, callback)->key.kind == WRIT_TYPE_CALLBACK) {
            index = writ_type(model, callback)->decl;
        }
    }
    if (index == WRIT_NONE) {
        writ_str_t text = writ_sym_str(model->session, node->name);
        writ_report(model->session, WRIT_D_REFINE_TARGET, node->loc, "`%.*s`", (int)text.length, text.data);
        return;
    }
    fn_facts_t stated = {0};
    for (uint32_t i = 0; i < node->child_count; i++) {
        uint32_t part = child(model, id, i);
        const writ_node_t *piece = node_at(model, part);
        writ_fn_rec_t *fn = &model->fns.data[index];
        uint32_t target = WRIT_NONE;
        if (piece->kind == WRIT_N_PARAM) {
            target = writ_fn_slot_named(model, fn, piece->name);
            if (target == WRIT_NONE) {
                writ_str_t text = writ_sym_str(model->session, piece->name);
                writ_report(model->session, WRIT_D_UNKNOWN_SLOT, piece->loc, "`%.*s`", (int)text.length, text.data);
                continue;
            }
        } else if (piece->kind == WRIT_N_RESULT) {
            if (!fn->has_result) {
                writ_report(model->session, WRIT_D_BAD_REFINE, piece->loc, "the function returns nothing");
                continue;
            }
            target = fn->slot_count;
        }
        if (target != WRIT_NONE) {
            writ_slot_rec_t refined = resolve_slot(r, part, index);
            writ_slot_rec_t *slot = &model->slots.data[model->fns.data[index].first_slot + target];
            if (refined.type && !physical_narrowing(model, slot->type, refined.type)) {
                writ_report(model->session, WRIT_D_BAD_REFINE, piece->loc, "it changes the physical type");
                continue;
            }
            refined.name = slot->name;
            refined.loc = slot->loc;
            if (piece->kind == WRIT_N_RESULT) {
                model->fns.data[index].result = refined.type;
            }
            *slot = refined;
        } else if (is_clause(piece->kind)) {
            apply_clause(r, fn, part, &stated);
        }
    }
}

static void
check_fn_expressions(writ_model_t *model, uint32_t f)
{
    const writ_fn_rec_t *fn = &model->fns.data[f];
    uint32_t count = fn->slot_count + (fn->has_result ? 1 : 0);
    for (uint32_t s = 0; s < count; s++) {
        const writ_slot_rec_t *slot = &model->slots.data[fn->first_slot + s];
        uint32_t exprs[] = {slot->count_expr, slot->cap_expr, slot->len_expr};
        for (size_t e = 0; e < 3; e++) {
            if (exprs[e]) {
                writ_expr_check(model, f, exprs[e]);
            }
        }
    }
    for (uint32_t o = 0; o < fn->outcome_count; o++) {
        const writ_outcome_rec_t *outcome = &model->outcomes.data[fn->first_outcome + o];
        if (outcome->when_expr) {
            writ_expr_check(model, f, outcome->when_expr);
        }
        if (outcome->via_expr) {
            writ_expr_check_detail(model, f, outcome->via_expr);
        }
    }
    for (uint32_t a = 0; a < fn->assign_count; a++) {
        writ_expr_check(model, f, model->assigns.data[fn->first_assign + a].expr);
    }
    uint32_t clauses[] = {fn->sizing_expr, fn->varargs_end};
    for (size_t c = 0; c < 2; c++) {
        if (clauses[c]) {
            writ_expr_check(model, f, clauses[c]);
        }
    }
    if (fn->expands_node) {
        const writ_node_t *expands = node_at(model, fn->expands_node);
        for (uint32_t i = 0; i < expands->child_count; i++) {
            writ_expr_check(model, f, child(model, fn->expands_node, i));
        }
    }
}

static void
check_expressions(writ_model_t *model)
{
    for (uint32_t f = 0; f < model->fns.len; f++) {
        check_fn_expressions(model, f);
    }
    for (uint32_t c = 0; c < model->channels.len; c++) {
        uint32_t source = model->channels.data[c].source_expr;
        if (node_at(model, source)->kind != WRIT_N_EXPR_CALL) {
            writ_report(model->session, WRIT_D_UNKNOWN_NAME, node_at(model, source)->loc,
                        "a channel's source is a call to a declared function");
            continue;
        }
        writ_expr_check_in(model, WRIT_EXPR_SCOPE_CHANNEL, WRIT_NONE, source);
    }
    for (uint32_t r = 0; r < model->records.len; r++) {
        const writ_record_rec_t *record = &model->records.data[r];
        for (uint32_t s = 0; s < record->field_count; s++) {
            const writ_slot_rec_t *field = &model->slots.data[record->first_field + s];
            uint32_t exprs[] = {field->count_expr, field->cap_expr, field->len_expr};
            for (size_t e = 0; e < 3; e++) {
                if (exprs[e]) {
                    writ_expr_check_in(model, WRIT_EXPR_SCOPE_RECORD, r, exprs[e]);
                }
            }
        }
    }
}

static void
resolve_handle_nones(writ_model_t *model, resolver_t *r)
{
    for (size_t i = 0; i < r->decls.len; i++) {
        uint32_t id = r->decls.data[i];
        const writ_node_t *node = node_at(model, id);
        if (node->kind != WRIT_N_HANDLE) {
            continue;
        }
        uint32_t none = first_of(model, id, WRIT_N_NONE_VALUE);
        if (!none) {
            continue;
        }
        writ_handle_rec_t *handle = &model->handles.data[writ_type(model, writ_type_named(model, node->name))->decl];
        uint32_t expr = child(model, none, 0);
        handle->has_none = writ_expr_check_in(model, WRIT_EXPR_SCOPE_CONSTANT, WRIT_NONE, expr)
                        && writ_expr_eval(model, expr, nullptr, &handle->none_value)
                        && writ_type_is_integer(model, handle->base);
        if (!handle->has_none) {
            writ_report(model->session, WRIT_D_BAD_NONE_VALUE, node_at(model, none)->loc, nullptr);
        }
    }
}

static writ_status_t
derive(writ_unit_t *unit, const writ_rust_t **rust, writ_str_t *text)
{
    size_t first_diag = unit->session->diags.len;
    *rust = writ_rust_read(unit, nullptr);
    writ_diag_flush(unit->session, first_diag);
    if (!*rust) {
        return WRIT_HAS_ERRORS;
    }
    *text = writ_rust_derive(*rust);
    return WRIT_OK;
}

writ_status_t
writ_derive_transport(writ_unit_t *unit, const writ_output_t *output)
{
    if (!unit || !output || !writ_unit_is_rust(unit)) {
        return WRIT_INVALID_ARGUMENT;
    }
    const writ_rust_t *rust = nullptr;
    writ_str_t text = {0};
    writ_status_t status = derive(unit, &rust, &text);
    if (status != WRIT_OK) {
        return status;
    }
    writ_str_t library = writ_sym_str(unit->session, rust->library);
    writ_str_t path = writ_str_format(unit->session, "%.*s.derived.writ", (int)library.length, library.data);
    return output->write_file(output->context, path, text);
}

static writ_status_t
resolve_rust(writ_unit_t *unit, const writ_platform_t *platform, const writ_model_t **out)
{
    const writ_rust_t *rust = nullptr;
    writ_str_t text = {0};
    writ_status_t status = derive(unit, &rust, &text);
    if (status != WRIT_OK) {
        return status;
    }
    writ_unit_t *derived = writ_unit_new(unit->session);
    writ_str_t library = writ_sym_str(unit->session, rust->library);
    writ_origin_t origin = {.host_path = writ_str_format(unit->session, "%.*s.derived.writ", (int)library.length,
                                                         library.data)};
    status = writ_unit_add_source(derived, WRIT_SOURCE_CONTRACT_FILE, origin, text, nullptr);
    if (status == WRIT_OK) {
        status = writ_resolve(derived, platform, out);
    }
    if (status == WRIT_OK) {
        ((writ_model_t *)*out)->rust = rust;
    }
    return status;
}

writ_status_t
writ_resolve(writ_unit_t *unit, const writ_platform_t *platform, const writ_model_t **out)
{
    if (!unit || !platform || !out) {
        return WRIT_INVALID_ARGUMENT;
    }
    if (writ_unit_is_rust(unit)) {
        return resolve_rust(unit, platform, out);
    }
    size_t first_diag = unit->session->diags.len;
    resolver_t r = {.model = model_new(unit, platform), .decls = core_list_new(uint32_t)};
    core_dict_init(&r.aliases, nullptr, nullptr);
    core_dict_init(&r.alias_state, nullptr, nullptr);
    writ_model_t *model = r.model;
    size_t roots = unit->roots.len;
    for (size_t i = 0; i < roots; i++) {
        gather(&r, unit->roots.data[i].root);
    }
    for (size_t i = 0; i < r.decls.len; i++) {
        writ_node_kind_t kind = node_at(model, r.decls.data[i])->kind;
        if (kind == WRIT_N_CONST) {
            resolve_const(&r, r.decls.data[i]);
        } else if (kind == WRIT_N_ENUM || kind == WRIT_N_FLAGS) {
            resolve_enum(&r, r.decls.data[i]);
        }
    }
    core_dict_foreach(&r.aliases, entry) {
        resolve_alias(&r, (writ_sym_t)(uintptr_t)entry->key);
    }
    for (size_t i = 0; i < r.decls.len; i++) {
        resolve_decl(&r, r.decls.data[i]);
    }
    resolve_handle_nones(model, &r);
    core_list_foreach(model->refines, refine) {
        apply_refine(&r, *refine);
    }
    check_expressions(model);
    writ_type_layout_records(model);
    if (unit->session->diags.len == first_diag) {
        writ_model_check(model);
    }
    core_list_free(r.decls);
    core_dict_free(&r.aliases);
    core_dict_free(&r.alias_state);
    bool failed = unit->session->diags.len != first_diag;
    if (!failed) {
        writ_model_build_views(model);
    }
    writ_diag_flush(unit->session, first_diag);
    *out = failed ? nullptr : model;
    return failed ? WRIT_HAS_ERRORS : WRIT_OK;
}
