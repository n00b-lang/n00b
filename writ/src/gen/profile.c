#include "gen.h"

typedef struct {
    writ_profile_t *profile;
    writ_unit_t    *unit;
    uint32_t        seen[WRIT_N_COUNT + 1];
} reader_t;

static const char *const naming_kinds[WRIT_NAMING_COUNT] = {"fn", "type", "const", "field", "module"};
static const char *const cases[] = {"keep", "snake", "upper", "pascal", "camel"};

static bool
is(const writ_profile_t *profile, writ_sym_t sym, const char *text)
{
    writ_str_t s = writ_sym_str(profile->session, sym);
    return s.length == strlen(text) && !memcmp(s.data, text, s.length);
}

static const writ_node_t *
node(const reader_t *r, uint32_t id)
{
    return writ_node(r->unit, id);
}

static void
put_repr(writ_profile_t *profile, writ_repr_t repr)
{
    core_list_foreach(profile->reprs, existing) {
        if (existing->id == repr.id && existing->form == repr.form) {
            *existing = repr;
            return;
        }
    }
    core_list_push(profile->reprs, repr);
}

static void
read_repr(reader_t *r, uint32_t id, const writ_node_t *item)
{
    writ_profile_t *profile = r->profile;
    const writ_node_t *type = node(r, writ_node_child(r->unit, id, 0));
    writ_repr_t repr = {.id = item->name, .node = id};
    uint32_t mode = writ_node_first_of(r->unit, id, WRIT_N_COPY_MODE);
    repr.zero_copy = mode && is(profile, node(r, mode)->name, "zero_copy");
    if (item->kind == WRIT_N_PACK_NATIVE) {
        repr.form = is(profile, item->payload, "keywords") ? WRIT_REPR_KEYWORDS : WRIT_REPR_REST;
    } else if (type->kind == WRIT_N_TYPE_NAMED && is(profile, type->name, "cstr")) {
        repr.form = WRIT_REPR_CSTR;
    } else if (type->kind == WRIT_N_TYPE_BYTES) {
        repr.form = is(profile, type->payload, "utf8") ? WRIT_REPR_UTF8 : WRIT_REPR_BYTES;
    } else {
        writ_report(profile->session, WRIT_D_UNKNOWN_TYPE, type->loc,
                    "a representation converts to `cstr`, `utf8[n]` or `bytes[n]`");
        return;
    }
    put_repr(profile, repr);
}

static bool
single(reader_t *r, uint32_t id, const writ_node_t *item)
{
    uint32_t key = item->kind == WRIT_N_VARIADIC_ITEM && is(r->profile, item->payload, "define") ? WRIT_N_COUNT
                                                                                                   : item->kind;
    if (r->seen[key]) {
        uint32_t diag = writ_report(r->profile->session, WRIT_D_PROFILE_REPEAT, item->loc, nullptr);
        writ_report_label(r->profile->session, diag, node(r, r->seen[key])->loc, writ_cstr("first stated here"));
        return false;
    }
    r->seen[key] = id;
    return true;
}

static bool
yes(const reader_t *r, uint32_t id)
{
    return is(r->profile, node(r, writ_node_first_of(r->unit, id, WRIT_N_YES_NO))->name, "yes");
}

