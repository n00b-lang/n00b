#include "model.h"

#include "../syntax/syntax.h"

static const char *const rust_prims[] = {"u8", "u16", "u32", "u64", "usize", "i8", "i16", "i32", "i64", "isize",
                                         "f32", "f64", "bool"};

typedef struct {
    writ_rust_t           *rust;
    writ_unit_t           *unit;
    core_list_t(uint32_t) *skipped;
    writ_sym_t             self;
    bool                   failed;
} reader_t;

static const writ_node_t *
node(const reader_t *r, uint32_t id)
{
    return writ_node(r->unit, id);
}

static bool
is(const reader_t *r, writ_sym_t sym, const char *text)
{
    writ_str_t s = writ_sym_str(r->unit->session, sym);
    return s.length == strlen(text) && memcmp(s.data, text, s.length) == 0;
}

static void
reject(reader_t *r, uint32_t id, const char *what)
{
    writ_report(r->unit->session, WRIT_D_UNSUPPORTED, node(r, id)->loc, "%s", what);
    r->failed = true;
}

bool
writ_unit_is_rust(const writ_unit_t *unit)
{
    for (size_t i = 0; i < unit->roots.len; i++) {
        uint32_t root = unit->roots.data[i].root;
        for (uint32_t c = 0; c < writ_node(unit, root)->child_count; c++) {
            const writ_node_t *item = writ_node(unit, writ_node_child(unit, root, c));
            writ_str_t lang = writ_sym_str(unit->session, item->payload);
            if (item->kind == WRIT_N_LANG && lang.length == 4 && memcmp(lang.data, "rust", 4) == 0) {
                return true;
            }
        }
    }
    return false;
}

const writ_rust_type_t *
writ_rust_type(const writ_rust_t *rust, writ_sym_t name)
{
    for (size_t i = 0; i < rust->types.len; i++) {
        if (rust->types.data[i].name == name) {
            return &rust->types.data[i];
        }
    }
    return nullptr;
}

static writ_sym_t
generic_arg(const reader_t *r, uint32_t id, uint32_t index)
{
    uint32_t args = writ_node_first_of(r->unit, id, WRIT_N_GENERIC_ARGS);
    return args && index < node(r, args)->child_count ? writ_node_child(r->unit, args, index) : 0;
}

static bool
read_value(reader_t *r, uint32_t id, writ_rust_value_t *value)
{
    const writ_node_t *type = node(r, id);
    if (type->kind == WRIT_N_TYPE_REF) {
        uint32_t target = writ_node_child(r->unit, id, type->child_count - 1);
        const writ_node_t *inner = node(r, target);
        if (inner->kind == WRIT_N_TYPE_SLICE) {
            writ_rust_value_t element = {0};
            if (!read_value(r, writ_node_child(r->unit, target, 0), &element)
                || (element.kind != WRIT_RUST_PRIM && element.kind != WRIT_RUST_STRUCT) || element.optional) {
                return false;
            }
            *value = (writ_rust_value_t){.kind = WRIT_RUST_SLICE, .element = element.kind, .name = element.name};
            return true;
        }
        if (inner->kind == WRIT_N_TYPE_NAMED && is(r, inner->name, "str")) {
            *value = (writ_rust_value_t){.kind = WRIT_RUST_STR};
            return true;
        }
        if (!read_value(r, target, value) || value->kind != WRIT_RUST_OPAQUE || value->optional) {
            return false;
        }
        value->borrowed = true;
        return true;
    }
    if (type->kind == WRIT_N_TYPE_TUPLE && type->child_count == 0) {
        *value = (writ_rust_value_t){.kind = WRIT_RUST_UNIT};
        return true;
    }
    if (type->kind != WRIT_N_TYPE_NAMED) {
        return false;
    }
    writ_sym_t name = is(r, type->name, "Self") ? r->self : type->name;
    if (is(r, name, "Result")) {
        uint32_t error = generic_arg(r, id, 1);
        if (!error || node(r, error)->name != r->rust->error || !read_value(r, generic_arg(r, id, 0), value)
            || value->fallible) {
            return false;
        }
        value->fallible = true;
        return true;
    }
    if (is(r, name, "Option")) {
        uint32_t inner = generic_arg(r, id, 0);
        if (!inner || !read_value(r, inner, value) || value->optional || value->kind == WRIT_RUST_UNIT) {
            return false;
        }
        value->optional = true;
        return true;
    }
    if (is(r, name, "Vec")) {
        writ_rust_value_t element = {0};
        uint32_t inner = generic_arg(r, id, 0);
        if (!inner || !read_value(r, inner, &element)
            || (element.kind != WRIT_RUST_PRIM && element.kind != WRIT_RUST_STRUCT) || element.optional) {
            return false;
        }
        *value = (writ_rust_value_t){.kind = WRIT_RUST_VEC, .element = element.kind, .name = element.name};
        return true;
    }
    if (is(r, name, "String")) {
        *value = (writ_rust_value_t){.kind = WRIT_RUST_STRING};
        return true;
    }
    for (size_t i = 0; i < sizeof(rust_prims) / sizeof(rust_prims[0]); i++) {
        if (is(r, name, rust_prims[i])) {
            *value = (writ_rust_value_t){.kind = WRIT_RUST_PRIM, .name = name};
            return true;
        }
    }
    const writ_rust_type_t *declared = writ_rust_type(r->rust, name);
    if (!declared) {
        return false;
    }
    *value = (writ_rust_value_t){.kind = declared->kind, .name = name};
    return true;
}

