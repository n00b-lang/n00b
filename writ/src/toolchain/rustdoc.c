#include "../core/syntax/syntax.h"
#include "toolchain.h"

typedef struct {
    writ_session_t                    *session;
    const writ_rust_import_request_t  *request;
    writ_unit_t                       *json;
    uint32_t                           index;
    writ_map_t                        *items;
    writ_str_t                         crate;
    writ_str_t                         error;
    writ_map_t                        *skipped;
} importer_t;

static const writ_node_t *
at(const importer_t *im, uint32_t id)
{
    return writ_node(im->json, id);
}

static uint32_t
get(const importer_t *im, uint32_t object, const char *key)
{
    if (!object || at(im, object)->kind != WRIT_N_JSON_OBJECT) {
        return 0;
    }
    for (uint32_t i = 0; i < at(im, object)->child_count; i++) {
        uint32_t member = writ_node_child(im->json, object, i);
        writ_str_t name = writ_sym_str(im->session, at(im, member)->name);
        if (name.length == strlen(key) && !memcmp(name.data, key, name.length)) {
            return writ_node_child(im->json, member, 0);
        }
    }
    return 0;
}

static writ_str_t
text(const importer_t *im, uint32_t value)
{
    return value && at(im, value)->kind == WRIT_N_JSON_STRING ? writ_sym_str(im->session, at(im, value)->name)
                                                               : (writ_str_t){"", 0};
}

static bool
text_is(const importer_t *im, uint32_t value, const char *expected)
{
    writ_str_t s = text(im, value);
    return s.length == strlen(expected) && !memcmp(s.data, expected, s.length);
}

static uint64_t
number(const importer_t *im, uint32_t value)
{
    writ_str_t s = value ? writ_sym_str(im->session, at(im, value)->name) : (writ_str_t){"0", 1};
    return strtoull(writ_str_copy(im->session, s.data, s.length).data, nullptr, 10);
}

static uint32_t
item(const importer_t *im, uint32_t id_value)
{
    uint64_t node = 0;
    return writ_map_get(im->items, number(im, id_value), &node) ? (uint32_t)node : 0;
}

static uint32_t
inner(const importer_t *im, uint32_t entry, const char *kind)
{
    return get(im, get(im, entry, "inner"), kind);
}

static uint32_t
first_member(const importer_t *im, uint32_t object, writ_str_t *key)
{
    if (!object || !at(im, object)->child_count) {
        return 0;
    }
    uint32_t member = writ_node_child(im->json, object, 0);
    *key = writ_sym_str(im->session, at(im, member)->name);
    return writ_node_child(im->json, member, 0);
}

static bool
print_type(importer_t *im, uint32_t type, writ_str_t self, core_buffer_t *out);

static bool
print_args(importer_t *im, uint32_t args, writ_str_t self, core_buffer_t *out)
{
    uint32_t list = get(im, get(im, args, "angle_bracketed"), "args");
    if (!args || at(im, args)->kind != WRIT_N_JSON_OBJECT || !list || !at(im, list)->child_count) {
        return !args || at(im, args)->kind != WRIT_N_JSON_OBJECT || list;
    }
    core_buffer_putc(out, '<');
    for (uint32_t i = 0; i < at(im, list)->child_count; i++) {
        uint32_t type = get(im, writ_node_child(im->json, list, i), "type");
        core_buffer_puts(out, i ? ", " : "");
        if (!type || !print_type(im, type, self, out)) {
            return false;
        }
    }
    core_buffer_putc(out, '>');
    return true;
}

