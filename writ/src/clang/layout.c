#include "../core/model/model.h"
#include "tu.h"


typedef struct {
    writ_tu_t  *tu;
    writ_str_t  spelling;
    CXType      found;
    bool        matched;
} lookup_t;

static enum CXChildVisitResult
find_type(CXCursor cursor, CXCursor parent, CXClientData data)
{
    (void)parent;
    lookup_t *lookup = data;
    enum CXCursorKind kind = clang_getCursorKind(cursor);
    writ_str_t name = writ_tu_text(lookup->tu->session, clang_getCursorSpelling(cursor));
    writ_str_t spelled = kind == CXCursor_StructDecl ? writ_str_format(lookup->tu->session, "struct %.*s", S(name))
                       : kind == CXCursor_UnionDecl  ? writ_str_format(lookup->tu->session, "union %.*s", S(name))
                                                     : name;
    bool candidate = kind == CXCursor_TypedefDecl || ((kind == CXCursor_StructDecl || kind == CXCursor_UnionDecl)
                                                      && clang_isCursorDefinition(cursor));
    if (candidate && writ_str_eq(spelled, lookup->spelling)) {
        lookup->found = clang_getCanonicalType(clang_getCursorType(cursor));
        lookup->matched = true;
        return CXChildVisit_Break;
    }
    return CXChildVisit_Continue;
}

static void
verify_record(writ_tu_t *tu, const writ_model_t *model, const writ_record_rec_t *record, writ_emit_t *report)
{
    writ_session_t *session = model->session;
    const writ_type_rec_t *type = writ_type(model, record->type);
    lookup_t lookup = {.tu = tu, .spelling = writ_sym_str(session, type->c_spelling)};
    clang_visitChildren(clang_getTranslationUnitCursor(tu->unit), find_type, &lookup);
    writ_str_t name = writ_sym_str(session, record->name);
    if (!lookup.matched) {
        writ_report(session, WRIT_D_LAYOUT_TOOLCHAIN, record->loc, "`%.*s` is not declared by the headers",
                    S(lookup.spelling));
        return;
    }
    long long size = clang_Type_getSizeOf(lookup.found);
    long long align = clang_Type_getAlignOf(lookup.found);
    if (size != type->size || align != type->align) {
        writ_report(session, WRIT_D_LAYOUT_TOOLCHAIN, record->loc, "`%.*s` is %lld bytes aligned to %lld; the contract computes %u aligned to %u",
                    S(name), size, align, type->size, type->align);
    }
    writ_emit_line(report, "%.*s size %lld align %lld", S(name), size, align);
    for (uint32_t i = 0; i < record->field_count; i++) {
        const writ_slot_rec_t *field = &model->slots.data[record->first_field + i];
        writ_str_t field_name = writ_sym_str(session, field->name);
        long long offset = clang_Type_getOffsetOf(lookup.found, writ_str_copy(session, field_name.data, field_name.length).data);
        if (offset < 0 || offset / 8 != field->offset) {
            writ_report(session, WRIT_D_LAYOUT_TOOLCHAIN, field->loc, "`%.*s.%.*s` is at %lld; the contract computes %u",
                        S(name), S(field_name), offset < 0 ? offset : offset / 8, field->offset);
        }
        writ_emit_line(report, "  @%lld %.*s", offset / 8, S(field_name));
    }
}

writ_status_t
writ_clang_verify_layout(const writ_model_t *model, writ_str_t sysroot, writ_str_t *report)
{
    if (!model || !report) {
        return WRIT_INVALID_ARGUMENT;
    }
    if (!writ_model_library(model)->headers_count) {
        *report = writ_cstr("skipped: the contract names no headers\n");
        return WRIT_OK;
    }
    writ_session_t *session = model->session;
    const writ_library_view_t *library = writ_model_library(model);
    writ_import_request_t request = {
        .struct_size         = sizeof(request),
        .library             = library->name,
        .headers_count       = library->headers_count,
        .headers             = library->headers,
        .defines_count       = library->defines_count,
        .defines             = library->defines,
        .include_paths_count = library->include_paths_count,
        .include_paths       = library->include_paths,
        .sysroot             = sysroot,
    };
    writ_tu_t tu;
    writ_str_t error = {0};
    size_t first_diag = session->diags.len;
    writ_emit_t emit;
    writ_emit_init(&emit);
    if (!library->headers_count) {
        *report = writ_cstr("");
        return WRIT_OK;
    }
    writ_status_t status = writ_tu_open(session, &tu, library->platform, &request, false, &error);
    if (status != WRIT_OK) {
        writ_report(session, WRIT_D_TOOLCHAIN, WRIT_NO_LOC, "%.*s", S(error));
        writ_emit_discard(&emit);
        writ_diag_flush(session, first_diag);
        return status;
    }
    for (size_t i = 0; i < model->records.len; i++) {
        verify_record(&tu, model, &model->records.data[i], &emit);
    }
    writ_tu_close(&tu);
    *report = writ_emit_finish(&emit, session);
    writ_diag_flush(session, first_diag);
    return session->diags.len == first_diag ? WRIT_OK : WRIT_HAS_ERRORS;
}
