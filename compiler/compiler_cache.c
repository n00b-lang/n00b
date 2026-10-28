#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cache_location.h"
#include "compiler_cache.h"
#include "handoff.h"
#include "compiler_phases.h"
#include "compiler_tu.h"
#include "config.h"
#include "driver.h"
#include "builtin_types.h"
#include "n00bb_version.h"
#include "ir/archive.h"
#include "ir/summary.h"
#include "lib/alloc.h"
#include "lib/os.h"
#include "lib/buffer.h"
#include "lib/hash.h"
#include "th/environment.h"

#define CACHE_MAGIC   UINT32_C(0x4f42304e)
#define CACHE_VERSION UINT32_C(1)

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
    uint64_t interface_identity;
    uint64_t summary_identity;
    uint64_t key;
    uint8_t *interface;
    size_t interface_bytes;
    core_list_t(size_t) inlined;
    bool root;
    bool cached;
} cache_scc_t;
core_list_decl(cache_scc_t);

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
    put_u64(out, length);
    if (length) core_buffer_append(out, data, length);
}

static void
put_text(core_buffer_t *out, core_string_t text)
{
    put_bytes(out, text.data, text.data ? text.u8_bytes : 0);
}

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

static bool
sections_parse(const uint8_t *data, size_t length,
               core_list_t(cache_section_t) *sections)
{
    size_t at = 0;
    if (length < 12) return false;
    uint32_t magic = 0, version = 0, count = 0;
    for (int i = 0; i < 4; i++) magic |= (uint32_t)data[i] << (8 * i);
    for (int i = 0; i < 4; i++) version |= (uint32_t)data[4 + i] << (8 * i);
    for (int i = 0; i < 4; i++) count |= (uint32_t)data[8 + i] << (8 * i);
    if (magic != CACHE_MAGIC || version != CACHE_VERSION) return false;
    at = 12;
    for (uint32_t s = 0; s < count; s++) {
        size_t fields[2] = {0};
        const uint8_t *starts[2] = {0};
        for (int f = 0; f < 2; f++) {
            if (length - at < 8) return false;
            uint64_t size = 0;
            for (int i = 0; i < 8; i++)
                size |= (uint64_t)data[at + i] << (8 * i);
            at += 8;
            if (size > length - at) return false;
            starts[f] = data + at;
            fields[f] = (size_t)size;
            at += (size_t)size;
        }
        cache_section_t section = {
            .name = core_string_dup(core_string_borrow(
                (const char *)starts[0], fields[0])),
            .data = malloc(fields[1] ? fields[1] : 1),
            .length = fields[1],
        };
        if (!section.name.data || !section.data) {
            core_free(section.name.data);
            free(section.data);
            return false;
        }
        if (fields[1]) memcpy(section.data, starts[1], fields[1]);
        core_list_push(*sections, section);
    }
    return true;
}

