#include "private/th/internal.h"
#include "lib/dict.h"

typedef struct {
    core_dict_t types;
    core_dict_t traits;
    core_dict_t functions;
    core_dict_t values;
} owner_index_t;

static void
index_owner(core_dict_t *index, const core_string_t *name, size_t at)
{
    core_dict_add(index, name, at + 1);
}

static void
owner_index_build(owner_index_t *index, const th_env *source)
{
    core_dict_init_string(&index->types);
    core_dict_init_string(&index->traits);
    core_dict_init_string(&index->functions);
    core_dict_init_string(&index->values);
    for (size_t i = 0; i < source->type_origins.len; i++)
        index_owner(&index->types, &source->type_origins.data[i].name, i);
    for (size_t i = 0; i < source->traits.len; i++)
        index_owner(&index->traits, &source->traits.data[i].name, i);
    for (size_t i = 0; i < source->functions.len; i++)
        index_owner(&index->functions,
                    &source->functions.data[i].qualified_name, i);
    for (size_t i = 0; i < source->values.len; i++)
        index_owner(&index->values, &source->values.data[i].qualified_name,
                    i);
}

static void
owner_index_free(owner_index_t *index)
{
    core_dict_free(&index->types);
    core_dict_free(&index->traits);
    core_dict_free(&index->functions);
    core_dict_free(&index->values);
}

static size_t
owner_at(core_dict_t *index, core_string_t owner)
{
    bool found = false;
    uintptr_t at = (uintptr_t)core_dict_get(index, &owner, &found);
    return found ? (size_t)at : 0;
}

static core_string_t
declaration_module(const th_env *source, owner_index_t *index,
                   th_env_metadata_owner_t kind, core_string_t owner)
{
    size_t at = 0;
    switch (kind) {
    case TH_ENV_METADATA_TYPE:
        at = owner_at(&index->types, owner);
        return at ? source->type_origins.data[at - 1].module
                  : (core_string_t){};
    case TH_ENV_METADATA_TRAIT:
        at = owner_at(&index->traits, owner);
        return at ? source->traits.data[at - 1].module : (core_string_t){};
    case TH_ENV_METADATA_FUNCTION:
        at = owner_at(&index->functions, owner);
        return at ? source->functions.data[at - 1].module
                  : (core_string_t){};
    case TH_ENV_METADATA_VALUE:
        at = owner_at(&index->values, owner);
        return at ? source->values.data[at - 1].module : (core_string_t){};
    case TH_ENV_METADATA_ENVIRONMENT:
        return (core_string_t){};
    }
    return (core_string_t){};
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
        ok = th_env_copy_function(env, entry,
                                  th_env_function_copy_source(entry));
    }
    for (size_t i = 0; ok && i < source->values.len; i++) {
        const th_env_value_t *entry = &source->values.data[i];
        if (!filter->module || !filter->module(context, entry->module))
            continue;
        ok = th_env_register_value(env, entry->qualified_name,
            entry->root_name, entry->module, entry->package,
            entry->visibility, entry->type);
    }
    owner_index_t index;
    owner_index_build(&index, source);
    core_dict_t carried;
    core_dict_init(&carried, core_hash_word, nullptr);
    for (size_t i = 0; ok && i < source->intrinsic_types.len; i++) {
        const th_env_intrinsic_type_t *entry =
            &source->intrinsic_types.data[i];
        if (!filter->intrinsic
            || !filter->intrinsic(context, entry->trait_name,
                                  entry->receiver_pattern)) continue;
        size_t at = owner_at(&index.traits, entry->trait_name);
        const th_env_trait_t *trait = at ? &source->traits.data[at - 1]
                                         : nullptr;
        bool present = !trait
            || (filter->module && filter->module(context, trait->module))
            || !core_dict_add(&carried, at, at);
        if (!present)
            ok = th_env_register_trait_contracts(env, trait->name,
                trait->module, trait->package, trait->visibility,
                trait->contracts);
        if (ok) ok = th_env_register_intrinsic_type_pattern(env,
            entry->trait_name, entry->receiver_pattern);
    }
    core_dict_free(&carried);
    for (size_t i = 0; ok && i < source->metadata.len; i++) {
        const th_env_metadata_record_t *entry = &source->metadata.data[i];
        core_string_t module = declaration_module(source, &index,
                                                  entry->owner_kind,
                                                  entry->owner_name);
        bool keep = entry->owner_kind == TH_ENV_METADATA_ENVIRONMENT
            ? filter->metadata && filter->metadata(context, entry->owner_kind,
                                                   entry->owner_name)
            : module.data && filter->module
                && filter->module(context, module);
        if (!keep) continue;
        ok = th_env_register_metadata(env, entry->owner_kind,
            entry->owner_name, entry->key, entry->version, entry->data,
            entry->size);
    }
    owner_index_free(&index);
    if (!ok || !th_env_freeze(env)) {
        th_env_free(env);
        return nullptr;
    }
    return env;
}
