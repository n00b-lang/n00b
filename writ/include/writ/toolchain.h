#ifndef WRIT_TOOLCHAIN_H
#define WRIT_TOOLCHAIN_H

#include "writ/model.h"

#ifdef __cplusplus
extern "C" {
#endif

writ_status_t writ_toolchain_verify_probes(const writ_model_t *model, writ_str_t sysroot, writ_str_t *report);
writ_status_t writ_toolchain_abi_classes(const writ_model_t *model, writ_str_t sysroot, writ_str_t *report);
writ_status_t writ_toolchain_verify_symbols(const writ_model_t *model, writ_str_t binary, writ_str_t sysroot,
                                            writ_str_t *report);

#ifdef __cplusplus
}
#endif

#endif
