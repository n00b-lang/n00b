#include <stdlib.h>

#include "tu.h"

static writ_str_t trimmed(writ_str_t text);

writ_str_t
writ_tu_text(writ_session_t *session, CXString text)
{
    const char *data = clang_getCString(text);
    writ_str_t copy = writ_str_copy(session, data ? data : "", data ? strlen(data) : 0);
    clang_disposeString(text);
    return copy;
}

static writ_str_t
sysroot_for(writ_session_t *session, writ_str_t requested)
{
    if (requested.length) {
        return requested;
    }
    const char *environment = getenv("SDKROOT");
    if (environment && *environment) {
        return writ_cstr(environment);
    }
#ifdef __APPLE__
    const char *argv[] = {"xcrun", "--show-sdk-path", nullptr};
    writ_run_result_t result;
    if (writ_run(session, argv, writ_cstr(""), &result) == WRIT_OK && !result.exit_code && result.out.length) {
        return trimmed(result.out);
    }
#endif
    return writ_cstr("");
}

static const char *
terminated(writ_session_t *session, writ_str_t text)
{
    return writ_str_copy(session, text.data, text.length).data;
}

static writ_str_t
trimmed(writ_str_t text)
{
    while (text.length && (text.data[text.length - 1] == '\n' || text.data[text.length - 1] == '\r')) {
        text.length--;
    }
    return text;
}

static writ_str_t
resource_dir(writ_session_t *session)
{
    const char *argv[] = {writ_tool_path(session, "clang").data, "-print-resource-dir", nullptr};
    writ_run_result_t result;
    if (writ_run(session, argv, writ_cstr(""), &result) != WRIT_OK || result.exit_code) {
        return writ_cstr("");
    }
    return trimmed(result.out);
}

writ_status_t
writ_tu_open(writ_session_t *session, writ_tu_t *tu, writ_str_t triple, const writ_import_request_t *request,
             bool macros, writ_str_t *error)
{
    *tu = (writ_tu_t){.session = session, .request = request};
    size_t capacity = 12 + 2 * (request->defines_count + request->include_paths_count);
    const char **args = writ_alloc(session, sizeof(char *) * capacity);
    int count = 0;
    args[count++] = "-x";
    args[count++] = "c";
    args[count++] = "-std=c11";
    args[count++] = "-target";
    args[count++] = terminated(session, triple);
    writ_str_t resources = resource_dir(session);
    if (resources.length) {
        args[count++] = "-resource-dir";
        args[count++] = terminated(session, resources);
    }
    writ_str_t sysroot = sysroot_for(session, request->sysroot);
    if (sysroot.length) {
        args[count++] = "-isysroot";
        args[count++] = terminated(session, sysroot);
    }
    for (size_t i = 0; i < request->include_paths_count; i++) {
        args[count++] = writ_str_format(session, "-I%.*s", (int)request->include_paths[i].length,
                                        request->include_paths[i].data).data;
    }
    for (size_t i = 0; i < request->defines_count; i++) {
        args[count++] = writ_str_format(session, "-D%.*s", (int)request->defines[i].length, request->defines[i].data).data;
    }
    writ_emit_t source;
    writ_emit_init(&source);
    for (size_t i = 0; i < request->headers_count; i++) {
        writ_emit_line(&source, "#include <%.*s>", (int)request->headers[i].length, request->headers[i].data);
    }
    writ_str_t text = writ_emit_finish(&source, session);
    struct CXUnsavedFile unsaved = {"writ-import.c", text.data, (unsigned long)text.length};
    tu->index = clang_createIndex(0, 0);
    unsigned options = CXTranslationUnit_SkipFunctionBodies | (macros ? CXTranslationUnit_DetailedPreprocessingRecord : 0);
    enum CXErrorCode code = clang_parseTranslationUnit2(tu->index, "writ-import.c", args, count, &unsaved, 1, options,
                                                        &tu->unit);
    if (code != CXError_Success || !tu->unit) {
        *error = writ_cstr("libclang could not parse the headers");
        writ_tu_close(tu);
        return WRIT_TOOL_FAILED;
    }
    for (unsigned i = 0; i < clang_getNumDiagnostics(tu->unit); i++) {
        CXDiagnostic diagnostic = clang_getDiagnostic(tu->unit, i);
        enum CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
        if (severity >= CXDiagnostic_Error) {
            *error = writ_tu_text(session, clang_formatDiagnostic(diagnostic, clang_defaultDiagnosticDisplayOptions()));
            clang_disposeDiagnostic(diagnostic);
            writ_tu_close(tu);
            return WRIT_TOOL_FAILED;
        }
        clang_disposeDiagnostic(diagnostic);
    }
    return WRIT_OK;
}

void
writ_tu_close(writ_tu_t *tu)
{
    if (tu->unit) {
        clang_disposeTranslationUnit(tu->unit);
    }
    if (tu->index) {
        clang_disposeIndex(tu->index);
    }
    tu->unit = nullptr;
    tu->index = nullptr;
}

bool
writ_tu_in_headers(const writ_tu_t *tu, CXCursor cursor)
{
    CXFile file;
    clang_getSpellingLocation(clang_getCursorLocation(cursor), &file, nullptr, nullptr, nullptr);
    if (!file) {
        return false;
    }
    writ_str_t path = writ_tu_text(tu->session, clang_getFileName(file));
    for (size_t i = 0; i < tu->request->headers_count; i++) {
        writ_str_t header = tu->request->headers[i];
        if (path.length > header.length && path.data[path.length - header.length - 1] == '/'
            && !memcmp(path.data + path.length - header.length, header.data, header.length)) {
            return true;
        }
    }
    return false;
}
