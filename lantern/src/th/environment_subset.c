#include <string.h>

#include "private/th/internal.h"

core_list_decl(size_t);

static const char *
declaration_module(const th_env *source, th_env_metadata_owner_t kind,
                   const char *owner)
{
    switch (kind) {
    case TH_ENV_METADATA_TYPE:
        for (size_t i = 0; i < source->type_origins.len; i++)
            if (!strcmp(source->type_origins.data[i].name, owner))
                return source->type_origins.data[i].module;
        return nullptr;
    case TH_ENV_METADATA_TRAIT:
        for (size_t i = 0; i < source->traits.len; i++)
            if (!strcmp(source->traits.data[i].name, owner))
                return source->traits.data[i].module;
        return nullptr;
    case TH_ENV_METADATA_FUNCTION:
        for (size_t i = 0; i < source->functions.len; i++)
            if (!strcmp(source->functions.data[i].qualified_name, owner))
                return source->functions.data[i].module;
        return nullptr;
    case TH_ENV_METADATA_VALUE:
        for (size_t i = 0; i < source->values.len; i++)
            if (!strcmp(source->values.data[i].qualified_name, owner))
                return source->values.data[i].module;
        return nullptr;
    case TH_ENV_METADATA_ENVIRONMENT:
        return nullptr;
    }
    return nullptr;
}

th_env *
th_env_subset(const th_env *source, const th_env_filter_t *filter)
{
    if (!source || !filter) return nullptr;
    th_env *env = th_env_new_in_context(source->types);
    bool ok = env != nullptr;
    void *context = filter->context;
    for (size_t i = 0; ok && i < source->type_origins.len; i++) {
        const th_env_type_origin_t *entry = &source->type_origins.data[i];
        if (!filter->module || !filter->module(context, entry->module))
            continue;
        ok = th_env_register_type(env, entry->name, entry->module,
                                  entry->package, entry->visibility);
        if (ok && entry->shape_kind != TH_ENV_TYPE_SHAPE_NONE)
            ok = th_env_register_type_shape(env, entry->name,
                                            entry->shape_kind, entry->shape);
    }
    for (size_t i = 0; ok && i < source->traits.len; i++) {
        const th_env_trait_t *entry = &source->traits.data[i];
        if (!filter->module || !filter->module(context, entry->module))
            continue;
        ok = th_env_register_trait_contracts(env, entry->name, entry->module,
            entry->package, entry->visibility, entry->contracts);
    }
    for (size_t i = 0; ok && i < source->functions.len; i++) {
        const th_env_function_t *entry = &source->functions.data[i];
        if (!filter->module || !filter->module(context, entry->module))
            continue;
        ok = th_env_register_function_contract(env, entry->qualified_name,
            entry->root_name, entry->module, entry->package,
            entry->visibility, entry->contract);
    }
    for (size_t i = 0; ok && i < source->values.len; i++) {
        const th_env_value_t *entry = &source->values.data[i];
        if (!filter->module || !filter->module(context, entry->module))
            continue;
        ok = th_env_register_value(env, entry->qualified_name,
            entry->root_name, entry->module, entry->package,
            entry->visibility, entry->type);
    }
    core_list_t(size_t) carried = core_list_new(size_t);
    for (size_t i = 0; ok && i < source->intrinsic_types.len; i++) {
        const th_env_intrinsic_type_t *entry =
            &source->intrinsic_types.data[i];
        if (!filter->intrinsic
            || !filter->intrinsic(context, entry->trait_name,
                                  entry->receiver_pattern)) continue;
        for (size_t t = 0; ok && t < source->traits.len; t++) {
            const th_env_trait_t *trait = &source->traits.data[t];
            if (strcmp(trait->name, entry->trait_name)) continue;
            bool present = filter->module
                && filter->module(context, trait->module);
            for (size_t c = 0; !present && c < carried.len; c++)
                present = carried.data[c] == t;
            if (present) break;
            ok = th_env_register_trait_contracts(env, trait->name,
                trait->module, trait->package, trait->visibility,
                trait->contracts);
            core_list_push(carried, t);
            break;
        }
        if (ok) ok = th_env_register_intrinsic_type_pattern(env,
            entry->trait_name, entry->receiver_pattern);
    }
    core_list_free(carried);
    for (size_t i = 0; ok && i < source->metadata.len; i++) {
        const th_env_metadata_record_t *entry = &source->metadata.data[i];
        const char *module = declaration_module(source, entry->owner_kind,
                                                entry->owner_name);
        bool keep = entry->owner_kind == TH_ENV_METADATA_ENVIRONMENT
            ? filter->metadata && filter->metadata(context, entry->owner_kind,
                                                   entry->owner_name)
            : module && filter->module && filter->module(context, module);
        if (!keep) continue;
        ok = th_env_register_metadata(env, entry->owner_kind,
            entry->owner_name, entry->key, entry->version, entry->data,
            entry->size);
    }
    if (!ok || !th_env_freeze(env)) {
        th_env_free(env);
        return nullptr;
    }
    return env;
}
