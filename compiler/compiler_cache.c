#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cache_location.h"
#include "compiler_cache.h"
#include "compiler_lower.h"
#include "compiler_phases.h"
#include "compiler_tu.h"
#include "config.h"
#include "driver.h"
#include "builtin_types.h"
#include "n00bb_version.h"
#include "cached_body.h"
#include "interface_summaries.h"
#include "ir/analyses.h"
#include "ir/archive.h"
#include "ir/summary.h"
#include "lib/alloc.h"
#include "lib/os.h"
#include "lib/buffer.h"
#include "lib/hash.h"
#include "th/environment.h"

#define CACHE_MAGIC   UINT32_C(0x4f42304e)
#define CACHE_VERSION UINT32_C(5)

core_list_decl(size_t);

typedef struct {
    core_string_t name;
    uint8_t *data;
    size_t length;
} cache_section_t;
core_list_decl(cache_section_t);

typedef struct {
    size_t index;
    core_list_t(core_translation_unit_ptr_t) members;
    core_list_t(size_t) imports;
    uint64_t freshness;
    uint64_t key;
    uint8_t *interface;
    size_t interface_bytes;
    core_list_t(size_t) inlined;
    bool root;
    bool cached;
} cache_scc_t;
core_list_decl(cache_scc_t);

static uint64_t
buffer_hash(const core_buffer_t *buffer)
{
    return core_xxh3_64bits_raw(buffer->data ? buffer->data : "",
                                buffer->byte_len);
}

static bool
read_file(core_string_t path, uint8_t **data, size_t *length)
{
    char *bytes = nullptr;
    *data = nullptr;
    *length = 0;
    if (!core_os_read_file(path, &bytes, length)) return false;
    *data = (uint8_t *)bytes;
    return true;
}

static void
cache_log(const char *format, ...)
{
    char *path = core_string_to_cstr(
        n00bb_setting_string(N00BB_KEY_DEV_CACHE_LOG));
    FILE *log = path && *path ? fopen(path, "a") : nullptr;
    core_free(path);
    if (!log) return;
    va_list arguments;
    va_start(arguments, format);
    vfprintf(log, format, arguments);
    va_end(arguments);
    fputc('\n', log);
    fclose(log);
}

static void
sections_release(core_list_t(cache_section_t) *sections)
{
    for (size_t i = 0; i < sections->len; i++) {
        core_free(sections->data[i].name.data);
        free(sections->data[i].data);
    }
    core_list_free(*sections);
}

static void
sections_serialize(const core_list_t(cache_section_t) *sections,
                   core_buffer_t *out)
{
    core_put_u32(out, CACHE_MAGIC);
    core_put_u32(out, CACHE_VERSION);
    core_put_u32(out, (uint32_t)sections->len);
    for (size_t i = 0; i < sections->len; i++) {
        core_put_text(out, sections->data[i].name);
        core_put_bytes(out, sections->data[i].data, sections->data[i].length);
    }
}

static bool
sections_set(core_list_t(cache_section_t) *sections, core_string_t name,
             const void *data, size_t length)
{
    uint8_t *copy = malloc(length ? length : 1);
    if (!copy) return false;
    if (length) memcpy(copy, data, length);
    for (size_t i = 0; i < sections->len; i++) {
        if (!core_string_eq(sections->data[i].name, name)) continue;
        free(sections->data[i].data);
        sections->data[i].data = copy;
        sections->data[i].length = length;
        return true;
    }
    core_string_t owned = core_string_dup(name);
    if (!owned.data) {
        free(copy);
        return false;
    }
    core_list_push(*sections, ((cache_section_t){owned, copy, length}));
    return true;
}

static bool
sections_parse(const uint8_t *data, size_t length,
               core_list_t(cache_section_t) *sections)
{
    core_byte_cursor_t in = core_cursor(data, length);
    if (core_cursor_u32(&in) != CACHE_MAGIC
        || core_cursor_u32(&in) != CACHE_VERSION) return false;
    uint32_t count = core_cursor_u32(&in);
    for (uint32_t s = 0; !in.failed && s < count; s++) {
        core_string_t name = core_cursor_text(&in);
        size_t bytes = 0;
        const uint8_t *start = core_cursor_bytes(&in, &bytes);
        if (in.failed || !name.data
            || !sections_set(sections, name, start, bytes)) return false;
    }
    return !in.failed;
}

static bool cache_check_only;

void
n00bb_cache_set_check_only(bool check_only)
{
    cache_check_only = check_only;
}

static bool
objects_optional(void)
{
    return cache_check_only;
}


static core_string_t
phase_name(void)
{
    return cache_check_only ? CORE_STRING_STATIC("check")
                            : CORE_STRING_STATIC("program");
}

static int
compare_units(const void *left, const void *right)
{
    const core_translation_unit_t *a = *(core_translation_unit_t *const *)left;
    const core_translation_unit_t *b = *(core_translation_unit_t *const *)right;
    return core_string_cmp(a->module.identifier, b->module.identifier);
}

static size_t
scc_of_unit(const core_list_t(cache_scc_t) *sccs,
            const core_translation_unit_t *unit)
{
    for (size_t s = 0; s < sccs->len; s++)
        for (size_t m = 0; m < sccs->data[s].members.len; m++)
            if (sccs->data[s].members.data[m] == unit) return s;
    return SIZE_MAX;
}

static size_t
scc_of_module_name(const core_list_t(cache_scc_t) *sccs, core_string_t name)
{
    for (size_t s = 0; s < sccs->len; s++)
        for (size_t m = 0; m < sccs->data[s].members.len; m++) {
            core_string_t identifier =
                sccs->data[s].members.data[m]->module.identifier;
            if (core_string_eq(identifier, name)) return s;
        }
    return SIZE_MAX;
}

static bool
module_member(const cache_scc_t *scc, core_string_t module)
{
    for (size_t m = 0; module.data && m < scc->members.len; m++)
        if (core_string_eq(scc->members.data[m]->module.identifier, module))
            return true;
    return false;
}

typedef struct {
    const cache_scc_t *scc;
    const core_list_t(cache_scc_t) *sccs;
} interface_filter_t;

static size_t
owner_module_length(const core_list_t(cache_scc_t) *sccs, core_string_t owner)
{
    size_t best = 0;
    size_t length = owner.data ? owner.u8_bytes : 0;
    for (size_t s = 0; s < sccs->len; s++)
        for (size_t m = 0; m < sccs->data[s].members.len; m++) {
            core_string_t identifier =
                sccs->data[s].members.data[m]->module.identifier;
            if (identifier.u8_bytes < length && identifier.u8_bytes > best
                && owner.data[identifier.u8_bytes] == '.'
                && !memcmp(identifier.data, owner.data, identifier.u8_bytes))
                best = identifier.u8_bytes;
        }
    return best;
}

static bool
scc_declares_package(const cache_scc_t *scc, core_string_t name)
{
    for (size_t m = 0; m < scc->members.len; m++) {
        n00bb_tu_state_t *state = tu_state(scc->members.data[m]);
        for (size_t p = 0; state && p < state->package_declarations.len; p++)
            if (core_string_eq(state->package_declarations.data[p].name, name))
                return true;
    }
    return false;
}

static bool
filter_module(void *context, core_string_t module)
{
    interface_filter_t *filter = context;
    return module.data && module_member(filter->scc, module);
}

static bool
filter_metadata(void *context, th_env_metadata_owner_t kind,
                core_string_t owner)
{
    interface_filter_t *filter = context;
    if (!owner.data || kind != TH_ENV_METADATA_ENVIRONMENT) return false;
    return module_member(filter->scc, owner)
        || scc_declares_package(filter->scc, owner);
}

static bool
filter_intrinsic(void *context, core_string_t trait, const th_type *receiver)
{
    interface_filter_t *filter = context;
    (void)trait;
    core_string_t name = {0};
    if (!receiver || !th_named_info((th_type *)receiver, &name, nullptr,
                                    nullptr) || !name.data) return false;
    size_t module = owner_module_length(filter->sccs, name);
    return module
        && module_member(filter->scc, core_string_slice(name, 0, module));
}

static bool
interface_bytes(n00bb_link_analysis_t *analysis,
                const core_list_t(cache_scc_t) *sccs, cache_scc_t *scc)
{
    interface_filter_t context = {.scc = scc, .sccs = sccs};
    th_env_filter_t filter = {
        .module = filter_module, .metadata = filter_metadata,
        .intrinsic = filter_intrinsic, .context = &context,
    };
    th_env *subset = analysis->interface_environment
        ? th_env_subset(analysis->interface_environment, &filter) : nullptr;
    if (!subset) return false;
    core_buffer_t encoded = {0};
    bool ok = th_env_encode(subset, &encoded);
    if (ok) {
        scc->interface = malloc(encoded.byte_len ? encoded.byte_len : 1);
        ok = scc->interface != nullptr;
        if (ok && encoded.byte_len)
            memcpy(scc->interface, encoded.data, encoded.byte_len);
        scc->interface_bytes = encoded.byte_len;
    }
    core_buffer_release(&encoded);
    th_env_free(subset);
    return ok;
}

static void
append_flags(core_buffer_t *out, parse_cmdr_result_t *args)
{
    static const n00bb_setting_id_t settings[] = {
        N00BB_KEY_BUILD_RELEASE, N00BB_KEY_BUILD_NATIVE,
        N00BB_KEY_BUILD_UNCHECKED_WIDE_NARROWING, N00BB_KEY_BUILD_PANIC_TRAP,
        N00BB_KEY_BUILD_LTO, N00BB_KEY_BUILD_ATOMIC128,
    };
    for (size_t i = 0; i < sizeof(settings) / sizeof(*settings); i++)
        core_put_u32(out, n00bb_setting_bool(settings[i]));
    core_put_u32(out, (uint32_t)n00bb_setting_int(N00BB_KEY_DEV_OPT_LEVEL));
    static const core_string_t flags[] = {
        CORE_STRING_STATIC("--gc-conservative-stacks"),
        CORE_STRING_STATIC("--no-specialize"),
        CORE_STRING_STATIC("--closed-world"),
        CORE_STRING_STATIC("--no-closed-world"),
        CORE_STRING_STATIC("--function-summaries"),
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(*flags); i++)
        core_put_u32(out, parse_cmdr_flag_present(args, flags[i]));
    n00bb_setting_id_t defines = N00BB_KEY_BUILD_DEFINES;
    core_put_u32(out, (uint32_t)n00bb_setting_count(defines));
    for (size_t i = 0; i < n00bb_setting_count(defines); i++) {
        core_string_t value = n00bb_setting_at(defines, i);
        core_put_bytes(out, value.data, value.u8_bytes);
    }
    static const core_string_t valued[] = {
        CORE_STRING_STATIC("--native-root"),
    };
    for (size_t f = 0; f < sizeof(valued) / sizeof(*valued); f++) {
        core_string_t flag = valued[f];
        int32_t count = parse_cmdr_flag_value_count(args, flag);
        core_put_u32(out, (uint32_t)(count > 0 ? count : 0));
        for (int32_t i = 0; i < count; i++) {
            core_string_t value = parse_cmdr_flag_str_at(args, flag, i);
            core_put_bytes(out, value.data, value.u8_bytes);
        }
    }
}

static core_string_t canonical_path(core_string_t path);

static void
generated_source_bytes(core_string_t owner_path, core_string_t inputs,
                       core_buffer_t *out)
{
    uint8_t *bytes = nullptr;
    size_t length = 0;
    if (read_file(owner_path, &bytes, &length) && bytes)
        core_buffer_append(out, (const char *)bytes, length);
    core_free(bytes);
    size_t at = 0;
    while (inputs.data && at < inputs.u8_bytes) {
        size_t end = at;
        while (end < inputs.u8_bytes && inputs.data[end] != '\n') end++;
        core_string_t path = core_string_slice(inputs, at, end - at);
        at = end + 1;
        if (!path.u8_bytes) continue;
        core_put_text(out, path);
        bytes = nullptr;
        length = 0;
        if (read_file(path, &bytes, &length) && bytes)
            core_buffer_append(out, (const char *)bytes, length);
        core_free(bytes);
    }
}

static void
freshness_member(core_buffer_t *input, const void *name, size_t name_bytes,
                 const void *source, size_t source_bytes)
{
    core_put_bytes(input, name, name_bytes);
    core_put_bytes(input, source, source_bytes);
}

static void
scc_freshness(cache_scc_t *scc)
{
    core_buffer_t input = {0};
    core_buffer_puts(&input, "n00b.scc.freshness.v1");
    for (size_t m = 0; m < scc->members.len; m++) {
        core_translation_unit_t *unit = scc->members.data[m];
        core_string_t owner = {0}, owner_path = {0};
        if (n00bb_source_file_generated_owner(&unit->source, &owner,
                                              &owner_path)) {
            core_string_t canonical = canonical_path(owner_path);
            core_buffer_t made = {0};
            generated_source_bytes(canonical,
                n00bb_source_file_generated_inputs(&unit->source), &made);
            core_free(canonical.data);
            freshness_member(&input, unit->module.identifier.data,
                             unit->module.identifier.u8_bytes,
                             made.data ? made.data : "", made.byte_len);
            core_buffer_release(&made);
        } else {
            uint8_t *read = nullptr;
            size_t read_bytes = 0;
            bool cached = !core_source_file_size(&unit->source)
                && read_file(core_source_file_path(&unit->source), &read,
                             &read_bytes);
            freshness_member(&input, unit->module.identifier.data,
                             unit->module.identifier.u8_bytes, cached
                                 ? (const char *)read
                                 : core_source_file_contents(&unit->source).data,
                             cached ? read_bytes
                                    : core_source_file_size(&unit->source));
            core_free(read);
        }
        for (size_t i = 0; i < core_translation_unit_import_count(unit);
             i++) {
            core_string_t name =
                core_translation_unit_import(unit, i)->module.identifier;
            core_put_bytes(&input, name.data, name.u8_bytes);
        }
    }
    scc->freshness = buffer_hash(&input);
    core_buffer_release(&input);
}

