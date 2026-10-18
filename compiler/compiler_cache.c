#include <errno.h>
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
    if (module_member(filter->scc, owner, strlen(owner))) return true;
    for (size_t m = 0; m < filter->scc->members.len; m++) {
        n00bb_tu_state_t *state = tu_state(filter->scc->members.data[m]);
        for (size_t p = 0; state && p < state->package_declarations.len; p++) {
            core_string_t package = state->package_declarations.data[p].name;
            if (package.u8_bytes == strlen(owner)
                && !memcmp(package.data, owner, package.u8_bytes))
                return true;
        }
    }
    return false;
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
interface_bytes(n00bb_link_analysis_t *analysis, const char *directory,
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
    (void)directory;
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
scc_freshness(cache_scc_t *scc)
{
    core_buffer_t input = {0};
    core_buffer_puts(&input, "n00b.scc.freshness.v1");
    for (size_t m = 0; m < scc->members.len; m++) {
        core_translation_unit_t *unit = scc->members.data[m];
        put_bytes(&input, unit->module.identifier.data,
                  unit->module.identifier.u8_bytes);
        put_bytes(&input, core_source_file_contents(&unit->source),
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

static bool
collect_sccs(core_link_unit_t *link, core_list_t(cache_scc_t) *sccs)
{
    core_translation_unit_t *root = core_link_unit_root(link, 0);
    for (size_t g = 0; g < core_link_unit_scc_count(link); g++) {
        cache_scc_t scc = {
            .index = sccs->len,
            .members = core_list_new(core_translation_unit_ptr_t),
            .imports = core_list_new(size_t),
        };
        for (size_t m = 0; m < core_link_unit_scc_member_count(link, g); m++) {
            core_translation_unit_t *unit = core_link_unit_scc_member(link,
                                                                      g, m);
            n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
            if (!state || !state->ir_module) {
                core_list_free(scc.members);
                core_list_free(scc.imports);
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
    }
    return true;
}

static char *
cache_directory(core_link_unit_t *link)
{
    core_translation_unit_t *root = core_link_unit_root(link, 0);
    core_string_t path = root ? core_source_file_path(&root->source)
                              : (core_string_t){0};
    if (!path.data || !path.u8_bytes || path.data[0] == '<') return nullptr;
    size_t slash = path.u8_bytes;
    while (slash && path.data[slash - 1] != '/') slash--;
    core_buffer_t directory = {0};
    if (slash) core_buffer_append(&directory, path.data, slash - 1);
    else core_buffer_puts(&directory, ".");
    core_buffer_puts(&directory, "/.n00b-cache/obj");
    return directory.data;
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

static bool
store_scc(const char *directory, const core_list_t(cache_scc_t) *sccs,
          const cache_scc_t *scc, const n00bb_cache_module_object_t *objects,
          size_t object_count)
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
        put_bytes(&manifest, unit->module.identifier.data,
                  unit->module.identifier.u8_bytes);
        put_bytes(&manifest, path.data, path.u8_bytes);
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
    put_u32(&manifest, (uint32_t)scc->imports.len);
    for (size_t i = 0; i < scc->imports.len; i++) {
        const cache_scc_t *imported = &sccs->data[scc->imports.data[i]];
        core_string_t name = imported->members.data[0]->module.identifier;
        put_bytes(&manifest, name.data, name.u8_bytes);
        put_u64(&manifest, imported->interface_identity);
    }
    ir_module_t **modules = core_alloc_array(ir_module_t *,
                                             scc->members.len);
    for (size_t m = 0; m < scc->members.len; m++)
        modules[m] = tu_state(scc->members.data[m])->ir_module;
    core_buffer_t ir = {0};
    bool ok = ir_modules_save(modules, scc->members.len, &ir);
    core_free(modules);
    core_buffer_t path = {0};
    core_buffer_printf(&path, "%s/%016llx.n00bo", directory,
                       (unsigned long long)scc->key);
    core_list_t(cache_section_t) sections = core_list_new(cache_section_t);
    uint8_t *existing = nullptr;
    size_t existing_bytes = 0;
    if (ok && read_file(path.data, &existing, &existing_bytes)
        && !sections_parse(existing, existing_bytes, &sections)) {
        sections_release(&sections);
        sections = core_list_new(cache_section_t);
    }
    free(existing);
    core_buffer_t name = {0};
    ok = ok && sections_set(&sections, "manifest", manifest.data,
                            manifest.byte_len);
    core_buffer_printf(&name, "%s.ir", phase);
    ok = ok && sections_set(&sections, name.data, ir.data, ir.byte_len);
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
        core_buffer_printf(&index, "%s/modules/%.*s.key", directory,
                           (int)module.u8_bytes, module.data);
        core_buffer_printf(&key, "%016llx\n", (unsigned long long)scc->key);
        ok = write_file_atomic(index.data, key.data, key.byte_len);
        core_buffer_release(&index);
        core_buffer_release(&key);
    }
    core_buffer_release(&serialized);
    sections_release(&sections);
    core_buffer_release(&path);
    core_buffer_release(&ir);
    core_buffer_release(&manifest);
    return ok;
}

bool
n00bb_cache_store(core_link_unit_t *link, n00bb_link_analysis_t *analysis,
                  parse_cmdr_result_t *args, const char *compiler_path,
                  const n00bb_cache_module_object_t *objects,
                  size_t object_count)
{
    char *directory = link && analysis ? cache_directory(link) : nullptr;
    if (!directory) return false;
    core_buffer_t modules_directory = {0};
    core_buffer_printf(&modules_directory, "%s/modules", directory);
    bool ok = make_directories(modules_directory.data);
    core_buffer_release(&modules_directory);
    core_list_t(cache_scc_t) sccs = core_list_new(cache_scc_t);
    ok = ok && collect_sccs(link, &sccs);
    for (size_t s = 0; ok && s < sccs.len; s++) {
        sccs.data[s].interface_identity = 0;
        if (!interface_bytes(analysis, directory, &sccs, &sccs.data[s])) {
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
    uint64_t compiler = ok ? compiler_identity(compiler_path) : 0;
    core_buffer_t flags = {0};
    append_flags(&flags, args);
    for (size_t s = 0; ok && s < sccs.len; s++) {
        cache_scc_t *scc = &sccs.data[s];
        core_list_t(size_t) inlined = core_list_new(size_t);
        inline_dependencies(&sccs, scc, &inlined);
        core_buffer_t key = {0};
        core_buffer_puts(&key, "n00b.scc.key.v1");
        put_u64(&key, scc->freshness);
        put_u32(&key, (uint32_t)scc->imports.len);
        for (size_t i = 0; i < scc->imports.len; i++)
            put_u64(&key, sccs.data[scc->imports.data[i]].interface_identity);
        put_u32(&key, (uint32_t)inlined.len);
        for (size_t i = 0; i < inlined.len; i++)
            put_u64(&key, sccs.data[inlined.data[i]].freshness);
        put_u64(&key, compiler);
        put_bytes(&key, flags.data, flags.byte_len);
        put_text(&key, analysis->target.triple);
        scc->key = buffer_hash(&key);
        core_buffer_release(&key);
        core_list_free(inlined);
    }
    core_buffer_release(&flags);
    for (size_t s = 0; ok && s < sccs.len; s++) {
        cache_scc_t *scc = &sccs.data[s];
        if (scc->root || scc->cached) continue;
        ok = store_scc(directory, &sccs, scc, objects, object_count);
    }
    for (size_t s = 0; s < sccs.len; s++) {
        free(sccs.data[s].interface);
        core_list_free(sccs.data[s].members);
        core_list_free(sccs.data[s].imports);
    }
    core_list_free(sccs);
    core_free(directory);
    return ok;
}
