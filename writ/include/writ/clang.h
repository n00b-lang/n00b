#ifndef WRIT_CLANG_H
#define WRIT_CLANG_H

#include "writ/model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t            struct_size;
    writ_str_t        library;
    size_t            links_count;
    const writ_str_t *links;
    size_t            headers_count;
    const writ_str_t *headers;
    size_t            defines_count;
    const writ_str_t *defines;
    size_t            include_paths_count;
    const writ_str_t *include_paths;
    writ_str_t        sysroot;
    size_t            only_count;
    const writ_str_t *only;
} writ_import_request_t;

writ_status_t writ_clang_import(writ_session_t *session, const writ_platform_t *platform,
                                const writ_import_request_t *request, const writ_output_t *output);
writ_status_t writ_clang_verify_layout(const writ_model_t *model, writ_str_t sysroot,
                                       writ_str_t *report);

#ifdef __cplusplus
}
#endif

#endif