static void
read_item(reader_t *r, uint32_t id)
{
    writ_profile_t *profile = r->profile;
    const writ_node_t *item = node(r, id);
    switch (item->kind) {
    case WRIT_N_STRING_ITEM:
    case WRIT_N_BUFFER_ITEM:
    case WRIT_N_PACK_NATIVE:
        read_repr(r, id, item);
        return;
    case WRIT_N_NAMING: {
        writ_sym_t style = node(r, writ_node_first_of(r->unit, id, WRIT_N_REF))->name;
        for (uint32_t k = 0; k < WRIT_NAMING_COUNT; k++) {
            for (uint32_t c = 0; is(profile, item->name, naming_kinds[k]) && c < 5; c++) {
                if (is(profile, style, cases[c])) {
                    profile->naming[k] = (writ_case_t)c;
                    return;
                }
            }
        }
        writ_report(profile->session, WRIT_D_UNKNOWN_NAME, item->loc, "naming takes fn, type, const, field or module, "
                                                                      "and snake, pascal, camel, upper or keep");
        return;
    }
    case WRIT_N_RESERVED:
        for (uint32_t i = 0; i < item->child_count; i++) {
            core_list_push(profile->reserved, node(r, writ_node_child(r->unit, id, i))->name);
        }
        return;
    case WRIT_N_LIFETIMES:
        if (single(r, id, item)) {
            profile->lifetimes = yes(r, id);
        }
        return;
    case WRIT_N_VARIADIC_ITEM:
        if (single(r, id, item) && is(profile, item->payload, "call")) {
            profile->variadic_call = yes(r, id);
        }
        return;
    case WRIT_N_HANDLE_CLOSE:
        if (single(r, id, item)) {
            profile->handle_close = writ_sym_str(profile->session, node(r, writ_node_child(r->unit, id, 0))->name);
        }
        return;
    case WRIT_N_MEMORY:
    case WRIT_N_KEEP_ALIVE:
    case WRIT_N_ERROR_STYLE:
    case WRIT_N_ERROR_TYPE:
    case WRIT_N_CALLBACK_STYLE:
        single(r, id, item);
        return;
    default:
        return;
    }
}

static void
profile_release(void *object)
{
    writ_profile_t *profile = object;
    core_list_free(profile->reprs);
    core_list_free(profile->reserved);
}

const writ_profile_t *
writ_profile_read(writ_unit_t *unit, writ_sym_t language)
{
    writ_session_t *session = unit->session;
    writ_profile_t *profile = writ_alloc(session, sizeof(writ_profile_t));
    *profile = (writ_profile_t){.session = session, .name = language, .reprs = core_list_new(writ_repr_t),
                                .reserved = core_list_new(writ_sym_t), .variadic_call = true};
    writ_on_session_free(session, profile_release, profile);
    bool found = false;
    for (uint32_t block = 0; (block = writ_unit_next_item(unit, WRIT_N_PROFILE, block));) {
        if (writ_node(unit, block)->name != language) {
            continue;
        }
        found = true;
        reader_t r = {.profile = profile, .unit = unit};
        for (uint32_t k = 0; k < writ_node(unit, block)->child_count; k++) {
            read_item(&r, writ_node_child(unit, block, k));
        }
    }
    if (!found) {
        writ_str_t name = writ_sym_str(session, language);
        writ_report(session, WRIT_D_NO_PROFILE, WRIT_NO_LOC, "`%.*s`", S(name));
        return nullptr;
    }
    return profile;
}

const writ_repr_t *
writ_profile_repr(const writ_profile_t *profile, writ_sym_t id)
{
    for (size_t i = 0; i < profile->reprs.len; i++) {
        if (profile->reprs.data[i].id == id) {
            return &profile->reprs.data[i];
        }
    }
    return nullptr;
}

const writ_repr_t *
writ_profile_default(const writ_profile_t *profile, writ_repr_form_t form)
{
    for (size_t i = 0; i < profile->reprs.len; i++) {
        if (profile->reprs.data[i].form == form) {
            return &profile->reprs.data[i];
        }
    }
    return nullptr;
}

writ_str_t
writ_profile_name(const writ_profile_t *profile, writ_naming_kind_t kind, writ_str_t name)
{
    writ_str_t cased = writ_name_case(profile->session, name, profile->naming[kind]);
    writ_sym_t sym = writ_intern_str(profile->session, cased);
    for (size_t i = 0; i < profile->reserved.len; i++) {
        if (profile->reserved.data[i] == sym) {
            return writ_str_format(profile->session, "%.*s_", S(cased));
        }
    }
    return cased;
}
