#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "compiler_cache.h"
#include "compiler_phases.h"
#include "compiler_tu.h"
#include "driver.h"
#include "builtin_types.h"
#include "ir/archive.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/hash.h"
#include "th/environment.h"

#define CACHE_MAGIC   UINT32_C(0x4f42304e)
#define CACHE_VERSION UINT32_C(1)

core_list_decl(size_t);

typedef struct {
    char *name;
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
put_text(core_buffer_t *out, const char *text)
{
    put_bytes(out, text, text ? strlen(text) : 0);
}

static uint64_t
buffer_hash(const core_buffer_t *buffer)
{
    return core_xxh3_64bits_raw(buffer->data ? buffer->data : "",
                                buffer->byte_len);
}

static bool
make_directories(const char *path)
{
    char *copy = core_strdup(path);
    if (!copy) return false;
    bool ok = true;
    for (char *at = copy + 1; ok && *at; at++) {
        if (*at != '/') continue;
        *at = '\0';
        ok = !mkdir(copy, 0700) || errno == EEXIST;
        *at = '/';
    }
    ok = ok && (!mkdir(copy, 0700) || errno == EEXIST);
    core_free(copy);
    return ok;
}

static bool
read_file(const char *path, uint8_t **data, size_t *length)
{
    *data = nullptr;
    *length = 0;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = !fseek(file, 0, SEEK_END);
    long size = ok ? ftell(file) : -1;
    ok = ok && size >= 0 && !fseek(file, 0, SEEK_SET);
    uint8_t *bytes = ok ? malloc(size ? (size_t)size : 1) : nullptr;
    ok = ok && bytes && fread(bytes, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    if (!ok) {
        free(bytes);
        return false;
    }
    *data = bytes;
    *length = (size_t)size;
    return true;
}

static bool
write_file_atomic(const char *path, const void *data, size_t length)
{
    core_buffer_t temporary = {0};
    core_buffer_printf(&temporary, "%s.tmp.%ld", path, (long)getpid());
    FILE *file = temporary.data ? fopen(temporary.data, "wb") : nullptr;
    bool ok = file && (!length || fwrite(data, 1, length, file) == length);
    if (file && fclose(file)) ok = false;
    if (ok) ok = !rename(temporary.data, path);
    else if (temporary.data) unlink(temporary.data);
    core_buffer_release(&temporary);
    return ok;
}

static void
sections_release(core_list_t(cache_section_t) *sections)
{
    for (size_t i = 0; i < sections->len; i++) {
        core_free(sections->data[i].name);
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
            .name = core_strndup((const char *)starts[0], fields[0]),
            .data = malloc(fields[1] ? fields[1] : 1),
            .length = fields[1],
        };
        if (!section.name || !section.data) return false;
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
sections_set(core_list_t(cache_section_t) *sections, const char *name,
             const void *data, size_t length)
{
    uint8_t *copy = malloc(length ? length : 1);
    if (!copy) return false;
    if (length) memcpy(copy, data, length);
    for (size_t i = 0; i < sections->len; i++) {
        if (strcmp(sections->data[i].name, name)) continue;
        free(sections->data[i].data);
        sections->data[i].data = copy;
        sections->data[i].length = length;
        return true;
    }
    char *owned = core_strdup(name);
    if (!owned) {
        free(copy);
        return false;
    }
    core_list_push(*sections, ((cache_section_t){owned, copy, length}));
    return true;
}

static const char *
phase_name(void)
{
    return n00bb_compile_phase() == PARSE_SYM_PHASE_COMPTIME
        ? "comptime" : "runtime";
}

static int
compare_units(const void *left, const void *right)
{
    const core_translation_unit_t *a = *(core_translation_unit_t *const *)left;
    const core_translation_unit_t *b = *(core_translation_unit_t *const *)right;
    size_t length = a->module.identifier.u8_bytes
        < b->module.identifier.u8_bytes
        ? a->module.identifier.u8_bytes : b->module.identifier.u8_bytes;
    int order = memcmp(a->module.identifier.data, b->module.identifier.data,
                       length);
    if (order) return order;
    return (a->module.identifier.u8_bytes > b->module.identifier.u8_bytes)
        - (a->module.identifier.u8_bytes < b->module.identifier.u8_bytes);
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
scc_of_module_name(const core_list_t(cache_scc_t) *sccs, const char *name)
{
    size_t length = name ? strlen(name) : 0;
    for (size_t s = 0; s < sccs->len; s++)
        for (size_t m = 0; m < sccs->data[s].members.len; m++) {
            core_string_t identifier =
                sccs->data[s].members.data[m]->module.identifier;
            if (identifier.u8_bytes == length
                && !memcmp(identifier.data, name, length)) return s;
        }
    return SIZE_MAX;
}

static bool
module_member(const cache_scc_t *scc, const char *module, size_t length)
{
    for (size_t m = 0; module && m < scc->members.len; m++) {
        core_string_t identifier = scc->members.data[m]->module.identifier;
        if (identifier.u8_bytes == length
            && !memcmp(identifier.data, module, length)) return true;
    }
    return false;
}

typedef struct {
    const cache_scc_t *scc;
    const core_list_t(cache_scc_t) *sccs;
} interface_filter_t;

static size_t
owner_module_length(const core_list_t(cache_scc_t) *sccs, const char *owner)
{
    size_t best = 0;
    size_t length = owner ? strlen(owner) : 0;
    for (size_t s = 0; s < sccs->len; s++)
        for (size_t m = 0; m < sccs->data[s].members.len; m++) {
            core_string_t identifier =
                sccs->data[s].members.data[m]->module.identifier;
            if (identifier.u8_bytes < length && identifier.u8_bytes > best
                && owner[identifier.u8_bytes] == '.'
                && !memcmp(identifier.data, owner, identifier.u8_bytes))
                best = identifier.u8_bytes;
        }
    return best;
}

static bool
scc_declares_package(const cache_scc_t *scc, const char *name)
{
    size_t length = strlen(name);
    for (size_t m = 0; m < scc->members.len; m++) {
        n00bb_tu_state_t *state = tu_state(scc->members.data[m]);
        for (size_t p = 0; state && p < state->package_declarations.len; p++) {
            core_string_t package = state->package_declarations.data[p].name;
            if (package.u8_bytes == length
                && !memcmp(package.data, name, length))
                return true;
        }
    }
    return false;
}

static bool
filter_module(void *context, const char *module)
{
    interface_filter_t *filter = context;
    return module && module_member(filter->scc, module, strlen(module));
}

static bool
filter_metadata(void *context, th_env_metadata_owner_t kind,
                const char *owner)
{
    interface_filter_t *filter = context;
    if (!owner || kind != TH_ENV_METADATA_ENVIRONMENT) return false;
    return module_member(filter->scc, owner, strlen(owner))
        || scc_declares_package(filter->scc, owner);
}

static bool
filter_intrinsic(void *context, const char *trait, const th_type *receiver)
{
    interface_filter_t *filter = context;
    (void)trait;
    const char *name = nullptr;
    size_t length = 0;
    if (!receiver || !th_named_info((th_type *)receiver, &name, &length,
                                    nullptr, nullptr) || !name) return false;
    char *owned = core_strndup(name, length);
    size_t module = owned ? owner_module_length(filter->sccs, owned) : 0;
    bool keep = module && module_member(filter->scc, owned, module);
    core_free(owned);
    return keep;
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

static uint64_t
compiler_identity(const char *compiler_path)
{
    uint8_t *bytes = nullptr;
    size_t length = 0;
    uint64_t identity = 0;
    if (compiler_path && read_file(compiler_path, &bytes, &length))
        identity = core_xxh3_64bits_raw(bytes, length);
    free(bytes);
    return identity;
}

static void
append_flags(core_buffer_t *out, parse_cmdr_result_t *args)
{
    static const char *const flags[] = {
        "--release", "--native", "--generic", "--lto",
        "--gc-conservative-stacks",
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(*flags); i++)
        put_u32(out, parse_cmdr_flag_present(args,
            core_string_from_cstr(flags[i])));
    int32_t defines = parse_cmdr_flag_value_count(args,
        CORE_STRING_STATIC("--define"));
    put_u32(out, (uint32_t)(defines > 0 ? defines : 0));
    for (int32_t i = 0; i < defines; i++) {
        core_string_t value = parse_cmdr_flag_str_at(args,
            CORE_STRING_STATIC("--define"), i);
        put_bytes(out, value.data, value.u8_bytes);
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
                         core_source_file_contents(&unit->source),
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
collect_sccs(core_link_unit_t *link, core_list_t(cache_scc_t) *sccs)
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
            if (!state || !state->ir_module) {
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
        inline_dependencies(sccs, scc, &scc->inlined);
    }
    return true;
}

typedef char *cache_text_t;
core_list_decl(cache_text_t);

typedef const th_env *cache_environment_t;
core_list_decl(cache_environment_t);

typedef struct {
    char *name;
    char *path;
    uint64_t source_hash;
    core_list_t(cache_text_t) imports;
    ir_module_t *module;
} cache_member_t;
core_list_decl(cache_member_t);

typedef struct {
    char *name;
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
    core_list_t(cache_member_t) members;
    core_list_t(cache_link_t) imports;
    core_list_t(cache_link_t) inlined;
    core_list_t(cache_text_t) references;
    core_list_t(cache_section_t) sections;
    bool fresh;
    entry_state_t state;
    entry_state_t load;
    th_env *interface;
} cache_entry_t;
core_list_decl(cache_entry_t);

typedef struct {
    char *module;
    core_buffer_t ir;
    core_buffer_t references;
} cache_snapshot_t;
core_list_decl(cache_snapshot_t);

struct n00bb_cache_t {
    char *directory;
    char *root_path;
    char *triple;
    uint64_t compiler;
    core_buffer_t flags;
    ir_ctx_t *ir_context;
    core_list_t(cache_link_t) module_keys;
    core_list_t(cache_entry_t) entries;
    core_list_t(cache_environment_t) interfaces;
    core_list_t(cache_snapshot_t) snapshots;
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

static char *
canonical_path(const char *path, size_t length)
{
    char *owned = core_strndup(path, length);
    char resolved[PATH_MAX];
    char *result = owned && realpath(owned, resolved)
        ? core_strdup(resolved) : nullptr;
    if (!result) result = owned;
    else core_free(owned);
    return result;
}

static const cache_section_t *
section_named(const core_list_t(cache_section_t) *sections, const char *name)
{
    for (size_t i = 0; i < sections->len; i++)
        if (!strcmp(sections->data[i].name, name)) return &sections->data[i];
    return nullptr;
}

static const cache_section_t *
phase_section(const cache_entry_t *entry, const char *kind,
              const char *module)
{
    core_buffer_t name = {0};
    if (module) core_buffer_printf(&name, "%s.%s.%s", phase_name(), kind,
                                   module);
    else core_buffer_printf(&name, "%s.%s", phase_name(), kind);
    const cache_section_t *section = section_named(&entry->sections,
                                                   name.data);
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

static char *
read_text(manifest_reader_t *r)
{
    uint64_t length = read_u64(r);
    if (r->failed || length > r->length - r->at) {
        r->failed = true;
        return nullptr;
    }
    char *text = core_strndup((const char *)r->data + r->at, (size_t)length);
    r->at += (size_t)length;
    if (!text) r->failed = true;
    return text;
}

static void
read_links(manifest_reader_t *r, core_list_t(cache_link_t) *links)
{
    uint32_t count = read_u32(r);
    for (uint32_t i = 0; !r->failed && i < count; i++) {
        char *name = read_text(r);
        uint64_t value = read_u64(r);
        if (!r->failed) core_list_push(*links, ((cache_link_t){name, value}));
        else core_free(name);
    }
}

static void
entry_release(cache_entry_t *entry)
{
    for (size_t m = 0; m < entry->members.len; m++) {
        cache_member_t *member = &entry->members.data[m];
        core_free(member->name);
        core_free(member->path);
        for (size_t i = 0; i < member->imports.len; i++)
            core_free(member->imports.data[i]);
        core_list_free(member->imports);
    }
    core_list_free(entry->members);
    for (size_t i = 0; i < entry->imports.len; i++)
        core_free(entry->imports.data[i].name);
    core_list_free(entry->imports);
    for (size_t i = 0; i < entry->inlined.len; i++)
        core_free(entry->inlined.data[i].name);
    core_list_free(entry->inlined);
    for (size_t i = 0; i < entry->references.len; i++)
        core_free(entry->references.data[i]);
    core_list_free(entry->references);
    sections_release(&entry->sections);
    if (entry->interface) th_env_free(entry->interface);
}

static bool
entry_parse(cache_entry_t *entry, const uint8_t *data, size_t length)
{
    entry->members = core_list_new(cache_member_t);
    entry->imports = core_list_new(cache_link_t);
    entry->inlined = core_list_new(cache_link_t);
    entry->references = core_list_new(cache_text_t);
    entry->sections = core_list_new(cache_section_t);
    if (!sections_parse(data, length, &entry->sections)) return false;
    core_buffer_t manifest_name = {0};
    core_buffer_printf(&manifest_name, "%s.manifest", phase_name());
    const cache_section_t *manifest = section_named(&entry->sections,
                                                    manifest_name.data);
    core_buffer_release(&manifest_name);
    if (!manifest) return false;
    manifest_reader_t r = {.data = manifest->data,
                           .length = manifest->length};
    uint64_t key = read_u64(&r);
    entry->freshness = read_u64(&r);
    entry->interface_identity = read_u64(&r);
    uint32_t members = read_u32(&r);
    for (uint32_t m = 0; !r.failed && m < members; m++) {
        cache_member_t member = {
            .name = read_text(&r), .path = read_text(&r),
            .imports = core_list_new(cache_text_t),
        };
        member.source_hash = read_u64(&r);
        uint32_t imports = read_u32(&r);
        for (uint32_t i = 0; !r.failed && i < imports; i++) {
            char *name = read_text(&r);
            if (name) core_list_push(member.imports, name);
        }
        core_list_push(entry->members, member);
    }
    read_links(&r, &entry->imports);
    read_links(&r, &entry->inlined);
    uint32_t references = read_u32(&r);
    for (uint32_t i = 0; !r.failed && i < references; i++) {
        char *name = read_text(&r);
        if (name) core_list_push(entry->references, name);
    }
    return !r.failed && r.at == r.length && key == entry->key;
}

static uint64_t
module_key(const n00bb_cache_t *cache, const char *module)
{
    for (size_t i = 0; module && i < cache->module_keys.len; i++)
        if (!strcmp(cache->module_keys.data[i].name, module))
            return cache->module_keys.data[i].value;
    return 0;
}

static cache_entry_t *
entry_for_module(n00bb_cache_t *cache, const char *module)
{
    uint64_t key = module_key(cache, module);
    for (size_t i = 0; key && i < cache->entries.len; i++)
        if (cache->entries.data[i].key == key) return &cache->entries.data[i];
    return nullptr;
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
            && (!cache->root_path || strcmp(member->path, cache->root_path))
            && read_file(member->path, &source, &source_bytes)
            && core_xxh3_64bits_raw(source ? (const void *)source : "",
                                    source_bytes) == member->source_hash;
        if (ok) {
            freshness_member(&freshness, member->name, strlen(member->name),
                             source, source_bytes);
            for (size_t i = 0; i < member->imports.len; i++)
                put_text(&freshness, member->imports.data[i]);
        }
        free(source);
    }
    ok = ok && buffer_hash(&freshness) == entry->freshness;
    core_buffer_release(&freshness);
    return ok;
}

static bool
entry_validate(n00bb_cache_t *cache, cache_entry_t *entry)
{
    if (entry->state == ENTRY_VALID) return true;
    if (entry->state != ENTRY_UNKNOWN) return false;
    entry->state = ENTRY_VISITING;
    bool ok = entry->fresh && phase_section(entry, "ir", nullptr)
        && phase_section(entry, "interface", nullptr);
    for (size_t m = 0; ok && m < entry->members.len; m++)
        ok = phase_section(entry, "native", entry->members.data[m].name)
            && phase_section(entry, "abi", entry->members.data[m].name);
    uint64_t *imports = core_alloc_array(uint64_t,
                                         entry->imports.len + 1);
    uint64_t *inlined = core_alloc_array(uint64_t,
                                         entry->inlined.len + 1);
    ok = ok && imports && inlined;
    for (size_t i = 0; ok && i < entry->imports.len; i++) {
        cache_entry_t *imported = entry_for_module(cache,
            entry->imports.data[i].name);
        ok = imported && entry_validate(cache, imported)
            && imported->interface_identity == entry->imports.data[i].value;
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
load_entries(n00bb_cache_t *cache)
{
    size_t inputs_used = 0;
    size_t module_count = 0;
    ir_archive_input_t *inputs = core_alloc_array(ir_archive_input_t,
                                                  cache->entries.len + 1);
    if (!inputs) return false;
    for (size_t e = 0; e < cache->entries.len; e++) {
        cache_entry_t *entry = &cache->entries.data[e];
        const cache_section_t *ir = entry->state == ENTRY_VALID
            ? phase_section(entry, "ir", nullptr) : nullptr;
        if (!ir) continue;
        inputs[inputs_used++] = (ir_archive_input_t){ir->data, ir->length};
        module_count += entry->members.len;
    }
    ir_module_t **modules = core_alloc_array(ir_module_t *,
                                             module_count + 1);
    const ir_profile_t *profiles[] = {n00bb_type_profile()};
    size_t loaded = 0;
    bool ok = modules && ir_archives_load(cache->ir_context, inputs,
        inputs_used, profiles, 1, nullptr, nullptr, modules, module_count,
        &loaded) && loaded == module_count;
    size_t next = 0;
    for (size_t e = 0; ok && e < cache->entries.len; e++) {
        cache_entry_t *entry = &cache->entries.data[e];
        if (entry->state != ENTRY_VALID) continue;
        for (size_t m = 0; ok && m < entry->members.len; m++) {
            ir_module_t *module = modules[next++];
            ok = !strcmp(ir_module_name(module), entry->members.data[m].name);
            entry->members.data[m].module = module;
        }
        const cache_section_t *interface = phase_section(entry, "interface",
                                                         nullptr);
        entry->interface = ok ? th_env_decode(nullptr, interface->data,
                                              interface->length) : nullptr;
        ok = entry->interface != nullptr;
        entry->load = ok ? ENTRY_VALID : ENTRY_INVALID;
    }
    if (!ok)
        for (size_t e = 0; e < cache->entries.len; e++)
            cache->entries.data[e].load = ENTRY_INVALID;
    core_free(modules);
    core_free(inputs);
    return ok;
}

static void
read_module_keys(n00bb_cache_t *cache)
{
    core_buffer_t path = {0};
    core_buffer_printf(&path, "%s/modules", cache->directory);
    DIR *directory = opendir(path.data);
    for (struct dirent *item = directory ? readdir(directory) : nullptr;
         item; item = readdir(directory)) {
        size_t length = strlen(item->d_name);
        core_buffer_t suffix = {0};
        core_buffer_printf(&suffix, ".%s.key", phase_name());
        bool matches = length > suffix.byte_len
            && !strcmp(item->d_name + length - suffix.byte_len, suffix.data);
        size_t suffix_bytes = suffix.byte_len;
        core_buffer_release(&suffix);
        if (!matches) continue;
        core_buffer_t file = {0};
        core_buffer_printf(&file, "%s/%s", path.data, item->d_name);
        uint8_t *bytes = nullptr;
        size_t byte_count = 0;
        if (read_file(file.data, &bytes, &byte_count) && byte_count >= 16) {
            char text[17] = {0};
            memcpy(text, bytes, 16);
            uint64_t key = strtoull(text, nullptr, 16);
            char *name = core_strndup(item->d_name, length - suffix_bytes);
            if (key && name)
                core_list_push(cache->module_keys,
                               ((cache_link_t){name, key}));
            else core_free(name);
        }
        free(bytes);
        core_buffer_release(&file);
    }
    if (directory) closedir(directory);
    core_buffer_release(&path);
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
        core_buffer_printf(&path, "%s/%016llx.n00bo", cache->directory,
                           (unsigned long long)key);
        uint8_t *bytes = nullptr;
        size_t length = 0;
        cache_entry_t entry = {.key = key};
        bool ok = read_file(path.data, &bytes, &length)
            && entry_parse(&entry, bytes, length);
        free(bytes);
        core_buffer_release(&path);
        if (ok) core_list_push(cache->entries, entry);
        else entry_release(&entry);
    }
}

n00bb_cache_t *
n00bb_cache_open(const char *root_path, parse_cmdr_result_t *args,
                 const char *compiler_path, const char *triple,
                 ir_ctx_t *ir_context)
{
    if (!root_path || !root_path[0] || root_path[0] == '<' || !triple
        || getenv("N00BB_NO_OBJECT_CACHE")
        || parse_cmdr_flag_present(args, CORE_STRING_STATIC("--lto")))
        return nullptr;
    n00bb_cache_t *cache = core_alloc(n00bb_cache_t);
    if (!cache) return nullptr;
    cache->root_path = canonical_path(root_path, strlen(root_path));
    const char *slash = cache->root_path
        ? strrchr(cache->root_path, '/') : nullptr;
    core_buffer_t directory = {0};
    if (slash) core_buffer_append(&directory, cache->root_path,
                                  (size_t)(slash - cache->root_path));
    else core_buffer_puts(&directory, ".");
    core_buffer_puts(&directory, "/.n00b-cache/obj");
    cache->directory = directory.data;
    cache->triple = core_strdup(triple);
    cache->compiler = compiler_identity(compiler_path);
    append_flags(&cache->flags, args);
    cache->ir_context = ir_context;
    cache->module_keys = core_list_new(cache_link_t);
    cache->entries = core_list_new(cache_entry_t);
    cache->interfaces = core_list_new(cache_environment_t);
    cache->snapshots = core_list_new(cache_snapshot_t);
    read_module_keys(cache);
    read_entries(cache);
    for (size_t e = 0; e < cache->entries.len; e++)
        cache->entries.data[e].fresh = entry_fresh(cache,
                                                   &cache->entries.data[e]);
    for (size_t e = 0; e < cache->entries.len; e++)
        (void)entry_validate(cache, &cache->entries.data[e]);
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t e = 0; e < cache->entries.len; e++) {
            cache_entry_t *entry = &cache->entries.data[e];
            if (entry->state != ENTRY_VALID) continue;
            for (size_t i = 0; i < entry->references.len; i++) {
                cache_entry_t *referenced = entry_for_module(cache,
                    entry->references.data[i]);
                bool member = false;
                for (size_t m = 0; !member && m < entry->members.len; m++)
                    member = !strcmp(entry->members.data[m].name,
                                     entry->references.data[i]);
                if (member || (referenced
                               && referenced->state == ENTRY_VALID))
                    continue;
                entry->state = ENTRY_INVALID;
                changed = true;
                break;
            }
        }
    }
    if (load_entries(cache))
        for (size_t e = 0; e < cache->entries.len; e++)
            if (cache->entries.data[e].load == ENTRY_VALID)
                core_list_push(cache->interfaces,
                               cache->entries.data[e].interface);
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
        core_free(cache->module_keys.data[i].name);
    core_list_free(cache->module_keys);
    core_list_free(cache->interfaces);
    for (size_t i = 0; i < cache->snapshots.len; i++) {
        core_free(cache->snapshots.data[i].module);
        core_buffer_release(&cache->snapshots.data[i].ir);
        core_buffer_release(&cache->snapshots.data[i].references);
    }
    core_list_free(cache->snapshots);
    core_buffer_release(&cache->flags);
    core_free(cache->directory);
    core_free(cache->root_path);
    core_free(cache->triple);
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

static const cache_member_t *
loaded_member(const n00bb_cache_t *cache, const char *module, size_t length,
              const cache_entry_t **owner)
{
    for (size_t e = 0; cache && module && e < cache->entries.len; e++) {
        const cache_entry_t *entry = &cache->entries.data[e];
        if (entry->load != ENTRY_VALID) continue;
        for (size_t m = 0; m < entry->members.len; m++) {
            const cache_member_t *member = &entry->members.data[m];
            if (strlen(member->name) == length
                && !memcmp(member->name, module, length)) {
                if (owner) *owner = entry;
                return member;
            }
        }
    }
    return nullptr;
}

ir_module_t *
n00bb_cache_module(const n00bb_cache_t *cache, core_string_t module)
{
    const cache_member_t *member = loaded_member(cache, module.data,
                                                 module.u8_bytes, nullptr);
    return member ? member->module : nullptr;
}

bool
n00bb_cache_module_object(const n00bb_cache_t *cache, const char *module,
                          n00bb_cache_module_object_t *object)
{
    const cache_entry_t *entry = nullptr;
    const cache_member_t *member = module
        ? loaded_member(cache, module, strlen(module), &entry) : nullptr;
    const cache_section_t *native = member
        ? phase_section(entry, "native", member->name) : nullptr;
    const cache_section_t *abi = member
        ? phase_section(entry, "abi", member->name) : nullptr;
    if (!native || !abi) return false;
    *object = (n00bb_cache_module_object_t){
        .module = member->name,
        .native = native->data, .native_bytes = native->length,
        .abi = abi->data, .abi_bytes = abi->length,
    };
    return true;
}

void
n00bb_cache_closed_functions(const n00bb_cache_t *cache,
                             core_list_t(n00bb_ir_func_ptr_t) *functions)
{
    for (size_t e = 0; cache && e < cache->entries.len; e++) {
        const cache_entry_t *entry = &cache->entries.data[e];
        if (entry->load != ENTRY_VALID) continue;
        for (size_t m = 0; m < entry->members.len; m++) {
            ir_module_t *module = entry->members.data[m].module;
            for (size_t f = 0; module && f < ir_func_count(module); f++) {
                ir_func_t *function = ir_func_at(module, f);
                if (function->n_blocks
                    && th_is_closed(ir_module_types(module), function->type))
                    core_list_push(*functions, function);
            }
        }
    }
}

bool
n00bb_cache_snapshot(n00bb_cache_t *cache, core_link_unit_t *link)
{
    if (!cache || !link) return false;
    core_list_t(cache_scc_t) sccs = core_list_new(cache_scc_t);
    bool ok = collect_sccs(link, &sccs);
    for (size_t s = 0; ok && s < sccs.len; s++) {
        cache_scc_t *scc = &sccs.data[s];
        if (scc->root || scc->cached) continue;
        ir_module_t **modules = core_alloc_array(ir_module_t *,
                                                 scc->members.len);
        for (size_t m = 0; m < scc->members.len; m++)
            modules[m] = tu_state(scc->members.data[m])->ir_module;
        cache_snapshot_t snapshot = {
            .module = core_strndup(scc->members.data[0]->module.identifier.data,
                scc->members.data[0]->module.identifier.u8_bytes),
        };
        ok = snapshot.module
            && ir_modules_save_references(modules, scc->members.len,
                                          &snapshot.ir, &snapshot.references);
        core_free(modules);
        if (ok) core_list_push(cache->snapshots, snapshot);
        else {
            core_free(snapshot.module);
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
        if (strlen(cache->snapshots.data[i].module) == module.u8_bytes
            && !memcmp(cache->snapshots.data[i].module, module.data,
                       module.u8_bytes))
            return &cache->snapshots.data[i];
    return nullptr;
}

static const n00bb_cache_module_object_t *
module_object(const n00bb_cache_module_object_t *objects, size_t count,
              core_string_t module)
{
    for (size_t i = 0; i < count; i++)
        if (objects[i].module && strlen(objects[i].module) == module.u8_bytes
            && !memcmp(objects[i].module, module.data, module.u8_bytes))
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

static bool
store_scc(const n00bb_cache_t *cache, const core_list_t(cache_scc_t) *sccs,
          const cache_scc_t *scc, const cache_snapshot_t *snapshot,
          const n00bb_cache_module_object_t *objects, size_t object_count)
{
    const char *phase = phase_name();
    core_buffer_t manifest = {0};
    put_u64(&manifest, scc->key);
    put_u64(&manifest, scc->freshness);
    put_u64(&manifest, scc->interface_identity);
    put_u32(&manifest, (uint32_t)scc->members.len);
    for (size_t m = 0; m < scc->members.len; m++) {
        core_translation_unit_t *unit = scc->members.data[m];
        core_string_t path = core_source_file_path(&unit->source);
        char *canonical = canonical_path(path.data, path.u8_bytes);
        put_bytes(&manifest, unit->module.identifier.data,
                  unit->module.identifier.u8_bytes);
        put_text(&manifest, canonical);
        core_free(canonical);
        put_u64(&manifest, core_xxh3_64bits_raw(
            core_source_file_contents(&unit->source),
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
    core_buffer_t path = {0};
    core_buffer_printf(&path, "%s/%016llx.n00bo", cache->directory,
                       (unsigned long long)scc->key);
    core_list_t(cache_section_t) sections = core_list_new(cache_section_t);
    uint8_t *existing = nullptr;
    size_t existing_bytes = 0;
    if (read_file(path.data, &existing, &existing_bytes)
        && !sections_parse(existing, existing_bytes, &sections)) {
        sections_release(&sections);
        sections = core_list_new(cache_section_t);
    }
    free(existing);
    core_buffer_t name = {0};
    core_buffer_printf(&name, "%s.manifest", phase);
    bool ok = sections_set(&sections, name.data, manifest.data,
                           manifest.byte_len);
    core_buffer_release(&name);
    core_buffer_printf(&name, "%s.ir", phase);
    ok = ok && sections_set(&sections, name.data, ir->data, ir->byte_len);
    core_buffer_release(&name);
    core_buffer_printf(&name, "%s.interface", phase);
    ok = ok && sections_set(&sections, name.data, scc->interface,
                            scc->interface_bytes);
    core_buffer_release(&name);
    for (size_t m = 0; ok && m < scc->members.len; m++) {
        core_string_t module = scc->members.data[m]->module.identifier;
        const n00bb_cache_module_object_t *object = module_object(objects,
            object_count, module);
        ok = object && object->native && object->abi;
        if (!ok) break;
        core_buffer_printf(&name, "%s.native.%.*s", phase,
                           (int)module.u8_bytes, module.data);
        ok = sections_set(&sections, name.data, object->native,
                          object->native_bytes);
        core_buffer_release(&name);
        core_buffer_printf(&name, "%s.abi.%.*s", phase,
                           (int)module.u8_bytes, module.data);
        ok = ok && sections_set(&sections, name.data, object->abi,
                                object->abi_bytes);
        core_buffer_release(&name);
    }
    core_buffer_t serialized = {0};
    if (ok) sections_serialize(&sections, &serialized);
    ok = ok && write_file_atomic(path.data, serialized.data,
                                 serialized.byte_len);
    for (size_t m = 0; ok && m < scc->members.len; m++) {
        core_string_t module = scc->members.data[m]->module.identifier;
        core_buffer_t index = {0};
        core_buffer_t key = {0};
        core_buffer_printf(&index, "%s/modules/%.*s.%s.key", cache->directory,
                           (int)module.u8_bytes, module.data, phase);
        core_buffer_printf(&key, "%016llx\n", (unsigned long long)scc->key);
        ok = write_file_atomic(index.data, key.data, key.byte_len);
        core_buffer_release(&index);
        core_buffer_release(&key);
    }
    core_buffer_release(&serialized);
    sections_release(&sections);
    core_buffer_release(&path);
    core_buffer_release(&manifest);
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
    core_buffer_printf(&modules_directory, "%s/modules", cache->directory);
    bool ok = make_directories(modules_directory.data);
    core_buffer_release(&modules_directory);
    core_list_t(cache_scc_t) sccs = core_list_new(cache_scc_t);
    ok = ok && collect_sccs(link, &sccs);
    for (size_t s = 0; ok && s < sccs.len; s++) {
        core_string_t first = sccs.data[s].members.data[0]->module.identifier;
        const cache_entry_t *entry = nullptr;
        if (sccs.data[s].cached
            && loaded_member(cache, first.data, first.u8_bytes, &entry)) {
            sccs.data[s].freshness = entry->freshness;
            sccs.data[s].interface_identity = entry->interface_identity;
            continue;
        }
        if (!interface_bytes(analysis, &sccs, &sccs.data[s])) {
            ok = false;
            break;
        }
        core_buffer_t identity = {0};
        core_buffer_puts(&identity, "n00b.scc.interface.v1");
        put_bytes(&identity, sccs.data[s].interface,
                  sccs.data[s].interface_bytes);
        sccs.data[s].interface_identity = buffer_hash(&identity);
        core_buffer_release(&identity);
    }
    for (size_t s = 0; ok && s < sccs.len; s++) {
        cache_scc_t *scc = &sccs.data[s];
        uint64_t *imports = core_alloc_array(uint64_t, scc->imports.len + 1);
        uint64_t *inlined = core_alloc_array(uint64_t, scc->inlined.len + 1);
        for (size_t i = 0; i < scc->imports.len; i++)
            imports[i] = sccs.data[scc->imports.data[i]].interface_identity;
        for (size_t i = 0; i < scc->inlined.len; i++)
            inlined[i] = sccs.data[scc->inlined.data[i]].freshness;
        scc->key = scc_key(cache, scc->freshness, imports, scc->imports.len,
                           inlined, scc->inlined.len);
        core_free(imports);
        core_free(inlined);
    }
    for (size_t s = 0; ok && s < sccs.len; s++) {
        cache_scc_t *scc = &sccs.data[s];
        const cache_snapshot_t *snapshot = scc->root || scc->cached
            ? nullptr
            : snapshot_for(cache, scc->members.data[0]->module.identifier);
        if (snapshot)
            ok = store_scc(cache, &sccs, scc, snapshot, objects,
                           object_count);
    }
    sccs_release(&sccs);
    return ok;
}
