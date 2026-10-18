#include "private/th/internal.h"

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
    for (size_t i = 0; ok && i < source->intrinsic_types.len; i++) {
        const th_env_intrinsic_type_t *entry =
            &source->intrinsic_types.data[i];
        if (!filter->intrinsic
            || !filter->intrinsic(context, entry->trait_name,
                                  entry->receiver_pattern)) continue;
        ok = th_env_register_intrinsic_type_pattern(env, entry->trait_name,
                                                    entry->receiver_pattern);
    }
    for (size_t i = 0; ok && i < source->metadata.len; i++) {
        const th_env_metadata_record_t *entry = &source->metadata.data[i];
        if (!filter->metadata
            || !filter->metadata(context, entry->owner_kind,
                                 entry->owner_name)) continue;
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
