#include "compiler_system.h"

static const core_string_t system_roots[] = {
    CORE_STRING_STATIC("builtin"),
    CORE_STRING_STATIC("sys"),
};

static const core_string_t compiler_provenance = CORE_STRING_STATIC("builtins");
static const core_string_t reserved_prefix     = CORE_STRING_STATIC("__");
static const core_string_t dereference_member  = CORE_STRING_STATIC("__value");
static const core_string_t generated_prefix    = CORE_STRING_STATIC("<generated:");

bool
n00bb_generated_source(core_string_t path)
{
    return path.data && core_string_starts_with(path, generated_prefix);
}

bool
n00bb_system_module(core_string_t module)
{
    if (core_string_eq(module, compiler_provenance)) return true;
    for (size_t i = 0; i < sizeof(system_roots) / sizeof(system_roots[0]);
         i++) {
        core_string_t root = system_roots[i];
        if (core_string_eq(module, root)) return true;
        if (module.u8_bytes > root.u8_bytes
            && core_string_starts_with(module, root)
            && module.data[root.u8_bytes] == '.') return true;
    }
    return false;
}

bool
n00bb_reserved_name(core_string_t name)
{
    return name.data && core_string_starts_with(name, reserved_prefix);
}

bool
n00bb_builtin_member(core_string_t name)
{
    return name.data && core_string_eq(name, dereference_member);
}