static bool
print_type(importer_t *im, uint32_t type, writ_str_t self, core_buffer_t *out)
{
    writ_str_t kind = {0};
    uint32_t value = first_member(im, type, &kind);
    if (!value) {
        return false;
    }
    if (writ_str_eq(kind, writ_cstr("primitive"))) {
        writ_str_t name = text(im, value);
        core_buffer_printf(out, "%.*s", S(name));
        return true;
    }
    if (writ_str_eq(kind, writ_cstr("generic"))) {
        writ_str_t name = text_is(im, value, "Self") ? self : text(im, value);
        core_buffer_printf(out, "%.*s", S(name));
        return true;
    }
    if (writ_str_eq(kind, writ_cstr("borrowed_ref"))) {
        core_buffer_puts(out, at(im, get(im, value, "is_mutable"))->name == writ_intern(im->session, "true", 4)
                                  ? "&mut "
                                  : "&");
        return print_type(im, get(im, value, "type"), self, out);
    }
    if (writ_str_eq(kind, writ_cstr("slice"))) {
        core_buffer_putc(out, '[');
        bool ok = print_type(im, value, self, out);
        core_buffer_putc(out, ']');
        return ok;
    }
    if (writ_str_eq(kind, writ_cstr("tuple")) && !at(im, value)->child_count) {
        core_buffer_puts(out, "()");
        return true;
    }
    if (writ_str_eq(kind, writ_cstr("resolved_path"))) {
        writ_str_t path = text(im, get(im, value, "path"));
        size_t start = path.length;
        while (start > 0 && path.data[start - 1] != ':') {
            start--;
        }
        core_buffer_printf(out, "%.*s", (int)(path.length - start), path.data + start);
        return print_args(im, get(im, value, "args"), self, out);
    }
    return false;
}

static writ_str_t
feature_of(const importer_t *im, uint32_t entry)
{
    uint32_t attrs = get(im, entry, "attrs");
    for (uint32_t i = 0; attrs && i < at(im, attrs)->child_count; i++) {
        writ_str_t attr = text(im, get(im, writ_node_child(im->json, attrs, i), "other"));
        const char *marker = "name: \"feature\", value: Some(\"";
        for (size_t p = 0; attr.length >= strlen(marker) && p + strlen(marker) <= attr.length; p++) {
            if (!memcmp(attr.data + p, marker, strlen(marker))) {
                size_t start = p + strlen(marker);
                size_t end = start;
                while (end < attr.length && attr.data[end] != '"') {
                    end++;
                }
                return (writ_str_t){attr.data + start, end - start};
            }
        }
    }
    return (writ_str_t){"", 0};
}

static bool
skipped(const importer_t *im, writ_str_t owner, writ_str_t name)
{
    uint64_t found = 0;
    writ_sym_t key = writ_intern_str(im->session, writ_str_format(im->session, "%.*s::%.*s", S(owner), S(name)));
    return writ_map_get(im->skipped, key, &found);
}

static void
emit_fn(importer_t *im, writ_emit_t *e, uint32_t entry, writ_str_t owner)
{
    writ_str_t name = text(im, get(im, entry, "name"));
    uint32_t sig = get(im, inner(im, entry, "function"), "sig");
    if (!sig || skipped(im, owner, name)) {
        return;
    }
    core_buffer_t *line = core_buffer_empty();
    core_buffer_printf(line, "fn %.*s(", S(name));
    uint32_t inputs = get(im, sig, "inputs");
    bool ok = true;
    for (uint32_t i = 0; ok && i < at(im, inputs)->child_count; i++) {
        uint32_t pair = writ_node_child(im->json, inputs, i);
        writ_str_t param = text(im, writ_node_child(im->json, pair, 0));
        uint32_t type = writ_node_child(im->json, pair, 1);
        core_buffer_puts(line, i ? ", " : "");
        if (writ_str_eq(param, writ_cstr("self"))) {
            core_buffer_t *spelled = core_buffer_empty();
            ok = print_type(im, type, writ_cstr("Self"), spelled);
            writ_str_t receiver = {spelled->data, spelled->byte_len};
            core_buffer_puts(line, writ_str_eq(receiver, writ_cstr("&Self"))      ? "&self"
                                   : writ_str_eq(receiver, writ_cstr("&mut Self")) ? "&mut self"
                                                                                  : "self");
            ok = ok && (writ_str_eq(receiver, writ_cstr("&Self")) || writ_str_eq(receiver, writ_cstr("&mut Self"))
                        || writ_str_eq(receiver, writ_cstr("Self")));
            core_buffer_free(spelled);
        } else {
            core_buffer_printf(line, "%.*s: ", S(param));
            ok = print_type(im, type, owner, line);
        }
    }
    core_buffer_putc(line, ')');
    uint32_t output = get(im, sig, "output");
    if (ok && output && at(im, output)->kind == WRIT_N_JSON_OBJECT) {
        core_buffer_puts(line, " -> ");
        ok = print_type(im, output, owner, line);
    }
    if (ok) {
        writ_emit_line(e, "%.*s", (int)line->byte_len, line->data);
    } else {
        writ_sym_t key = writ_intern_str(im->session, writ_str_format(im->session, "%.*s::%.*s", S(owner), S(name)));
        writ_map_put(im->skipped, key, 1);
        writ_report(im->session, WRIT_D_NOT_TRANSPORTED, WRIT_NO_LOC, "`%.*s::%.*s`", S(owner), S(name));
    }
    core_buffer_free(line);
}