static void
read_fn(reader_t *r, uint32_t id, writ_sym_t feature)
{
    writ_rust_t *rust = r->rust;
    writ_rust_fn_t fn = {.owner = r->self, .name = node(r, id)->name, .feature = feature,
                         .first = (uint32_t)rust->members.len};
    const char *problem = nullptr;
    uint32_t at = id;
    for (uint32_t i = 0; !problem && i < node(r, id)->child_count; i++) {
        at = writ_node_child(r->unit, id, i);
        const writ_node_t *p = node(r, at);
        if (p->kind == WRIT_N_SELF_PARAM) {
            fn.receiver = is(r, p->name, "self") ? WRIT_RUST_BY_VALUE
                        : is(r, p->name, "&self") ? WRIT_RUST_BY_REF : WRIT_RUST_BY_MUT;
        } else if (p->kind == WRIT_N_PARAM) {
            writ_rust_member_t member = {.name = p->name};
            if (!read_value(r, writ_node_child(r->unit, at, 0), &member.value) || member.value.optional
                || member.value.kind == WRIT_RUST_UNIT || member.value.kind == WRIT_RUST_STRING
                || member.value.kind == WRIT_RUST_VEC) {
                problem = "a parameter type no transport rule covers (dsl.md §8.9)";
            }
            core_list_push(rust->members, member);
            fn.count++;
        } else if (p->kind == WRIT_N_RESULT
                   && (!read_value(r, writ_node_child(r->unit, at, 0), &fn.result) || fn.result.borrowed
                       || fn.result.kind == WRIT_RUST_STR || fn.result.kind == WRIT_RUST_SLICE)) {
            problem = "a result type no transport rule covers (dsl.md §8.9)";
        } else if (p->kind != WRIT_N_RESULT) {
            problem = "clauses on Rust functions";
        }
    }
    if (!problem) {
        core_list_push(rust->fns, fn);
    } else if (r->skipped) {
        core_list_push(*r->skipped, id);
    } else {
        reject(r, at, problem);
    }
}

static void read_items(reader_t *r, uint32_t parent, uint32_t from, writ_sym_t feature);

static void
read_when(reader_t *r, uint32_t id)
{
    const writ_node_t *condition = node(r, writ_node_child(r->unit, id, 0));
    if (condition->kind != WRIT_N_EXPR_FEATURE || writ_node_first_of(r->unit, id, WRIT_N_ELSE)) {
        reject(r, id, "conditions in a Rust contract other than `when feature F`");
        return;
    }
    read_items(r, id, 1, condition->name);
}

