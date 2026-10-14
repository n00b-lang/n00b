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
} writ_gen_c_request_t;

writ_status_t writ_gen_c(const writ_model_t *model, const writ_gen_c_request_t *request,
                         const writ_output_t *output);

typedef struct {
    size_t     struct_size;
    writ_str_t crate_path;
} writ_gen_rust_request_t;

writ_status_t writ_gen_rust(const writ_model_t *model, const writ_gen_rust_request_t *request,
                            const writ_output_t *output);

#ifdef __cplusplus
}
#endif

#endif
