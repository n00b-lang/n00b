#include "interface_summaries.h"

#include <string.h>

#include "compiler_tu.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "lib/hash.h"
#include "th/environment.h"

#define SUMMARY_KEY   "n00b.module-summaries"
#define SUMMARY_OWNER "summaries:"

enum { SUMMARY_METADATA_VERSION = 1 };

typedef struct {
    core_string_t module;
    core_string_t function;
    const uint8_t *data;
    size_t size;
} interface_summary_t;

typedef interface_summary_t *interface_summary_ptr_t;
core_list_decl(interface_summary_ptr_t);

struct n00bb_interface_summaries_t {
    core_dict_t by_function;
    core_list_t(interface_summary_ptr_t) owned;
};

static core_hash_value_t
summary_hash(void *key)
{
    const interface_summary_t *entry = key;
    return core_string_hash(entry->module) * 31
        + core_string_hash(entry->function);
}

static bool
summary_equal(const void *left, const void *right)
{
    const interface_summary_t *a = left, *b = right;
    return core_string_eq(a->module, b->module)
        && core_string_eq(a->function, b->function);
}

static void
put_u32(core_buffer_t *out, uint32_t value)
{
    uint8_t bytes[4];
    for (int i = 0; i < 4; i++) bytes[i] = (uint8_t)(value >> (8 * i));
    core_buffer_append(out, (const char *)bytes, 4);
}

static bool
module_summaries(ir_summaries_t *summaries, ir_module_t *module,
                 core_buffer_t *out)
{
    core_buffer_t encoded = {0};
    uint32_t count = 0;
    bool ok = true;
    for (size_t f = 0; ok && f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        ir_summary_t *summary = function->n_blocks
            ? ir_summary_of(summaries, function) : nullptr;
        if (!summary) continue;
        core_buffer_t bytes = {0};
        ok = ir_summary_encode(summary, &bytes);
        put_u32(&encoded, (uint32_t)function->name.u8_bytes);
        core_buffer_append(&encoded, function->name.data,
                           function->name.u8_bytes);
        put_u32(&encoded, (uint32_t)bytes.byte_len);
        core_buffer_append(&encoded, bytes.data, bytes.byte_len);
        core_buffer_release(&bytes);
        count++;
    }
    put_u32(out, count);
    core_buffer_append(out, encoded.data, encoded.byte_len);
    core_buffer_release(&encoded);
    return ok;
}

bool
n00bb_publish_function_summaries(n00bb_link_analysis_t *analysis)
{
    th_env *environment = analysis ? analysis->interface_environment
                                   : nullptr;
    if (!environment || th_env_is_frozen(environment)
        || !analysis->summaries) return true;
    core_buffer_t owner = {0};
    core_buffer_t data = {0};
    bool ok = true;
    for (size_t u = 0; ok && u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        if (!state || !state->ir_module || state->interface_module) continue;
        owner.byte_len = 0;
        data.byte_len = 0;
        core_buffer_puts(&owner, SUMMARY_OWNER);
        core_buffer_append_string(&owner, ir_module_name(state->ir_module));
        core_string_t name = core_buffer_string(&owner);
        ok = module_summaries(analysis->summaries, state->ir_module, &data)
            && (th_env_replace_metadata(environment,
                    TH_ENV_METADATA_ENVIRONMENT, name,
                    CORE_STRING_STATIC(SUMMARY_KEY),
                    SUMMARY_METADATA_VERSION, data.data, data.byte_len)
                || th_env_register_metadata(environment,
                    TH_ENV_METADATA_ENVIRONMENT, name,
                    CORE_STRING_STATIC(SUMMARY_KEY),
                    SUMMARY_METADATA_VERSION, data.data, data.byte_len));
    }
    core_buffer_release(&owner);
    core_buffer_release(&data);
    return ok;
}

static bool
read_u32(const uint8_t *data, size_t size, size_t *at, uint32_t *out)
{
    if (size - *at < 4) return false;
    *out = 0;
    for (int i = 0; i < 4; i++)
        *out |= (uint32_t)data[*at + (size_t)i] << (8 * i);
    *at += 4;
    return true;
}