static bool
implements(const importer_t *im, uint32_t impls, const char *trait, bool *negative)
{
    for (uint32_t i = 0; impls && i < at(im, impls)->child_count; i++) {
        uint32_t impl = inner(im, item(im, writ_node_child(im->json, impls, i)), "impl");
        uint32_t path = get(im, get(im, impl, "trait"), "path");
        if (impl && text_is(im, path, trait) && at(im, get(im, impl, "blanket_impl"))->kind == WRIT_N_JSON_WORD) {
            *negative = at(im, get(im, impl, "is_negative"))->name == writ_intern(im->session, "true", 4);
            return true;
        }
    }
    return false;
}

static writ_str_t
impl_fns(importer_t *im, uint32_t impls, writ_str_t owner, writ_str_t feature)
{
    writ_emit_t fns;
    writ_emit_init(&fns);
    bool negative = false;
    if (!feature.length && implements(im, impls, "Default", &negative)) {
        writ_emit_line(&fns, "fn default() -> %.*s", S(owner));
    }
    for (uint32_t i = 0; i < at(im, impls)->child_count; i++) {
        uint32_t impl = inner(im, item(im, writ_node_child(im->json, impls, i)), "impl");
        if (!impl || at(im, get(im, impl, "trait"))->kind != WRIT_N_JSON_WORD) {
            continue;
        }
        uint32_t members = get(im, impl, "items");
        for (uint32_t m = 0; m < at(im, members)->child_count; m++) {
            uint32_t entry = item(im, writ_node_child(im->json, members, m));
            if (text_is(im, get(im, entry, "visibility"), "public") && inner(im, entry, "function")
                && writ_str_eq(feature_of(im, entry), feature)) {
                emit_fn(im, &fns, entry, owner);
            }
        }
    }
    return writ_emit_finish(&fns, im->session);
}

static void
emit_lines(writ_emit_t *e, writ_str_t lines)
{
    size_t start = 0;
    for (size_t i = 0; i < lines.length; i++) {
        if (lines.data[i] == '\n') {
            writ_emit_line(e, "%.*s", (int)(i - start), lines.data + start);
            start = i + 1;
        }
    }
}

static void
emit_impls(importer_t *im, writ_emit_t *e, uint32_t impls, writ_str_t owner)
{
    writ_str_t ungated = impl_fns(im, impls, owner, (writ_str_t){"", 0});
    size_t features = im->request->features_count;
    writ_str_t *gated = writ_alloc(im->session, sizeof(writ_str_t) * (features + 1));
    bool any = ungated.length;
    for (size_t f = 0; f < features; f++) {
        gated[f] = impl_fns(im, impls, owner, im->request->features[f]);
        any = any || gated[f].length;
    }
    if (!any) {
        return;
    }
    writ_emit_line(e, "");
    writ_emit_open(e, "impl %.*s", S(owner));
    emit_lines(e, ungated);
    for (size_t f = 0; f < features; f++) {
        if (gated[f].length) {
            writ_emit_open(e, "when feature %.*s", S(im->request->features[f]));
            emit_lines(e, gated[f]);
            writ_emit_close(e, nullptr);
        }
    }
    writ_emit_close(e, nullptr);
}

