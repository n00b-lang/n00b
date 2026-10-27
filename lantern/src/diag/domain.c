#include "diag/domain.h"

const core_diag_code_t *
core_diag_domain_code(const core_diag_domain_t *domain, uint32_t id)
{
    return domain && id < domain->count ? &domain->codes[id] : nullptr;
}

static bool
code_matches(const core_diag_domain_t *domain, core_string_t code,
             core_string_t query)
{
    if (core_string_eq(code, query)) return true;
    core_string_t prefix = domain->prefix;
    return prefix.u8_bytes
        && code.u8_bytes == query.u8_bytes + prefix.u8_bytes
        && core_string_starts_with(code, prefix)
        && core_string_eq(core_string_slice(code, prefix.u8_bytes,
                                            query.u8_bytes), query);
}

bool
core_diag_domain_lookup(const core_diag_domain_t *domain, core_string_t code,
                        uint32_t *out)
{
    for (uint32_t i = 0; domain && code.data && i < domain->count; i++) {
        if (!code_matches(domain, domain->codes[i].code, code)) continue;
        if (out) *out = i;
        return true;
    }
    return false;
}