static void
inline_dependencies(const core_list_t(cache_scc_t) *sccs, cache_scc_t *scc,
                    core_list_t(size_t) *out)
{
    for (size_t m = 0; m < scc->members.len; m++) {
        ir_module_t *module = tu_state(scc->members.data[m])->ir_module;
        for (size_t f = 0; module && f < ir_func_count(module); f++) {
            ir_func_t *function = ir_func_at(module, f);
            for (uint32_t b = 0; b < function->n_blocks; b++)
                for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                    ir_instr_t *call = function->blocks[b]->instrs[i];
                    ir_func_t *target = ir_call_function(call);
                    if (!target || !ir_func_is_always_inline(target))
                        continue;
                    size_t owner = scc_of_module_name(sccs,
                        ir_module_name(target->owner));
                    if (owner == SIZE_MAX || owner == scc->index) continue;
                    bool seen = false;
                    for (size_t k = 0; k < out->len; k++)
                        seen |= out->data[k] == owner;
                    if (!seen) core_list_push(*out, owner);
                }
        }
    }
}

static void
sccs_release(core_list_t(cache_scc_t) *sccs)
{
    for (size_t s = 0; s < sccs->len; s++) {
        free(sccs->data[s].interface);
        core_list_free(sccs->data[s].members);
        core_list_free(sccs->data[s].imports);
        core_list_free(sccs->data[s].inlined);
    }
    core_list_free(*sccs);
}

static void
close_imports(core_list_t(cache_scc_t) *sccs)
{
    for (size_t s = 0; s < sccs->len; s++) {
        cache_scc_t *scc = &sccs->data[s];
        for (size_t k = 0; k < scc->imports.len; k++) {
            const cache_scc_t *imported = &sccs->data[scc->imports.data[k]];
            for (size_t j = 0; j < imported->imports.len; j++) {
                size_t next = imported->imports.data[j];
                bool seen = next == s;
                for (size_t n = 0; !seen && n < scc->imports.len; n++)
                    seen = scc->imports.data[n] == next;
                if (!seen) core_list_push(scc->imports, next);
            }
        }
    }
}

static bool
collect_sccs(core_link_unit_t *link, core_list_t(cache_scc_t) *sccs,
             bool lowered)
{
    core_translation_unit_t *root = core_link_unit_root(link, 0);
    for (size_t g = 0; g < core_link_unit_scc_count(link); g++) {
        cache_scc_t scc = {
            .index = sccs->len,
            .members = core_list_new(core_translation_unit_ptr_t),
            .imports = core_list_new(size_t),
            .inlined = core_list_new(size_t),
        };
        for (size_t m = 0; m < core_link_unit_scc_member_count(link, g); m++) {
            core_translation_unit_t *unit = core_link_unit_scc_member(link,
                                                                      g, m);
            n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
            if (!state || (lowered && !state->ir_module)) {
                core_list_free(scc.members);
                core_list_free(scc.imports);
                core_list_free(scc.inlined);
                return false;
            }
            if (state->interface_module) scc.cached = true;
            if (unit == root) scc.root = true;
            core_list_push(scc.members, unit);
        }
        qsort(scc.members.data, scc.members.len, sizeof(*scc.members.data),
              compare_units);
        core_list_push(*sccs, scc);
    }
    for (size_t s = 0; s < sccs->len; s++) {
        cache_scc_t *scc = &sccs->data[s];
        for (size_t m = 0; m < scc->members.len; m++) {
            core_translation_unit_t *unit = scc->members.data[m];
            for (size_t i = 0; i < core_translation_unit_import_count(unit);
                 i++) {
                size_t imported = scc_of_unit(sccs,
                    core_translation_unit_import(unit, i));
                if (imported == SIZE_MAX || imported == s) continue;
                bool seen = false;
                for (size_t k = 0; k < scc->imports.len; k++)
                    seen |= scc->imports.data[k] == imported;
                if (!seen) core_list_push(scc->imports, imported);
            }
        }
        scc_freshness(scc);
        if (lowered) inline_dependencies(sccs, scc, &scc->inlined);
    }
    close_imports(sccs);
    return true;
}

typedef const th_env *cache_environment_t;
core_list_decl(cache_environment_t);

typedef struct {
    core_string_t name;
    core_string_t path;
    uint64_t source_hash;
    core_list_t(core_string_t) imports;
    ir_module_t *module;
    core_string_t owner;
    core_string_t owner_path;
    core_buffer_t inputs;
} cache_member_t;
core_list_decl(cache_member_t);

typedef struct {
    core_string_t name;
    uint64_t value;
} cache_link_t;
core_list_decl(cache_link_t);

constexpr size_t cache_module_candidates = 32;
constexpr size_t cache_foreign_candidates = 4;

typedef enum {
    ENTRY_UNKNOWN,
    ENTRY_VISITING,
    ENTRY_VALID,
    ENTRY_INVALID,
} entry_state_t;

typedef struct {
    uint64_t key;
    uint64_t freshness;
    core_list_t(cache_member_t) members;
    core_list_t(cache_link_t) imports;
    core_list_t(cache_link_t) inlined;
    core_list_t(core_string_t) references;
    core_list_t(cache_section_t) sections;
    bool fresh;
    bool excluded;
    bool loaded;
    entry_state_t state;
    entry_state_t load;
    th_env *interface;
    th_type_reader_t *types;
} cache_entry_t;
core_list_decl(cache_entry_t);

typedef ir_module_t *cache_module_ptr_t;
core_list_decl(cache_module_ptr_t);

static const core_string_t summary_sections[N00BB_PHASE_FINAL] = {
    CORE_STRING_STATIC("escapes"),
    CORE_STRING_STATIC("summaries"),
    CORE_STRING_STATIC("final"),
};

typedef struct {
    core_string_t module;
    core_buffer_t ir;
    core_buffer_t references;
    core_list_t(core_string_t) inlined;
    core_list_t(cache_module_ptr_t) modules;
    core_list_t(core_string_t) names;
    core_buffer_t summaries[N00BB_PHASE_FINAL];
    bool captured[N00BB_PHASE_FINAL];
    th_type_writer_t *types;
} cache_snapshot_t;
core_list_decl(cache_snapshot_t);

static void
snapshot_release(cache_snapshot_t *snapshot)
{
    core_free(snapshot->module.data);
    core_buffer_release(&snapshot->ir);
    core_buffer_release(&snapshot->references);
    for (size_t i = 0; i < snapshot->inlined.len; i++)
        core_free(snapshot->inlined.data[i].data);
    core_list_free(snapshot->inlined);
    for (size_t i = 0; i < snapshot->names.len; i++)
        core_free(snapshot->names.data[i].data);
    core_list_free(snapshot->names);
    core_list_free(snapshot->modules);
    for (size_t p = 0; p < N00BB_PHASE_FINAL; p++)
        core_buffer_release(&snapshot->summaries[p]);
    th_type_writer_free(snapshot->types);
}

static void
capture_snapshot(cache_snapshot_t *snapshot, n00bb_link_analysis_t *analysis,
                 ir_summaries_t *summaries, uint8_t phase)
{
    core_buffer_t *out = &snapshot->summaries[phase - 1];
    out->byte_len = 0;
    snapshot->captured[phase - 1] = summaries != nullptr;
    for (size_t m = 0; summaries && m < snapshot->modules.len; m++) {
        core_buffer_t encoded = {0};
        if (n00bb_encode_stored_summaries(analysis, summaries, nullptr,
                                          snapshot->modules.data[m], phase,
                                          snapshot->types, &encoded)) {
            core_put_text(out, snapshot->names.data[m]);
            core_put_bytes(out, encoded.data, encoded.byte_len);
        }
        core_buffer_release(&encoded);
    }
}

typedef struct {
    const ir_module_t *module;
    core_list_t(cache_section_t) sections;
} cache_instance_t;
core_list_decl(cache_instance_t);

struct n00bb_cache_t {
    core_dict_t loaded_members;
    core_string_t directory;
    core_string_t shared;
    core_string_t stdlib_root;
    core_string_t root_path;
    core_string_t triple;
    uint64_t compiler;
    core_buffer_t flags;
    ir_ctx_t *ir_context;
    core_list_t(cache_link_t) module_keys;
    core_list_t(cache_entry_t) entries;
    core_list_t(cache_environment_t) interfaces;
    core_list_t(cache_snapshot_t) snapshots;
    core_list_t(cache_scc_t) sccs;
    bool prepared;
    bool prepare_failed;
    bool ir_load_failed;
    core_list_t(cache_instance_t) instances;
    n00bb_cache_locate_fn locate;
    void *locate_context;
    bool unrecorded;
    bool recorded;
    cache_entry_t record;
    core_list_t(cache_section_t) recording;
    th_type_writer_t *recording_types;
    core_buffer_t unstored;
    core_buffer_t libraries;
};

static bool
stdlib_path(const n00bb_cache_t *cache, core_string_t path)
{
    return cache->stdlib_root.u8_bytes
        && path.u8_bytes > cache->stdlib_root.u8_bytes
        && core_string_starts_with(path, cache->stdlib_root)
        && path.data[cache->stdlib_root.u8_bytes] == '/';
}

static uint64_t
scc_key(const n00bb_cache_t *cache, bool stdlib, uint64_t freshness,
        const uint64_t *imports, size_t import_count,
        const uint64_t *inlined, size_t inlined_count)
{
    core_buffer_t key = {0};
    core_buffer_puts(&key, "n00b.scc.key.v6");
    core_put_text(&key, phase_name());
    core_put_u64(&key, freshness);
    core_put_u32(&key, (uint32_t)import_count);
    for (size_t i = 0; i < import_count; i++) core_put_u64(&key, imports[i]);
    core_put_u32(&key, (uint32_t)inlined_count);
    for (size_t i = 0; i < inlined_count; i++) core_put_u64(&key, inlined[i]);
    core_put_u64(&key, cache->compiler);
    core_put_bytes(&key, cache->flags.data, cache->flags.byte_len);
    n00bb_setting_id_t preferences = N00BB_KEY_SYNTAX_PREFERENCES;
    bool syntax = !stdlib || stdlib_path(cache, cache->root_path);
    core_put_u32(&key, syntax && n00bb_setting_bool(N00BB_KEY_SYNTAX_WARNINGS));
    for (size_t i = 0; syntax && i < n00bb_setting_count(preferences); i++) {
        core_string_t value = n00bb_setting_at(preferences, i);
        core_put_bytes(&key, value.data, value.u8_bytes);
    }
    core_put_text(&key, cache->triple);
    uint64_t result = buffer_hash(&key);
    core_buffer_release(&key);
    return result;
}

static core_string_t
canonical_path(core_string_t path)
{
    if (!path.data) return (core_string_t){0};
    core_string_t resolved = core_os_real_path(path);
    return resolved.data ? resolved : core_string_dup(path);
}

static const cache_section_t *
section_named(const core_list_t(cache_section_t) *sections,
              core_string_t name)
{
    for (size_t i = 0; i < sections->len; i++)
        if (core_string_eq(sections->data[i].name, name))
            return &sections->data[i];
    return nullptr;
}

static const cache_section_t *
phase_section(const cache_entry_t *entry, core_string_t kind,
              core_string_t module)
{
    core_buffer_t name = {0};
    core_buffer_append_string(&name, kind);
    if (module.data) {
        core_buffer_putc(&name, '.');
        core_buffer_append_string(&name, module);
    }
    const cache_section_t *section = section_named(&entry->sections,
        core_buffer_string(&name));
    core_buffer_release(&name);
    return section;
}

static core_string_t
read_text(core_byte_cursor_t *r)
{
    return core_string_dup(core_cursor_text(r));
}

static void
read_links(core_byte_cursor_t *r, core_list_t(cache_link_t) *links)
{
    uint32_t count = core_cursor_u32(r);
    for (uint32_t i = 0; !r->failed && i < count; i++) {
        core_string_t name = read_text(r);
        uint64_t value = core_cursor_u64(r);
        if (!r->failed) core_list_push(*links, ((cache_link_t){name, value}));
        else core_free(name.data);
    }
}

static void
entry_release(cache_entry_t *entry)
{
    for (size_t m = 0; m < entry->members.len; m++) {
        cache_member_t *member = &entry->members.data[m];
        core_free(member->name.data);
        core_free(member->path.data);
        core_free(member->owner.data);
        core_free(member->owner_path.data);
        core_buffer_release(&member->inputs);
        for (size_t i = 0; i < member->imports.len; i++)
            core_free(member->imports.data[i].data);
        core_list_free(member->imports);
    }
    core_list_free(entry->members);
    for (size_t i = 0; i < entry->imports.len; i++)
        core_free(entry->imports.data[i].name.data);
    core_list_free(entry->imports);
    for (size_t i = 0; i < entry->inlined.len; i++)
        core_free(entry->inlined.data[i].name.data);
    core_list_free(entry->inlined);
    for (size_t i = 0; i < entry->references.len; i++)
        core_free(entry->references.data[i].data);
    core_list_free(entry->references);
    sections_release(&entry->sections);
    if (entry->interface) th_env_free(entry->interface);
    th_type_reader_free(entry->types);
}

