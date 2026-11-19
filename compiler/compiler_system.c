#include "compiler_system.h"

#include "lib/alloc.h"

#include <stdatomic.h>

static const core_string_t reserved_prefix     = CORE_STRING_STATIC("__");
static const core_string_t dereference_member  = CORE_STRING_STATIC("__value");
static const core_string_t generated_prefix    = CORE_STRING_STATIC("<generated:");

bool
n00bb_generated_source(core_string_t path)
{
    return path.data && core_string_starts_with(path, generated_prefix);
}

// Set once, by the resolver, before any module is located; read by workers.
static _Atomic(core_string_t *) stdlib_directory;

void
n00bb_system_set_stdlib(core_string_t canonical)
{
    if (!canonical.data || atomic_load(&stdlib_directory)) return;
    core_string_t *stored = core_alloc(core_string_t);
    *stored = core_string_dup(canonical);
    core_string_t *expected = nullptr;
    if (!atomic_compare_exchange_strong(&stdlib_directory, &expected,
                                        stored)) {
        core_free(stored->data);
        core_free(stored);
    }
}

bool
n00bb_system_package(core_string_t package_id)
{
    if (!package_id.data) return false;
    if (core_string_eq(package_id, N00BB_COMPILER_PACKAGE)) return true;
    core_string_t *stdlib = atomic_load(&stdlib_directory);
    if (!stdlib || !stdlib->u8_bytes
        || !core_string_starts_with(package_id, *stdlib)) return false;
    size_t length = stdlib->u8_bytes;
    return package_id.u8_bytes == length
        || package_id.data[length] == '/' || package_id.data[length] == '\\'
        || stdlib->data[length - 1] == '/' || stdlib->data[length - 1] == '\\';
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
