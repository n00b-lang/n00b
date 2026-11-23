#include "private/th/internal.h"
#include "lib/alloc.h"

bool
th_env_encode(const th_env *env, core_buffer_t *out)
{
    if (!env || !out) return false;
    th_type_writer_t *types = th_type_writer_new(env->types);
    if (!types) return false;
    core_buffer_t body = {0};
    core_put_u64(&body, env->type_origins.len);
    for (size_t i = 0; i < env->type_origins.len; i++) {
        const th_env_type_origin_t *entry = &env->type_origins.data[i];
        core_put_text(&body, entry->name);
        core_put_text(&body, entry->module);
        core_put_text(&body, entry->package);
        core_put_u32(&body, (uint32_t)entry->visibility);
        core_put_u32(&body, (uint32_t)entry->shape_kind);
        core_put_u32(&body, th_type_writer_add(types, entry->shape));
    }
    core_put_u64(&body, env->traits.len);
    for (size_t i = 0; i < env->traits.len; i++) {
        const th_env_trait_t *entry = &env->traits.data[i];
        core_put_text(&body, entry->name);
        core_put_text(&body, entry->module);
        core_put_text(&body, entry->package);
        core_put_u32(&body, (uint32_t)entry->visibility);
        core_put_u32(&body, th_type_writer_add(types, entry->contracts));
    }
    core_put_u64(&body, env->functions.len);
    for (size_t i = 0; i < env->functions.len; i++) {
        const th_env_function_t *entry = &env->functions.data[i];
        core_put_text(&body, entry->qualified_name);
        core_put_text(&body, entry->root_name);
        core_put_text(&body, entry->module);
        core_put_text(&body, entry->package);
        core_put_u32(&body, (uint32_t)entry->visibility);
        core_put_u64(&body, entry->requirement_count);
        core_put_u32(&body, th_type_writer_add(types,
                                          th_env_function_copy_source(entry)));
        core_put_u64(&body, entry->required_params);
        for (size_t r = 0; r < entry->requirement_count; r++) {
            core_put_u32(&body, (uint32_t)entry->requirement_kinds[r]);
            core_put_text(&body, entry->requirement_traits[r]);
        }
    }
    core_put_u64(&body, env->values.len);
    for (size_t i = 0; i < env->values.len; i++) {
        const th_env_value_t *entry = &env->values.data[i];
        core_put_text(&body, entry->qualified_name);
        core_put_text(&body, entry->root_name);
        core_put_text(&body, entry->module);
        core_put_text(&body, entry->package);
        core_put_u32(&body, (uint32_t)entry->visibility);
        core_put_u32(&body, th_type_writer_add(types, entry->type));
    }
    core_put_u64(&body, env->intrinsic_types.len);
    for (size_t i = 0; i < env->intrinsic_types.len; i++) {
        const th_env_intrinsic_type_t *entry = &env->intrinsic_types.data[i];
        core_put_text(&body, entry->trait_name);
        core_put_u32(&body, th_type_writer_add(types, entry->receiver_pattern));
    }
    core_put_u64(&body, env->metadata.len);
    for (size_t i = 0; i < env->metadata.len; i++) {
        const th_env_metadata_record_t *entry = &env->metadata.data[i];
        core_put_u32(&body, (uint32_t)entry->owner_kind);
        core_put_text(&body, entry->owner_name);
        core_put_text(&body, entry->key);
        core_put_u32(&body, entry->version);
        core_put_bytes(&body, entry->data, entry->size);
    }
    bool ok = th_type_writer_finish(types, out);
    th_type_writer_free(types);
    if (ok) core_buffer_append(out, body.data, body.byte_len);
    core_buffer_release(&body);
    return ok;
}

typedef struct {
    core_byte_cursor_t in;
    th_type_reader_t *types;
    th_ctx *ctx;
} cursor_t;

static core_string_t
get_text(cursor_t *c)
{
    return th_cursor_text(&c->in, c->ctx);
}

static th_env_visibility_t
get_declared(cursor_t *c, core_string_t names[4])
{
    for (size_t i = 0; i < 4; i++) names[i] = get_text(c);
    return (th_env_visibility_t)core_cursor_u32(&c->in);
}

static th_type *
get_type(cursor_t *c)
{
    uint32_t index = core_cursor_u32(&c->in);
    if (!index) return nullptr;
    th_type *type = th_type_reader_get(c->types, index);
    if (!type) c->in.failed = true;
    return type;
}

static uint64_t
get_count(cursor_t *c)
{
    uint64_t count = core_cursor_u64(&c->in);
    if (count > (uint64_t)(c->in.end - c->in.at)) c->in.failed = true;
    return c->in.failed ? 0 : count;
}