static bool
entry_parse(cache_entry_t *entry, const uint8_t *data, size_t length)
{
    entry->members = core_list_new(cache_member_t);
    entry->imports = core_list_new(cache_link_t);
    entry->inlined = core_list_new(cache_link_t);
    entry->references = core_list_new(core_string_t);
    entry->sections = core_list_new(cache_section_t);
    if (!sections_parse(data, length, &entry->sections)) return false;
    const cache_section_t *manifest = section_named(&entry->sections,
        CORE_STRING_STATIC("manifest"));
    if (!manifest) return false;
    core_byte_cursor_t r = core_cursor(manifest->data, manifest->length);
    uint64_t key = core_cursor_u64(&r);
    entry->freshness = core_cursor_u64(&r);
    uint32_t members = core_cursor_u32(&r);
    for (uint32_t m = 0; !r.failed && m < members; m++) {
        cache_member_t member = {
            .name = read_text(&r), .path = read_text(&r),
            .imports = core_list_new(core_string_t),
        };
        member.source_hash = core_cursor_u64(&r);
        uint32_t imports = core_cursor_u32(&r);
        for (uint32_t i = 0; !r.failed && i < imports; i++) {
            core_string_t name = read_text(&r);
            if (name.data) core_list_push(member.imports, name);
        }
        if (core_cursor_u32(&r)) {
            member.owner = read_text(&r);
            member.owner_path = read_text(&r);
            core_buffer_append_string(&member.inputs, core_cursor_text(&r));
        }
        core_list_push(entry->members, member);
    }
    read_links(&r, &entry->imports);
    read_links(&r, &entry->inlined);
    uint32_t references = core_cursor_u32(&r);
    for (uint32_t i = 0; !r.failed && i < references; i++) {
        core_string_t name = read_text(&r);
        if (name.data) core_list_push(entry->references, name);
    }
    return !r.failed && r.at == r.end && key == entry->key;
}

static bool
module_has_key(const n00bb_cache_t *cache, core_string_t module,
               uint64_t key)
{
    for (size_t i = 0; module.data && i < cache->module_keys.len; i++)
        if (core_string_eq(cache->module_keys.data[i].name, module)
            && (!key || cache->module_keys.data[i].value == key))
            return true;
    return false;
}

static cache_entry_t *
entry_with_key(n00bb_cache_t *cache, uint64_t key)
{
    for (size_t i = 0; key && i < cache->entries.len; i++)
        if (cache->entries.data[i].key == key) return &cache->entries.data[i];
    return nullptr;
}

static cache_entry_t *
selected_entry(n00bb_cache_t *cache, core_string_t module)
{
    for (size_t i = 0; module.data && i < cache->module_keys.len; i++) {
        if (!core_string_eq(cache->module_keys.data[i].name, module))
            continue;
        cache_entry_t *entry = entry_with_key(cache,
            cache->module_keys.data[i].value);
        if (entry && entry->state == ENTRY_VALID) return entry;
    }
    return nullptr;
}

static bool entry_validate(n00bb_cache_t *cache, cache_entry_t *entry);

static bool
import_valid(n00bb_cache_t *cache, core_string_t module, uint64_t key)
{
    cache_entry_t *entry = module_has_key(cache, module, key)
        ? entry_with_key(cache, key) : nullptr;
    return entry && entry_validate(cache, entry);
}

static cache_entry_t *
inlined_entry(n00bb_cache_t *cache, core_string_t module, uint64_t freshness)
{
    for (size_t i = 0; module.data && i < cache->module_keys.len; i++) {
        if (!core_string_eq(cache->module_keys.data[i].name, module))
            continue;
        cache_entry_t *entry = entry_with_key(cache,
            cache->module_keys.data[i].value);
        if (entry && entry->fresh && entry->freshness == freshness)
            return entry;
    }
    return nullptr;
}

static bool
still_located(const n00bb_cache_t *cache, core_string_t name,
              core_string_t path)
{
    if (!cache->locate) return true;
    core_string_t located = {0};
    if (!cache->locate(cache->locate_context, name, &located))
        return false;
    core_string_t canonical = canonical_path(located);
    bool same = core_string_eq(canonical, path);
    core_free(canonical.data);
    core_free(located.data);
    return same;
}

static bool
entry_fresh(const n00bb_cache_t *cache, const cache_entry_t *entry,
            bool program)
{
    bool ok = entry->members.len != 0;
    core_buffer_t freshness = {0};
    core_buffer_puts(&freshness, "n00b.scc.freshness.v1");
    for (size_t m = 0; ok && m < entry->members.len; m++) {
        const cache_member_t *member = &entry->members.data[m];
        uint8_t *source = nullptr;
        size_t source_bytes = 0;
        bool generated = member->owner.data != nullptr;
        core_string_t name = generated ? member->owner : member->name;
        core_string_t path = generated ? member->owner_path : member->path;
        core_buffer_t made = {0};
        if (generated)
            generated_source_bytes(path,
                core_buffer_string(&member->inputs), &made);
        bool root = cache->root_path.data
            && core_string_eq(member->path, cache->root_path);
        ok = (program || (module_has_key(cache, member->name, entry->key)
                          && !root))
            && (root || still_located(cache, name, path))
            && (generated || read_file(path, &source, &source_bytes));
        const void *bytes = generated ? (made.data ? made.data : "")
                                      : (source ? (const void *)source : "");
        size_t length = generated ? made.byte_len : source_bytes;
        if (ok) {
            freshness_member(&freshness, member->name.data,
                             member->name.u8_bytes, bytes, length);
            for (size_t i = 0; i < member->imports.len; i++)
                core_put_text(&freshness, member->imports.data[i]);
        }
        core_free(source);
        core_buffer_release(&made);
    }
    ok = ok && buffer_hash(&freshness) == entry->freshness;
    core_buffer_release(&freshness);
    return ok;
}

static bool
name_listed(core_string_t list, core_string_t name)
{
    size_t at = 0;
    while (list.data && at < list.u8_bytes) {
        core_string_t rest = core_string_slice(list, at, list.u8_bytes - at);
        int64_t comma = core_string_find_byte(rest, ',');
        core_string_t item = core_string_slice(rest, 0,
            comma >= 0 ? (size_t)comma : rest.u8_bytes);
        if (core_string_eq(name, item)) return true;
        at = comma >= 0 ? at + (size_t)comma + 1 : list.u8_bytes;
    }
    return false;
}

static bool
entry_excluded(const n00bb_cache_t *cache, const cache_entry_t *entry,
               const cache_section_t *changed)
{
    for (size_t m = 0; m < entry->members.len; m++) {
        core_string_t name = entry->members.data[m].name;
        if (changed ? name_listed(core_string_borrow(
                          (const char *)changed->data, changed->length), name)
                : cache->unrecorded && phase_section(entry,
                      CORE_STRING_STATIC("state"), name))
            return true;
    }
    return false;
}

static bool
library_module_name(const n00bb_cache_t *cache, core_string_t name)
{
    core_string_t list = core_buffer_string(&cache->libraries);
    size_t at = 0;
    while (name.data && list.data && at < list.u8_bytes) {
        core_string_t rest = core_string_slice(list, at, list.u8_bytes - at);
        int64_t comma = core_string_find_byte(rest, ',');
        core_string_t item = core_string_slice(rest, 0,
            comma >= 0 ? (size_t)comma : rest.u8_bytes);
        if (item.u8_bytes && core_string_starts_with(name, item)
            && (name.u8_bytes == item.u8_bytes
                || name.data[item.u8_bytes] == '.'))
            return true;
        at = comma >= 0 ? at + (size_t)comma + 1 : list.u8_bytes;
    }
    return false;
}

bool
n00bb_cache_library_module(const n00bb_cache_t *cache, core_string_t module)
{
    return cache && library_module_name(cache, module);
}

static bool
entry_uses_library(const n00bb_cache_t *cache, const cache_entry_t *entry)
{
    for (size_t m = 0; m < entry->members.len; m++)
        if (library_module_name(cache, entry->members.data[m].name))
            return true;
    for (size_t i = 0; i < entry->imports.len; i++)
        if (library_module_name(cache, entry->imports.data[i].name))
            return true;
    return false;
}

static bool
scc_members_library(const n00bb_cache_t *cache, const cache_scc_t *scc)
{
    for (size_t m = 0; m < scc->members.len; m++)
        if (library_module_name(cache,
                scc->members.data[m]->module.identifier))
            return true;
    return false;
}

static bool
scc_uses_library(const n00bb_cache_t *cache,
                 const core_list_t(cache_scc_t) *sccs, const cache_scc_t *scc)
{
    if (scc_members_library(cache, scc)) return true;
    for (size_t i = 0; i < scc->imports.len; i++)
        if (scc_members_library(cache, &sccs->data[scc->imports.data[i]]))
            return true;
    return false;
}

static bool
entry_validate(n00bb_cache_t *cache, cache_entry_t *entry)
{
    if (entry->state == ENTRY_VALID) return true;
    if (entry->state != ENTRY_UNKNOWN) return false;
    entry->state = ENTRY_VISITING;
    bool ok = entry->fresh && !entry->excluded
        && phase_section(entry, CORE_STRING_STATIC("ir"), (core_string_t){0})
        && phase_section(entry, CORE_STRING_STATIC("interface"),
                         (core_string_t){0});
    for (size_t m = 0; ok && !objects_optional() && m < entry->members.len;
         m++)
        ok = phase_section(entry, CORE_STRING_STATIC("object"),
                           entry->members.data[m].name)
            && phase_section(entry, CORE_STRING_STATIC("abi"),
                             entry->members.data[m].name);
    uint64_t *imports = core_alloc_array(uint64_t,
                                         entry->imports.len + 1);
    uint64_t *inlined = core_alloc_array(uint64_t,
                                         entry->inlined.len + 1);
    ok = ok && imports && inlined;
    for (size_t i = 0; ok && i < entry->imports.len; i++) {
        ok = import_valid(cache, entry->imports.data[i].name,
                          entry->imports.data[i].value);
        imports[i] = entry->imports.data[i].value;
    }
    for (size_t i = 0; ok && i < entry->inlined.len; i++) {
        cache_entry_t *callee = inlined_entry(cache,
            entry->inlined.data[i].name, entry->inlined.data[i].value);
        ok = callee != nullptr;
        if (ok) inlined[i] = callee->freshness;
    }
    const cache_member_t *lead = entry->members.data;
    ok = ok && scc_key(cache, stdlib_path(cache, lead->owner.data
                           ? lead->owner_path : lead->path),
                       entry->freshness, imports, entry->imports.len, inlined,
                       entry->inlined.len) == entry->key;
    core_free(imports);
    core_free(inlined);
    entry->state = ok ? ENTRY_VALID : ENTRY_INVALID;
    return ok;
}

static bool
decode_interfaces(n00bb_cache_t *cache)
{
    bool ok = true;
    for (size_t e = 0; ok && e < cache->entries.len; e++) {
        cache_entry_t *entry = &cache->entries.data[e];
        if (entry->state != ENTRY_VALID) continue;
        const cache_section_t *interface = phase_section(entry,
            CORE_STRING_STATIC("interface"), (core_string_t){0});
        entry->interface = interface ? th_env_decode(nullptr,
            interface->data, interface->length) : nullptr;
        ok = entry->interface != nullptr;
        entry->load = ok ? ENTRY_VALID : ENTRY_INVALID;
    }
    if (!ok)
        for (size_t e = 0; e < cache->entries.len; e++)
            cache->entries.data[e].load = ENTRY_INVALID;
    return ok;
}

static bool
next_key_line(const uint8_t *bytes, size_t length, size_t *at, uint64_t *key,
              uint64_t *compiler)
{
    while (*at < length) {
        size_t start = *at;
        size_t end   = start;
        while (end < length && bytes[end] != '\n') end++;
        *at = end + 1;
        if (end - start != 33 || bytes[start + 16] != ' ') continue;
        *key = core_string_strtoull(
            core_string_borrow((const char *)bytes + start, 16), 16);
        *compiler = core_string_strtoull(
            core_string_borrow((const char *)bytes + start + 17, 16), 16);
        return true;
    }
    return false;
}

static void
read_module_keys_in(n00bb_cache_t *cache, core_string_t root)
{
    core_buffer_t path = {0};
    core_buffer_append_string(&path, root);
    core_buffer_puts(&path, "/modules");
    char *native_path = core_string_to_cstr(core_buffer_string(&path));
    DIR *directory = native_path ? opendir(native_path) : nullptr;
    core_free(native_path);
    core_buffer_t suffix = {0};
    core_buffer_putc(&suffix, '.');
    core_buffer_append_string(&suffix, phase_name());
    core_buffer_puts(&suffix, ".key");
    for (struct dirent *item = directory ? readdir(directory) : nullptr;
         item; item = readdir(directory)) {
        core_string_t entry_name = core_string_from_cstr(item->d_name);
        bool matches = entry_name.u8_bytes > suffix.byte_len
            && core_string_ends_with(entry_name, core_buffer_string(&suffix));
        if (!matches) {
            core_free(entry_name.data);
            continue;
        }
        core_buffer_t file = {0};
        core_buffer_append_string(&file, core_buffer_string(&path));
        core_buffer_putc(&file, '/');
        core_buffer_append_string(&file, entry_name);
        uint8_t *bytes = nullptr;
        size_t byte_count = 0;
        core_string_t module = core_string_slice(entry_name, 0,
            entry_name.u8_bytes - suffix.byte_len);
        bool readable = read_file(core_buffer_string(&file), &bytes,
                                  &byte_count);
        size_t   at       = 0;
        uint64_t key      = 0;
        uint64_t compiler = 0;
        while (readable && next_key_line(bytes, byte_count, &at, &key,
                                         &compiler)) {
            core_string_t name = key && compiler == cache->compiler
                ? core_string_dup(module) : (core_string_t){0};
            if (name.data)
                core_list_push(cache->module_keys,
                               ((cache_link_t){name, key}));
        }
        core_free(bytes);
        core_buffer_release(&file);
        core_free(entry_name.data);
    }
    if (directory) closedir(directory);
    core_buffer_release(&suffix);
    core_buffer_release(&path);
}

