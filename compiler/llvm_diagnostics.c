#include "llvm_diagnostics.h"

void
n00bb_backend_internal_error(core_diagnostics_t *diagnostics,
                             core_diag_location_t at, core_string_t message)
{
    if (!diagnostics) return;
    n00bb_internal_error(diagnostics, nullptr, at, "generating machine code",
                         "%.*s", (int)message.u8_bytes, message.data);
}