static void
read_impl(reader_t *r, uint32_t id, writ_sym_t feature)
{
    r->self = node(r, id)->name;
    const writ_rust_type_t *owner = writ_rust_type(r->rust, r->self);
    if (!owner || owner->kind == WRIT_RUST_ENUM) {
        reject(r, id, "`impl` blocks for types other than declared structs and opaque types");
        return;
    }
    read_items(r, id, 0, feature);
    r->self = 0;
}

static void
read_type(reader_t *r, uint32_t id, writ_rust_kind_t kind)
{
    writ_rust_t *rust = r->rust;
    const writ_node_t *decl = node(r, id);
    writ_rust_type_t type = {.kind = kind, .name = decl->name, .first = (uint32_t)rust->members.len,
                             .rule = is(r, decl->payload, "shared")          ? WRIT_THREAD_RULE_SHARED
                                   : is(r, decl->payload, "thread_affine") ? WRIT_THREAD_RULE_AFFINE
                                                                            : WRIT_THREAD_RULE_SERIALIZED};
    for (uint32_t i = 0; i < decl->child_count; i++) {
        uint32_t part = writ_node_child(r->unit, id, i);
        writ_rust_member_t member = {.name = node(r, part)->name};
        if (node(r, part)->kind == WRIT_N_FIELD
            && (!read_value(r, writ_node_child(r->unit, part, 0), &member.value)
                || member.value.kind != WRIT_RUST_PRIM || member.value.optional)) {
            reject(r, part, "struct fields other than primitives in a Rust contract");
            return;
        }
        if (node(r, part)->kind == WRIT_N_ENUM_ITEM && node(r, part)->child_count) {
            reject(r, part, "explicit discriminants in a Rust contract");
            return;
        }
        core_list_push(rust->members, member);
        type.count++;
    }
    core_list_push(rust->types, type);
}

static void
read_items(reader_t *r, uint32_t parent, uint32_t from, writ_sym_t feature)
{
    writ_rust_t *rust = r->rust;
    for (uint32_t i = from; i < node(r, parent)->child_count; i++) {
        uint32_t id = writ_node_child(r->unit, parent, i);
        const writ_node_t *item = node(r, id);
        switch (item->kind) {
        case WRIT_N_LIBRARY:
            rust->library = item->name;
            break;
        case WRIT_N_LANG:
            break;
        case WRIT_N_CRATE:
            rust->crate = item->name;
            rust->version = node(r, writ_node_child(r->unit, id, 0))->name;
            break;
        case WRIT_N_FEATURE:
            if (!is(r, item->payload, "cargo")) {
                reject(r, id, "features in a Rust contract are cargo features");
                break;
            }
            core_list_push(rust->features,
                           ((writ_rust_feature_t){item->name, node(r, writ_node_child(r->unit, id, 0))->name}));
            break;
        case WRIT_N_RUST_ERROR:
        case WRIT_N_ENUM:
        case WRIT_N_STRUCT:
        case WRIT_N_OPAQUE:
            if (node(r, parent)->kind != WRIT_N_FILE) {
                reject(r, id, "type declarations inside `impl` or `when` in a Rust contract");
            }
            break;
        case WRIT_N_IMPL:
            read_impl(r, id, feature);
            break;
        case WRIT_N_WHEN:
            read_when(r, id);
            break;
        case WRIT_N_FN:
            if (!r->self) {
                reject(r, id, "free functions in a Rust contract; declare them in an `impl` block");
                break;
            }
            read_fn(r, id, feature);
            break;
        default:
            reject(r, id, "this declaration in a Rust contract (dsl.md §8.9)");
            break;
        }
    }
}

static void
read_declaration(reader_t *r, uint32_t id)
{
    const writ_node_t *item = node(r, id);
    if (item->kind == WRIT_N_RUST_ERROR) {
        writ_str_t path = writ_sym_str(r->rust->session, item->payload);
        size_t start = path.length;
        while (start > 0 && path.data[start - 1] != ':') {
            start--;
        }
        r->rust->error = writ_intern(r->rust->session, path.data + start, path.length - start);
    } else if (item->kind == WRIT_N_ENUM || item->kind == WRIT_N_STRUCT || item->kind == WRIT_N_OPAQUE) {
        read_type(r, id, item->kind == WRIT_N_ENUM     ? WRIT_RUST_ENUM
                         : item->kind == WRIT_N_STRUCT ? WRIT_RUST_STRUCT
                                                       : WRIT_RUST_OPAQUE);
    }
}

