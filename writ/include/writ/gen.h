#ifndef WRIT_GEN_H
#define WRIT_GEN_H

#include "writ/model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t     struct_size;
    writ_str_t prefix;
    bool       conformance;
    bool       pointer_normalized;
} writ_gen_c_request_t;

writ_status_t writ_gen_c(const writ_model_t *model, const writ_gen_c_request_t *request,
                         const writ_output_t *output);

typedef struct {
    size_t     struct_size;
    writ_str_t crate_path;
} writ_gen_rust_request_t;

writ_status_t writ_gen_rust(const writ_model_t *model, const writ_gen_rust_request_t *request,
                            const writ_output_t *output);

typedef struct writ_binding writ_binding_t;

typedef struct {
    size_t       struct_size;
    writ_unit_t *profiles;
    writ_unit_t *binding;
    writ_str_t   language;
} writ_bind_request_t;

writ_status_t writ_bind(const writ_model_t *model, const writ_bind_request_t *request,
                        const writ_binding_t **binding);
writ_status_t writ_binding_render(const writ_binding_t *binding, writ_str_t *rendered);

typedef struct {
    size_t                struct_size;
    const writ_binding_t *binding;
    writ_str_t            module_name;
    writ_str_t            shim_prefix;
    writ_str_t            type_namespace;
} writ_gen_n00b_request_t;

typedef struct {
    writ_str_t name;
    writ_str_t symbol;
    writ_str_t signature;
} writ_n00b_extern_t;

typedef struct {
    writ_str_t function;
    writ_str_t symbol;
} writ_n00b_export_t;

typedef struct {
    size_t                    count;
    const writ_n00b_extern_t *items;
    size_t                    exports_count;
    const writ_n00b_export_t *exports;
    writ_str_t                header;
} writ_n00b_externs_t;

typedef struct {
    size_t            libraries_count;
    const writ_str_t *libraries;
    size_t            frameworks_count;
    const writ_str_t *frameworks;
    size_t            search_paths_count;
    const writ_str_t *search_paths;
    bool              load_dynamic;
} writ_link_requirements_t;

writ_status_t writ_gen_n00b(const writ_gen_n00b_request_t *request, const writ_output_t *output,
                            writ_link_requirements_t *links, writ_n00b_externs_t *externs);

#ifdef __cplusplus
}
#endif

#endif
