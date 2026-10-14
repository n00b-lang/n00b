#include <stddef.h>
#include <stdint.h>

#include "writ/model.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    writ_session_t *session = writ_session_new(nullptr);
    writ_unit_t *unit = writ_unit_new(session);
    writ_origin_t origin = {.host_path = {"fuzz.writ", 9}};
    writ_str_t text = {(const char *)data, size};
    if (writ_unit_add_source(unit, WRIT_SOURCE_CONTRACT_FILE, origin, text, nullptr) == WRIT_OK) {
        writ_platform_t *platform = nullptr;
        writ_platform_from_triple(session, writ_platform_host_triple(), &platform);
        const writ_model_t *model = nullptr;
        if (writ_resolve(unit, platform, &model) == WRIT_OK) {
            for (size_t i = 0; i < writ_model_fn_count(model); i++) {
                writ_model_fn_plan(model, writ_model_fn_at(model, i));
            }
        }
    }
    writ_session_free(session);
    return 0;
}