static bool
fieldless(const importer_t *im, uint32_t variants)
{
    for (uint32_t i = 0; i < at(im, variants)->child_count; i++) {
        uint32_t variant = inner(im, item(im, writ_node_child(im->json, variants, i)), "variant");
        if (!text_is(im, get(im, variant, "kind"), "plain")
            || at(im, get(im, variant, "discriminant"))->kind != WRIT_N_JSON_WORD) {
            return false;
        }
    }
    return true;
}

static bool
repr_c(const importer_t *im, uint32_t entry)
{
    uint32_t attrs = get(im, entry, "attrs");
    for (uint32_t i = 0; attrs && i < at(im, attrs)->child_count; i++) {
        if (text_is(im, get(im, get(im, writ_node_child(im->json, attrs, i), "repr"), "kind"), "c")) {
            return true;
        }
    }
    return false;
}

static bool
emit_record(importer_t *im, writ_emit_t *e, uint32_t entry, writ_str_t name)
{
    uint32_t plain = get(im, get(im, inner(im, entry, "struct"), "kind"), "plain");
    uint32_t fields = get(im, plain, "fields");
    if (!repr_c(im, entry) || !fields || at(im, get(im, plain, "has_stripped_fields"))->name
                                              == writ_intern(im->session, "true", 4)) {
        return false;
    }
    core_buffer_t *body = core_buffer_empty();
    bool ok = true;
    for (uint32_t i = 0; ok && i < at(im, fields)->child_count; i++) {
        uint32_t field = item(im, writ_node_child(im->json, fields, i));
        writ_str_t kind = {0};
        uint32_t primitive = first_member(im, inner(im, field, "struct_field"), &kind);
        ok = writ_str_eq(kind, writ_cstr("primitive"));
        writ_str_t field_name = text(im, get(im, field, "name"));
        writ_str_t type = text(im, primitive);
        core_buffer_printf(body, "%s%.*s: %.*s", i ? ", " : "", S(field_name), S(type));
    }
    if (ok) {
        writ_emit_line(e, "struct %.*s { %.*s }", S(name), (int)body->byte_len, body->data);
    }
    core_buffer_free(body);
    return ok;
}

static void
emit_type(importer_t *im, writ_emit_t *e, uint32_t entry)
{
    writ_str_t name = text(im, get(im, entry, "name"));
    uint32_t enumeration = inner(im, entry, "enum");
    uint32_t structure = inner(im, entry, "struct");
    uint32_t impls = get(im, enumeration ? enumeration : structure, "impls");
    bool negative = false;
    if (enumeration && implements(im, impls, "Display", &negative) && implements(im, impls, "Error", &negative)) {
        im->error = name;
        writ_emit_line(e, "error %.*s::%.*s display", S(im->crate), S(name));
        return;
    }
    if (enumeration && fieldless(im, get(im, enumeration, "variants"))) {
        uint32_t variants = get(im, enumeration, "variants");
        writ_emit_text(e, "enum %.*s {", S(name));
        for (uint32_t i = 0; i < at(im, variants)->child_count; i++) {
            writ_str_t variant = text(im, get(im, item(im, writ_node_child(im->json, variants, i)), "name"));
            writ_emit_text(e, "%s %.*s", i ? "," : "", S(variant));
        }
        writ_emit_line(e, " }");
        return;
    }
    if (structure && emit_record(im, e, entry, name)) {
        return;
    }
    bool send = implements(im, impls, "Send", &negative) && !negative;
    bool sync = implements(im, impls, "Sync", &negative) && !negative;
    writ_emit_line(e, "opaque %.*s%s", S(name), send && sync ? " shared" : send ? "" : " thread_affine");
}