static void
rust_release(void *object)
{
    writ_rust_t *rust = object;
    core_list_free(rust->features);
    core_list_free(rust->types);
    core_list_free(rust->members);
    core_list_free(rust->fns);
}

writ_rust_t *
writ_rust_read(writ_unit_t *unit, core_list_t(uint32_t) *skipped)
{
    writ_session_t *session = unit->session;
    writ_rust_t *rust = writ_alloc(session, sizeof(writ_rust_t));
    *rust = (writ_rust_t){.session = session,
                          .features = core_list_new(writ_rust_feature_t),
                          .types = core_list_new(writ_rust_type_t),
                          .members = core_list_new(writ_rust_member_t),
                          .fns = core_list_new(writ_rust_fn_t)};
    writ_on_session_free(session, rust_release, rust);
    reader_t r = {.rust = rust, .unit = unit, .skipped = skipped};
    for (size_t i = 0; i < unit->roots.len; i++) {
        uint32_t root = unit->roots.data[i].root;
        for (uint32_t c = 0; c < writ_node(unit, root)->child_count; c++) {
            read_declaration(&r, writ_node_child(unit, root, c));
        }
    }
    for (size_t i = 0; i < unit->roots.len; i++) {
        read_items(&r, unit->roots.data[i].root, 0, 0);
    }
    if (!rust->crate) {
        rust->crate = rust->library;
    }
    writ_str_t crate = writ_name_case(session, writ_sym_str(session, rust->crate), WRIT_CASE_SNAKE);
    rust->prefix = writ_str_format(session, "writ_%.*s", (int)crate.length, crate.data);
    core_list_foreach(rust->fns, fn) {
        writ_str_t owner = writ_name_case(session, writ_sym_str(session, fn->owner), WRIT_CASE_SNAKE);
        writ_str_t name = writ_sym_str(session, fn->name);
        fn->symbol = writ_intern_str(session, writ_str_format(session, "%.*s_%.*s_%.*s", (int)rust->prefix.length,
                                                              rust->prefix.data, (int)owner.length, owner.data,
                                                              (int)name.length, name.data));
    }
    return r.failed ? nullptr : rust;
}

typedef struct {
    const writ_rust_t *rust;
    writ_emit_t        emit;
} deriver_t;

static writ_str_t
text_of(const deriver_t *d, writ_sym_t sym)
{
    return writ_sym_str(d->rust->session, sym);
}

static writ_str_t
c_name(const deriver_t *d, writ_sym_t type)
{
    writ_str_t snake = writ_name_case(d->rust->session, text_of(d, type), WRIT_CASE_SNAKE);
    return writ_str_format(d->rust->session, "%.*s_%.*s", (int)d->rust->prefix.length, d->rust->prefix.data,
                           (int)snake.length, snake.data);
}

static void
emit_element(deriver_t *d, const writ_rust_value_t *value)
{
    writ_str_t name = text_of(d, value->name);
    writ_emit_text(&d->emit, "%.*s", (int)name.length, name.data);
}

static void
emit_param(deriver_t *d, writ_str_t name, const writ_rust_value_t *value)
{
    writ_emit_t *e = &d->emit;
    writ_emit_text(e, "%.*s: ", (int)name.length, name.data);
    switch (value->kind) {
    case WRIT_RUST_STR:
        writ_emit_line(e, "utf8[%.*s_len] null if empty,", (int)name.length, name.data);
        writ_emit_line(e, "%.*s_len: usize,", (int)name.length, name.data);
        return;
    case WRIT_RUST_SLICE: {
        writ_str_t element = text_of(d, value->name);
        if (element.length == 2 && memcmp(element.data, "u8", 2) == 0) {
            writ_emit_line(e, "bytes[%.*s_len] null if empty,", (int)name.length, name.data);
        } else {
            writ_emit_text(e, "*[");
            emit_element(d, value);
            writ_emit_line(e, "; %.*s_len] null if empty,", (int)name.length, name.data);
        }
        writ_emit_line(e, "%.*s_len: usize,", (int)name.length, name.data);
        return;
    }
    case WRIT_RUST_OPAQUE:
        writ_emit_text(e, value->borrowed ? "" : "consume ");
        break;
    default:
        break;
    }
    emit_element(d, value);
    writ_emit_line(e, ",");
}