static void
read_module_keys(n00bb_cache_t *cache)
{
    read_module_keys_in(cache, cache->shared);
    if (!core_string_eq(cache->shared, cache->directory))
        read_module_keys_in(cache, cache->directory);
}

static void
object_path_in(core_string_t root, uint64_t key, core_buffer_t *path)
{
    core_buffer_printf(path, "%.*s/%016llx.n00bo", (int)root.u8_bytes,
                       root.data, (unsigned long long)key);
}

static void
object_path(const n00bb_cache_t *cache, uint64_t key, core_buffer_t *path)
{
    object_path_in(cache->directory, key, path);
    if (core_string_eq(cache->shared, cache->directory)) return;
    char *native = core_string_to_cstr(core_buffer_string(path));
    struct stat status;
    bool local = native && stat(native, &status) == 0;
    core_free(native);
    if (local) return;
    path->byte_len = 0;
    object_path_in(cache->shared, key, path);
}


static bool
reference_holds(n00bb_cache_t *cache, const cache_entry_t *entry,
                core_string_t reference)
{
    for (size_t m = 0; m < entry->members.len; m++)
        if (core_string_eq(entry->members.data[m].name, reference))
            return true;
    return module_has_key(cache, reference, 0)
        && selected_entry(cache, reference);
}

static bool
scc_in_stdlib(const n00bb_cache_t *cache, const cache_scc_t *scc)
{
    bool inside = scc->members.len > 0;
    for (size_t m = 0; inside && m < scc->members.len; m++) {
        core_string_t owner = {0}, owner_path = {0};
        const core_source_file_t *source = &scc->members.data[m]->source;
        core_string_t path = canonical_path(
            n00bb_source_file_generated_owner(source, &owner, &owner_path)
                ? owner_path : core_source_file_path(source));
        inside = stdlib_path(cache, path);
        core_free(path.data);
    }
    return inside;
}

static core_string_t
scc_directory(const n00bb_cache_t *cache, const cache_scc_t *scc)
{
    return scc_in_stdlib(cache, scc) ? cache->shared : cache->directory;
}

typedef struct {
    core_dict_t by_module;
    core_dict_t visited;
    core_dict_t read;
    uintptr_t  *next;
} entry_walk_t;

static cache_entry_t *
walk_entry(n00bb_cache_t *cache, entry_walk_t *walk, uint64_t key)
{
    if (!core_dict_add(&walk->read, key, 1)) return nullptr;
    core_buffer_t path = {0};
    object_path(cache, key, &path);
    uint8_t *bytes = nullptr;
    size_t length = 0;
    cache_entry_t entry = {.key = key};
    bool ok = read_file(core_buffer_string(&path), &bytes, &length)
        && entry_parse(&entry, bytes, length);
    core_free(bytes);
    core_buffer_release(&path);
    if (!ok) {
        entry_release(&entry);
        return nullptr;
    }
    core_list_push(cache->entries, entry);
    return &cache->entries.data[cache->entries.len - 1];
}

static bool
module_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_' || c == '.';
}

static void
push_module_name(core_list_t(core_string_t) *names, core_string_t name)
{
    if (name.u8_bytes) core_list_push(*names, core_string_dup(name));
}

static size_t
next_word(core_string_t line, size_t at, core_string_t *word)
{
    while (at < line.u8_bytes && !module_name_char(line.data[at])) at++;
    size_t start = at;
    while (at < line.u8_bytes && module_name_char(line.data[at])) at++;
    *word = core_string_slice(line, start, at - start);
    return at;
}

static void
scan_import_line(core_string_t line, core_list_t(core_string_t) *names)
{
    if (core_string_starts_with(line, CORE_STRING_STATIC("bring "))) {
        int64_t from = core_string_find(line, CORE_STRING_STATIC(" from "));
        core_string_t word = {0};
        if (from >= 0) (void)next_word(line, (size_t)from + 6, &word);
        push_module_name(names, word);
        return;
    }
    if (!core_string_starts_with(line, CORE_STRING_STATIC("import ")))
        return;
    bool alias = false;
    for (size_t at = 7; at < line.u8_bytes;) {
        core_string_t word = {0};
        at = next_word(line, at, &word);
        if (!word.u8_bytes
            || core_string_eq(word, CORE_STRING_STATIC("bring"))) break;
        if (core_string_eq(word, CORE_STRING_STATIC("as"))) alias = true;
        else if (alias) alias = false;
        else push_module_name(names, word);
    }
}

static void
scan_root_imports(core_string_t path, core_list_t(core_string_t) *names)
{
    uint8_t *bytes = nullptr;
    size_t length = 0;
    if (!read_file(path, &bytes, &length)) return;
    core_string_t text = core_string_borrow((const char *)bytes, length);
    for (size_t at = 0; at < text.u8_bytes;) {
        size_t end = at;
        while (end < text.u8_bytes && text.data[end] != '\n') end++;
        size_t start = at;
        while (start < end
               && (text.data[start] == ' ' || text.data[start] == '\t'))
            start++;
        scan_import_line(core_string_slice(text, start, end - start), names);
        at = end + 1;
    }
    core_free(bytes);
}

static void
queue_entry_links(const cache_entry_t *entry,
                  core_list_t(core_string_t) *pending)
{
    for (size_t m = 0; m < entry->members.len; m++)
        for (size_t i = 0; i < entry->members.data[m].imports.len; i++)
            push_module_name(pending, entry->members.data[m].imports.data[i]);
    for (size_t i = 0; i < entry->imports.len; i++)
        push_module_name(pending, entry->imports.data[i].name);
    for (size_t i = 0; i < entry->inlined.len; i++)
        push_module_name(pending, entry->inlined.data[i].name);
    for (size_t i = 0; i < entry->references.len; i++)
        push_module_name(pending, entry->references.data[i]);
}

static bool
read_module_entries(n00bb_cache_t *cache, entry_walk_t *walk,
                    core_string_t name, core_list_t(core_string_t) *pending)
{
    core_buffer_t package = {0};
    core_buffer_append_string(&package, name);
    core_buffer_puts(&package, ".__init");
    core_string_t names[2] = {name, core_buffer_string(&package)};
    bool found = false;
    for (int n = 0; n < 2; n++)
        for (uintptr_t i = (uintptr_t)core_dict_get(&walk->by_module,
                 &names[n], nullptr);
             i; i = walk->next[i - 1]) {
            found = true;
            const cache_entry_t *entry = walk_entry(cache, walk,
                cache->module_keys.data[i - 1].value);
            if (entry) queue_entry_links(entry, pending);
        }
    core_buffer_release(&package);
    return found || library_module_name(cache, name);
}

static void
read_reachable_entries(n00bb_cache_t *cache)
{
    entry_walk_t walk = {
        .next = core_alloc_array(uintptr_t, cache->module_keys.len + 1),
    };
    core_dict_init_string(&walk.by_module);
    core_dict_init_string(&walk.visited);
    core_dict_init(&walk.read, nullptr, nullptr);
    for (size_t i = cache->module_keys.len; i > 0; i--) {
        core_string_t *name = &cache->module_keys.data[i - 1].name;
        walk.next[i - 1] = (uintptr_t)core_dict_get(&walk.by_module, name,
                                                    nullptr);
        core_dict_put(&walk.by_module, name, (void *)i);
    }
    core_list_t(core_string_t) pending = core_list_new(core_string_t);
    scan_root_imports(cache->root_path, &pending);
    push_module_name(&pending, CORE_STRING_STATIC("builtin"));
    if (!cache_check_only) {
        push_module_name(&pending, CORE_STRING_STATIC("sys.gc.alloc"));
        push_module_name(&pending, CORE_STRING_STATIC("sys.marshal"));
        push_module_name(&pending, CORE_STRING_STATIC("sys.box"));
    }
    bool complete = true;
    while (complete && pending.len) {
        core_string_t name = pending.data[--pending.len];
        if (core_dict_contains(&walk.visited, &name)) {
            core_free(name.data);
            continue;
        }
        complete = read_module_entries(cache, &walk, name, &pending);
        core_string_t phase = phase_name();
        if (!complete)
            cache_log("%.*s load all: %.*s is not cached",
                      (int)phase.u8_bytes, phase.data,
                      (int)name.u8_bytes, name.data);
        core_dict_put(&walk.visited, core_dict_string_key_copy(name), 1);
        core_free(name.data);
    }
    for (size_t i = 0; !complete && i < cache->module_keys.len; i++)
        (void)walk_entry(cache, &walk, cache->module_keys.data[i].value);
    for (size_t i = 0; i < pending.len; i++) core_free(pending.data[i].data);
    core_list_free(pending);
    core_dict_foreach(&walk.visited, item)
        core_dict_string_key_free(item->key);
    core_dict_free(&walk.visited);
    core_dict_free(&walk.by_module);
    core_dict_free(&walk.read);
    core_free(walk.next);
}

static void
member_names(const cache_entry_t *entry, core_buffer_t *out)
{
    for (size_t m = 0; m < entry->members.len; m++) {
        if (m) core_buffer_putc(out, ',');
        core_buffer_append_string(out, entry->members.data[m].name);
    }
}

static bool take_fill_lock(n00bb_cache_t *cache);

static uint64_t
record_key(const n00bb_cache_t *cache)
{
    core_buffer_t root = {0};
    core_buffer_puts(&root, "n00b.program.record.v1");
    core_put_text(&root, cache->root_path);
    core_put_text(&root, core_buffer_string(&cache->libraries));
    uint64_t key = scc_key(cache, false, buffer_hash(&root), nullptr, 0,
                           nullptr, 0);
    core_buffer_release(&root);
    return key;
}

static const cache_section_t *
read_record(n00bb_cache_t *cache)
{
    uint8_t *bytes = nullptr;
    size_t length = 0;
    bool starts = !cache_check_only
        && read_file(cache->root_path, &bytes, &length)
        && core_string_find(core_string_borrow((const char *)bytes, length),
               CORE_STRING_STATIC(N00BB_COMPTIME_START_NAME)) >= 0;
    core_free(bytes);
    bytes = nullptr;
    core_buffer_t path = {0};
    cache->record = (cache_entry_t){.key = record_key(cache)};
    object_path_in(cache->directory, cache->record.key, &path);
    cache->recorded = read_file(core_buffer_string(&path), &bytes, &length)
        && entry_parse(&cache->record, bytes, length)
        && entry_fresh(cache, &cache->record, true);
    core_free(bytes);
    core_buffer_release(&path);
    const cache_section_t *table = cache->recorded ? phase_section(
        &cache->record, CORE_STRING_STATIC("types"), (core_string_t){0})
        : nullptr;
    if (table)
        cache->record.types = th_type_reader_open(
            ir_ctx_types(cache->ir_context), table->data, table->length,
            nullptr);
    if (cache->recorded) {
        core_string_t phase = phase_name();
        cache_log("%.*s reuse record", (int)phase.u8_bytes, phase.data);
    }
    cache->unrecorded = starts && !cache->recorded;
    return starts && cache->recorded ? phase_section(&cache->record,
        CORE_STRING_STATIC("changed"), (core_string_t){0}) : nullptr;
}

n00bb_cache_t *
n00bb_cache_open(core_string_t root_path, parse_cmdr_result_t *args,
                 core_string_t triple,
                 ir_ctx_t *ir_context, n00bb_cache_locate_fn locate,
                 void *locate_context, core_string_t libraries)
{
    if (!root_path.data || !root_path.u8_bytes || root_path.data[0] == '<'
        || !triple.data
        || n00bb_setting_bool(N00BB_KEY_DEV_NO_OBJECT_CACHE))
        return nullptr;
    n00bb_cache_t *cache = core_alloc(n00bb_cache_t);
    if (!cache) return nullptr;
    core_dict_init_string(&cache->loaded_members);
    cache->root_path = canonical_path(root_path);
    int64_t slash = cache->root_path.data
        ? core_string_rfind_byte(cache->root_path, '/') : -1;
    cache->directory = slash >= 0
        ? n00bb_cache_mirror_directory(CORE_STRING_STATIC("obj"),
              core_string_slice(cache->root_path, 0,
                                slash ? (size_t)slash : 1))
        : (core_string_t){0};
    if (!cache->directory.data) {
        core_dict_free(&cache->loaded_members);
        core_free(cache->root_path.data);
        core_free(cache);
        return nullptr;
    }
    cache->stdlib_root = canonical_path(n00bb_toolchain()->stdlib);
    cache->shared = cache->stdlib_root.u8_bytes
        ? n00bb_cache_mirror_directory(CORE_STRING_STATIC("obj"),
                                       cache->stdlib_root)
        : (core_string_t){0};
    if (!cache->shared.data) cache->shared = core_string_dup(cache->directory);
    cache->triple = core_string_dup(triple);
    cache->compiler = n00bb_compiler_identity;
    append_flags(&cache->flags, args);
    cache->ir_context = ir_context;
    cache->locate = locate;
    cache->locate_context = locate_context;
    cache->module_keys = core_list_new(cache_link_t);
    cache->entries = core_list_new(cache_entry_t);
    cache->interfaces = core_list_new(cache_environment_t);
    cache->snapshots = core_list_new(cache_snapshot_t);
    cache->sccs = core_list_new(cache_scc_t);
    cache->instances = core_list_new(cache_instance_t);
    cache->recording = core_list_new(cache_section_t);
    core_buffer_append_string(&cache->libraries, libraries);
    read_module_keys(cache);
    read_reachable_entries(cache);
    const cache_section_t *rebuilt = read_record(cache);
    for (size_t e = 0; e < cache->entries.len; e++) {
        cache_entry_t *entry = &cache->entries.data[e];
        entry->fresh = entry_fresh(cache, entry, false);
        entry->excluded = entry_excluded(cache, entry, rebuilt)
            || entry_uses_library(cache, entry);
    }
    for (size_t e = 0; e < cache->entries.len; e++)
        (void)entry_validate(cache, &cache->entries.data[e]);
    for (size_t e = 0; e < cache->entries.len; e++) {
        cache_entry_t *entry = &cache->entries.data[e];
        for (size_t m = 0; entry->state == ENTRY_VALID
             && m < entry->members.len; m++)
            if (selected_entry(cache, entry->members.data[m].name) != entry)
                entry->state = ENTRY_INVALID;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t e = 0; e < cache->entries.len; e++) {
            cache_entry_t *entry = &cache->entries.data[e];
            if (entry->state != ENTRY_VALID) continue;
            for (size_t i = 0; i < entry->references.len; i++) {
                if (reference_holds(cache, entry,
                                    entry->references.data[i]))
                    continue;
                entry->state = ENTRY_INVALID;
                changed = true;
                break;
            }
        }
    }
    if (decode_interfaces(cache))
        for (size_t e = 0; e < cache->entries.len; e++)
            if (cache->entries.data[e].load == ENTRY_VALID) {
                core_list_push(cache->interfaces,
                               cache->entries.data[e].interface);
                core_buffer_t members = {0};
                member_names(&cache->entries.data[e], &members);
                core_string_t phase = phase_name();
                cache_log("%.*s reuse scc %.*s",
                          (int)phase.u8_bytes, phase.data,
                          (int)members.byte_len,
                          members.data ? members.data : "");
                core_buffer_release(&members);
            }
    for (size_t e = 0; e < cache->entries.len; e++) {
        cache_entry_t *entry = &cache->entries.data[e];
        for (size_t m = 0; entry->load == ENTRY_VALID
             && m < entry->members.len; m++)
            if (!core_dict_contains(&cache->loaded_members,
                                    &entry->members.data[m].name))
                core_dict_put(&cache->loaded_members,
                              &entry->members.data[m].name, e + 1);
    }
    if (take_fill_lock(cache)) {
        n00bb_cache_free(cache);
        return n00bb_cache_open(root_path, args, triple, ir_context, locate,
                                locate_context, libraries);
    }
    return cache;
}

