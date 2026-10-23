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

static const th_type *
trait_methods(const th_env_declaration_view_t *view, size_t *count)
{
    const th_type *contract = view->contract;
    while (contract && th_kind_of(contract) == TH_BINDER)
        contract = th_binder_body(contract);
    *count = !contract ? 0
        : th_kind_of(contract) == TH_RECORD ? th_record_len(contract)
        : th_kind_of(contract) == TH_TRAIT ? th_trait_len(contract) : 0;
    return contract;
}

static const th_member *
trait_method(const th_type *methods, size_t index)
{
    return th_kind_of(methods) == TH_TRAIT ? th_trait_method(methods, index)
                                           : th_record_member(methods, index);
}

static size_t
edit_distance(core_string_t left, core_string_t right)
{
    if (left.u8_bytes > 64 || right.u8_bytes > 64) return SIZE_MAX;
    size_t row[65];
    for (size_t j = 0; j <= right.u8_bytes; j++) row[j] = j;
    for (size_t i = 1; i <= left.u8_bytes; i++) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= right.u8_bytes; j++) {
            size_t above = row[j];
            size_t substitute = diagonal
                + (left.data[i - 1] == right.data[j - 1] ? 0 : 1);
            size_t best = above + 1 < row[j - 1] + 1 ? above + 1
                                                     : row[j - 1] + 1;
            row[j] = substitute < best ? substitute : best;
            diagonal = above;
        }
    }
    return row[right.u8_bytes];
}

static core_string_t
scan_language_hooks(const th_env *environment, core_string_t name,
                    size_t *distance)
{
    core_string_t nearest = {0};
    *distance = SIZE_MAX;
    size_t count = environment ? th_env_declaration_count(environment) : 0;
    for (size_t d = 0; d < count; d++) {
        th_env_declaration_view_t view = {0};
        if (!th_env_declaration_at(environment, d, &view)
            || view.kind != TH_ENV_DECLARATION_TRAIT
            || !n00bb_system_module(view.module)) continue;
        size_t members = 0;
        const th_type *methods = trait_methods(&view, &members);
        for (size_t m = 0; m < members; m++) {
            const th_member *method = trait_method(methods, m);
            if (!method || !n00bb_reserved_name(method->name)) continue;
            size_t gap = edit_distance(name, method->name);
            if (gap < *distance) {
                *distance = gap;
                nearest = method->name;
            }
            if (gap == 0) return nearest;
        }
    }
    return nearest;
}

bool
n00bb_language_hook(const th_env *environment, core_string_t name)
{
    size_t distance;
    scan_language_hooks(environment, name, &distance);
    return distance == 0;
}

core_string_t
n00bb_nearest_language_hook(const th_env *environment, core_string_t name)
{
    size_t distance;
    core_string_t nearest = scan_language_hooks(environment, name, &distance);
    return distance > 0 && distance <= 2 ? nearest : (core_string_t){0};
}
