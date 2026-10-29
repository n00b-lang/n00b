#include "language_hooks.h"
#include "compiler_system.h"
#include "diagnostics.h"

#include "lib/list.h"

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

core_list_decl(core_string_t);

static core_list_t(core_string_t)
language_hooks(const th_env *environment)
{
    core_list_t(core_string_t) hooks = core_list_new(core_string_t);
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
            if (method && n00bb_reserved_name(method->name))
                core_list_push(hooks, method->name);
        }
    }
    return hooks;
}

bool
n00bb_language_hook(const th_env *environment, core_string_t name)
{
    core_list_t(core_string_t) hooks = language_hooks(environment);
    bool found = false;
    for (size_t i = 0; i < hooks.len && !found; i++)
        found = core_string_eq(hooks.data[i], name);
    core_list_free(hooks);
    return found;
}

core_string_t
n00bb_nearest_language_hook(const th_env *environment, core_string_t name)
{
    core_list_t(core_string_t) hooks = language_hooks(environment);
    size_t best[1];
    core_string_t nearest = !n00bb_language_hook(environment, name)
        && n00bb_closest_names(name, hooks.data, hooks.len, best, 1)
        ? hooks.data[best[0]] : (core_string_t){0};
    core_list_free(hooks);
    return nearest;
}