void
n00bb_cache_free(n00bb_cache_t *cache)
{
    if (!cache) return;
    core_dict_free(&cache->loaded_members);
    for (size_t e = 0; e < cache->entries.len; e++)
        entry_release(&cache->entries.data[e]);
    core_list_free(cache->entries);
    for (size_t i = 0; i < cache->module_keys.len; i++)
        core_free(cache->module_keys.data[i].name.data);
    core_list_free(cache->module_keys);
    core_list_free(cache->interfaces);
    for (size_t i = 0; i < cache->snapshots.len; i++)
        snapshot_release(&cache->snapshots.data[i]);
    core_list_free(cache->snapshots);
    sccs_release(&cache->sccs);
    for (size_t i = 0; i < cache->instances.len; i++)
        sections_release(&cache->instances.data[i].sections);
    core_list_free(cache->instances);
    entry_release(&cache->record);
    sections_release(&cache->recording);
    th_type_writer_free(cache->recording_types);
    core_buffer_release(&cache->flags);
    core_buffer_release(&cache->unstored);
    core_buffer_release(&cache->libraries);
    core_free(cache->directory.data);
    core_free(cache->shared.data);
    core_free(cache->stdlib_root.data);
    core_free(cache->root_path.data);
    core_free(cache->triple.data);
    core_free(cache);
}

size_t
n00bb_cache_interface_count(const n00bb_cache_t *cache)
{
    return cache ? cache->interfaces.len : 0;
}

const th_env *
n00bb_cache_interface(const n00bb_cache_t *cache, size_t index)
{
    return cache && index < cache->interfaces.len
        ? cache->interfaces.data[index] : nullptr;
}

bool
n00bb_cache_interface_has_module(const n00bb_cache_t *cache, size_t index,
                                 core_string_t module)
{
    const th_env *interface = n00bb_cache_interface(cache, index);
    for (size_t e = 0; interface && e < cache->entries.len; e++) {
        const cache_entry_t *entry = &cache->entries.data[e];
        if (entry->interface != interface) continue;
        for (size_t m = 0; m < entry->members.len; m++)
            if (core_string_eq(entry->members.data[m].name, module))
                return true;
        return false;
    }
    return false;
}

static const cache_member_t *
loaded_member(const n00bb_cache_t *cache, core_string_t module,
              const cache_entry_t **owner)
{
    uintptr_t at = cache && module.data
        ? (uintptr_t)core_dict_get((core_dict_t *)&cache->loaded_members,
                                   &module, nullptr)
        : 0;
    const cache_entry_t *entry = at ? &cache->entries.data[at - 1] : nullptr;
    for (size_t m = 0; entry && m < entry->members.len; m++) {
        const cache_member_t *member = &entry->members.data[m];
        if (core_string_eq(member->name, module)) {
            if (owner) *owner = entry;
            return member;
        }
    }
    return nullptr;
}

bool
n00bb_cache_has_module(const n00bb_cache_t *cache, core_string_t module)
{
    return loaded_member(cache, module, nullptr) != nullptr;
}

bool
n00bb_cache_module_source(const n00bb_cache_t *cache, core_string_t module,
                          core_string_t *path, uint64_t *source_hash,
                          core_string_t generated[3])
{
    const cache_member_t *member = loaded_member(cache, module, nullptr);
    if (!member) return false;
    *path = member->path;
    *source_hash = member->source_hash;
    if (generated) {
        generated[0] = member->owner;
        generated[1] = member->owner_path;
        generated[2] = core_buffer_string(&member->inputs);
    }
    return true;
}

bool
n00bb_cache_module_section(const n00bb_cache_t *cache, core_string_t module,
                           core_string_t kind, const uint8_t **data,
                           size_t *length, const th_type_reader_t **types)
{
    const cache_entry_t *entry = nullptr;
    const cache_member_t *member = loaded_member(cache, module, &entry);
    const cache_section_t *section = member
        ? phase_section(entry, kind, member->name) : nullptr;
    if (!section) return false;
    *data   = section->data;
    *length = section->length;
    *types  = entry->types;
    return true;
}

bool
n00bb_cache_record_section(const n00bb_cache_t *cache, core_string_t module,
                           core_string_t kind, const uint8_t **data,
                           size_t *length, const th_type_reader_t **types)
{
    const cache_section_t *section = cache && cache->recorded
        ? phase_section(&cache->record, kind, module) : nullptr;
    if (!section) return false;
    *data = section->data;
    *length = section->length;
    if (types) *types = cache->record.types;
    return true;
}

void
n00bb_cache_record_put(n00bb_cache_t *cache, core_string_t module,
                       core_string_t kind, const core_buffer_t *bytes)
{
    if (!cache || cache->recorded) return;
    core_buffer_t name = {0};
    core_buffer_append_string(&name, kind);
    if (module.data) {
        core_buffer_putc(&name, '.');
        core_buffer_append_string(&name, module);
    }
    (void)sections_set(&cache->recording, core_buffer_string(&name),
                       bytes->data ? bytes->data : "", bytes->byte_len);
    core_buffer_release(&name);
}

static void
record_program_summaries(n00bb_cache_t *cache, n00bb_link_analysis_t *analysis)
{
    if (cache->recorded || analysis->may_link_further || !analysis->summaries
        || analysis->summaries == analysis->library_summaries) return;
    if (!cache->recording_types)
        cache->recording_types = th_type_writer_new(
            ir_ctx_types(cache->ir_context));
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        ir_module_t *body = interface_body_module(analysis,
                                                  unit->module.identifier);
        ir_module_t *module = body ? body : state ? state->ir_module : nullptr;
        core_buffer_t encoded = {0};
        if (module && n00bb_encode_stored_summaries(analysis,
                analysis->summaries, analysis->library_summaries, module,
                N00BB_PHASE_MAIN, cache->recording_types, &encoded))
            n00bb_cache_record_put(cache, unit->module.identifier,
                                   CORE_STRING_STATIC("program"), &encoded);
        core_buffer_release(&encoded);
    }
}

bool
n00bb_cache_replay_warnings(const n00bb_cache_t *cache,
                            const core_translation_unit_t *unit,
                            core_diagnostics_t *into)
{
    const cache_entry_t *entry = nullptr;
    const cache_member_t *member = unit
        ? loaded_member(cache, unit->module.identifier, &entry) : nullptr;
    const cache_section_t *section = member
        ? phase_section(entry, CORE_STRING_STATIC("warnings"), member->name)
        : nullptr;
    if (!section) return false;
    if (section->length < 4) return false;
    uint32_t count = core_le_load_u32(section->data);
    size_t at = 4;
    for (uint32_t i = 0; i < count; i++) {
        core_diagnostic_t item = {0};
        if (!core_diagnostic_decode(section->data, section->length, &at,
                                    &item)) return false;
        core_diagnostics_append(into, &item);
        core_diagnostic_release(&item);
    }
    return true;
}

void
n00bb_cache_log_runtime_object(core_string_t module)
{
    core_string_t phase = phase_name();
    cache_log("%.*s runtime object %.*s", (int)phase.u8_bytes, phase.data,
              (int)module.u8_bytes, module.data);
}

static bool
entry_used(const cache_entry_t *entry, n00bb_cache_module_used_fn used,
           void *context)
{
    for (size_t m = 0; m < entry->members.len; m++)
        if (used(context, entry->members.data[m].name)) return true;
    return false;
}

bool
n00bb_cache_load_ir(n00bb_cache_t *cache, ir_module_lookup_fn lookup,
                    n00bb_cache_module_used_fn used, void *context)
{
    size_t inputs_used = 0, module_count = 0;
    ir_archive_input_t *inputs = cache
        ? core_alloc_array(ir_archive_input_t, cache->entries.len + 1)
        : nullptr;
    if (!inputs) return !cache;
    for (size_t e = 0; e < cache->entries.len; e++) {
        cache_entry_t *entry = &cache->entries.data[e];
        if (entry->load != ENTRY_VALID || entry->loaded
            || !entry_used(entry, used, context)) continue;
        const cache_section_t *ir = phase_section(entry,
            CORE_STRING_STATIC("ir"), (core_string_t){0});
        const cache_section_t *types = phase_section(entry,
            CORE_STRING_STATIC("types"), (core_string_t){0});
        if (types && !entry->types)
            entry->types = th_type_reader_open(ir_ctx_types(cache->ir_context),
                                               types->data, types->length,
                                               nullptr);
        if (!ir || !entry->types) {
            core_free(inputs);
            return false;
        }
        inputs[inputs_used++] = (ir_archive_input_t){ir->data, ir->length,
                                                     entry->types};
        module_count += entry->members.len;
    }
    ir_module_t **modules = core_alloc_array(ir_module_t *,
                                             module_count + 1);
    const ir_profile_t *profiles[] = {n00bb_type_profile()};
    size_t loaded = 0;
    bool ok = modules && (!inputs_used || (ir_archives_load(cache->ir_context,
        inputs, inputs_used, profiles, 1, lookup, context, modules,
        module_count, &loaded) && loaded == module_count));
    size_t next = 0;
    for (size_t e = 0; ok && e < cache->entries.len; e++) {
        cache_entry_t *entry = &cache->entries.data[e];
        if (entry->load != ENTRY_VALID || entry->loaded
            || !entry_used(entry, used, context)) continue;
        for (size_t m = 0; ok && m < entry->members.len; m++) {
            ir_module_t *module = modules[next++];
            ok = core_string_eq(ir_module_name(module),
                                entry->members.data[m].name);
            entry->members.data[m].module = module;
        }
        entry->loaded = ok;
    }
    core_free(modules);
    core_free(inputs);
    return ok;
}

void
n00bb_cache_skip_store(n00bb_cache_t *cache, core_string_t module)
{
    if (!cache || !module.u8_bytes) return;
    if (cache->unstored.byte_len) core_buffer_putc(&cache->unstored, ',');
    core_buffer_append_string(&cache->unstored, module);
}

bool
n00bb_cache_owns(const n00bb_cache_t *cache, const ir_module_t *module)
{
    for (size_t e = 0; cache && module && e < cache->entries.len; e++)
        for (size_t m = 0; m < cache->entries.data[e].members.len; m++)
            if (cache->entries.data[e].members.data[m].module == module)
                return true;
    return false;
}

ir_module_t *
n00bb_cache_module(const n00bb_cache_t *cache, core_string_t module)
{
    const cache_member_t *member = loaded_member(cache, module, nullptr);
    return member ? member->module : nullptr;
}

bool
n00bb_cache_module_object(const n00bb_cache_t *cache, core_string_t module,
                          n00bb_cache_module_object_t *object)
{
    const cache_entry_t *entry = nullptr;
    const cache_member_t *member = module.data
        ? loaded_member(cache, module, &entry) : nullptr;
    const cache_section_t *code = member
        ? phase_section(entry, CORE_STRING_STATIC("object"), member->name)
        : nullptr;
    const cache_section_t *abi = member
        ? phase_section(entry, CORE_STRING_STATIC("abi"), member->name)
        : nullptr;
    const cache_section_t *mutators = member
        ? phase_section(entry, CORE_STRING_STATIC("mutators"), member->name)
        : nullptr;
    if (!code || !abi || !mutators) return false;
    const cache_section_t *runtime = phase_section(entry,
        CORE_STRING_STATIC("runtime-object"), member->name);
    *object = (n00bb_cache_module_object_t){
        .module = member->name,
        .object = code->data, .object_bytes = code->length,
        .abi = abi->data, .abi_bytes = abi->length,
        .runtime_object = runtime ? runtime->data : nullptr,
        .runtime_object_bytes = runtime ? runtime->length : 0,
        .mutators = mutators->data, .mutators_bytes = mutators->length,
        .ir = member->module,
    };
    return true;
}

static const cache_instance_t *loaded_instance(const n00bb_cache_t *cache,
                                               const ir_module_t *module);

