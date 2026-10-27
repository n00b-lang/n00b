#pragma once

#include "diag/diagnostics.h"
#include "diag/text.h"

typedef struct {
    core_string_t        code;
    core_diag_severity_t severity;
    core_string_t        title;
    core_string_t        explanation;
} core_diag_code_t;

typedef struct {
    core_string_t              prefix;
    const core_diag_code_t    *codes;
    uint32_t                   count;
    const core_text_catalog_t *texts;
} core_diag_domain_t;

const core_diag_code_t *core_diag_domain_code(const core_diag_domain_t *domain,
                                              uint32_t id);
bool core_diag_domain_lookup(const core_diag_domain_t *domain,
                             core_string_t code, uint32_t *out);