th_env *
th_env_decode(th_ctx *context, const uint8_t *data, size_t length)
{
    if (length && !data) return nullptr;
    th_env *env = context ? th_env_new_in_context(context) : th_env_new();
    if (!env) return nullptr;
    th_ctx *types = env->types;
    size_t consumed = 0;
    th_type_reader_t *reader = th_type_reader_open(types, data, length,
                                                   &consumed);
    if (!reader) {
        th_env_free(env);
        return nullptr;
    }
    cursor_t c = {
        .in = core_cursor(data + consumed, length - consumed),
        .types = reader,
        .ctx = types,
    };
    bool ok = true;
    uint64_t count = get_count(&c);
    for (uint64_t i = 0; ok && !c.in.failed && i < count; i++) {
        core_string_t name = get_text(&c);
        core_string_t module = get_text(&c);
        core_string_t package = get_text(&c);
        th_env_visibility_t visibility = (th_env_visibility_t)core_cursor_u32(&c.in);
        th_env_type_shape_kind_t shape_kind =
            (th_env_type_shape_kind_t)core_cursor_u32(&c.in);
        th_type *shape = get_type(&c);
        ok = !c.in.failed && th_env_register_type(env, name, module, package,
                                               visibility);
        if (ok && shape_kind != TH_ENV_TYPE_SHAPE_NONE)
            ok = th_env_register_type_shape(env, name, shape_kind, shape);
    }
    count = get_count(&c);
    for (uint64_t i = 0; ok && !c.in.failed && i < count; i++) {
        core_string_t name = get_text(&c);
        core_string_t module = get_text(&c);
        core_string_t package = get_text(&c);
        th_env_visibility_t visibility = (th_env_visibility_t)core_cursor_u32(&c.in);
        th_type *contracts = get_type(&c);
        ok = !c.in.failed && th_env_register_trait_contracts(env, name, module,
            package, visibility, contracts);
    }
    count = get_count(&c);
    for (uint64_t i = 0; ok && !c.in.failed && i < count; i++) {
        core_string_t names[4];
        th_env_visibility_t visibility = get_declared(&c, names);
        uint64_t requirement_count = core_cursor_u64(&c.in);
        th_type *copied = get_type(&c);
        uint64_t required = core_cursor_u64(&c.in);
        if (requirement_count > SIZE_MAX / sizeof(core_string_t)) {
            ok = false;
            break;
        }
        th_con_kind *kinds = requirement_count
            ? core_alloc_array(th_con_kind, requirement_count) : nullptr;
        core_string_t *traits = requirement_count
            ? core_alloc_array(core_string_t, requirement_count) : nullptr;
        for (uint64_t r = 0; !c.in.failed && r < requirement_count; r++) {
            kinds[r] = (th_con_kind)core_cursor_u32(&c.in);
            traits[r] = get_text(&c);
        }
        th_env_function_t function = {
            .qualified_name = names[0], .root_name = names[1],
            .module = names[2], .package = names[3], .visibility = visibility,
            .required_params = required,
            .requirement_kinds = kinds, .requirement_traits = traits,
            .requirement_count = requirement_count,
        };
        ok = !c.in.failed && copied && th_env_copy_function(env, &function,
                                                         copied);
        core_free(kinds);
        core_free(traits);
    }
    count = get_count(&c);
    for (uint64_t i = 0; ok && !c.in.failed && i < count; i++) {
        core_string_t names[4];
        th_env_visibility_t visibility = get_declared(&c, names);
        th_type *type = get_type(&c);
        ok = !c.in.failed && th_env_register_value(env, names[0], names[1],
            names[2], names[3], visibility, type);
    }
    count = get_count(&c);
    for (uint64_t i = 0; ok && !c.in.failed && i < count; i++) {
        core_string_t trait = get_text(&c);
        th_type *receiver = get_type(&c);
        ok = !c.in.failed && th_env_register_intrinsic_type_pattern(env, trait,
                                                                 receiver);
    }
    count = get_count(&c);
    for (uint64_t i = 0; ok && !c.in.failed && i < count; i++) {
        th_env_metadata_owner_t kind = (th_env_metadata_owner_t)core_cursor_u32(&c.in);
        core_string_t owner = get_text(&c);
        core_string_t key = get_text(&c);
        uint32_t version = core_cursor_u32(&c.in);
        size_t size = 0;
        const uint8_t *bytes = core_cursor_bytes(&c.in, &size);
        ok = !c.in.failed && th_env_register_metadata(env, kind, owner, key,
            version, bytes, size);
    }
    th_type_reader_free(reader);
    if (!ok || c.in.failed || !th_env_freeze(env)) {
        th_env_free(env);
        return nullptr;
    }
    return env;
}