static bool
entry_seeds(const n00bb_cache_t *cache, const cache_entry_t *entry,
            th_ctx *types, ir_module_lookup_fn lookup, void *context,
            core_list_t(n00bb_specialization_seed_t) *seeds)
{
    const cache_section_t *section = phase_section(entry,
        CORE_STRING_STATIC("requests"), (core_string_t){0});
    if (!section) return true;
    core_byte_cursor_t in = core_cursor(section->data, section->length);
    size_t blob = 0;
    const uint8_t *table = core_cursor_bytes(&in, &blob);
    th_type_reader_t *reader = table
        ? th_type_reader_open(types, table, blob, nullptr) : nullptr;
    if (!reader) return false;
    uint32_t count = core_cursor_u32(&in);
    bool ok = !in.failed;
    for (uint32_t r = 0; ok && r < count; r++) {
        core_string_t module = core_cursor_text(&in);
        core_string_t name = core_cursor_text(&in);
        uint32_t index = core_cursor_u32(&in);
        ok = !in.failed;
        if (!ok) break;
        ir_module_t *owner = n00bb_cache_module(cache, module);
        if (!owner && lookup) owner = lookup(context, module);
        ir_func_t *generic = owner && name.data
            ? ir_func_find(owner, name) : nullptr;
        th_type *signature = th_type_reader_get(reader, index);
        ok = signature != nullptr;
        if (ok && generic)
            core_list_push(*seeds, ((n00bb_specialization_seed_t){
                .generic = generic, .signature = signature,
            }));
    }
    th_type_reader_free(reader);
    return ok && in.at == in.end;
}

void
n00bb_cache_seeds(const n00bb_cache_t *cache, th_ctx *types,
                  ir_module_lookup_fn lookup, void *context,
                  core_list_t(n00bb_specialization_seed_t) *seeds)
{
    for (size_t e = 0; cache && e < cache->entries.len; e++) {
        const cache_entry_t *entry = &cache->entries.data[e];
        if (entry->load == ENTRY_VALID && entry->loaded)
            (void)entry_seeds(cache, entry, types, lookup, context, seeds);
    }
}

static bool
requested_by(const n00bb_link_analysis_t *analysis, const cache_scc_t *scc,
             const ir_module_t *requester,
             const core_list_t(n00bb_ir_func_ptr_t) *closure)
{
    for (size_t m = 0; m < scc->members.len; m++)
        if (tu_state(scc->members.data[m])->ir_module == requester)
            return true;
    for (size_t i = 0; i < closure->len; i++)
        if (closure->data[i]->owner == requester) return true;
    (void)analysis;
    return false;
}

typedef struct {
    const n00bb_specialization_t *item;
    core_buffer_t                 key;
} cache_request_t;

static int
compare_requests(const void *left, const void *right)
{
    const cache_request_t *a = left;
    const cache_request_t *b = right;
    return core_string_cmp(core_buffer_string(&a->key),
                           core_buffer_string(&b->key));
}

static bool
request_key(const n00bb_specialization_t *item, core_buffer_t *key)
{
    core_put_text(key, ir_module_name(item->generic->owner));
    core_put_text(key, item->generic->name);
    th_type_writer_t *writer = th_type_writer_new(
        ir_module_types(item->generic->owner));
    bool ok = writer && th_type_writer_add(writer, item->signature)
        && th_type_writer_finish(writer, key);
    th_type_writer_free(writer);
    return ok;
}

static void
scc_requests(const n00bb_link_analysis_t *analysis, const cache_scc_t *scc,
             core_buffer_t *out)
{
    core_list_t(n00bb_ir_func_ptr_t) closure =
        core_list_new(n00bb_ir_func_ptr_t);
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t r = 0; r < analysis->specialization_requests.len; r++) {
            const n00bb_specialization_request_t *request =
                &analysis->specialization_requests.data[r];
            bool seen = false;
            for (size_t i = 0; !seen && i < closure.len; i++)
                seen = closure.data[i] == request->instance;
            if (seen || !requested_by(analysis, scc, request->requester,
                                      &closure)) continue;
            core_list_push(closure, request->instance);
            changed = true;
        }
    }
    cache_request_t *requests = closure.len
        ? core_alloc_array(cache_request_t, closure.len) : nullptr;
    uint32_t count = 0;
    for (size_t i = 0; (requests || !closure.len) && i < closure.len; i++)
        for (size_t s = 0; s < analysis->specializations.len; s++) {
            const n00bb_specialization_t *item =
                &analysis->specializations.data[s];
            if (item->instance != closure.data[i]) continue;
            requests[count] = (cache_request_t){.item = item};
            if (request_key(item, &requests[count].key)) count++;
            else core_buffer_release(&requests[count].key);
            break;
        }
    if (count)
        qsort(requests, count, sizeof(*requests), compare_requests);
    th_type_writer_t *writer = th_type_writer_new(
        count ? ir_module_types(requests[0].item->generic->owner) : nullptr);
    core_buffer_t entries = {0};
    for (uint32_t i = 0; writer && i < count; i++) {
        const n00bb_specialization_t *item = requests[i].item;
        core_put_text(&entries, ir_module_name(item->generic->owner));
        core_put_text(&entries, item->generic->name);
        core_put_u32(&entries, th_type_writer_add(writer, item->signature));
    }
    for (uint32_t i = 0; i < count; i++)
        core_buffer_release(&requests[i].key);
    core_free(requests);
    if (!writer) count = 0;
    core_buffer_t blob = {0};
    if (writer) th_type_writer_finish(writer, &blob);
    core_put_bytes(out, blob.data, blob.byte_len);
    core_put_u32(out, count);
    if (entries.byte_len)
        core_buffer_append(out, entries.data, entries.byte_len);
    core_buffer_release(&blob);
    core_buffer_release(&entries);
    if (writer) th_type_writer_free(writer);
    core_list_free(closure);
}

void
n00bb_cache_capture_summaries(n00bb_cache_t *cache,
                              n00bb_link_analysis_t *analysis, uint8_t phase)
{
    ir_summaries_t *summaries = !analysis ? nullptr
        : phase == N00BB_PHASE_ESCAPES ? analysis->escapes
                                       : analysis->library_summaries;
    for (size_t i = 0; cache && phase && phase <= N00BB_PHASE_FINAL
         && i < cache->snapshots.len; i++)
        capture_snapshot(&cache->snapshots.data[i], analysis, summaries,
                         phase);
    if (cache && analysis && phase == N00BB_PHASE_MAIN)
        record_program_summaries(cache, analysis);
}

static bool
scc_clean(const core_list_t(cache_scc_t) *sccs, size_t s, uint8_t *clean)
{
    if (clean[s]) return clean[s] == 1;
    const cache_scc_t *scc = &sccs->data[s];
    bool ok = true;
    for (size_t m = 0; ok && m < scc->members.len; m++)
        ok = !core_diagnostics_failed(core_translation_unit_diagnostics(
            scc->members.data[m]));
    clean[s] = 2;
    for (size_t i = 0; ok && i < scc->imports.len; i++)
        ok = scc->imports.data[i] < sccs->len
            && scc_clean(sccs, scc->imports.data[i], clean);
    clean[s] = ok ? 1 : 2;
    return ok;
}

bool
n00bb_cache_snapshot(n00bb_cache_t *cache, core_link_unit_t *link,
                     n00bb_link_analysis_t *analysis, bool clean_only)
{
    if (!cache || !link) return false;
    core_list_t(cache_scc_t) sccs = core_list_new(cache_scc_t);
    bool ok = collect_sccs(link, &sccs, true);
    uint8_t *clean = clean_only && ok
        ? core_alloc_array(uint8_t, sccs.len ? sccs.len : 1) : nullptr;
    for (size_t s = 0; ok && s < sccs.len; s++) {
        cache_scc_t *scc = &sccs.data[s];
        if (scc->root || scc->cached
            || (clean && !scc_clean(&sccs, s, clean))) continue;
        ir_module_t **modules = core_alloc_array(ir_module_t *,
                                                 scc->members.len);
        for (size_t m = 0; m < scc->members.len; m++)
            modules[m] = tu_state(scc->members.data[m])->ir_module;
        cache_snapshot_t snapshot = {
            .module = core_string_dup(
                scc->members.data[0]->module.identifier),
            .inlined = core_list_new(core_string_t),
            .modules = core_list_new(cache_module_ptr_t),
            .names   = core_list_new(core_string_t),
            .types   = th_type_writer_new(ir_module_types(modules[0])),
        };
        for (size_t m = 0; m < scc->members.len; m++) {
            core_list_push(snapshot.modules, modules[m]);
            core_list_push(snapshot.names, core_string_dup(
                scc->members.data[m]->module.identifier));
        }
        if (analysis) {
            capture_snapshot(&snapshot, analysis, analysis->escapes,
                             N00BB_PHASE_ESCAPES);
            if (analysis->library_summaries)
                capture_snapshot(&snapshot, analysis,
                                 analysis->library_summaries,
                                 N00BB_PHASE_MAIN);
        }
        for (size_t i = 0; i < scc->inlined.len; i++)
            core_list_push(snapshot.inlined, core_string_dup(
                sccs.data[scc->inlined.data[i]].members.data[0]
                    ->module.identifier));
        bool saved = snapshot.module.data
            && ir_modules_save_references(modules, scc->members.len,
                                          &snapshot.ir, &snapshot.references,
                                          snapshot.types);
        core_free(modules);
        if (saved) core_list_push(cache->snapshots, snapshot);
        else snapshot_release(&snapshot);
    }
    core_free(clean);
    sccs_release(&sccs);
    return ok;
}

static const cache_snapshot_t *
snapshot_for(const n00bb_cache_t *cache, core_string_t module)
{
    for (size_t i = 0; i < cache->snapshots.len; i++)
        if (core_string_eq(cache->snapshots.data[i].module, module))
            return &cache->snapshots.data[i];
    return nullptr;
}

static const n00bb_cache_module_object_t *
module_object(const n00bb_cache_module_object_t *objects, size_t count,
              core_string_t module)
{
    for (size_t i = 0; i < count; i++)
        if (objects[i].module.data
            && core_string_eq(objects[i].module, module))
            return &objects[i];
    return nullptr;
}

static void
put_links(core_buffer_t *out, const core_list_t(cache_scc_t) *sccs,
          const core_list_t(size_t) *links, bool freshness)
{
    core_put_u32(out, (uint32_t)links->len);
    for (size_t i = 0; i < links->len; i++) {
        const cache_scc_t *linked = &sccs->data[links->data[i]];
        core_string_t name = linked->members.data[0]->module.identifier;
        core_put_bytes(out, name.data, name.u8_bytes);
        core_put_u64(out, freshness ? linked->freshness : linked->key);
    }
}

static void
module_section_name(core_buffer_t *out, core_string_t kind,
                    core_string_t module)
{
    out->byte_len = 0;
    core_buffer_append_string(out, kind);
    core_buffer_putc(out, '.');
    core_buffer_append_string(out, module);
}

static void
append_scope_path(core_buffer_t *out, const parse_scope_t *scope)
{
    if (!scope) return;
    append_scope_path(out, scope->parent);
    if (scope->name.data && scope->name.u8_bytes) {
        core_buffer_append_string(out, scope->name);
        core_buffer_putc(out, '.');
    }
}

void
n00bb_symbol_use_key(core_buffer_t *out, const core_translation_unit_t *unit,
                     const parse_scope_t *scope,
                     const parse_sym_entry_t *symbol)
{
    out->byte_len = 0;
    core_buffer_append_string(out, unit->module.identifier);
    core_buffer_putc(out, '|');
    append_scope_path(out, scope);
    core_buffer_append_string(out, symbol->name);
}


typedef struct {
    const core_diagnostic_t *const *items;
    size_t count;
} warning_group_t;

static int
compare_text(core_string_t left, core_string_t right)
{
    size_t shared = left.u8_bytes < right.u8_bytes ? left.u8_bytes
                                                   : right.u8_bytes;
    int order = shared ? memcmp(left.data, right.data, shared) : 0;
    if (order) return order;
    return (left.u8_bytes > right.u8_bytes) - (left.u8_bytes < right.u8_bytes);
}

static int
warning_group_order(const void *left_raw, const void *right_raw)
{
    const core_diagnostic_t *left = ((const warning_group_t *)left_raw)
        ->items[0];
    const core_diagnostic_t *right = ((const warning_group_t *)right_raw)
        ->items[0];
    int order = compare_text(left->path, right->path);
    if (!order)
        order = (left->location.line > right->location.line)
            - (left->location.line < right->location.line);
    if (!order)
        order = (left->location.column > right->location.column)
            - (left->location.column < right->location.column);
    if (!order) order = compare_text(left->code, right->code);
    if (!order) order = compare_text(left->message, right->message);
    return order;
}

static void
put_warnings(core_buffer_t *out, core_translation_unit_t *unit)
{
    const core_diagnostics_t *diagnostics =
        core_translation_unit_diagnostics(unit);
    size_t total = core_diagnostics_count(diagnostics);
    const core_diagnostic_t **items = core_alloc_array(
        const core_diagnostic_t *, total + 1);
    warning_group_t *groups = core_alloc_array(warning_group_t, total + 1);
    size_t group_count = 0, item_count = 0;
    bool attached = false;
    for (size_t i = 0; i < total; i++) {
        const core_diagnostic_t *item = core_diagnostics_at(diagnostics, i);
        bool note = item->severity == CORE_DIAG_NOTE
            || item->severity == CORE_DIAG_HELP;
        attached = note ? attached
            : core_diag_is_advisory(item->severity)
                && !n00bb_diag_reported_afresh(item->code);
        if (!attached) continue;
        if (!note)
            groups[group_count++] = (warning_group_t){
                .items = &items[item_count]};
        items[item_count++] = item;
        groups[group_count - 1].count++;
    }
    qsort(groups, group_count, sizeof(*groups), warning_group_order);
    core_put_u32(out, (uint32_t)item_count);
    for (size_t g = 0; g < group_count; g++)
        for (size_t i = 0; i < groups[g].count; i++)
            core_diagnostic_encode(out, groups[g].items[i]);
    core_free(groups);
    core_free(items);
}