static void
emit_result_params(deriver_t *d, const writ_rust_value_t *value)
{
    writ_emit_t *e = &d->emit;
    switch (value->kind) {
    case WRIT_RUST_UNIT:
        return;
    case WRIT_RUST_STRING:
        writ_emit_line(e, "out: out *[u8; cap out_cap, len min(*out_total, out_cap)],");
        writ_emit_line(e, "out_cap: usize,");
        writ_emit_line(e, "out_total: out *usize,");
        return;
    case WRIT_RUST_VEC:
        writ_emit_text(e, "out: out *[");
        emit_element(d, value);
        writ_emit_line(e, "; cap out_cap, len min(*out_total, out_cap)],");
        writ_emit_line(e, "out_cap: usize,");
        writ_emit_line(e, "out_total: out *usize,");
        return;
    default:
        writ_emit_text(e, "out: out *");
        emit_element(d, value);
        writ_emit_line(e, value->kind == WRIT_RUST_OPAQUE ? " own," : ",");
        return;
    }
}

static void
emit_fn(deriver_t *d, const writ_rust_fn_t *fn)
{
    writ_emit_t *e = &d->emit;
    const writ_rust_t *rust = d->rust;
    writ_str_t symbol = text_of(d, fn->symbol);
    size_t skip = rust->prefix.length + 1;
    writ_emit_line(e, "fn %.*s(", (int)(symbol.length - skip), symbol.data + skip);
    writ_emit_indent(e, 1);
    if (fn->receiver != WRIT_RUST_FREE) {
        writ_rust_value_t self = {.kind = WRIT_RUST_OPAQUE, .name = fn->owner,
                                  .borrowed = fn->receiver != WRIT_RUST_BY_VALUE};
        emit_param(d, writ_cstr("self"), &self);
    }
    for (uint32_t i = 0; i < fn->count; i++) {
        const writ_rust_member_t *param = &rust->members.data[fn->first + i];
        emit_param(d, text_of(d, param->name), &param->value);
    }
    emit_result_params(d, &fn->result);
    writ_emit_indent(e, -1);
    writ_emit_line(e, ") -> c_int as Status");
    writ_emit_indent(e, 1);
    writ_emit_line(e, "outcome ok success when ret == Status_Ok");
    if (fn->result.optional) {
        writ_emit_line(e, "outcome none success when ret == Status_None");
    }
    writ_emit_line(e, "outcome failed failure when ret == Status_Error via last_error");
    writ_emit_line(e, "outcome panicked failure via last_error");
    if (fn->result.kind != WRIT_RUST_UNIT) {
        writ_emit_line(e, "on error out: unset");
    }
    if (fn->result.optional) {
        writ_emit_line(e, "on none out: unset");
    }
    if (fn->result.kind == WRIT_RUST_VEC || fn->result.kind == WRIT_RUST_STRING) {
        writ_emit_line(e, "sizing retry for out needs *out_total");
    }
    writ_emit_line(e, "symbol \"%.*s\"", (int)symbol.length, symbol.data);
    writ_emit_indent(e, -1);
}