static void
sections_serialize(const core_list_t(cache_section_t) *sections,
                   core_buffer_t *out)
{
    put_u32(out, CACHE_MAGIC);
    put_u32(out, CACHE_VERSION);
    put_u32(out, (uint32_t)sections->len);
    for (size_t i = 0; i < sections->len; i++) {
        put_text(out, sections->data[i].name);
        put_bytes(out, sections->data[i].data, sections->data[i].length);
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

static bool cache_check_only;

void
n00bb_cache_set_check_only(bool check_only)
{
    cache_check_only = check_only;
}

static bool
objects_optional(void)
{
    return cache_check_only
        || n00bb_compile_phase() == PARSE_SYM_PHASE_COMPTIME;
}

static core_string_t
phase_name(void)
{
    bool comptime = n00bb_compile_phase() == PARSE_SYM_PHASE_COMPTIME;
    if (cache_check_only)
        return comptime ? CORE_STRING_STATIC("check-comptime")
                        : CORE_STRING_STATIC("check-runtime");
    return comptime ? CORE_STRING_STATIC("comptime")
                    : CORE_STRING_STATIC("runtime");
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
        N00BB_KEY_BUILD_RELEASE, N00BB_KEY_BUILD_NATIVE, N00BB_KEY_BUILD_LTO,
        N00BB_KEY_BUILD_UNCHECKED_WIDE_NARROWING, N00BB_KEY_BUILD_PANIC_TRAP,
    };
    for (size_t i = 0; i < sizeof(settings) / sizeof(*settings); i++)
        put_u32(out, n00bb_setting_bool(settings[i]));
    static const core_string_t flags[] = {
        CORE_STRING_STATIC("--gc-conservative-stacks"),
        CORE_STRING_STATIC("--no-specialize"),
        CORE_STRING_STATIC("--closed-world"),
        CORE_STRING_STATIC("--function-summaries"),
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(*flags); i++)
        put_u32(out, parse_cmdr_flag_present(args, flags[i]));
    n00bb_setting_id_t defines = N00BB_KEY_BUILD_DEFINES;
    put_u32(out, (uint32_t)n00bb_setting_count(defines));
    for (size_t i = 0; i < n00bb_setting_count(defines); i++) {
        core_string_t value = n00bb_setting_at(defines, i);
        put_bytes(out, value.data, value.u8_bytes);
    }
    static const core_string_t valued[] = {
        CORE_STRING_STATIC("--native-root"),
    };
    for (size_t f = 0; f < sizeof(valued) / sizeof(*valued); f++) {
        core_string_t flag = valued[f];
        int32_t count = parse_cmdr_flag_value_count(args, flag);
        put_u32(out, (uint32_t)(count > 0 ? count : 0));
        for (int32_t i = 0; i < count; i++) {
            core_string_t value = parse_cmdr_flag_str_at(args, flag, i);
            put_bytes(out, value.data, value.u8_bytes);
        }
    }
}

static void
freshness_member(core_buffer_t *input, const void *name, size_t name_bytes,
                 const void *source, size_t source_bytes)
{
    put_bytes(input, name, name_bytes);
    put_bytes(input, source, source_bytes);
}

static void
scc_freshness(cache_scc_t *scc)
{
    core_buffer_t input = {0};
    core_buffer_puts(&input, "n00b.scc.freshness.v1");
    for (size_t m = 0; m < scc->members.len; m++) {
        core_translation_unit_t *unit = scc->members.data[m];
        freshness_member(&input, unit->module.identifier.data,
                         unit->module.identifier.u8_bytes,
                         core_source_file_contents(&unit->source).data,
                         core_source_file_size(&unit->source));
        for (size_t i = 0; i < core_translation_unit_import_count(unit);
             i++) {
            core_string_t name =
                core_translation_unit_import(unit, i)->module.identifier;
            put_bytes(&input, name.data, name.u8_bytes);
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
                    ir_value_t *callee = call->op == IR_OP_CALL
                        && call->n_operands
                        ? ir_operand_value(&call->operands[0]) : nullptr;
                    ir_func_t *target = callee && callee->kind == IR_VAL_FUNC
                        ? callee->owner : nullptr;
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
} cache_member_t;
core_list_decl(cache_member_t);

typedef struct {
    core_string_t name;
    uint64_t value;
} cache_link_t;
core_list_decl(cache_link_t);

typedef enum {
    ENTRY_UNKNOWN,
    ENTRY_VISITING,
    ENTRY_VALID,
    ENTRY_INVALID,
} entry_state_t;

typedef struct {
    uint64_t key;
    uint64_t freshness;
    uint64_t interface_identity;
    uint64_t summary_identity;
    core_list_t(cache_member_t) members;
    core_list_t(cache_link_t) imports;
    core_list_t(cache_link_t) import_summaries;
    core_list_t(cache_link_t) inlined;
    core_list_t(core_string_t) references;
    core_list_t(cache_section_t) sections;
    core_list_t(size_t) assumed;
    bool fresh;
    bool deferred;
    bool loaded;
    bool dispatch_stale;
    entry_state_t state;
    entry_state_t load;
    th_env *interface;
} cache_entry_t;
core_list_decl(cache_entry_t);

typedef struct {
    core_string_t module;
    core_buffer_t ir;
    core_buffer_t references;
} cache_snapshot_t;
core_list_decl(cache_snapshot_t);

typedef struct {
    const ir_module_t *module;
    core_list_t(cache_section_t) sections;
} cache_instance_t;
core_list_decl(cache_instance_t);

struct n00bb_cache_t {
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
    core_list_t(cache_link_t) summary_identities;
    bool prepared;
    bool prepare_failed;
    bool strict;
    bool ir_load_failed;
    bool assumptions_confirmed;
    core_list_t(cache_instance_t) instances;
    n00bb_cache_locate_fn locate;
    void *locate_context;
};

static uint64_t
scc_key(const n00bb_cache_t *cache, uint64_t freshness,
        const uint64_t *imports, size_t import_count,
        const uint64_t *inlined, size_t inlined_count)
{
    core_buffer_t key = {0};
    core_buffer_puts(&key, "n00b.scc.key.v1");
    put_u64(&key, freshness);
    put_u32(&key, (uint32_t)import_count);
    for (size_t i = 0; i < import_count; i++) put_u64(&key, imports[i]);
    put_u32(&key, (uint32_t)inlined_count);
    for (size_t i = 0; i < inlined_count; i++) put_u64(&key, inlined[i]);
    put_u64(&key, cache->compiler);
    put_bytes(&key, cache->flags.data, cache->flags.byte_len);
    put_text(&key, cache->triple);
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

static void
phase_name_into(core_buffer_t *out, core_string_t kind)
{
    core_buffer_append_string(out, phase_name());
    core_buffer_putc(out, '.');
    core_buffer_append_string(out, kind);
}

static const cache_section_t *
phase_section(const cache_entry_t *entry, core_string_t kind,
              core_string_t module)
{
    core_buffer_t name = {0};
    phase_name_into(&name, kind);
    if (module.data) {
        core_buffer_putc(&name, '.');
        core_buffer_append_string(&name, module);
    }
    const cache_section_t *section = section_named(&entry->sections,
        core_buffer_string(&name));
    core_buffer_release(&name);
    return section;
}

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t at;
    bool failed;
} manifest_reader_t;

static uint64_t
read_u64(manifest_reader_t *r)
{
    if (r->failed || r->length - r->at < 8) {
        r->failed = true;
        return 0;
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; i++)
        value |= (uint64_t)r->data[r->at + i] << (8 * i);
    r->at += 8;
    return value;
}

static uint32_t
read_u32(manifest_reader_t *r)
{
    if (r->failed || r->length - r->at < 4) {
        r->failed = true;
        return 0;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; i++)
        value |= (uint32_t)r->data[r->at + i] << (8 * i);
    r->at += 4;
    return value;
}

static core_string_t
read_text(manifest_reader_t *r)
{
    uint64_t length = read_u64(r);
    if (r->failed || length > r->length - r->at) {
        r->failed = true;
        return (core_string_t){0};
    }
    core_string_t text = core_string_dup(core_string_borrow(
        (const char *)r->data + r->at, (size_t)length));
    r->at += (size_t)length;
    if (!text.data) r->failed = true;
    return text;
}

static void
read_links(manifest_reader_t *r, core_list_t(cache_link_t) *links)
{
    uint32_t count = read_u32(r);
    for (uint32_t i = 0; !r->failed && i < count; i++) {
        core_string_t name = read_text(r);
        uint64_t value = read_u64(r);
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
        for (size_t i = 0; i < member->imports.len; i++)
            core_free(member->imports.data[i].data);
        core_list_free(member->imports);
    }
    core_list_free(entry->members);
    for (size_t i = 0; i < entry->imports.len; i++)
        core_free(entry->imports.data[i].name.data);
    core_list_free(entry->imports);
    for (size_t i = 0; i < entry->import_summaries.len; i++)
        core_free(entry->import_summaries.data[i].name.data);
    core_list_free(entry->import_summaries);
    for (size_t i = 0; i < entry->inlined.len; i++)
        core_free(entry->inlined.data[i].name.data);
    core_list_free(entry->inlined);
    for (size_t i = 0; i < entry->references.len; i++)
        core_free(entry->references.data[i].data);
    core_list_free(entry->references);
    core_list_free(entry->assumed);
    sections_release(&entry->sections);
    if (entry->interface) th_env_free(entry->interface);
}

static bool
entry_parse(cache_entry_t *entry, const uint8_t *data, size_t length)
{
    entry->members = core_list_new(cache_member_t);
    entry->imports = core_list_new(cache_link_t);
    entry->import_summaries = core_list_new(cache_link_t);
    entry->inlined = core_list_new(cache_link_t);
    entry->references = core_list_new(core_string_t);
    entry->sections = core_list_new(cache_section_t);
    if (!sections_parse(data, length, &entry->sections)) return false;
    core_buffer_t manifest_name = {0};
    phase_name_into(&manifest_name, CORE_STRING_STATIC("manifest"));
    const cache_section_t *manifest = section_named(&entry->sections,
        core_buffer_string(&manifest_name));
    core_buffer_release(&manifest_name);
    if (!manifest) return false;
    manifest_reader_t r = {.data = manifest->data,
                           .length = manifest->length};
    uint64_t key = read_u64(&r);
    entry->freshness = read_u64(&r);
    entry->interface_identity = read_u64(&r);
    entry->summary_identity = read_u64(&r);
    uint32_t members = read_u32(&r);
    for (uint32_t m = 0; !r.failed && m < members; m++) {
        cache_member_t member = {
            .name = read_text(&r), .path = read_text(&r),
            .imports = core_list_new(core_string_t),
        };
        member.source_hash = read_u64(&r);
        uint32_t imports = read_u32(&r);
        for (uint32_t i = 0; !r.failed && i < imports; i++) {
            core_string_t name = read_text(&r);
            if (name.data) core_list_push(member.imports, name);
        }
        core_list_push(entry->members, member);
    }
    read_links(&r, &entry->imports);
    read_links(&r, &entry->import_summaries);
    read_links(&r, &entry->inlined);
    uint32_t references = read_u32(&r);
    for (uint32_t i = 0; !r.failed && i < references; i++) {
        core_string_t name = read_text(&r);
        if (name.data) core_list_push(entry->references, name);
    }
    return !r.failed && r.at == r.length && key == entry->key;
}

static uint64_t
module_key(const n00bb_cache_t *cache, core_string_t module)
{
    for (size_t i = 0; module.data && i < cache->module_keys.len; i++)
        if (core_string_eq(cache->module_keys.data[i].name, module))
            return cache->module_keys.data[i].value;
    return 0;
}

static cache_entry_t *
entry_for_module(n00bb_cache_t *cache, core_string_t module)
{
    uint64_t key = module_key(cache, module);
    for (size_t i = 0; key && i < cache->entries.len; i++)
        if (cache->entries.data[i].key == key) return &cache->entries.data[i];
    return nullptr;
}

static bool
member_still_located(const n00bb_cache_t *cache, const cache_member_t *member)
{
    if (!cache->locate) return true;
    core_string_t located = {0};
    if (!cache->locate(cache->locate_context, member->name, &located))
        return false;
    core_string_t canonical = canonical_path(located);
    bool same = core_string_eq(canonical, member->path);
    core_free(canonical.data);
    core_free(located.data);
    return same;
}

static bool
entry_fresh(const n00bb_cache_t *cache, const cache_entry_t *entry)
{
    bool ok = entry->members.len != 0;
    core_buffer_t freshness = {0};
    core_buffer_puts(&freshness, "n00b.scc.freshness.v1");
    for (size_t m = 0; ok && m < entry->members.len; m++) {
        const cache_member_t *member = &entry->members.data[m];
        uint8_t *source = nullptr;
        size_t source_bytes = 0;
        ok = module_key(cache, member->name) == entry->key
            && (!cache->root_path.data
                || !core_string_eq(member->path, cache->root_path))
            && member_still_located(cache, member)
            && read_file(member->path, &source, &source_bytes)
            && core_xxh3_64bits_raw(source ? (const void *)source : "",
                                    source_bytes) == member->source_hash;
        if (ok) {
            freshness_member(&freshness, member->name.data,
                             member->name.u8_bytes, source, source_bytes);
            for (size_t i = 0; i < member->imports.len; i++)
                put_text(&freshness, member->imports.data[i]);
        }
        core_free(source);
    }
    ok = ok && buffer_hash(&freshness) == entry->freshness;
    core_buffer_release(&freshness);
    return ok;
}

static bool
entry_excluded(const cache_entry_t *entry, core_string_t excluded)
{
    size_t at = 0;
    while (excluded.data && at < excluded.u8_bytes) {
        core_string_t rest = core_string_slice(excluded, at,
                                               excluded.u8_bytes - at);
        int64_t comma = core_string_find_byte(rest, ',');
        core_string_t item = core_string_slice(rest, 0,
            comma >= 0 ? (size_t)comma : rest.u8_bytes);
        for (size_t m = 0; m < entry->members.len; m++)
            if (core_string_eq(entry->members.data[m].name, item))
                return true;
        at = comma >= 0 ? at + (size_t)comma + 1 : excluded.u8_bytes;
    }
    return false;
}

static bool
entry_validate(n00bb_cache_t *cache, cache_entry_t *entry)
{
    if (entry->state == ENTRY_VALID) return true;
    if (entry->state != ENTRY_UNKNOWN) return false;
    entry->state = ENTRY_VISITING;
    bool ok = entry->fresh
        && phase_section(entry, CORE_STRING_STATIC("ir"), (core_string_t){0})
        && phase_section(entry, CORE_STRING_STATIC("interface"),
                         (core_string_t){0});
    for (size_t m = 0; ok && !objects_optional() && m < entry->members.len;
         m++)
        ok = phase_section(entry, CORE_STRING_STATIC("native"),
                           entry->members.data[m].name)
            && phase_section(entry, CORE_STRING_STATIC("abi"),
                             entry->members.data[m].name);
    uint64_t *imports = core_alloc_array(uint64_t,
                                         entry->imports.len + 1);
    uint64_t *inlined = core_alloc_array(uint64_t,
                                         entry->inlined.len + 1);
    ok = ok && imports && inlined;
    for (size_t i = 0; ok && i < entry->imports.len; i++) {
        cache_entry_t *imported = entry_for_module(cache,
            entry->imports.data[i].name);
        bool current = imported && entry_validate(cache, imported);
        ok = imported && (current || !cache->strict)
            && imported->interface_identity == entry->imports.data[i].value;
        if (ok && !current) core_list_push(entry->assumed, i);
        if (ok) imports[i] = imported->interface_identity;
    }
    for (size_t i = 0; ok && i < entry->inlined.len; i++) {
        cache_entry_t *callee = entry_for_module(cache,
            entry->inlined.data[i].name);
        ok = callee && callee->fresh
            && callee->freshness == entry->inlined.data[i].value;
        if (ok) inlined[i] = callee->freshness;
    }
    ok = ok && scc_key(cache, entry->freshness, imports, entry->imports.len,
                       inlined, entry->inlined.len) == entry->key;
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
        if (read_file(core_buffer_string(&file), &bytes, &byte_count)
            && byte_count >= 16) {
            core_string_t text = core_string_borrow((const char *)bytes, 16);
            int64_t terminator = core_string_find_byte(text, '\0');
            if (terminator >= 0) text.u8_bytes = (size_t)terminator;
            uint64_t key = core_string_strtoull(text, 16);
            core_string_t name = core_string_dup(core_string_slice(entry_name,
                0, entry_name.u8_bytes - suffix.byte_len));
            if (key && name.data)
                core_list_push(cache->module_keys,
                               ((cache_link_t){name, key}));
            else core_free(name.data);
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

static void
variant_path_in(core_string_t root, uint64_t key, uint64_t variant,
                core_buffer_t *path)
{
    core_buffer_printf(path, "%.*s/%016llx.%016llx.n00bo",
                       (int)root.u8_bytes, root.data,
                       (unsigned long long)key, (unsigned long long)variant);
}

static bool
in_phase(core_string_t name)
{
    core_string_t phase = phase_name();
    return name.u8_bytes > phase.u8_bytes
        && core_string_starts_with(name, phase)
        && name.data[phase.u8_bytes] == '.';
}

static void
drop_phase_sections(core_list_t(cache_section_t) *sections)
{
    core_list_t(cache_section_t) kept = core_list_new(cache_section_t);
    for (size_t i = 0; i < sections->len; i++) {
        if (!in_phase(sections->data[i].name)) {
            core_list_push(kept, sections->data[i]);
            continue;
        }
        core_free(sections->data[i].name.data);
        free(sections->data[i].data);
    }
    core_list_free(*sections);
    *sections = kept;
}

static bool
replace_phase_sections(core_list_t(cache_section_t) *into,
                       const core_list_t(cache_section_t) *from)
{
    drop_phase_sections(into);
    bool ok = true;
    for (size_t i = 0; ok && i < from->len; i++)
        if (in_phase(from->data[i].name))
            ok = sections_set(into, from->data[i].name, from->data[i].data,
                              from->data[i].length);
    return ok;
}

static bool
sections_equal(const cache_section_t *left, const cache_section_t *right)
{
    return left && right && left->length == right->length
        && (!left->length || !memcmp(left->data, right->data, left->length));
}

static const cache_section_t *
phase_section_in(const core_list_t(cache_section_t) *sections,
                 core_string_t kind)
{
    core_buffer_t name = {0};
    phase_name_into(&name, kind);
    const cache_section_t *section = section_named(sections,
        core_buffer_string(&name));
    core_buffer_release(&name);
    return section;
}

static bool
store_dispatch_variant(core_string_t root, uint64_t key,
                       const core_list_t(cache_section_t) *sections)
{
    const cache_section_t *dispatch = phase_section_in(sections,
        CORE_STRING_STATIC("dispatch"));
    if (!dispatch || dispatch->length <= 4) return true;
    core_buffer_t path = {0};
    variant_path_in(root, key, core_xxh3_64bits_raw(dispatch->data,
                                                    dispatch->length), &path);
    core_list_t(cache_section_t) variant = core_list_new(cache_section_t);
    uint8_t *existing = nullptr;
    size_t existing_bytes = 0;
    if (read_file(core_buffer_string(&path), &existing, &existing_bytes)
        && !sections_parse(existing, existing_bytes, &variant)) {
        sections_release(&variant);
        variant = core_list_new(cache_section_t);
    }
    core_free(existing);
    core_buffer_t serialized = {0};
    bool ok = replace_phase_sections(&variant, sections);
    if (ok) sections_serialize(&variant, &serialized);
    ok = ok && core_os_write_file_atomic(core_buffer_string(&path), serialized.data,
                                 serialized.byte_len);
    core_buffer_release(&serialized);
    sections_release(&variant);
    core_buffer_release(&path);
    return ok;
}

static bool
reference_holds(n00bb_cache_t *cache, const cache_entry_t *entry,
                core_string_t reference)
{
    for (size_t m = 0; m < entry->members.len; m++)
        if (core_string_eq(entry->members.data[m].name, reference))
            return true;
    if (!module_key(cache, reference)) {
        const cache_section_t *dispatch = phase_section_in(&entry->sections,
            CORE_STRING_STATIC("dispatch"));
        return dispatch && dispatch->length > 4;
    }
    cache_entry_t *referenced = entry_for_module(cache, reference);
    return referenced && referenced->state == ENTRY_VALID
        && (!referenced->deferred || entry->deferred);
}

static size_t
references_bytes(const cache_entry_t *entry)
{
    size_t bytes = 4;
    for (size_t i = 0; i < entry->references.len; i++)
        bytes += 8 + entry->references.data[i].u8_bytes;
    return bytes;
}

static bool
same_manifest(const cache_entry_t *left, const cache_entry_t *right)
{
    const cache_section_t *a = phase_section_in(&left->sections,
        CORE_STRING_STATIC("manifest"));
    const cache_section_t *b = phase_section_in(&right->sections,
        CORE_STRING_STATIC("manifest"));
    size_t a_bytes = a ? a->length - references_bytes(left) : 0;
    size_t b_bytes = b ? b->length - references_bytes(right) : 0;
    return a && b && a_bytes == b_bytes
        && !memcmp(a->data, b->data, a_bytes);
}

static bool
adopt_dispatch_variant(n00bb_cache_t *cache, cache_entry_t *entry,
                       const core_buffer_t *dispatch)
{
    uint64_t variant = buffer_hash(dispatch);
    const core_string_t roots[] = {cache->directory, cache->shared};
    const cache_section_t current = {
        .data = (uint8_t *)dispatch->data, .length = dispatch->byte_len,
    };
    bool adopted = false;
    for (size_t r = 0; !adopted && r < 2; r++) {
        if (r && core_string_eq(roots[r], roots[0])) break;
        core_buffer_t path = {0};
        variant_path_in(roots[r], entry->key, variant, &path);
        cache_entry_t candidate = {.key = entry->key,
                                   .deferred = entry->deferred};
        uint8_t *bytes = nullptr;
        size_t length = 0;
        bool ok = read_file(core_buffer_string(&path), &bytes, &length)
            && entry_parse(&candidate, bytes, length)
            && sections_equal(phase_section_in(&candidate.sections,
                   CORE_STRING_STATIC("dispatch")), &current)
            && sections_equal(phase_section_in(&candidate.sections,
                   CORE_STRING_STATIC("interface")),
                   phase_section_in(&entry->sections,
                   CORE_STRING_STATIC("interface")))
            && same_manifest(&candidate, entry);
        bool holds = true;
        for (size_t i = 0; ok && holds && i < candidate.references.len; i++)
            holds = reference_holds(cache, &candidate,
                                    candidate.references.data[i]);
        ok = ok && (holds || (!cache->strict && entry->deferred));
        adopted = ok && replace_phase_sections(&entry->sections,
                                               &candidate.sections);
        if (adopted) {
            core_list_t(core_string_t) references = entry->references;
            entry->references = candidate.references;
            candidate.references = references;
        }
        core_free(bytes);
        entry_release(&candidate);
        core_buffer_release(&path);
    }
    return adopted;
}

static bool
scc_in_stdlib(const n00bb_cache_t *cache, const cache_scc_t *scc)
{
    if (!cache->stdlib_root.u8_bytes) return false;
    bool inside = scc->members.len > 0;
    for (size_t m = 0; inside && m < scc->members.len; m++) {
        core_string_t path = canonical_path(core_source_file_path(
            &scc->members.data[m]->source));
        inside = path.u8_bytes > cache->stdlib_root.u8_bytes
            && core_string_starts_with(path, cache->stdlib_root)
            && path.data[cache->stdlib_root.u8_bytes] == '/';
        core_free(path.data);
    }
    return inside;
}

static core_string_t
scc_directory(const n00bb_cache_t *cache, const cache_scc_t *scc)
{
    return scc_in_stdlib(cache, scc) ? cache->shared : cache->directory;
}

static void
read_entries(n00bb_cache_t *cache)
{
    for (size_t i = 0; i < cache->module_keys.len; i++) {
        uint64_t key = cache->module_keys.data[i].value;
        bool seen = false;
        for (size_t e = 0; e < cache->entries.len; e++)
            seen |= cache->entries.data[e].key == key;
        if (seen) continue;
        core_buffer_t path = {0};
        object_path(cache, key, &path);
        uint8_t *bytes = nullptr;
        size_t length = 0;
        cache_entry_t entry = {.key = key};
        bool ok = read_file(core_buffer_string(&path), &bytes, &length)
            && entry_parse(&entry, bytes, length);
        core_free(bytes);
        core_buffer_release(&path);
        if (ok) core_list_push(cache->entries, entry);
        else entry_release(&entry);
    }
}

static void
member_names(const cache_entry_t *entry, core_buffer_t *out)
{
    for (size_t m = 0; m < entry->members.len; m++) {
        if (m) core_buffer_putc(out, ',');
        core_buffer_append_string(out, entry->members.data[m].name);
    }
}

n00bb_cache_t *
n00bb_cache_open(core_string_t root_path, parse_cmdr_result_t *args,
                 core_string_t triple,
                 ir_ctx_t *ir_context, n00bb_cache_locate_fn locate,
                 void *locate_context)
{
    if (!root_path.data || !root_path.u8_bytes || root_path.data[0] == '<'
        || !triple.data
        || n00bb_setting_bool(N00BB_KEY_DEV_NO_OBJECT_CACHE)
        || n00bb_setting_bool(N00BB_KEY_BUILD_LTO))
        return nullptr;
    n00bb_cache_t *cache = core_alloc(n00bb_cache_t);
    if (!cache) return nullptr;
    cache->root_path = canonical_path(root_path);
    int64_t slash = cache->root_path.data
        ? core_string_rfind_byte(cache->root_path, '/') : -1;
    cache->directory = slash >= 0
        ? n00bb_cache_mirror_directory(CORE_STRING_STATIC("obj"),
              core_string_slice(cache->root_path, 0,
                                slash ? (size_t)slash : 1))
        : (core_string_t){0};
    if (!cache->directory.data) {
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
    cache->summary_identities = core_list_new(cache_link_t);
    cache->snapshots = core_list_new(cache_snapshot_t);
    cache->sccs = core_list_new(cache_scc_t);
    cache->instances = core_list_new(cache_instance_t);
    cache->strict = n00bb_handoff()->cache_strict;
    read_module_keys(cache);
    read_entries(cache);
    core_string_t excluded = n00bb_handoff()->cache_exclude;
    for (size_t e = 0; e < cache->entries.len; e++)
        cache->entries.data[e].fresh = entry_fresh(cache,
                                                   &cache->entries.data[e])
            && !entry_excluded(&cache->entries.data[e], excluded);
    for (size_t e = 0; e < cache->entries.len; e++)
        (void)entry_validate(cache, &cache->entries.data[e]);
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
                if (cache->strict) entry->state = ENTRY_INVALID;
                else if (!entry->deferred) entry->deferred = true;
                else continue;
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
    return cache;
}

void
n00bb_cache_free(n00bb_cache_t *cache)
{
    if (!cache) return;
    for (size_t e = 0; e < cache->entries.len; e++)
        entry_release(&cache->entries.data[e]);
    core_list_free(cache->entries);
    for (size_t i = 0; i < cache->module_keys.len; i++)
        core_free(cache->module_keys.data[i].name.data);
    core_list_free(cache->module_keys);
    core_list_free(cache->interfaces);
    for (size_t i = 0; i < cache->snapshots.len; i++) {
        core_free(cache->snapshots.data[i].module.data);
        core_buffer_release(&cache->snapshots.data[i].ir);
        core_buffer_release(&cache->snapshots.data[i].references);
    }
    core_list_free(cache->snapshots);
    sccs_release(&cache->sccs);
    core_list_free(cache->summary_identities);
    for (size_t i = 0; i < cache->instances.len; i++)
        sections_release(&cache->instances.data[i].sections);
    core_list_free(cache->instances);
    core_buffer_release(&cache->flags);
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
    for (size_t e = 0; cache && module.data && e < cache->entries.len; e++) {
        const cache_entry_t *entry = &cache->entries.data[e];
        if (entry->load != ENTRY_VALID) continue;
        for (size_t m = 0; m < entry->members.len; m++) {
            const cache_member_t *member = &entry->members.data[m];
            if (core_string_eq(member->name, module)) {
                if (owner) *owner = entry;
                return member;
            }
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
                          core_string_t *path, uint64_t *source_hash)
{
    const cache_member_t *member = loaded_member(cache, module, nullptr);
    if (!member) return false;
    *path = member->path;
    *source_hash = member->source_hash;
    return true;
}

bool
n00bb_cache_declaration_sites(const n00bb_cache_t *cache,
                              core_string_t module, core_buffer_t *out)
{
    const cache_entry_t *entry = nullptr;
    const cache_member_t *member = loaded_member(cache, module, &entry);
    const cache_section_t *section = member
        ? phase_section(entry, CORE_STRING_STATIC("sites"), member->name)
        : nullptr;
    if (!section) return false;
    out->byte_len = 0;
    core_buffer_append(out, (const char *)section->data, section->length);
    return true;
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
    manifest_reader_t reader = {
        .data = section->data, .length = section->length,
    };
    uint32_t count = read_u32(&reader);
    for (uint32_t i = 0; !reader.failed && i < count; i++) {
        core_diagnostic_t item = {
            .severity = (core_diag_severity_t)read_u32(&reader),
            .code = read_text(&reader),
            .message = read_text(&reader),
            .module = unit->module.identifier,
        };
        item.path = read_text(&reader);
        item.location.line = read_u32(&reader);
        item.location.column = read_u32(&reader);
        item.location.end_line = read_u32(&reader);
        item.location.end_column = read_u32(&reader);
        item.location.has_location = read_u32(&reader) != 0;
        if (!reader.failed) core_diagnostics_append(into, &item);
        core_free(item.code.data);
        core_free(item.message.data);
        core_free(item.path.data);
    }
    return !reader.failed;
}

bool
n00bb_cache_optimistic(const n00bb_cache_t *cache)
{
    for (size_t e = 0; cache && e < cache->entries.len; e++) {
        const cache_entry_t *entry = &cache->entries.data[e];
        if (entry->load == ENTRY_VALID
            && (entry->deferred || entry->assumed.len)) return true;
    }
    return false;
}

bool
n00bb_cache_module_speculative(const n00bb_cache_t *cache,
                               core_string_t module)
{
    const cache_entry_t *entry = nullptr;
    if (!loaded_member(cache, module, &entry)) return false;
    if (!cache_check_only) return entry->deferred || entry->assumed.len;
    return entry->assumed.len && !cache->assumptions_confirmed;
}

void
n00bb_cache_log_restart(core_string_t reason)
{
    core_string_t phase = phase_name();
    cache_log("%.*s restart %.*s", (int)phase.u8_bytes, phase.data,
              (int)reason.u8_bytes, reason.data);
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
        if (!ir) {
            core_free(inputs);
            return false;
        }
        inputs[inputs_used++] = (ir_archive_input_t){ir->data, ir->length};
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
    cache->ir_load_failed = cache->ir_load_failed || !ok;
    return ok;
}

void
n00bb_cache_unloadable_modules(const n00bb_cache_t *cache, core_buffer_t *out)
{
    for (size_t e = 0; cache && cache->ir_load_failed
         && e < cache->entries.len; e++) {
        const cache_entry_t *entry = &cache->entries.data[e];
        const cache_section_t *dispatch = phase_section(entry,
            CORE_STRING_STATIC("dispatch"), (core_string_t){0});
        if (entry->load != ENTRY_VALID
            || (!entry->deferred && (!dispatch || dispatch->length <= 4)))
            continue;
        for (size_t m = 0; m < entry->members.len; m++) {
            if (out->byte_len) core_buffer_putc(out, ',');
            core_buffer_append_string(out, entry->members.data[m].name);
        }
    }
}

static core_translation_unit_t *
analysis_unit(n00bb_link_analysis_t *analysis, core_string_t module)
{
    for (size_t i = 0; module.data && i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        if (core_string_eq(module, unit->module.identifier)) return unit;
    }
    return nullptr;
}

bool
n00bb_cache_check_dispatch(n00bb_cache_t *cache,
                           n00bb_link_analysis_t *analysis,
                           const th_env *environment)
{
    if (!cache || !analysis || !environment) return true;
    for (size_t e = 0; e < cache->entries.len; e++) {
        cache_entry_t *entry = &cache->entries.data[e];
        if (entry->load != ENTRY_VALID) continue;
        const cache_section_t *section = phase_section(entry,
            CORE_STRING_STATIC("dispatch"), (core_string_t){0});
        if (!section) continue;
        manifest_reader_t r = {section->data, section->length, 0, false};
        uint32_t count = read_u32(&r);
        core_buffer_t current = {0};
        put_u32(&current, count);
        bool matches = true;
        bool known = true;
        for (uint32_t i = 0; !r.failed && known && i < count; i++) {
            core_string_t module = read_text(&r);
            core_string_t package = read_text(&r);
            core_string_t trait = read_text(&r);
            core_string_t method = read_text(&r);
            uint64_t recorded = read_u64(&r);
            core_translation_unit_t *unit = r.failed ? nullptr
                : analysis_unit(analysis, module);
            n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
            uint64_t now = recorded;
            if (state && state->types)
                known = th_ctx_set_environment_scope(state->types,
                        module, package)
                    && n00bb_dispatch_fingerprint(state->types, environment,
                        trait, method, &now);
            matches = matches && known && now == recorded;
            put_text(&current, module);
            put_text(&current, package);
            put_text(&current, trait);
            put_text(&current, method);
            put_u64(&current, now);
            core_free(module.data);
            core_free(package.data);
            core_free(trait.data);
            core_free(method.data);
        }
        bool stale = r.failed || !matches;
        if (stale && !r.failed && known && !entry->loaded)
            stale = !adopt_dispatch_variant(cache, entry, &current);
        entry->dispatch_stale = stale;
        core_buffer_release(&current);
    }
    return true;
}

void
n00bb_cache_stale_modules(const n00bb_cache_t *cache, core_buffer_t *out)
{
    for (size_t e = 0; cache && e < cache->entries.len; e++) {
        const cache_entry_t *entry = &cache->entries.data[e];
        if (!entry->dispatch_stale) continue;
        for (size_t m = 0; m < entry->members.len; m++) {
            if (out->byte_len) core_buffer_putc(out, ',');
            core_buffer_append_string(out, entry->members.data[m].name);
        }
    }
}

void
n00bb_cache_objectless_modules(const n00bb_cache_t *cache, core_buffer_t *out)
{
    for (size_t e = 0; cache && e < cache->entries.len; e++) {
        const cache_entry_t *entry = &cache->entries.data[e];
        if (entry->load != ENTRY_VALID) continue;
        for (size_t m = 0; m < entry->members.len; m++) {
            core_string_t name = entry->members.data[m].name;
            if (phase_section(entry, CORE_STRING_STATIC("native"), name)
                && phase_section(entry, CORE_STRING_STATIC("abi"), name))
                continue;
            if (out->byte_len) core_buffer_putc(out, ',');
            core_buffer_append_string(out, name);
        }
    }
}

bool
n00bb_cache_assumptions_hold(n00bb_cache_t *cache,
                             core_link_unit_t *link,
                             n00bb_link_analysis_t *analysis)
{
    if (!cache || !link || !analysis) return false;
    core_list_t(cache_scc_t) sccs = core_list_new(cache_scc_t);
    bool ok = collect_sccs(link, &sccs, false);
    for (size_t e = 0; ok && e < cache->entries.len; e++) {
        const cache_entry_t *entry = &cache->entries.data[e];
        if (entry->load != ENTRY_VALID) continue;
        for (size_t a = 0; ok && a < entry->assumed.len; a++) {
            const cache_link_t *import =
                &entry->imports.data[entry->assumed.data[a]];
            size_t found = SIZE_MAX;
            for (size_t s = 0; found == SIZE_MAX && s < sccs.len; s++)
                for (size_t m = 0; m < sccs.data[s].members.len; m++) {
                    core_string_t name =
                        sccs.data[s].members.data[m]->module.identifier;
                    if (core_string_eq(import->name, name)) found = s;
                }
            cache_scc_t *scc = found != SIZE_MAX ? &sccs.data[found]
                                                 : nullptr;
            ok = scc && !scc->cached && interface_bytes(analysis, &sccs, scc);
            if (!ok) break;
            core_buffer_t identity = {0};
            core_buffer_puts(&identity, "n00b.scc.interface.v1");
            put_bytes(&identity, scc->interface, scc->interface_bytes);
            ok = buffer_hash(&identity) == import->value;
            core_buffer_release(&identity);
        }
    }
    sccs_release(&sccs);
    cache->assumptions_confirmed = ok && cache_check_only;
    return ok;
}

static uint64_t
recorded_summary_identity(const n00bb_cache_t *cache, core_string_t module)
{
    for (size_t i = 0; cache && i < cache->summary_identities.len; i++)
        if (core_string_eq(cache->summary_identities.data[i].name, module))
            return cache->summary_identities.data[i].value;
    return 0;
}

static void
scc_function_roots(const cache_scc_t *scc,
                   core_list_t(n00bb_ir_func_ptr_t) *roots)
{
    for (size_t m = 0; m < scc->members.len; m++) {
        ir_module_t *module = tu_state(scc->members.data[m])->ir_module;
        for (size_t f = 0; module && f < ir_func_count(module); f++) {
            ir_func_t *function = ir_func_at(module, f);
            if (function->n_blocks) core_list_push(*roots, function);
        }
    }
}

bool
n00bb_cache_record_summaries(n00bb_cache_t *cache, core_link_unit_t *link,
                             n00bb_link_analysis_t *analysis)
{
    if (!cache || !link || !analysis) return true;
    core_list_t(cache_scc_t) sccs = core_list_new(cache_scc_t);
    core_list_t(n00bb_ir_func_ptr_t) roots =
        core_list_new(n00bb_ir_func_ptr_t);
    bool ok = collect_sccs(link, &sccs, false);
    for (size_t s = 0; ok && s < sccs.len; s++)
        if (!sccs.data[s].cached) scc_function_roots(&sccs.data[s], &roots);
    ir_summaries_t *summaries = ok ? ir_summaries_compute(roots.data,
        roots.len, &analysis->summary_hooks) : nullptr;
    ok = ok && summaries;
    cache->summary_identities.len = 0;
    for (size_t s = 0; ok && s < sccs.len; s++) {
        cache_scc_t *scc = &sccs.data[s];
        if (scc->cached) continue;
        core_list_t(n00bb_ir_func_ptr_t) functions =
            core_list_new(n00bb_ir_func_ptr_t);
        scc_function_roots(scc, &functions);
        core_buffer_t input = {0};
        core_buffer_puts(&input, "n00b.scc.summaries.v1");
        for (size_t f = 0; ok && f < functions.len; f++) {
            put_text(&input, ir_module_name(functions.data[f]->owner));
            put_text(&input, functions.data[f]->name);
            ok = ir_summary_encode(ir_summary_of(summaries,
                                                 functions.data[f]), &input);
        }
        core_list_push(cache->summary_identities, ((cache_link_t){
            .name = scc->members.data[0]->module.identifier,
            .value = buffer_hash(&input),
        }));
        core_buffer_release(&input);
        core_list_free(functions);
    }
    ir_summaries_free(summaries);
    core_list_free(roots);
    sccs_release(&sccs);
    return ok;
}

bool
n00bb_cache_summary_assumptions_hold(const n00bb_cache_t *cache)
{
    for (size_t e = 0; cache && e < cache->entries.len; e++) {
        const cache_entry_t *entry = &cache->entries.data[e];
        if (entry->load != ENTRY_VALID) continue;
        for (size_t a = 0; a < entry->assumed.len; a++) {
            core_string_t name = entry->imports.data[entry->assumed.data[a]]
                .name;
            bool found = false;
            for (size_t i = 0; !found && i < entry->import_summaries.len; i++)
                found = core_string_eq(entry->import_summaries.data[i].name,
                                       name)
                    && entry->import_summaries.data[i].value
                        == recorded_summary_identity(cache, name);
            if (!found) return false;
        }
    }
    return true;
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
    const cache_section_t *native = member
        ? phase_section(entry, CORE_STRING_STATIC("native"), member->name)
        : nullptr;
    const cache_section_t *abi = member
        ? phase_section(entry, CORE_STRING_STATIC("abi"), member->name)
        : nullptr;
    if (!native || !abi) return false;
    *object = (n00bb_cache_module_object_t){
        .module = member->name,
        .native = native->data, .native_bytes = native->length,
        .abi = abi->data, .abi_bytes = abi->length,
    };
    return true;
}

static const cache_instance_t *loaded_instance(const n00bb_cache_t *cache,
                                               const ir_module_t *module);

static bool
cache_has_marker(const n00bb_cache_t *cache, core_string_t marker)
{
    for (size_t e = 0; cache && e < cache->entries.len; e++) {
        const cache_entry_t *entry = &cache->entries.data[e];
        if (entry->load == ENTRY_VALID && entry->loaded
            && phase_section(entry, marker, (core_string_t){0}))
            return true;
    }
    return false;
}

bool
n00bb_cache_needs_build_time_capture(const n00bb_cache_t *cache)
{
    return cache_has_marker(cache, CORE_STRING_STATIC("build-time-capture"));
}

bool
n00bb_cache_needs_literal_capture(const n00bb_cache_t *cache)
{
    return cache_has_marker(cache, CORE_STRING_STATIC("literal-capture"));
}

static bool
read_request_text(const uint8_t *data, size_t size, size_t *at,
                  core_string_t *out)
{
    if (size - *at < 8) return false;
    uint64_t length = 0;
    for (int i = 0; i < 8; i++)
        length |= (uint64_t)data[*at + i] << (8 * i);
    *at += 8;
    if (length > size - *at) return false;
    *out = core_string_borrow((const char *)data + *at, (size_t)length);
    *at += (size_t)length;
    return true;
}

static bool
entry_seeds(const n00bb_cache_t *cache, const cache_entry_t *entry,
            th_ctx *types, ir_module_lookup_fn lookup, void *context,
            core_list_t(n00bb_specialization_seed_t) *seeds)
{
    const cache_section_t *section = phase_section(entry,
        CORE_STRING_STATIC("requests"), (core_string_t){0});
    if (!section) return true;
    const uint8_t *data = section->data;
    size_t size = section->length, consumed = 0;
    if (size < 8) return false;
    uint64_t blob = 0;
    for (int i = 0; i < 8; i++) blob |= (uint64_t)data[i] << (8 * i);
    if (blob > size - 8) return false;
    th_type_reader_t *reader = th_type_reader_open(types, data + 8,
                                                   (size_t)blob, &consumed);
    if (!reader) return false;
    size_t at = 8 + (size_t)blob;
    bool ok = size - at >= 4;
    uint32_t count = 0;
    for (int i = 0; ok && i < 4; i++) count |= (uint32_t)data[at + i] << (8 * i);
    at += 4;
    for (uint32_t r = 0; ok && r < count; r++) {
        core_string_t module = {0}, name = {0};
        ok = read_request_text(data, size, &at, &module)
            && read_request_text(data, size, &at, &name) && size - at >= 4;
        if (!ok) break;
        uint32_t index = 0;
        for (int i = 0; i < 4; i++) index |= (uint32_t)data[at + i] << (8 * i);
        at += 4;
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
    return ok && at == size;
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
    put_text(key, ir_module_name(item->generic->owner));
    put_text(key, item->generic->name);
    th_type_writer_t *writer = th_type_writer_new();
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
    th_type_writer_t *writer = th_type_writer_new();
    core_buffer_t entries = {0};
    for (uint32_t i = 0; writer && i < count; i++) {
        const n00bb_specialization_t *item = requests[i].item;
        put_text(&entries, ir_module_name(item->generic->owner));
        put_text(&entries, item->generic->name);
        put_u32(&entries, th_type_writer_add(writer, item->signature));
    }
    for (uint32_t i = 0; i < count; i++)
        core_buffer_release(&requests[i].key);
    core_free(requests);
    if (!writer) count = 0;
    core_buffer_t blob = {0};
    if (writer) th_type_writer_finish(writer, &blob);
    put_u64(out, blob.byte_len);
    if (blob.byte_len) core_buffer_append(out, blob.data, blob.byte_len);
    put_u32(out, count);
    if (entries.byte_len)
        core_buffer_append(out, entries.data, entries.byte_len);
    core_buffer_release(&blob);
    core_buffer_release(&entries);
    if (writer) th_type_writer_free(writer);
    core_list_free(closure);
}

bool
n00bb_cache_snapshot(n00bb_cache_t *cache, core_link_unit_t *link)
{
    if (!cache || !link) return false;
    core_list_t(cache_scc_t) sccs = core_list_new(cache_scc_t);
    bool ok = collect_sccs(link, &sccs, true);
    for (size_t s = 0; ok && s < sccs.len; s++) {
        cache_scc_t *scc = &sccs.data[s];
        if (scc->root || scc->cached) continue;
        ir_module_t **modules = core_alloc_array(ir_module_t *,
                                                 scc->members.len);
        for (size_t m = 0; m < scc->members.len; m++)
            modules[m] = tu_state(scc->members.data[m])->ir_module;
        cache_snapshot_t snapshot = {
            .module = core_string_dup(
                scc->members.data[0]->module.identifier),
        };
        bool saved = snapshot.module.data
            && ir_modules_save_references(modules, scc->members.len,
                                          &snapshot.ir, &snapshot.references);
        core_free(modules);
        if (saved) core_list_push(cache->snapshots, snapshot);
        else {
            core_free(snapshot.module.data);
            core_buffer_release(&snapshot.ir);
            core_buffer_release(&snapshot.references);
        }
    }
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
    put_u32(out, (uint32_t)links->len);
    for (size_t i = 0; i < links->len; i++) {
        const cache_scc_t *linked = &sccs->data[links->data[i]];
        core_string_t name = linked->members.data[0]->module.identifier;
        put_bytes(out, name.data, name.u8_bytes);
        put_u64(out, freshness ? linked->freshness
                               : linked->interface_identity);
    }
}

static void
put_summary_links(core_buffer_t *out, const core_list_t(cache_scc_t) *sccs,
                  const core_list_t(size_t) *links)
{
    put_u32(out, (uint32_t)links->len);
    for (size_t i = 0; i < links->len; i++) {
        const cache_scc_t *linked = &sccs->data[links->data[i]];
        core_string_t name = linked->members.data[0]->module.identifier;
        put_bytes(out, name.data, name.u8_bytes);
        put_u64(out, linked->summary_identity);
    }
}

static void
module_section_name(core_buffer_t *out, core_string_t kind,
                    core_string_t module)
{
    out->byte_len = 0;
    phase_name_into(out, kind);
    core_buffer_putc(out, '.');
    core_buffer_append_string(out, module);
}

static void
phase_section_name(core_buffer_t *out, core_string_t kind)
{
    out->byte_len = 0;
    phase_name_into(out, kind);
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

static core_translation_unit_t *
node_owner_unit(parse_tree_t *node)
{
    for (parse_tree_t *at = node; at; at = parse_tree_parent(at)) {
        parse_scope_t *scope = parse_tree_scope(at);
        if (scope && scope->owner_unit) return scope->owner_unit;
    }
    return nullptr;
}

static bool
used_by(const parse_sym_entry_t *symbol, core_translation_unit_t *unit)
{
    for (size_t u = 0; u < symbol->use_count; u++)
        if (node_owner_unit(symbol->use_nodes[u]) == unit) return true;
    return false;
}

static void
put_symbol_uses(core_buffer_t *out, const n00bb_link_analysis_t *analysis,
                core_translation_unit_t *member)
{
    core_buffer_t items = {0};
    core_buffer_t key = {0};
    uint32_t count = 0;
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (unit == member || !state || !state->annotations) continue;
        for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
             scope; scope = scope->all_next)
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if ((symbol->kind != PARSE_SYM_FUNCTION && !symbol->is_field)
                    || !used_by(symbol, member))
                    continue;
                n00bb_symbol_use_key(&key, unit, scope, symbol);
                put_text(&items, core_buffer_string(&key));
                count++;
            }
    }
    put_u32(out, count);
    if (items.byte_len) core_buffer_append(out, items.data, items.byte_len);
    core_buffer_release(&items);
    core_buffer_release(&key);
}

void
n00bb_symbol_uses_init(n00bb_symbol_uses_t *uses)
{
    core_dict_init_string(&uses->keys);
    uses->owned = core_list_new(n00bb_use_key_t);
}

void
n00bb_symbol_uses_release(n00bb_symbol_uses_t *uses)
{
    for (size_t i = 0; i < uses->owned.len; i++)
        core_dict_string_key_free(uses->owned.data[i]);
    core_list_free(uses->owned);
    core_dict_free(&uses->keys);
}

bool
n00bb_symbol_uses_contains(n00bb_symbol_uses_t *uses, core_string_t key)
{
    return core_dict_contains(&uses->keys, &key);
}

bool
n00bb_cache_symbol_uses(const n00bb_cache_t *cache, core_string_t module,
                        n00bb_symbol_uses_t *into)
{
    const cache_entry_t *entry = nullptr;
    const cache_member_t *member = loaded_member(cache, module, &entry);
    const cache_section_t *section = member
        ? phase_section(entry, CORE_STRING_STATIC("uses"), member->name)
        : nullptr;
    if (!section) return false;
    manifest_reader_t reader = {
        .data = section->data, .length = section->length,
    };
    uint32_t count = read_u32(&reader);
    for (uint32_t i = 0; !reader.failed && i < count; i++) {
        core_string_t key = read_text(&reader);
        if (reader.failed) break;
        if (!n00bb_symbol_uses_contains(into, key)) {
            n00bb_use_key_t owned = core_dict_string_key_copy(key);
            core_list_push(into->owned, owned);
            core_dict_put(&into->keys, owned, (void *)1);
        }
        core_free(key.data);
    }
    return !reader.failed;
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
        attached = core_diag_is_advisory(item->severity) || (note && attached);
        if (!attached) continue;
        if (!note)
            groups[group_count++] = (warning_group_t){
                .items = &items[item_count]};
        items[item_count++] = item;
        groups[group_count - 1].count++;
    }
    qsort(groups, group_count, sizeof(*groups), warning_group_order);
    put_u32(out, (uint32_t)item_count);
    for (size_t g = 0; g < group_count; g++)
        for (size_t i = 0; i < groups[g].count; i++) {
            const core_diagnostic_t *item = groups[g].items[i];
            put_u32(out, (uint32_t)item->severity);
            put_text(out, item->code);
            put_text(out, item->message);
            put_text(out, item->path);
            put_u32(out, item->location.line);
            put_u32(out, item->location.column);
            put_u32(out, item->location.end_line);
            put_u32(out, item->location.end_column);
            put_u32(out, item->location.has_location);
        }
    core_free(groups);
    core_free(items);
}

static bool
store_scc(const n00bb_cache_t *cache, const n00bb_link_analysis_t *analysis,
          const core_list_t(cache_scc_t) *sccs,
          const cache_scc_t *scc, const cache_snapshot_t *snapshot,
          const n00bb_cache_module_object_t *objects, size_t object_count)
{
    core_string_t phase = phase_name();
    core_buffer_t manifest = {0};
    put_u64(&manifest, scc->key);
    put_u64(&manifest, scc->freshness);
    put_u64(&manifest, scc->interface_identity);
    put_u64(&manifest, scc->summary_identity);
    put_u32(&manifest, (uint32_t)scc->members.len);
    for (size_t m = 0; m < scc->members.len; m++) {
        core_translation_unit_t *unit = scc->members.data[m];
        core_string_t path = core_source_file_path(&unit->source);
        core_string_t canonical = canonical_path(path);
        put_bytes(&manifest, unit->module.identifier.data,
                  unit->module.identifier.u8_bytes);
        put_text(&manifest, canonical);
        core_free(canonical.data);
        put_u64(&manifest, core_xxh3_64bits_raw(
            core_source_file_contents(&unit->source).data,
            core_source_file_size(&unit->source)));
        put_u32(&manifest,
                (uint32_t)core_translation_unit_import_count(unit));
        for (size_t i = 0; i < core_translation_unit_import_count(unit);
             i++) {
            core_string_t name =
                core_translation_unit_import(unit, i)->module.identifier;
            put_bytes(&manifest, name.data, name.u8_bytes);
        }
    }
    put_links(&manifest, sccs, &scc->imports, false);
    put_summary_links(&manifest, sccs, &scc->imports);
    put_links(&manifest, sccs, &scc->inlined, true);
    const core_buffer_t *ir = &snapshot->ir;
    core_buffer_t references = {0};
    uint32_t reference_count = 0;
    for (const char *at = snapshot->references.data,
             *end = at ? at + snapshot->references.byte_len : nullptr;
         at && at < end;) {
        const char *line = memchr(at, '\n', (size_t)(end - at));
        size_t span = line ? (size_t)(line - at) : (size_t)(end - at);
        put_bytes(&references, at, span);
        reference_count++;
        at = line ? line + 1 : end;
    }
    put_u32(&manifest, reference_count);
    if (references.byte_len)
        core_buffer_append(&manifest, references.data, references.byte_len);
    core_buffer_release(&references);
    core_buffer_t dispatch = {0};
    uint32_t dispatch_count = 0;
    for (size_t m = 0; m < scc->members.len; m++)
        dispatch_count += (uint32_t)tu_state(scc->members.data[m])
            ->dispatch_records.len;
    put_u32(&dispatch, dispatch_count);
    for (size_t m = 0; m < scc->members.len; m++) {
        core_translation_unit_t *member = scc->members.data[m];
        n00bb_tu_state_t *member_state = tu_state(member);
        for (size_t d = 0; d < member_state->dispatch_records.len; d++) {
            const n00bb_dispatch_record_t *record =
                &member_state->dispatch_records.data[d];
            core_string_t package = n00bb_package_id(member);
            put_bytes(&dispatch, member->module.identifier.data,
                      member->module.identifier.u8_bytes);
            put_bytes(&dispatch, package.data, package.u8_bytes);
            put_text(&dispatch, record->trait);
            put_text(&dispatch, record->method);
            put_u64(&dispatch, record->fingerprint);
        }
    }
    core_string_t root = scc_directory(cache, scc);
    core_buffer_t path = {0};
    object_path_in(root, scc->key, &path);
    core_list_t(cache_section_t) sections = core_list_new(cache_section_t);
    uint8_t *existing = nullptr;
    size_t existing_bytes = 0;
    if (read_file(core_buffer_string(&path), &existing, &existing_bytes)
        && !sections_parse(existing, existing_bytes, &sections)) {
        sections_release(&sections);
        sections = core_list_new(cache_section_t);
    }
    core_free(existing);
    const cache_section_t new_ir = {
        .data = (uint8_t *)ir->data, .length = ir->byte_len,
    };
    const cache_section_t new_dispatch = {
        .data = (uint8_t *)dispatch.data, .length = dispatch.byte_len,
    };
    if (!sections_equal(phase_section_in(&sections, CORE_STRING_STATIC("ir")),
                        &new_ir)
        || !sections_equal(phase_section_in(&sections,
                               CORE_STRING_STATIC("dispatch")),
                           &new_dispatch))
        drop_phase_sections(&sections);
    core_buffer_t name = {0};
    phase_section_name(&name, CORE_STRING_STATIC("manifest"));
    bool ok = sections_set(&sections, core_buffer_string(&name),
                           manifest.data, manifest.byte_len);
    phase_section_name(&name, CORE_STRING_STATIC("ir"));
    ok = ok && sections_set(&sections, core_buffer_string(&name), ir->data,
                            ir->byte_len);
    phase_section_name(&name, CORE_STRING_STATIC("interface"));
    ok = ok && sections_set(&sections, core_buffer_string(&name),
                            scc->interface, scc->interface_bytes);
    if (objects) {
        core_buffer_t requests = {0};
        scc_requests(analysis, scc, &requests);
        phase_section_name(&name, CORE_STRING_STATIC("requests"));
        ok = ok && sections_set(&sections, core_buffer_string(&name),
                                requests.data, requests.byte_len);
        core_buffer_release(&requests);
    }
    phase_section_name(&name, CORE_STRING_STATIC("dispatch"));
    ok = ok && sections_set(&sections, core_buffer_string(&name),
                            dispatch.data, dispatch.byte_len);
    core_buffer_release(&dispatch);
    bool needs_capture = false;
    bool needs_literals = false;
    for (size_t m = 0; m < scc->members.len; m++) {
        n00bb_tu_state_t *member = tu_state(scc->members.data[m]);
        needs_capture = needs_capture || member->needs_build_time_capture;
        needs_literals = needs_literals || member->needs_literal_capture;
    }
    if (needs_capture) {
        phase_section_name(&name, CORE_STRING_STATIC("build-time-capture"));
        ok = ok && sections_set(&sections, core_buffer_string(&name),
                                (const uint8_t *)"", 0);
    }
    if (needs_literals) {
        phase_section_name(&name, CORE_STRING_STATIC("literal-capture"));
        ok = ok && sections_set(&sections, core_buffer_string(&name),
                                (const uint8_t *)"", 0);
    }
    for (size_t m = 0; ok && m < scc->members.len; m++) {
        core_string_t module = scc->members.data[m]->module.identifier;
        const n00bb_cache_module_object_t *object = module_object(objects,
            object_count, module);
        bool present = object && object->native && object->abi;
        ok = present || objects_optional();
        if (!ok) break;
        if (present) {
            module_section_name(&name, CORE_STRING_STATIC("native"), module);
            ok = sections_set(&sections, core_buffer_string(&name),
                              object->native, object->native_bytes);
            module_section_name(&name, CORE_STRING_STATIC("abi"), module);
            ok = ok && sections_set(&sections, core_buffer_string(&name),
                                    object->abi, object->abi_bytes);
        }
        core_buffer_t warnings = {0};
        core_translation_unit_t *root = analysis->units.len
            ? analysis->units.data[0] : nullptr;
        if (root && n00bb_resolver_same_root(analysis->resolver, root,
                                             scc->members.data[m]))
            put_warnings(&warnings, scc->members.data[m]);
        else put_u32(&warnings, 0);
        module_section_name(&name, CORE_STRING_STATIC("warnings"), module);
        ok = ok && sections_set(&sections, core_buffer_string(&name),
                                warnings.data ? (const uint8_t *)warnings.data
                                              : (const uint8_t *)"",
                                warnings.byte_len);
        core_buffer_release(&warnings);
        core_buffer_t uses = {0};
        put_symbol_uses(&uses, analysis, scc->members.data[m]);
        module_section_name(&name, CORE_STRING_STATIC("uses"), module);
        ok = ok && sections_set(&sections, core_buffer_string(&name),
                                uses.data ? (const uint8_t *)uses.data
                                          : (const uint8_t *)"",
                                uses.byte_len);
        core_buffer_release(&uses);
        const core_buffer_t *sites =
            &tu_state(scc->members.data[m])->declaration_sites;
        module_section_name(&name, CORE_STRING_STATIC("sites"), module);
        ok = ok && sections_set(&sections, core_buffer_string(&name),
                                sites->data ? (const uint8_t *)sites->data
                                            : (const uint8_t *)"",
                                sites->byte_len);
    }
    core_buffer_release(&name);
    core_buffer_t serialized = {0};
    if (ok) sections_serialize(&sections, &serialized);
    ok = ok && core_os_write_file_atomic(core_buffer_string(&path), serialized.data,
                                 serialized.byte_len)
        && store_dispatch_variant(root, scc->key, &sections);
    for (size_t m = 0; ok && m < scc->members.len; m++) {
        core_string_t module = scc->members.data[m]->module.identifier;
        core_buffer_t index = {0};
        core_buffer_t key = {0};
        core_buffer_printf(&index, "%.*s/modules/%.*s.%.*s.key",
                           (int)root.u8_bytes, root.data,
                           (int)module.u8_bytes, module.data,
                           (int)phase.u8_bytes, phase.data);
        core_buffer_printf(&key, "%016llx\n", (unsigned long long)scc->key);
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
            sccs->data[s].interface_identity = entry->interface_identity;
            sccs->data[s].summary_identity = entry->summary_identity;
            sccs->data[s].key = entry->key;
            continue;
        }
        sccs->data[s].summary_identity = recorded_summary_identity(cache,
                                                                   first);
        if (!interface_bytes(analysis, sccs, &sccs->data[s])) {
            ok = false;
            break;
        }
        core_buffer_t identity = {0};
        core_buffer_puts(&identity, "n00b.scc.interface.v1");
        put_bytes(&identity, sccs->data[s].interface,
                  sccs->data[s].interface_bytes);
        sccs->data[s].interface_identity = buffer_hash(&identity);
        core_buffer_release(&identity);
    }
    for (size_t s = 0; ok && s < sccs->len; s++) {
        cache_scc_t *scc = &sccs->data[s];
        if (scc->cached) continue;
        uint64_t *imports = core_alloc_array(uint64_t, scc->imports.len + 1);
        uint64_t *inlined = core_alloc_array(uint64_t, scc->inlined.len + 1);
        for (size_t i = 0; i < scc->imports.len; i++)
            imports[i] = sccs->data[scc->imports.data[i]].interface_identity;
        for (size_t i = 0; i < scc->inlined.len; i++)
            inlined[i] = sccs->data[scc->inlined.data[i]].freshness;
        scc->key = scc_key(cache, scc->freshness, imports, scc->imports.len,
                           inlined, scc->inlined.len);
        core_free(imports);
        core_free(inlined);
    }
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
instance_key(n00bb_cache_t *cache, const n00bb_specialization_t *item,
             uint64_t *key)
{
    const core_list_t(cache_scc_t) *sccs = &cache->sccs;
    size_t generic = scc_of_module_name(sccs,
        ir_module_name(item->generic->owner));
    if (generic == SIZE_MAX) return false;
    core_list_t(size_t) owners = core_list_new(size_t);
    collect_type_owners(sccs, item->signature, &owners, 0);
    if (owners.len)
        qsort(owners.data, owners.len, sizeof(*owners.data), compare_indices);
    uint64_t generic_ir = 0;
    bool saved = scc_ir_hash(cache, &sccs->data[generic], &generic_ir);
    core_buffer_t input = {0};
    core_buffer_puts(&input, "n00b.instance.key.v2");
    put_u64(&input, sccs->data[generic].key);
    put_u64(&input, generic_ir);
    put_text(&input, ir_module_name(item->instance->owner));
    put_text(&input, item->instance->name);
    put_u32(&input, (uint32_t)owners.len);
    for (size_t i = 0; i < owners.len; i++)
        put_u64(&input, sccs->data[owners.data[i]].interface_identity);
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
            && sections_parse(bytes, length, &loaded.sections);
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
    core_buffer_t name = {0};
    phase_section_name(&name, CORE_STRING_STATIC("native"));
    const cache_section_t *native = section_named(&found->sections,
        core_buffer_string(&name));
    core_buffer_release(&name);
    if (!native) return false;
    *object = (n00bb_cache_module_object_t){
        .module = ir_module_name(module),
        .native = native->data, .native_bytes = native->length,
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
    uint8_t *existing = nullptr;
    size_t existing_bytes = 0;
    if (read_file(core_buffer_string(&path), &existing, &existing_bytes)
        && !sections_parse(existing, existing_bytes, &sections)) {
        sections_release(&sections);
        sections = core_list_new(cache_section_t);
    }
    core_free(existing);
    core_buffer_t name = {0};
    phase_section_name(&name, CORE_STRING_STATIC("native"));
    bool ok = sections_set(&sections, core_buffer_string(&name),
                           object->native, object->native_bytes);
    core_buffer_release(&name);
    core_buffer_t serialized = {0};
    if (ok) sections_serialize(&sections, &serialized);
    ok = ok && core_os_write_file_atomic(core_buffer_string(&path), serialized.data,
                                 serialized.byte_len);
    core_buffer_release(&serialized);
    sections_release(&sections);
    core_buffer_release(&path);
    return ok;
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
        const cache_snapshot_t *snapshot = scc->root || scc->cached
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
        if (object && object->native)
            ok = store_instance(cache, item, object);
        if (object && ok)
            log_instance(CORE_STRING_STATIC("store"), item);
    }
    return ok;
}