static bool
snapshot_summaries(const cache_snapshot_t *snapshot, uint8_t phase,
                   core_string_t module, const uint8_t **data, size_t *length)
{
    if (!snapshot || !snapshot->captured[phase]) return false;
    core_byte_cursor_t r = core_cursor(snapshot->summaries[phase].data,
                                       snapshot->summaries[phase].byte_len);
    while (!r.failed && r.at < r.end) {
        core_string_t name = core_cursor_text(&r);
        *data = core_cursor_bytes(&r, length);
        if (!r.failed && core_string_eq(name, module)) return true;
    }
    return false;
}

static void
put_members(core_buffer_t *manifest, const cache_scc_t *scc)
{
    core_put_u64(manifest, scc->key);
    core_put_u64(manifest, scc->freshness);
    core_put_u32(manifest, (uint32_t)scc->members.len);
    for (size_t m = 0; m < scc->members.len; m++) {
        core_translation_unit_t *unit = scc->members.data[m];
        core_string_t path = core_source_file_path(&unit->source);
        core_string_t canonical = canonical_path(path);
        core_put_bytes(manifest, unit->module.identifier.data,
                  unit->module.identifier.u8_bytes);
        core_put_text(manifest, canonical);
        core_free(canonical.data);
        core_put_u64(manifest, core_xxh3_64bits_raw(
            core_source_file_contents(&unit->source).data,
            core_source_file_size(&unit->source)));
        core_put_u32(manifest,
                (uint32_t)core_translation_unit_import_count(unit));
        for (size_t i = 0; i < core_translation_unit_import_count(unit);
             i++) {
            core_string_t name =
                core_translation_unit_import(unit, i)->module.identifier;
            core_put_bytes(manifest, name.data, name.u8_bytes);
        }
        core_string_t owner = {0}, owner_path = {0};
        core_string_t owner_canonical = {0};
        if (n00bb_source_file_generated_owner(&unit->source, &owner,
                                              &owner_path))
            owner_canonical = canonical_path(owner_path);
        core_put_u32(manifest, owner_canonical.data ? 1 : 0);
        if (owner_canonical.data) {
            core_put_text(manifest, owner);
            core_put_text(manifest, owner_canonical);
            core_put_text(manifest,
                n00bb_source_file_generated_inputs(&unit->source));
        }
        core_free(owner_canonical.data);
    }
}

static bool
holds_state(core_translation_unit_t *unit)
{
    ir_module_t *module = tu_state(unit)->ir_module;
    for (size_t g = 0; g < ir_global_count(module); g++) {
        ir_global_t *global = ir_global_at(module, g);
        if (!global->is_extern && global->mutability != IR_MUT_CONST)
            return true;
    }
    return false;
}

static void
store_record(n00bb_cache_t *cache, const core_list_t(cache_scc_t) *sccs)
{
    cache_scc_t program = {
        .key = record_key(cache),
        .members = core_list_new(core_translation_unit_ptr_t),
    };
    for (size_t s = 0; s < sccs->len; s++)
        for (size_t m = 0; m < sccs->data[s].members.len; m++)
            core_list_push(program.members, sccs->data[s].members.data[m]);
    qsort(program.members.data, program.members.len,
          sizeof(*program.members.data), compare_units);
    scc_freshness(&program);
    core_buffer_t manifest = {0}, serialized = {0}, path = {0}, table = {0};
    put_members(&manifest, &program);
    for (int i = 0; i < 3; i++) core_put_u32(&manifest, 0);
    object_path_in(cache->directory, program.key, &path);
    if (sections_set(&cache->recording, CORE_STRING_STATIC("manifest"),
                     manifest.data, manifest.byte_len)
        && sections_set(&cache->recording, CORE_STRING_STATIC("changed"),
                        cache->unstored.data, cache->unstored.byte_len)
        && (!cache->recording_types
            || (th_type_writer_finish(cache->recording_types, &table)
                && sections_set(&cache->recording,
                                CORE_STRING_STATIC("types"), table.data,
                                table.byte_len)))) {
        sections_serialize(&cache->recording, &serialized);
        (void)core_os_write_file_atomic(core_buffer_string(&path),
                                        serialized.data, serialized.byte_len);
    }
    core_buffer_release(&table);
    core_buffer_release(&serialized);
    core_buffer_release(&path);
    core_buffer_release(&manifest);
    core_list_free(program.members);
}

static bool
store_scc(const n00bb_cache_t *cache, const n00bb_link_analysis_t *analysis,
          const core_list_t(cache_scc_t) *sccs,
          const cache_scc_t *scc, const cache_snapshot_t *snapshot,
          const n00bb_cache_module_object_t *objects, size_t object_count)
{
    core_string_t phase = phase_name();
    core_buffer_t manifest = {0};
    put_members(&manifest, scc);
    put_links(&manifest, sccs, &scc->imports, false);
    put_links(&manifest, sccs, &scc->inlined, true);
    const core_buffer_t *ir = &snapshot->ir;
    core_buffer_t references = {0};
    uint32_t reference_count = 0;
    for (const char *at = snapshot->references.data,
             *end = at ? at + snapshot->references.byte_len : nullptr;
         at && at < end;) {
        const char *line = memchr(at, '\n', (size_t)(end - at));
        size_t span = line ? (size_t)(line - at) : (size_t)(end - at);
        core_put_bytes(&references, at, span);
        reference_count++;
        at = line ? line + 1 : end;
    }
    core_put_u32(&manifest, reference_count);
    if (references.byte_len)
        core_buffer_append(&manifest, references.data, references.byte_len);
    core_buffer_release(&references);
    core_string_t root = scc_directory(cache, scc);
    core_buffer_t path = {0};
    object_path_in(root, scc->key, &path);
    core_list_t(cache_section_t) sections = core_list_new(cache_section_t);
    core_buffer_t name = {0};
    bool ok = sections_set(&sections, CORE_STRING_STATIC("manifest"),
                           manifest.data, manifest.byte_len)
        && sections_set(&sections, CORE_STRING_STATIC("ir"), ir->data,
                        ir->byte_len);
    core_buffer_t table = {0};
    ok = ok && th_type_writer_finish(snapshot->types, &table)
        && sections_set(&sections, CORE_STRING_STATIC("types"), table.data,
                        table.byte_len)
        && sections_set(&sections, CORE_STRING_STATIC("interface"),
                        scc->interface, scc->interface_bytes);
    core_buffer_release(&table);
    if (objects) {
        core_buffer_t requests = {0};
        scc_requests(analysis, scc, &requests);
        ok = ok && sections_set(&sections, CORE_STRING_STATIC("requests"),
                                requests.data, requests.byte_len);
        core_buffer_release(&requests);
    }
    for (size_t m = 0; ok && m < scc->members.len; m++) {
        core_string_t module = scc->members.data[m]->module.identifier;
        const n00bb_cache_module_object_t *object = module_object(objects,
            object_count, module);
        bool present = object && object->object && object->abi;
        ok = present || objects_optional();
        if (!ok) break;
        core_buffer_t warnings = {0};
        core_translation_unit_t *root = analysis->units.len
            ? analysis->units.data[0] : nullptr;
        if (root && n00bb_resolver_same_root(analysis->resolver, root,
                                             scc->members.data[m]))
            put_warnings(&warnings, scc->members.data[m]);
        else core_put_u32(&warnings, 0);
        n00bb_tu_state_t *state = tu_state(scc->members.data[m]);
        struct { core_string_t kind; const void *data; size_t length; }
            parts[8 + N00BB_PHASE_FINAL] = {
            {CORE_STRING_STATIC("object"), present ? object->object : nullptr,
             present ? object->object_bytes : 0},
            {CORE_STRING_STATIC("abi"), present ? object->abi : nullptr,
             present ? object->abi_bytes : 0},
            {CORE_STRING_STATIC("mutators"), !present ? nullptr
                : object->mutators ? object->mutators : (const uint8_t *)"",
             present ? object->mutators_bytes : 0},
            {CORE_STRING_STATIC("runtime-object"),
             present ? object->runtime_object : nullptr,
             present ? object->runtime_object_bytes : 0},
            {CORE_STRING_STATIC("warnings"), warnings.data ? warnings.data : "",
             warnings.byte_len},
            {CORE_STRING_STATIC("unused"), state->unused_facts.data
                ? state->unused_facts.data : "", state->unused_facts.byte_len},
            {CORE_STRING_STATIC("sites"), state->declaration_sites.data
                ? state->declaration_sites.data : "",
             state->declaration_sites.byte_len},
            {CORE_STRING_STATIC("state"),
             holds_state(scc->members.data[m]) ? "" : nullptr, 0},
        };
        for (uint8_t p = 0; p < N00BB_PHASE_FINAL; p++) {
            const uint8_t *stored = nullptr;
            size_t stored_bytes = 0;
            bool found = snapshot_summaries(snapshot, p, module, &stored,
                                            &stored_bytes);
            parts[8 + p] = (typeof(parts[0])){summary_sections[p],
                found ? (stored ? (const void *)stored : "") : nullptr,
                stored_bytes};
        }
        for (size_t s = 0; ok && s < sizeof(parts) / sizeof(*parts); s++) {
            if (!parts[s].data) continue;
            module_section_name(&name, parts[s].kind, module);
            ok = sections_set(&sections, core_buffer_string(&name),
                              parts[s].data, parts[s].length);
        }
        core_buffer_release(&warnings);
    }
    core_buffer_release(&name);
    core_buffer_t serialized = {0};
    if (ok) sections_serialize(&sections, &serialized);
    ok = ok && core_os_write_file_atomic(core_buffer_string(&path), serialized.data,
                                 serialized.byte_len);
    for (size_t m = 0; ok && m < scc->members.len; m++) {
        core_string_t module = scc->members.data[m]->module.identifier;
        core_buffer_t index = {0};
        core_buffer_t key = {0};
        core_buffer_printf(&index, "%.*s/modules/%.*s.%.*s.key",
                           (int)root.u8_bytes, root.data,
                           (int)module.u8_bytes, module.data,
                           (int)phase.u8_bytes, phase.data);
        core_buffer_printf(&key, "%016llx %016llx\n",
                           (unsigned long long)scc->key,
                           (unsigned long long)cache->compiler);
        uint8_t *previous = nullptr;
        size_t previous_bytes = 0;
        size_t kept = 1;
        size_t foreign = 0;
        size_t at = 0;
        uint64_t older = 0;
        uint64_t compiler = 0;
        bool readable = read_file(core_buffer_string(&index), &previous,
                                  &previous_bytes);
        while (readable && next_key_line(previous, previous_bytes, &at,
                                         &older, &compiler)) {
            if (!older || older == scc->key) continue;
            bool ours = compiler == cache->compiler;
            if (compiler && (ours ? kept < cache_module_candidates
                                  : foreign < cache_foreign_candidates)) {
                core_buffer_printf(&key, "%016llx %016llx\n",
                                   (unsigned long long)older,
                                   (unsigned long long)compiler);
                if (ours) kept++;
                else foreign++;
                continue;
            }
            core_buffer_t stale = {0};
            object_path_in(root, older, &stale);
            core_os_remove_file(core_buffer_string(&stale));
            core_buffer_release(&stale);
        }
        core_free(previous);
        ok = core_os_write_file_atomic(core_buffer_string(&index), key.data,
                               key.byte_len);
        core_buffer_release(&index);
        core_buffer_release(&key);
    }
    core_buffer_release(&serialized);
    sections_release(&sections);
    core_buffer_release(&path);
    core_buffer_release(&manifest);
    return ok;
}

static uint64_t
scc_key_of(const n00bb_cache_t *cache, core_list_t(cache_scc_t) *sccs,
           size_t s)
{
    cache_scc_t *scc = &sccs->data[s];
    if (scc->key) return scc->key;
    const cache_snapshot_t *snapshot = snapshot_for(cache,
        scc->members.data[0]->module.identifier);
    if (snapshot) {
        scc->inlined.len = 0;
        for (size_t i = 0; i < snapshot->inlined.len; i++) {
            size_t owner = scc_of_module_name(sccs,
                                              snapshot->inlined.data[i]);
            if (owner != SIZE_MAX) core_list_push(scc->inlined, owner);
        }
    }
    uint64_t *imports = core_alloc_array(uint64_t, scc->imports.len + 1);
    uint64_t *inlined = core_alloc_array(uint64_t, scc->inlined.len + 1);
    for (size_t i = 0; i < scc->imports.len; i++)
        imports[i] = scc_key_of(cache, sccs, scc->imports.data[i]);
    for (size_t i = 0; i < scc->inlined.len; i++)
        inlined[i] = sccs->data[scc->inlined.data[i]].freshness;
    scc->key = scc_key(cache, scc_in_stdlib(cache, scc), scc->freshness,
                       imports, scc->imports.len, inlined, scc->inlined.len);
    core_free(imports);
    core_free(inlined);
    return scc->key;
}

static bool
prepare_sccs(n00bb_cache_t *cache, core_link_unit_t *link,
             n00bb_link_analysis_t *analysis)
{
    if (cache->prepared) return !cache->prepare_failed;
    cache->prepared = true;
    core_list_t(cache_scc_t) *sccs = &cache->sccs;
    bool ok = collect_sccs(link, sccs, true);
    for (size_t s = 0; ok && s < sccs->len; s++) {
        core_string_t first = sccs->data[s].members.data[0]->module.identifier;
        const cache_entry_t *entry = nullptr;
        if (sccs->data[s].cached && loaded_member(cache, first, &entry)) {
            sccs->data[s].freshness = entry->freshness;
            sccs->data[s].key = entry->key;
            continue;
        }
        ok = interface_bytes(analysis, sccs, &sccs->data[s]);
    }
    for (size_t s = 0; ok && s < sccs->len; s++)
        (void)scc_key_of(cache, sccs, s);
    cache->prepare_failed = !ok;
    return ok;
}

