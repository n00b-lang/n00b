#include <sys/stat.h>

#include "toolchain.h"

static bool
exists(writ_session_t *session, writ_str_t path)
{
    struct stat info;
    return !stat(writ_str_copy(session, path.data, path.length).data, &info);
}

static writ_str_t
find_binary(const writ_model_t *model, writ_str_t sysroot)
{
    writ_session_t *session = model->session;
    const writ_library_view_t *library = writ_model_library(model);
    if (!library->links_count) {
        return writ_cstr("");
    }
    writ_str_t link = library->links[0];
    writ_str_t root = writ_sysroot(session, sysroot);
    const char *patterns[] = {"%.*s/usr/lib/lib%.*s.tbd", "%.*s/usr/lib/lib%.*s.dylib",
                              "%.*s/usr/lib/aarch64-linux-gnu/lib%.*s.so", "%.*s/usr/lib/x86_64-linux-gnu/lib%.*s.so",
                              "%.*s/usr/lib/lib%.*s.so", "%.*s/lib/aarch64-linux-gnu/lib%.*s.so.1"};
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        writ_str_t candidate = writ_str_format(session, patterns[i], S(root), S(link));
        if (exists(session, candidate)) {
            return candidate;
        }
    }
    return writ_cstr("");
}

static void
collect_symbols(writ_session_t *session, writ_str_t listing, writ_map_t *symbols)
{
    size_t start = 0;
    for (size_t at = 0; at <= listing.length; at++) {
        if (at < listing.length && listing.data[at] != '\n') {
            continue;
        }
        size_t word = at;
        while (word > start && listing.data[word - 1] != ' ') {
            word--;
        }
        size_t end = word;
        while (end < at && listing.data[end] != '@') {
            end++;
        }
        if (word > start) {
            writ_sym_t name = writ_intern(session, listing.data + word, end - word);
            writ_map_put(symbols, name, 1);
        }
        start = at + 1;
    }
}

writ_status_t
writ_toolchain_verify_symbols(const writ_model_t *model, writ_str_t binary, writ_str_t sysroot, writ_str_t *report)
{
    if (!model || !report) {
        return WRIT_INVALID_ARGUMENT;
    }
    writ_session_t *session = model->session;
    size_t first_diag = session->diags.len;
    writ_str_t path = binary.length ? binary : find_binary(model, sysroot);
    if (!path.length) {
        writ_report(session, WRIT_D_TOOLCHAIN, WRIT_NO_LOC, "no binary found for the library's `link`");
        writ_diag_flush(session, first_diag);
        return WRIT_HAS_ERRORS;
    }
    bool macho = model->platform->object_format == writ_intern(session, "macho", 5);
    const char *argv[] = {writ_tool_path(session, "llvm-nm").data, "-g", "--defined-only",
                          macho ? writ_str_copy(session, path.data, path.length).data : "-D",
                          macho ? nullptr : writ_str_copy(session, path.data, path.length).data, nullptr};
    writ_run_result_t result;
    if (writ_run(session, argv, writ_cstr(""), &result) != WRIT_OK || result.exit_code) {
        writ_report(session, WRIT_D_TOOLCHAIN, WRIT_NO_LOC, "llvm-nm failed on %.*s: %.*s", S(path), S(result.err));
        writ_diag_flush(session, first_diag);
        return WRIT_HAS_ERRORS;
    }
    writ_map_t *symbols = writ_map_new(session);
    collect_symbols(session, result.out, symbols);
    writ_emit_t emit;
    writ_emit_init(&emit);
    for (uint32_t f = 0; f < model->fns.len; f++) {
        const writ_fn_rec_t *fn = &model->fns.data[f];
        if (fn->kind != WRIT_FN_FUNCTION || fn->expands_node || fn->no_symbol) {
            continue;
        }
        writ_str_t symbol = writ_sym_str(session, fn->link_symbol);
        writ_sym_t decorated = writ_intern_str(session, writ_str_format(session, "%s%.*s", macho ? "_" : "", S(symbol)));
        bool present = writ_map_get(symbols, decorated, nullptr);
        if (!present) {
            writ_report(session, WRIT_D_SYMBOL_MISSING, fn->loc, "`%.*s` in %.*s", S(symbol), S(path));
        }
        writ_emit_line(&emit, "%s %.*s", present ? "ok" : "missing", S(symbol));
    }
    *report = writ_emit_finish(&emit, session);
    writ_diag_flush(session, first_diag);
    return session->diags.len == first_diag ? WRIT_OK : WRIT_HAS_ERRORS;
}
