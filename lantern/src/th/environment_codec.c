#include <string.h>

#include "private/th/internal.h"
#include "lib/alloc.h"

static void
put_u32(core_buffer_t *out, uint32_t value)
{
    uint8_t bytes[4];
    for (int i = 0; i < 4; i++) bytes[i] = (uint8_t)(value >> (8 * i));
    core_buffer_append(out, (const char *)bytes, 4);
}

static void
put_u64(core_buffer_t *out, uint64_t value)
{
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (8 * i));
    core_buffer_append(out, (const char *)bytes, 8);
}

static void
put_bytes(core_buffer_t *out, const void *data, size_t length)
{
    put_u64(out, data ? length : UINT64_MAX);
    if (data && length) core_buffer_append(out, data, length);
}

static void
put_text(core_buffer_t *out, const char *text)
{
    put_bytes(out, text, text ? strlen(text) : 0);
}

bool
th_env_encode(const th_env *env, core_buffer_t *out)
{
    if (!env || !out) return false;
    th_type_writer_t *types = th_type_writer_new();
    if (!types) return false;
    core_buffer_t body = {0};
    put_u64(&body, env->type_origins.len);
    for (size_t i = 0; i < env->type_origins.len; i++) {
        const th_env_type_origin_t *entry = &env->type_origins.data[i];
        put_text(&body, entry->name);
        put_text(&body, entry->module);
        put_text(&body, entry->package);
        put_u32(&body, (uint32_t)entry->visibility);
        put_u32(&body, (uint32_t)entry->shape_kind);
        put_u32(&body, th_type_writer_add(types, entry->shape));
    }
    put_u64(&body, env->traits.len);
    for (size_t i = 0; i < env->traits.len; i++) {
        const th_env_trait_t *entry = &env->traits.data[i];
        put_text(&body, entry->name);
        put_text(&body, entry->module);
        put_text(&body, entry->package);
        put_u32(&body, (uint32_t)entry->visibility);
        put_u32(&body, th_type_writer_add(types, entry->contracts));
    }
    put_u64(&body, env->functions.len);
    for (size_t i = 0; i < env->functions.len; i++) {
        const th_env_function_t *entry = &env->functions.data[i];
        put_text(&body, entry->qualified_name);
        put_text(&body, entry->root_name);
        put_text(&body, entry->module);
        put_text(&body, entry->package);
        put_u32(&body, (uint32_t)entry->visibility);
        put_u32(&body, th_type_writer_add(types, entry->contract));
    }
    put_u64(&body, env->values.len);
    for (size_t i = 0; i < env->values.len; i++) {
        const th_env_value_t *entry = &env->values.data[i];
        put_text(&body, entry->qualified_name);
        put_text(&body, entry->root_name);
        put_text(&body, entry->module);
        put_text(&body, entry->package);
        put_u32(&body, (uint32_t)entry->visibility);
        put_u32(&body, th_type_writer_add(types, entry->type));
    }
    put_u64(&body, env->intrinsic_types.len);
    for (size_t i = 0; i < env->intrinsic_types.len; i++) {
        const th_env_intrinsic_type_t *entry = &env->intrinsic_types.data[i];
        put_text(&body, entry->trait_name);
        put_u32(&body, th_type_writer_add(types, entry->receiver_pattern));
    }
    put_u64(&body, env->metadata.len);
    for (size_t i = 0; i < env->metadata.len; i++) {
        const th_env_metadata_record_t *entry = &env->metadata.data[i];
        put_u32(&body, (uint32_t)entry->owner_kind);
        put_text(&body, entry->owner_name);
        put_text(&body, entry->key);
        put_u32(&body, entry->version);
        put_bytes(&body, entry->data, entry->size);
    }
    bool ok = th_type_writer_finish(types, out);
    th_type_writer_free(types);
    if (ok) core_buffer_append(out, body.data, body.byte_len);
    core_buffer_release(&body);
    return ok;
}

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t at;
    bool failed;
    th_type_reader_t *types;
    th_ctx *ctx;
} cursor_t;

static uint32_t
get_u32(cursor_t *c)
{
    if (c->failed || c->length - c->at < 4) {
        c->failed = true;
        return 0;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) value |= (uint32_t)c->data[c->at + i] << (8 * i);
    c->at += 4;
    return value;
}