static void
index_module(n00bb_interface_summaries_t *index, core_string_t module,
             const uint8_t *data, size_t size)
{
    size_t at = 0;
    uint32_t count = 0;
    if (!read_u32(data, size, &at, &count)) return;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t name_bytes = 0, summary_bytes = 0;
        if (!read_u32(data, size, &at, &name_bytes)
            || size - at < name_bytes) return;
        core_string_t function = core_string_borrow(
            (const char *)data + at, name_bytes);
        at += name_bytes;
        if (!read_u32(data, size, &at, &summary_bytes)
            || size - at < summary_bytes) return;
        interface_summary_t *entry = core_alloc(interface_summary_t);
        *entry = (interface_summary_t){
            .module = module, .function = function,
            .data = data + at, .size = summary_bytes,
        };
        at += summary_bytes;
        if (core_dict_add(&index->by_function, entry, entry))
            core_list_push(index->owned, entry);
        else core_free(entry);
    }
}

static void
index_environment(n00bb_interface_summaries_t *index,
                  const th_env *environment)
{
    core_string_t prefix = CORE_STRING_STATIC(SUMMARY_OWNER);
    for (size_t i = 0; environment
         && i < th_env_metadata_count(environment); i++) {
        th_env_metadata_view_t view = {0};
        if (!th_env_metadata_at(environment, i, &view)
            || view.owner_kind != TH_ENV_METADATA_ENVIRONMENT
            || view.version != SUMMARY_METADATA_VERSION
            || !core_string_eq(view.key, CORE_STRING_STATIC(SUMMARY_KEY))
            || !core_string_starts_with(view.owner_name, prefix)) continue;
        index_module(index, core_string_slice(view.owner_name,
                         prefix.u8_bytes, view.owner_name.u8_bytes),
                     view.data, view.size);
    }
}

static n00bb_interface_summaries_t *
interface_index(n00bb_link_analysis_t *analysis)
{
    if (analysis->interface_summaries) return analysis->interface_summaries;
    n00bb_interface_summaries_t *index =
        core_alloc(n00bb_interface_summaries_t);
    core_dict_init(&index->by_function, summary_hash, summary_equal);
    index->owned = core_list_new(interface_summary_ptr_t);
    for (size_t e = 0; e < analysis->linked_interface_count; e++)
        index_environment(index, analysis->linked_interfaces[e]);
    analysis->interface_summaries = index;
    return index;
}

static const ir_global_t *
resolve_global(void *context, core_string_t module, core_string_t name)
{
    n00bb_link_analysis_t *analysis = context;
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        ir_module_t *candidate = state ? state->ir_module : nullptr;
        if (!candidate || !core_string_eq(ir_module_name(candidate), module))
            continue;
        ir_global_t *global = ir_global_find(candidate, name);
        return global ? ir_global_identity(global) : nullptr;
    }
    return nullptr;
}

ir_summary_t *
n00bb_interface_summary(n00bb_link_analysis_t *analysis,
                        ir_summaries_t *summaries, const ir_func_t *function)
{
    if (!analysis || !function || function->n_blocks
        || !analysis->linked_interface_count) return nullptr;
    core_string_t module = ir_module_name(function->owner);
    core_string_t name   = function->name;
    if (name.u8_bytes > module.u8_bytes
        && core_string_starts_with(name, module)
        && name.data[module.u8_bytes] == '.')
        name = core_string_slice(name, module.u8_bytes + 1, name.u8_bytes);
    interface_summary_t probe = {.module = module, .function = name};
    bool found = false;
    interface_summary_t *entry = core_dict_get(
        &interface_index(analysis)->by_function, &probe, &found);
    return found ? ir_summary_decode(summaries, function, entry->data,
                                     entry->size, resolve_global, analysis)
                 : nullptr;
}

void
n00bb_interface_summaries_free(n00bb_interface_summaries_t *index)
{
    if (!index) return;
    for (size_t i = 0; i < index->owned.len; i++)
        core_free(index->owned.data[i]);
    core_list_free(index->owned);
    core_dict_free(&index->by_function);
    core_free(index);
}
