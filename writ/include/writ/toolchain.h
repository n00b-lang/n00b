#ifndef WRIT_TOOLCHAIN_H
#define WRIT_TOOLCHAIN_H

#include "writ/model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t            struct_size;
    writ_str_t        library;
    writ_str_t        crate;
    writ_str_t        manifest;
    writ_str_t        package;
    writ_str_t        rustdoc_json;
    size_t            features_count;
    const writ_str_t *features;
} writ_rust_import_request_t;

writ_status_t writ_toolchain_import_rust(writ_session_t *session, const writ_rust_import_request_t *request,
                                         const writ_output_t *output);
writ_status_t writ_toolchain_verify_probes(const writ_model_t *model, writ_str_t sysroot, writ_str_t *report);
writ_status_t writ_toolchain_abi_classes(const writ_model_t *model, writ_str_t sysroot, writ_str_t *report);
writ_status_t writ_toolchain_verify_symbols(const writ_model_t *model, writ_str_t binary, writ_str_t sysroot,
                                            writ_str_t *report);

#ifdef __cplusplus
}
#endif

#endif