static void
emit_type(deriver_t *d, const writ_rust_type_t *type)
{
    writ_emit_t *e = &d->emit;
    const writ_rust_t *rust = d->rust;
    writ_str_t name = text_of(d, type->name);
    if (type->kind == WRIT_RUST_OPAQUE) {
        writ_str_t c = c_name(d, type->name);
        writ_str_t snake = writ_name_case(rust->session, name, WRIT_CASE_SNAKE);
        static const char *const rules[] = {"", " shared", " thread_affine"};
        writ_emit_line(e, "opaque %.*s", (int)c.length, c.data);
        writ_emit_line(e, "handle %.*s = *%.*s release %.*s_free%s", (int)name.length, name.data, (int)c.length,
                       c.data, (int)snake.length, snake.data, rules[type->rule]);
        writ_emit_line(e, "fn %.*s_free(value: consume ?%.*s) symbol \"%.*s_free\"", (int)snake.length, snake.data,
                       (int)c.length, c.data, (int)c.length, c.data);
        writ_emit_line(e, "");
        return;
    }
    writ_emit_open(e, type->kind == WRIT_RUST_ENUM ? "enum %.*s : c_int" : "struct %.*s", (int)name.length,
                   name.data);
    for (uint32_t i = 0; i < type->count; i++) {
        const writ_rust_member_t *member = &rust->members.data[type->first + i];
        writ_str_t member_name = text_of(d, member->name);
        if (type->kind == WRIT_RUST_ENUM) {
            writ_emit_line(e, "%.*s_%.*s = %u,", (int)name.length, name.data, (int)member_name.length,
                           member_name.data, i);
        } else {
            writ_str_t field = text_of(d, member->value.name);
            writ_emit_line(e, "%.*s: %.*s,", (int)member_name.length, member_name.data, (int)field.length,
                           field.data);
        }
    }
    writ_emit_close(e, type->kind == WRIT_RUST_ENUM ? " closed" : nullptr);
    writ_emit_line(e, "");
}

writ_str_t
writ_rust_derive(const writ_rust_t *rust)
{
    writ_session_t *session = rust->session;
    deriver_t d = {.rust = rust};
    writ_emit_init(&d.emit);
    writ_emit_t *e = &d.emit;
    writ_str_t library = text_of(&d, rust->library);
    writ_str_t crate = writ_name_case(session, text_of(&d, rust->crate), WRIT_CASE_SNAKE);
    writ_emit_line(e, "library %.*s", (int)library.length, library.data);
    writ_emit_line(e, "link \"%.*s_writ\"", (int)crate.length, crate.data);
    for (size_t i = 0; i < rust->features.len; i++) {
        writ_str_t name = text_of(&d, rust->features.data[i].name);
        writ_str_t cargo = text_of(&d, rust->features.data[i].cargo);
        writ_emit_line(e, "feature %.*s = cargo \"%.*s\"", (int)name.length, name.data, (int)cargo.length,
                       cargo.data);
    }
    writ_emit_line(e, "");
    writ_emit_open(e, "enum Status : c_int");
    static const char *const statuses[] = {"Ok", "None", "Error", "Panic"};
    for (uint32_t i = 0; i < 4; i++) {
        writ_emit_line(e, "Status_%s = %u,", statuses[i], i);
    }
    writ_emit_close(e, " closed");
    writ_emit_line(e, "");
    writ_emit_line(e, "fn error_message(buf: out *[u8; cap cap, len min(ret, cap)], cap: usize) -> usize");
    writ_emit_line(e, "    sizing retry needs ret");
    writ_emit_line(e, "    symbol \"%.*s_last_error\"", (int)rust->prefix.length, rust->prefix.data);
    writ_emit_line(e, "");
    writ_emit_open(e, "error_channel last_error per thread");
    writ_emit_line(e, "text error_message(buf, cap)");
    writ_emit_line(e, "valid until next failure on thread");
    writ_emit_close(e, nullptr);
    writ_emit_line(e, "");
    for (size_t i = 0; i < rust->types.len; i++) {
        emit_type(&d, &rust->types.data[i]);
    }
    writ_sym_t open_feature = 0;
    for (size_t i = 0; i < rust->fns.len; i++) {
        const writ_rust_fn_t *fn = &rust->fns.data[i];
        if (fn->feature != open_feature) {
            if (open_feature) {
                writ_emit_close(e, nullptr);
                writ_emit_line(e, "");
            }
            if (fn->feature) {
                writ_str_t name = text_of(&d, fn->feature);
                writ_emit_open(e, "when feature %.*s", (int)name.length, name.data);
            }
            open_feature = fn->feature;
        } else if (i > 0) {
            writ_emit_line(e, "");
        }
        emit_fn(&d, fn);
    }
    if (open_feature) {
        writ_emit_close(e, nullptr);
    }
    return writ_emit_finish(e, session);
}