static bool
crosses(importer_t *im, uint32_t entry, bool report)
{
    uint32_t declared = inner(im, entry, "enum") ? inner(im, entry, "enum") : inner(im, entry, "struct");
    if (!declared || !text_is(im, get(im, entry, "visibility"), "public")) {
        return false;
    }
    uint32_t params = get(im, get(im, declared, "generics"), "params");
    if (params && at(im, params)->child_count) {
        writ_str_t name = text(im, get(im, entry, "name"));
        uint64_t seen = 0;
        if (report && !writ_map_get(im->skipped, writ_intern_str(im->session, name), &seen)) {
            writ_map_put(im->skipped, writ_intern_str(im->session, name), 1);
            writ_report(im->session, WRIT_D_NOT_TRANSPORTED, WRIT_NO_LOC, "`%.*s` has generic parameters",
                        S(name));
        }
        return false;
    }
    return true;
}

static writ_str_t
render(importer_t *im)
{
    writ_session_t *session = im->session;
    const writ_rust_import_request_t *request = im->request;
    uint32_t members = get(im, inner(im, item(im, get(im, im->index, "root")), "module"), "items");
    writ_emit_t e;
    writ_emit_init(&e);
    writ_emit_line(&e, "library %.*s", S(request->library));
    writ_emit_line(&e, "lang rust");
    writ_str_t version = text(im, get(im, im->index, "crate_version"));
    writ_emit_line(&e, "crate %.*s \"%.*s\"", S(im->crate), S(version));
    for (size_t i = 0; i < request->features_count; i++) {
        writ_emit_line(&e, "feature %.*s = cargo \"%.*s\"", S(request->features[i]), S(request->features[i]));
    }
    writ_emit_line(&e, "");
    for (uint32_t i = 0; i < at(im, members)->child_count; i++) {
        uint32_t entry = item(im, writ_node_child(im->json, members, i));
        if (crosses(im, entry, true)) {
            emit_type(im, &e, entry);
        }
    }
    for (uint32_t i = 0; i < at(im, members)->child_count; i++) {
        uint32_t entry = item(im, writ_node_child(im->json, members, i));
        if (crosses(im, entry, false) && inner(im, entry, "struct")) {
            emit_impls(im, &e, get(im, inner(im, entry, "struct"), "impls"), text(im, get(im, entry, "name")));
        }
    }
    return writ_emit_finish(&e, session);
}

static void
note_skipped(importer_t *im, writ_str_t contract)
{
    writ_unit_t *probe = writ_unit_new(im->session);
    writ_origin_t origin = {.host_path = writ_cstr("rustdoc.writ")};
    size_t first_diag = im->session->diags.len;
    if (writ_unit_add_source(probe, WRIT_SOURCE_CONTRACT_FILE, origin, contract, nullptr) != WRIT_OK) {
        return;
    }
    core_list_t(uint32_t) skipped = core_list_new(uint32_t);
    writ_rust_read(probe, &skipped);
    im->session->diags.len = first_diag;
    core_list_foreach(skipped, fn) {
        uint32_t impl = 0;
        for (uint32_t n = *fn + 1; n < probe->nodes.len && !impl; n++) {
            const writ_node_t *candidate = writ_node(probe, n);
            if (candidate->kind == WRIT_N_IMPL && candidate->loc.start <= writ_node(probe, *fn)->loc.start
                && candidate->loc.end >= writ_node(probe, *fn)->loc.end) {
                impl = n;
            }
        }
        writ_str_t owner = writ_sym_str(im->session, impl ? writ_node(probe, impl)->name : 0);
        writ_str_t name = writ_sym_str(im->session, writ_node(probe, *fn)->name);
        writ_sym_t key = writ_intern_str(im->session, writ_str_format(im->session, "%.*s::%.*s", S(owner), S(name)));
        writ_map_put(im->skipped, key, 1);
        writ_report(im->session, WRIT_D_NOT_TRANSPORTED, WRIT_NO_LOC, "`%.*s::%.*s`", S(owner), S(name));
    }
    core_list_free(skipped);
}