static uint64_t
get_u64(cursor_t *c)
{
    if (c->failed || c->length - c->at < 8) {
        c->failed = true;
        return 0;
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value |= (uint64_t)c->data[c->at + i] << (8 * i);
    c->at += 8;
    return value;
}

static const uint8_t *
get_bytes(cursor_t *c, size_t *length)
{
    uint64_t size = get_u64(c);
    *length = 0;
    if (c->failed || size == UINT64_MAX) return nullptr;
    if (size > c->length - c->at) {
        c->failed = true;
        return nullptr;
    }
    const uint8_t *start = c->data + c->at;
    c->at += (size_t)size;
    *length = (size_t)size;
    return start;
}

static const char *
get_text(cursor_t *c)
{
    size_t length = 0;
    const uint8_t *bytes = get_bytes(c, &length);
    if (!bytes) return nullptr;
    char *text = th_strdup(c->ctx, (const char *)bytes, length);
    if (!text) c->failed = true;
    return text;
}

static th_type *
get_type(cursor_t *c)
{
    uint32_t index = get_u32(c);
    if (!index) return nullptr;
    th_type *type = th_type_reader_get(c->types, index);
    if (!type) c->failed = true;
    return type;
}

static uint64_t
get_count(cursor_t *c)
{
    uint64_t count = get_u64(c);
    if (count > c->length) c->failed = true;
    return c->failed ? 0 : count;
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
        .data = data, .length = length, .at = consumed, .types = reader,
        .ctx = types,
    };
    bool ok = true;
    uint64_t count = get_count(&c);
    for (uint64_t i = 0; ok && !c.failed && i < count; i++) {
        const char *name = get_text(&c);
        const char *module = get_text(&c);
        const char *package = get_text(&c);
        th_env_visibility_t visibility = (th_env_visibility_t)get_u32(&c);
        th_env_type_shape_kind_t shape_kind =
            (th_env_type_shape_kind_t)get_u32(&c);
        th_type *shape = get_type(&c);
        ok = !c.failed && th_env_register_type(env, name, module, package,
                                               visibility);
        if (ok && shape_kind != TH_ENV_TYPE_SHAPE_NONE)
            ok = th_env_register_type_shape(env, name, shape_kind, shape);
    }
    count = get_count(&c);
    for (uint64_t i = 0; ok && !c.failed && i < count; i++) {
        const char *name = get_text(&c);
        const char *module = get_text(&c);
        const char *package = get_text(&c);
        th_env_visibility_t visibility = (th_env_visibility_t)get_u32(&c);
        th_type *contracts = get_type(&c);
        ok = !c.failed && th_env_register_trait_contracts(env, name, module,
            package, visibility, contracts);
    }
    count = get_count(&c);
    for (uint64_t i = 0; ok && !c.failed && i < count; i++) {
        const char *qualified = get_text(&c);
        const char *root = get_text(&c);
        const char *module = get_text(&c);
        const char *package = get_text(&c);
        th_env_visibility_t visibility = (th_env_visibility_t)get_u32(&c);
        th_type *contract = get_type(&c);
        ok = !c.failed && th_env_register_function_contract(env, qualified,
            root, module, package, visibility, contract);
    }
    count = get_count(&c);
    for (uint64_t i = 0; ok && !c.failed && i < count; i++) {
        const char *qualified = get_text(&c);
        const char *root = get_text(&c);
        const char *module = get_text(&c);
        const char *package = get_text(&c);
        th_env_visibility_t visibility = (th_env_visibility_t)get_u32(&c);
        th_type *type = get_type(&c);
        ok = !c.failed && th_env_register_value(env, qualified, root, module,
            package, visibility, type);
    }
    count = get_count(&c);
    for (uint64_t i = 0; ok && !c.failed && i < count; i++) {
        const char *trait = get_text(&c);
        th_type *receiver = get_type(&c);
        ok = !c.failed && th_env_register_intrinsic_type_pattern(env, trait,
                                                                 receiver);
    }
    count = get_count(&c);
    for (uint64_t i = 0; ok && !c.failed && i < count; i++) {
        th_env_metadata_owner_t kind = (th_env_metadata_owner_t)get_u32(&c);
        const char *owner = get_text(&c);
        const char *key = get_text(&c);
        uint32_t version = get_u32(&c);
        size_t size = 0;
        const uint8_t *bytes = get_bytes(&c, &size);
        ok = !c.failed && th_env_register_metadata(env, kind, owner, key,
            version, bytes, size);
    }
    th_type_reader_free(reader);
    if (!ok || c.failed || !th_env_freeze(env)) {
        th_env_free(env);
        return nullptr;
    }
    return env;
}
