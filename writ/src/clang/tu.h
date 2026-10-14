#pragma once

#include <clang-c/Index.h>

#include "../core/internal.h"
#include "writ/clang.h"

typedef struct {
    writ_session_t              *session;
    const writ_import_request_t *request;
    CXIndex                      index;
    CXTranslationUnit            unit;
} writ_tu_t;

writ_status_t writ_tu_open(writ_session_t *session, writ_tu_t *tu, writ_str_t triple,
                           const writ_import_request_t *request, bool macros, writ_str_t *error);
void          writ_tu_close(writ_tu_t *tu);
bool          writ_tu_in_headers(const writ_tu_t *tu, CXCursor cursor);
writ_str_t    writ_tu_text(writ_session_t *session, CXString text);