static void
collect_type_owners(const core_list_t(cache_scc_t) *sccs, th_type *type,
                    core_list_t(size_t) *owners, unsigned depth)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type || depth > 64) return;
    switch (th_kind_of(type)) {
    case TH_NAMED: {
        core_string_t name = {0};
        size_t count = 0;
        if (!th_named_info(type, &name, nullptr, &count)) return;
        size_t prefix = name.data ? owner_module_length(sccs, name) : 0;
        if (prefix) {
            size_t scc = scc_of_module_name(sccs,
                core_string_slice(name, 0, prefix));
            bool seen = scc == SIZE_MAX;
            for (size_t i = 0; !seen && i < owners->len; i++)
                seen = owners->data[i] == scc;
            if (!seen) core_list_push(*owners, scc);
        }
        for (size_t i = 0; i < count; i++) {
            const th_arg *arg = th_named_arg(type, i);
            if (arg && arg->kind == TH_ARG_TYPE)
                collect_type_owners(sccs, arg->type, owners, depth + 1);
        }
        return;
    }
    case TH_INDIRECTION:
        collect_type_owners(sccs, th_ind_referent(type), owners, depth + 1);
        return;
    case TH_SEQUENCE:
        collect_type_owners(sccs, th_seq_elem(type), owners, depth + 1);
        return;
    case TH_FUNCTION:
        collect_type_owners(sccs, th_func_ret(type), owners, depth + 1);
        collect_type_owners(sccs, th_func_error(type), owners, depth + 1);
        for (size_t i = 0; i < th_func_nparams(type); i++)
            collect_type_owners(sccs, th_func_param(type, i)->type, owners,
                                depth + 1);
        if (th_func_rest(type))
            collect_type_owners(sccs, th_func_rest(type)->type, owners,
                                depth + 1);
        for (size_t i = 0; i < th_func_nkw(type); i++)
            collect_type_owners(sccs, th_func_kw(type, i)->type, owners,
                                depth + 1);
        return;
    case TH_SUM:
        for (size_t i = 0; i < th_sum_len(type); i++)
            collect_type_owners(sccs, th_sum_arm(type, i), owners,
                                depth + 1);
        return;
    case TH_RECORD:
        for (size_t i = 0; i < th_record_len(type); i++)
            collect_type_owners(sccs, th_record_member(type, i)->type, owners,
                                depth + 1);
        return;
    default:
        return;
    }
}

static int
compare_indices(const void *left, const void *right)
{
    size_t a = *(const size_t *)left, b = *(const size_t *)right;
    return (a > b) - (a < b);
}

static bool
scc_ir_hash(const n00bb_cache_t *cache, const cache_scc_t *scc,
            uint64_t *out)
{
    core_string_t first = scc->members.data[0]->module.identifier;
    const cache_entry_t *entry = nullptr;
    if (scc->cached && loaded_member(cache, first, &entry)) {
        const cache_section_t *ir = phase_section(entry,
            CORE_STRING_STATIC("ir"), (core_string_t){0});
        if (!ir) return false;
        *out = core_xxh3_64bits_raw(ir->data, ir->length);
        return true;
    }
    const cache_snapshot_t *snapshot = snapshot_for(cache, first);
    if (!snapshot) return false;
    *out = buffer_hash(&snapshot->ir);
    return true;
}

static bool
allocates_direct(const ir_func_t *function)
{
    ir_summary_t *summary = function->n_blocks
        ? ir_summary_of(ir_analysis_summaries(), function) : nullptr;
    return summary
        && (ir_summary_flags(summary) & IR_SUMMARY_ALLOCATES_DIRECT);
}

static bool
instance_key(n00bb_cache_t *cache, const n00bb_specialization_t *item,
             uint64_t *key)
{
    const core_list_t(cache_scc_t) *sccs = &cache->sccs;
    size_t generic = scc_of_module_name(sccs,
        ir_module_name(item->generic->owner));
    if (generic == SIZE_MAX
        || scc_uses_library(cache, sccs, &sccs->data[generic]))
        return false;
    core_list_t(size_t) owners = core_list_new(size_t);
    collect_type_owners(sccs, item->signature, &owners, 0);
    for (size_t i = 0; i < owners.len; i++)
        if (scc_uses_library(cache, sccs, &sccs->data[owners.data[i]])) {
            core_list_free(owners);
            return false;
        }
    if (owners.len)
        qsort(owners.data, owners.len, sizeof(*owners.data), compare_indices);
    uint64_t generic_ir = 0;
    bool saved = scc_ir_hash(cache, &sccs->data[generic], &generic_ir);
    core_buffer_t input = {0};
    core_buffer_puts(&input, "n00b.instance.key.v4");
    core_put_u64(&input, sccs->data[generic].key);
    core_put_u64(&input, generic_ir);
    core_put_text(&input, ir_module_name(item->instance->owner));
    core_put_text(&input, item->instance->name);
    core_put_u32(&input, (uint32_t)owners.len);
    for (size_t i = 0; i < owners.len; i++)
        core_put_u64(&input, sccs->data[owners.data[i]].key);
    ir_module_t *module = item->instance->owner;
    for (size_t f = 0; f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        core_put_u32(&input, allocates_direct(function));
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_func_t *callee = n00bb_direct_call_target(
                    function->blocks[b]->instrs[i]);
                if (!callee || !allocates_direct(callee)) continue;
                core_put_text(&input, ir_module_name(callee->owner));
                core_put_text(&input, callee->name);
            }
    }
    *key = buffer_hash(&input);
    core_buffer_release(&input);
    core_list_free(owners);
    return saved;
}

static const cache_instance_t *
loaded_instance(const n00bb_cache_t *cache, const ir_module_t *module)
{
    for (size_t i = 0; cache && i < cache->instances.len; i++)
        if (cache->instances.data[i].module == module)
            return &cache->instances.data[i];
    return nullptr;
}

static void
log_instance(core_string_t action, const n00bb_specialization_t *item)
{
    core_string_t phase = phase_name();
    core_string_t module = ir_module_name(item->generic->owner);
    core_string_t function = item->generic->name;
    cache_log("%.*s %.*s instance %.*s.%.*s",
              (int)phase.u8_bytes, phase.data,
              (int)action.u8_bytes, action.data,
              (int)module.u8_bytes, module.data ? module.data : "",
              (int)function.u8_bytes, function.data ? function.data : "");
}

bool
n00bb_cache_instance_object(n00bb_cache_t *cache, core_link_unit_t *link,
                            n00bb_link_analysis_t *analysis,
                            const n00bb_specialization_t *item,
                            n00bb_cache_module_object_t *object)
{
    if (!cache || !item || !object) return false;
    const ir_module_t *module = item->instance->owner;
    const cache_instance_t *found = loaded_instance(cache, module);
    uint64_t key = 0;
    if (!found) {
        if (!prepare_sccs(cache, link, analysis)
            || !instance_key(cache, item, &key)) return false;
        core_buffer_t path = {0};
        object_path(cache, key, &path);
        uint8_t *bytes = nullptr;
        size_t length = 0;
        cache_instance_t loaded = {
            .module = module, .sections = core_list_new(cache_section_t),
        };
        bool ok = read_file(core_buffer_string(&path), &bytes, &length)
            && sections_parse(bytes, length, &loaded.sections)
            && section_named(&loaded.sections, CORE_STRING_STATIC("object"));
        core_free(bytes);
        core_buffer_release(&path);
        if (!ok) {
            sections_release(&loaded.sections);
            return false;
        }
        core_list_push(cache->instances, loaded);
        found = &cache->instances.data[cache->instances.len - 1];
        log_instance(CORE_STRING_STATIC("reuse"), item);
    }
    const cache_section_t *code = section_named(&found->sections,
        CORE_STRING_STATIC("object"));
    const cache_section_t *mutators = section_named(&found->sections,
        CORE_STRING_STATIC("mutators"));
    if (!code || !mutators) return false;
    *object = (n00bb_cache_module_object_t){
        .module = ir_module_name(module),
        .object = code->data, .object_bytes = code->length,
        .mutators = mutators->data, .mutators_bytes = mutators->length,
    };
    return true;
}

static bool
store_instance(n00bb_cache_t *cache, const n00bb_specialization_t *item,
               const n00bb_cache_module_object_t *object)
{
    uint64_t key = 0;
    if (!instance_key(cache, item, &key)) return true;
    core_buffer_t path = {0};
    object_path(cache, key, &path);
    core_list_t(cache_section_t) sections = core_list_new(cache_section_t);
    bool ok = sections_set(&sections, CORE_STRING_STATIC("object"),
                           object->object, object->object_bytes)
        && sections_set(&sections, CORE_STRING_STATIC("mutators"),
            object->mutators ? object->mutators : (const uint8_t *)"",
            object->mutators_bytes);
    core_buffer_t serialized = {0};
    if (ok) sections_serialize(&sections, &serialized);
    ok = ok && core_os_write_file_atomic(core_buffer_string(&path), serialized.data,
                                 serialized.byte_len);
    core_buffer_release(&serialized);
    sections_release(&sections);
    core_buffer_release(&path);
    return ok;
}

static int fill_lock = -1;
static bool fill_waited = false;

static void
release_fill_lock(void)
{
    if (fill_lock < 0) return;
    n00bb_unlock_file(fill_lock);
    close(fill_lock);
    fill_lock = -1;
}

static bool
take_fill_lock(n00bb_cache_t *cache)
{
    core_string_t builtin = CORE_STRING_STATIC("builtin.__init");
    if (fill_lock >= 0 || fill_waited || !cache->shared.data
        || core_dict_contains(&cache->loaded_members, &builtin))
        return false;
    core_buffer_t variant = {0};
    core_put_text(&variant, phase_name());
    core_put_u64(&variant, cache->compiler);
    core_put_bytes(&variant, cache->flags.data, cache->flags.byte_len);
    core_put_text(&variant, cache->triple);
    core_buffer_t path = {0};
    core_buffer_append_string(&path, cache->shared);
    core_buffer_printf(&path, "/fill-%016llx.lock",
                       (unsigned long long)buffer_hash(&variant));
    core_buffer_release(&variant);
    bool waited = false;
    int fd = n00bb_make_directories(cache->shared)
        ? n00bb_open_lock_file(core_buffer_string(&path)) : -1;
    if (fd >= 0 && !n00bb_lock_file(fd, false)) {
        core_string_t phase = phase_name();
        cache_log("%.*s wait for another build to cache builtin",
                  (int)phase.u8_bytes, phase.data);
        waited = n00bb_lock_file(fd, true);
        if (waited) n00bb_unlock_file(fd);
        close(fd);
        fd = -1;
        fill_waited = true;
    }
    fill_lock = fd;
    core_buffer_release(&path);
    return waited;
}

bool
n00bb_cache_store(n00bb_cache_t *cache, core_link_unit_t *link,
                  n00bb_link_analysis_t *analysis,
                  const n00bb_cache_module_object_t *objects,
                  size_t object_count)
{
    if (!cache || !link || !analysis) return false;
    core_buffer_t modules_directory = {0};
    core_buffer_append_string(&modules_directory, cache->directory);
    core_buffer_puts(&modules_directory, "/modules");
    core_buffer_t shared_directory = {0};
    core_buffer_append_string(&shared_directory, cache->shared);
    core_buffer_puts(&shared_directory, "/modules");
    bool ok = n00bb_make_directories(core_buffer_string(&modules_directory))
        && n00bb_make_directories(core_buffer_string(&shared_directory))
        && prepare_sccs(cache, link, analysis);
    core_buffer_release(&modules_directory);
    core_buffer_release(&shared_directory);
    core_list_t(cache_scc_t) *sccs = &cache->sccs;
    for (size_t s = 0; ok && s < sccs->len; s++) {
        cache_scc_t *scc = &sccs->data[s];
        bool unstored = scc_uses_library(cache, sccs, scc);
        for (size_t m = 0; !unstored && m < scc->members.len; m++)
            unstored = name_listed(core_buffer_string(&cache->unstored),
                                   scc->members.data[m]->module.identifier);
        const cache_snapshot_t *snapshot = scc->root || scc->cached
                || unstored
            ? nullptr
            : snapshot_for(cache, scc->members.data[0]->module.identifier);
        if (snapshot)
            ok = store_scc(cache, analysis, sccs, scc, snapshot, objects,
                           object_count);
        if (snapshot && ok) {
            core_buffer_t members = {0};
            for (size_t m = 0; m < scc->members.len; m++) {
                if (m) core_buffer_putc(&members, ',');
                core_buffer_append_string(&members,
                    scc->members.data[m]->module.identifier);
            }
            core_string_t phase = phase_name();
            cache_log("%.*s store scc %.*s", (int)phase.u8_bytes, phase.data,
                      (int)members.byte_len,
                      members.data ? members.data : "");
            core_buffer_release(&members);
        }
    }
    for (size_t i = 0; ok && i < analysis->specializations.len; i++) {
        const n00bb_specialization_t *item =
            &analysis->specializations.data[i];
        if (loaded_instance(cache, item->instance->owner)) continue;
        const n00bb_cache_module_object_t *object = module_object(objects,
            object_count, ir_module_name(item->instance->owner));
        if (object && object->object)
            ok = store_instance(cache, item, object);
        if (object && ok)
            log_instance(CORE_STRING_STATIC("store"), item);
    }
    if (ok && !cache->recorded) store_record(cache, sccs);
    release_fill_lock();
    return ok;
}