static writ_status_t
run_rustdoc(writ_session_t *session, const writ_rust_import_request_t *request, writ_str_t *json_path)
{
    const char *argv[64] = {"env", "RUSTC_BOOTSTRAP=1", "cargo", "rustdoc", "-q", "--manifest-path",
                            writ_str_copy(session, request->manifest.data, request->manifest.length).data, "-p",
                            writ_str_copy(session, request->package.data, request->package.length).data};
    size_t count = 9;
    for (size_t i = 0; i < request->features_count && count < 56; i++) {
        argv[count++] = "--features";
        argv[count++] = writ_str_copy(session, request->features[i].data, request->features[i].length).data;
    }
    const char *tail[] = {"--", "-Z", "unstable-options", "--output-format", "json"};
    for (size_t i = 0; i < 5; i++) {
        argv[count++] = tail[i];
    }
    writ_run_result_t result;
    if (writ_run(session, argv, (writ_str_t){0}, &result) != WRIT_OK || result.exit_code) {
        writ_report(session, WRIT_D_TOOLCHAIN, WRIT_NO_LOC, "cargo rustdoc failed: %.*s", S(result.err));
        return WRIT_TOOL_FAILED;
    }
    const char *metadata[] = {"cargo", "metadata", "-q", "--format-version", "1", "--no-deps", "--manifest-path",
                              argv[6], nullptr};
    if (writ_run(session, metadata, (writ_str_t){0}, &result) != WRIT_OK || result.exit_code) {
        writ_report(session, WRIT_D_TOOLCHAIN, WRIT_NO_LOC, "cargo metadata failed: %.*s", S(result.err));
        return WRIT_TOOL_FAILED;
    }
    const char *key = "\"target_directory\":\"";
    for (size_t p = 0; p + strlen(key) <= result.out.length; p++) {
        if (!memcmp(result.out.data + p, key, strlen(key))) {
            size_t start = p + strlen(key);
            size_t end = start;
            while (end < result.out.length && result.out.data[end] != '"') {
                end++;
            }
            *json_path = writ_str_format(session, "%.*s/doc/%.*s.json", (int)(end - start), result.out.data + start,
                                         S(request->crate));
            return WRIT_OK;
        }
    }
    return WRIT_TOOL_FAILED;
}

writ_status_t
writ_toolchain_import_rust(writ_session_t *session, const writ_rust_import_request_t *request,
                           const writ_output_t *output)
{
    if (!session || !request || !output || !request->library.length || !request->crate.length) {
        return WRIT_INVALID_ARGUMENT;
    }
    writ_str_t path = request->rustdoc_json;
    if (!path.length) {
        writ_status_t status = run_rustdoc(session, request, &path);
        if (status != WRIT_OK) {
            return status;
        }
    }
    importer_t im = {.session = session, .request = request, .json = writ_unit_new(session),
                     .items = writ_map_new(session), .crate = request->crate, .skipped = writ_map_new(session)};
    writ_status_t status = writ_unit_read_json(im.json, path, &im.index);
    if (status != WRIT_OK) {
        return status;
    }
    uint32_t index = get(&im, im.index, "index");
    for (uint32_t i = 0; i < at(&im, index)->child_count; i++) {
        uint32_t member = writ_node_child(im.json, index, i);
        writ_str_t key = writ_sym_str(session, at(&im, member)->name);
        writ_map_put(im.items, strtoull(writ_str_copy(session, key.data, key.length).data, nullptr, 10),
                     writ_node_child(im.json, member, 0));
    }
    note_skipped(&im, render(&im));
    writ_unit_t *contract = writ_unit_new(session);
    writ_origin_t origin = {.host_path = writ_str_format(session, "%.*s.writ", S(request->library))};
    status = writ_unit_add_source(contract, WRIT_SOURCE_CONTRACT_FILE, origin, render(&im), nullptr);
    return status == WRIT_OK ? writ_format_unit(contract, output) : status;
}
