#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1

#include "driver.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <wchar.h>
#include <windows.h>
#define N00BB_PATH_SEP '\\'
#define N00BB_LIST_SEP ';'
#else
#include <unistd.h>
#define N00BB_PATH_SEP '/'
#define N00BB_LIST_SEP ':'
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/diagnostic.h"
#include "lib/hash.h"
#include "lib/list.h"
#include "lib/snapshot_inventory.h"
#include "th/typehash.h"

typedef struct {
    core_string_t path;
    bool is_default_package;
} n00bb_root_t;
core_list_decl(n00bb_root_t);

typedef struct {
    core_string_t package_id;
    core_string_t package_namespace;
    bool is_default_package;
    n00bb_native_declarations_t natives;
} n00bb_source_origin_t;

typedef struct {
    core_string_t path;
    core_string_t relative;
    size_t root_index;
    core_module_spec_t module;
} n00bb_manifest_candidate_t;
core_list_decl(n00bb_manifest_candidate_t);

typedef struct {
    core_string_t identifier;
    core_string_t text;
    core_string_t package;
    core_string_t package_namespace;
    bool is_default_package;
    n00bb_native_declarations_t natives;
    n00bb_line_origins_t origins;
} n00bb_generated_module_t;
core_list_decl(n00bb_generated_module_t);

struct n00bb_resolver_t {
    core_list_t(n00bb_root_t) roots;
    core_snapshot_inventory_t *inventory;
    core_list_t(n00bb_manifest_candidate_t) manifests;
    bool manifests_indexed;
    uint64_t manifests_generation;
    uint64_t package_syntax_profile;
    core_list_t(n00bb_generated_module_t) generated;
    pthread_mutex_t generated_lock;
};

static char *join_path(const char *a, const char *b);
static size_t source_suffix_length(const char *path);
static uint64_t package_syntax_profile(const char *argv0);
static void set_source_origin(core_source_file_t *source,
                              core_string_t package_id,
                              core_string_t package_namespace,
                              bool is_default_package);
#define PACKAGE_SYNTAX_KIND CORE_STRING_STATIC("n00bb.package-syntax.v1")
static bool
accept_source_path(void *context, core_string_t relative)
{
    (void)context;
    size_t length = relative.u8_bytes;
    return (length >= 2 && !memcmp(relative.data + length - 2, ".n", 2))
        || (length >= 5
            && !memcmp(relative.data + length - 5, ".n00b", 5));
}
#ifdef _WIN32
static char *wide_to_utf8(const wchar_t *wide, int length);
static wchar_t *utf8_to_wide(const char *utf8);
#endif

#ifdef _WIN32
static char *
wide_to_utf8(const wchar_t *wide, int length)
{
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0,
                                   nullptr, nullptr);
    if (bytes <= 0) return nullptr;
    char *utf8 = core_alloc_size(1, (size_t)bytes + 1);
    WideCharToMultiByte(CP_UTF8, 0, wide, length, utf8, bytes, nullptr, nullptr);
    utf8[bytes] = '\0';
    return utf8;
}

static wchar_t *
utf8_to_wide(const char *utf8)
{
    int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                                    nullptr, 0);
    if (chars <= 0) return nullptr;
    wchar_t *wide = core_alloc_array(wchar_t, chars);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, chars);
    return wide;
}
#endif

static bool
path_is_absolute(const char *path)
{
#ifdef _WIN32
    return path[0] == '/' || path[0] == '\\'
        || (path[0] && path[1] == ':'
            && (path[2] == '/' || path[2] == '\\'));
#else
    return path[0] == '/';
#endif
}

static char *
canonical_path(const char *path)
{
#ifdef _WIN32
    wchar_t *wide = utf8_to_wide(path);
    if (!wide) return nullptr;
    wchar_t *full = _wfullpath(nullptr, wide, 0);
    core_free(wide);
    if (!full) return nullptr;
    char *result = wide_to_utf8(full, -1);
    free(full);
    return result;
#else
    return realpath(path, nullptr);
#endif
}

static FILE *
open_source(const char *path)
{
#ifdef _WIN32
    wchar_t *wide = utf8_to_wide(path);
    if (!wide) return nullptr;
    FILE *file = _wfopen(wide, L"rb");
    core_free(wide);
    return file;
#else
    return fopen(path, "rb");
#endif
}

static char *
join_path(const char *a, const char *b)
{
    core_buffer_t *out = core_buffer_empty();
    core_buffer_puts(out, a);
    if (out->byte_len && out->data[out->byte_len - 1] != '/'
        && out->data[out->byte_len - 1] != '\\')
        core_buffer_putc(out, N00BB_PATH_SEP);
    core_buffer_puts(out, b);
    return core_buffer_take(out);
}

static char *
parent_path(const char *path)
{
    char *copy = core_strdup(path);
    size_t len = strlen(copy);
    while (len > 1 && (copy[len - 1] == '/' || copy[len - 1] == '\\'))
        copy[--len] = '\0';
#ifdef _WIN32
    if (len == 2 && copy[1] == ':') {
        copy = core_realloc(copy, 4);
        copy[2] = '\\';
        copy[3] = '\0';
        return copy;
    }
#endif
    char *slash = strrchr(copy, '/');
    char *backslash = strrchr(copy, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    if (!slash) {
        core_free(copy);
        return core_strdup(".");
    }
    if (slash == copy) slash[1] = '\0';
#ifdef _WIN32
    else if (slash == copy + 2 && copy[1] == ':') slash[1] = '\0';
#endif
    else *slash = '\0';
    return copy;
}

static char *
cwd_path(void)
{
#ifdef _WIN32
    DWORD cap = GetCurrentDirectoryW(0, nullptr);
    wchar_t *wide = core_alloc_array(wchar_t, cap);
    DWORD n = GetCurrentDirectoryW(cap, wide);
    char *path = n && n < cap ? wide_to_utf8(wide, (int)n) : nullptr;
    core_free(wide);
    return path;
#else
    return getcwd(nullptr, 0);
#endif
}

char *
n00bb_executable_path(const char *argv0)
{
#ifdef _WIN32
    DWORD cap = 256;
    wchar_t *wide = nullptr;
    for (;;) {
        wide = core_realloc(wide, cap * sizeof(*wide));
        DWORD n = GetModuleFileNameW(nullptr, wide, cap);
        if (!n) break;
        if (n < cap - 1) {
            char *path = wide_to_utf8(wide, (int)n);
            core_free(wide);
            return path;
        }
        cap *= 2;
    }
    core_free(wide);
    return argv0 ? canonical_path(argv0) : nullptr;
#elif defined(__APPLE__)
    uint32_t cap = 0;
    _NSGetExecutablePath(nullptr, &cap);
    char *path = core_alloc_size(1, cap + 1);
    if (_NSGetExecutablePath(path, &cap) == 0) {
        char *resolved = realpath(path, nullptr);
        if (resolved) {
            core_free(path);
            return resolved;
        }
        return path;
    }
    core_free(path);
    return argv0 ? canonical_path(argv0) : nullptr;
#elif defined(__linux__)
    size_t cap = 256;
    char *path = nullptr;
    for (;;) {
        path = core_realloc(path, cap + 1);
        ssize_t n = readlink("/proc/self/exe", path, cap);
        if (n < 0) break;
        if ((size_t)n < cap) {
            path[n] = '\0';
            return path;
        }
        cap *= 2;
    }
    core_free(path);
    return argv0 ? canonical_path(argv0) : nullptr;
#else
    return argv0 ? canonical_path(argv0) : nullptr;
#endif
}

static void
add_root(n00bb_resolver_t *resolver, const char *path,
         bool is_default_package)
{
    if (!path || !*path) return;
    char *canonical = canonical_path(path);
    n00bb_root_t root = {
        .path = core_string_from_cstr(canonical ? canonical : path),
        .is_default_package = is_default_package,
    };
    core_free(canonical);
    core_list_push(resolver->roots, root);
}

static void
add_environment_roots(n00bb_resolver_t *resolver, const char *cwd)
{
#ifdef _WIN32
    DWORD cap = GetEnvironmentVariableW(L"N00B_PATH", nullptr, 0);
    if (!cap) return;
    wchar_t *wide = core_alloc_array(wchar_t, cap);
    DWORD n = GetEnvironmentVariableW(L"N00B_PATH", wide, cap);
    char *value = n && n < cap ? wide_to_utf8(wide, (int)n) : nullptr;
    core_free(wide);
#else
    const char *value = getenv("N00B_PATH");
#endif
    if (!value) return;
    if (!*value) {
#ifdef _WIN32
        core_free(value);
#endif
        return;
    }
    const char *part = value;
    const char *end;
    while ((end = strchr(part, N00BB_LIST_SEP))) {
        if (end > part) {
            char *entry = core_strndup(part, (size_t)(end - part));
            char *absolute = path_is_absolute(entry)
                                 ? core_strdup(entry) : join_path(cwd, entry);
            add_root(resolver, absolute, false);
            core_free(absolute);
            core_free(entry);
        }
        part = end + 1;
    }
    if (*part) {
        char *absolute = path_is_absolute(part)
                             ? core_strdup(part) : join_path(cwd, part);
        add_root(resolver, absolute, false);
        core_free(absolute);
    }
#ifdef _WIN32
    core_free(value);
#endif
}

static void
add_executable_library_roots(n00bb_resolver_t *resolver, const char *argv0)
{
    char *exe = n00bb_executable_path(argv0);
    if (!exe) return;
    char *dir = parent_path(exe);
    for (;;) {
        char *n00b = join_path(dir, "n00b");
        char *lib = join_path(n00b, "lib");
        add_root(resolver, lib, false);
        core_free(lib);
        core_free(n00b);
        char *parent = parent_path(dir);
        if (!strcmp(parent, dir)) {
            core_free(parent);
            break;
        }
        core_free(dir);
        dir = parent;
    }
    core_free(dir);
    core_free(exe);
}

static void
add_bundled_library_root(n00bb_resolver_t *resolver, const char *argv0)
{
    char *exe = n00bb_executable_path(argv0);
    if (!exe) return;
    char *bin = parent_path(exe);
    char *prefix = parent_path(bin);
    char *lib = join_path(prefix, "lib");
    add_root(resolver, lib, false);
    core_free(lib);
    core_free(prefix);
    core_free(bin);
    core_free(exe);
}

n00bb_resolver_t *
n00bb_resolver_new(const char *argv0)
{
    char *cwd = cwd_path();
    if (!cwd) return nullptr;
    n00bb_resolver_t *resolver = core_alloc(n00bb_resolver_t);
    resolver->roots = core_list_new(n00bb_root_t);
    resolver->manifests = core_list_new(n00bb_manifest_candidate_t);
    resolver->manifests_indexed = false;
    resolver->generated = core_list_new(n00bb_generated_module_t);
    pthread_mutex_init(&resolver->generated_lock, nullptr);
    add_bundled_library_root(resolver, argv0);
    add_root(resolver, cwd, true);
    add_environment_roots(resolver, cwd);
    add_executable_library_roots(resolver, argv0);
    core_string_t *roots = core_alloc_array(core_string_t,
                                            resolver->roots.len);
    for (size_t i = 0; i < resolver->roots.len; i++)
        roots[i] = resolver->roots.data[i].path;
    resolver->inventory = core_snapshot_inventory_new(roots,
        resolver->roots.len, accept_source_path, nullptr);
    if (resolver->inventory && !getenv("N00BB_NO_PACKAGE_CACHE"))
        if (core_snapshot_inventory_enable_local_cache(
                resolver->inventory, CORE_STRING_STATIC(".n00b-cache"), 1))
            resolver->package_syntax_profile = package_syntax_profile(argv0);
    core_free(roots);
    core_free(cwd);
    if (!resolver->inventory) {
        n00bb_resolver_free(resolver);
        return nullptr;
    }
    return resolver;
}

void
n00bb_resolver_free(n00bb_resolver_t *resolver)
{
    if (!resolver) return;
    core_snapshot_inventory_free(resolver->inventory);
    for (size_t i = 0; i < resolver->roots.len; i++)
        core_free(resolver->roots.data[i].path.data);
    core_list_free(resolver->roots);
    for (size_t i = 0; i < resolver->manifests.len; i++) {
        core_free(resolver->manifests.data[i].path.data);
        core_free(resolver->manifests.data[i].relative.data);
        core_module_spec_release(&resolver->manifests.data[i].module);
    }
    core_list_free(resolver->manifests);
    for (size_t i = 0; i < resolver->generated.len; i++) {
        core_free(resolver->generated.data[i].identifier.data);
        core_free(resolver->generated.data[i].text.data);
        core_free(resolver->generated.data[i].package.data);
        core_free(resolver->generated.data[i].package_namespace.data);
        n00bb_native_declarations_release(&resolver->generated.data[i].natives);
        n00bb_line_origins_release(&resolver->generated.data[i].origins);
    }
    core_list_free(resolver->generated);
    pthread_mutex_destroy(&resolver->generated_lock);
    core_free(resolver);
}

bool
n00bb_resolver_add_generated(n00bb_resolver_t *resolver, core_string_t identifier, core_string_t text,
                             const core_translation_unit_t *owner,
                             n00bb_native_declarations_t natives,
                             n00bb_line_origins_t origins)
{
    const n00bb_source_origin_t *origin = owner
        ? core_source_file_user_data(&owner->source) : nullptr;
    if (!resolver || !identifier.u8_bytes || !origin) {
        n00bb_native_declarations_release(&natives);
        n00bb_line_origins_release(&origins);
        return false;
    }
    pthread_mutex_lock(&resolver->generated_lock);
    core_list_push(resolver->generated, ((n00bb_generated_module_t){
        .identifier = core_string_dup(identifier),
        .text = core_string_dup(text),
        .package = core_string_dup(origin->package_id),
        .package_namespace = core_string_dup(origin->package_namespace),
        .is_default_package = origin->is_default_package,
        .natives = natives,
        .origins = origins,
    }));
    pthread_mutex_unlock(&resolver->generated_lock);
    return true;
}

void
n00bb_native_declarations_release(n00bb_native_declarations_t *declarations)
{
    if (!declarations) return;
    for (size_t i = 0; i < declarations->count; i++) {
        core_free(declarations->items[i].name.data);
        core_free(declarations->items[i].symbol.data);
        core_free(declarations->items[i].signature.data);
    }
    core_free(declarations->items);
    *declarations = (n00bb_native_declarations_t){0};
}

void
n00bb_line_origins_release(n00bb_line_origins_t *origins)
{
    if (!origins) return;
    for (size_t i = 0; i < origins->count; i++)
        core_free(origins->items[i].path.data);
    core_free(origins->items);
    *origins = (n00bb_line_origins_t){0};
}

static const n00bb_line_origin_t *
generated_origin(const n00bb_generated_module_t *module, core_string_t path,
                 uint32_t line)
{
    size_t prefix = strlen("<generated:");
    if (path.u8_bytes != prefix + module->identifier.u8_bytes + 1
        || memcmp(path.data, "<generated:", prefix)
        || memcmp(path.data + prefix, module->identifier.data,
                  module->identifier.u8_bytes)) return nullptr;
    const n00bb_line_origin_t *nearest = nullptr;
    for (size_t i = 0; i < module->origins.count; i++)
        if (module->origins.items[i].line <= line
            && (!nearest || module->origins.items[i].line > nearest->line))
            nearest = &module->origins.items[i];
    return nearest;
}

void
n00bb_resolver_remap_diagnostics(n00bb_resolver_t *resolver,
                                 core_diagnostics_t *diagnostics)
{
    if (!resolver || !diagnostics) return;
    pthread_mutex_lock(&resolver->generated_lock);
    for (size_t d = 0; d < core_diagnostics_count(diagnostics); d++) {
        const core_diagnostic_t *diagnostic =
            core_diagnostics_at(diagnostics, d);
        if (!diagnostic->location.has_location) continue;
        const n00bb_line_origin_t *origin = nullptr;
        for (size_t m = 0; !origin && m < resolver->generated.len; m++)
            origin = generated_origin(&resolver->generated.data[m],
                diagnostic->path, diagnostic->location.line);
        if (!origin) continue;
        core_diagnostic_set_path(diagnostics, d, origin->path);
        core_diagnostic_set_location(diagnostics, d, (core_diag_location_t){
            origin->origin_line, origin->origin_column,
            origin->origin_line, origin->origin_column, true});
    }
    pthread_mutex_unlock(&resolver->generated_lock);
}

bool
n00bb_resolver_generated_text(n00bb_resolver_t *resolver,
                              core_string_t identifier, core_string_t *text)
{
    if (!resolver || !text) return false;
    bool found = false;
    pthread_mutex_lock(&resolver->generated_lock);
    for (size_t i = 0; !found && i < resolver->generated.len; i++)
        if (core_string_eq(resolver->generated.data[i].identifier,
                           identifier)) {
            *text = resolver->generated.data[i].text;
            found = true;
        }
    pthread_mutex_unlock(&resolver->generated_lock);
    return found;
}

n00bb_native_declarations_t
n00bb_source_natives(const core_translation_unit_t *unit)
{
    const n00bb_source_origin_t *origin = unit
        ? core_source_file_user_data(&unit->source) : nullptr;
    return origin ? origin->natives : (n00bb_native_declarations_t){0};
}

static core_module_resolve_status_t
read_generated_source(n00bb_resolver_t *resolver, const char *operand, core_source_file_t *source_out)
{
    core_module_resolve_status_t status = CORE_MODULE_NOT_FOUND;
    pthread_mutex_lock(&resolver->generated_lock);
    for (size_t i = 0; i < resolver->generated.len; i++) {
        n00bb_generated_module_t *module = &resolver->generated.data[i];
        if (strlen(operand) != module->identifier.u8_bytes
            || memcmp(operand, module->identifier.data, module->identifier.u8_bytes)) continue;
        core_buffer_t path = {0};
        core_buffer_printf(&path, "<generated:%s>", operand);
        bool ok = core_source_file_init(source_out, core_string_borrow(path.data, path.byte_len),
                                        module->text.data, module->text.u8_bytes);
        core_buffer_release(&path);
        if (ok) set_source_origin(source_out, module->package, module->package_namespace,
                                  module->is_default_package);
        n00bb_source_origin_t *origin = ok
            ? (n00bb_source_origin_t *)core_source_file_user_data(source_out) : nullptr;
        if (origin) origin->natives = module->natives;
        status = ok ? CORE_MODULE_RESOLVED : CORE_MODULE_RESOLVE_ERROR;
        break;
    }
    pthread_mutex_unlock(&resolver->generated_lock);
    return status;
}

static char *
module_relative_path(const core_module_spec_t *module)
{
    core_string_t id = module->identifier;
    if (!id.data || !id.u8_bytes || memchr(id.data, '\0', id.u8_bytes))
        return nullptr;
    core_buffer_t *relative = core_buffer_empty();
    size_t segment_start = 0;
    for (size_t i = 0; i <= id.u8_bytes; i++) {
        if (i < id.u8_bytes && id.data[i] != '.') {
            if (id.data[i] == '/' || id.data[i] == '\\') {
                core_buffer_free(relative);
                return nullptr;
            }
#ifdef _WIN32
            if (id.data[i] == ':') {
                core_buffer_free(relative);
                return nullptr;
            }
#endif
            continue;
        }
        size_t segment_len = i - segment_start;
        if (!segment_len || (segment_len == 1 && id.data[segment_start] == '.')
            || (segment_len == 2 && id.data[segment_start] == '.'
                && id.data[segment_start + 1] == '.')) {
            core_buffer_free(relative);
            return nullptr;
        }
        if (segment_start) core_buffer_putc(relative, N00BB_PATH_SEP);
        core_buffer_append(relative, id.data + segment_start, segment_len);
        segment_start = i + 1;
    }
    return core_buffer_take(relative);
}

static bool
has_path_separator(const char *path)
{
    return strchr(path, '/') || strchr(path, '\\');
}

static size_t
source_suffix_length(const char *path)
{
    size_t len = strlen(path);
    if (len >= 2 && !strcmp(path + len - 2, ".n")) return 2;
    if (len >= 5 && !strcmp(path + len - 5, ".n00b")) return 5;
    return 0;
}

static core_string_t
locator_scheme(const char *operand)
{
    if (!operand || (!(operand[0] >= 'A' && operand[0] <= 'Z')
            && !(operand[0] >= 'a' && operand[0] <= 'z')))
        return (core_string_t){0};
    size_t i = 1;
    while ((operand[i] >= 'A' && operand[i] <= 'Z')
           || (operand[i] >= 'a' && operand[i] <= 'z')
           || (operand[i] >= '0' && operand[i] <= '9')
           || operand[i] == '+' || operand[i] == '-'
           || operand[i] == '.') i++;
    return operand[i] == ':' && operand[i + 1] == '/'
            && operand[i + 2] == '/'
        ? core_string_borrow(operand, i) : (core_string_t){0};
}

bool
n00bb_resolver_supports_locator(n00bb_resolver_t *resolver,
                                const char *operand,
                                core_string_t *unsupported_scheme)
{
    if (unsupported_scheme) *unsupported_scheme = (core_string_t){0};
    if (!resolver || !operand) return false;
    core_string_t scheme = locator_scheme(operand);
    if (!scheme.data) return true;
    if (unsupported_scheme) *unsupported_scheme = scheme;
    return false;
}

static void
set_file_operand_origin(const n00bb_resolver_t *resolver, const char *operand,
                        core_source_file_t *source)
{
    char *canonical = canonical_path(operand);
    char *directory = canonical ? parent_path(canonical) : nullptr;
    const n00bb_root_t *owner = nullptr;
    for (size_t i = 0; directory && !owner && i < resolver->roots.len; i++)
        if (!strcmp(resolver->roots.data[i].path.data, directory))
            owner = &resolver->roots.data[i];
    if (owner)
        set_source_origin(source, owner->path, (core_string_t){0},
                          owner->is_default_package);
    else set_source_origin(source, (core_string_t){0}, (core_string_t){0},
                           true);
    core_free(directory);
    core_free(canonical);
}

static bool
is_file_operand(const char *operand)
{
    if (has_path_separator(operand) || source_suffix_length(operand)) return true;
    FILE *file = open_source(operand);
    if (!file) return false;
    fclose(file);
    return true;
}

bool
n00bb_operand_module_spec(const char *operand, core_module_spec_t *out)
{
    if (!operand || !*operand || !out) return false;
    const char *name = operand;
    if (is_file_operand(operand)) {
        const char *slash = strrchr(name, '/');
        const char *backslash = strrchr(name, '\\');
        if (!slash || (backslash && backslash > slash)) slash = backslash;
        name = slash ? slash + 1 : name;
        size_t len = strlen(name);
        len -= source_suffix_length(name);
        if (!th_name_syntax_valid(name, len)) return false;
        return core_module_spec_init(out, core_string_borrow(name, len));
    }
    size_t len = strlen(operand);
    return th_name_syntax_valid(operand, len)
        && core_module_spec_init(out, core_string_borrow(operand, len));
}

static core_module_resolve_status_t
read_source_path(const char *path, core_source_file_t *out)
{
    FILE *file = open_source(path);
    if (!file) {
        return errno == ENOENT || errno == ENOTDIR ? CORE_MODULE_NOT_FOUND
                                                  : CORE_MODULE_RESOLVE_ERROR;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return CORE_MODULE_RESOLVE_ERROR;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return CORE_MODULE_RESOLVE_ERROR;
    }
    char *contents = core_alloc_size(1, (size_t)length + 1);
    size_t read = fread(contents, 1, (size_t)length, file);
    bool read_error = ferror(file) || read != (size_t)length;
    fclose(file);
    if (read_error) {
        core_free(contents);
        return CORE_MODULE_RESOLVE_ERROR;
    }
    char *physical = canonical_path(path);
    core_string_t resolved_path =
        core_string_from_cstr(physical ? physical : path);
    bool initialized = core_source_file_init(out, resolved_path, contents,
                                            (size_t)length);
    core_free(resolved_path.data);
    core_free(physical);
    core_free(contents);
    return initialized ? CORE_MODULE_RESOLVED : CORE_MODULE_RESOLVE_ERROR;
}

static void
free_source_origin(void *data)
{
    n00bb_source_origin_t *origin = data;
    if (!origin) return;
    core_free(origin->package_id.data);
    core_free(origin->package_namespace.data);
    core_free(origin);
}

static void
set_source_origin(core_source_file_t *source, core_string_t package_id,
                  core_string_t package_namespace, bool is_default_package)
{
    n00bb_source_origin_t *origin = core_alloc(n00bb_source_origin_t);
    origin->package_id = core_string_dup(package_id);
    origin->package_namespace = core_string_dup(package_namespace);
    origin->is_default_package = is_default_package;
    core_source_file_set_user_data(source, origin, free_source_origin);
}

void
n00bb_source_file_set_package(core_source_file_t *source,
                              const char *package_id)
{
    core_string_t id = core_string_from_cstr(package_id ? package_id : "");
    bool is_default = !package_id || !strcmp(package_id, "default");
    size_t base = id.u8_bytes;
    while (base && id.data[base - 1] != '/' && id.data[base - 1] != '\\')
        base--;
    core_string_t package_namespace = is_default ? (core_string_t){0}
        : core_string_borrow(id.data + base, id.u8_bytes - base);
    set_source_origin(source, id, package_namespace, is_default);
    core_free(id.data);
}

core_string_t
n00bb_package_namespace(const core_translation_unit_t *unit)
{
    const n00bb_source_origin_t *origin = unit
        ? core_source_file_user_data(&unit->source) : nullptr;
    return origin && !origin->is_default_package
        ? origin->package_namespace : (core_string_t){0};
}

bool
n00bb_same_package(const core_translation_unit_t *left,
                   const core_translation_unit_t *right)
{
    if (!left || !right) return false;
    const n00bb_source_origin_t *a =
        core_source_file_user_data(&left->source);
    const n00bb_source_origin_t *b =
        core_source_file_user_data(&right->source);
    if (!a || !b) return false;
    if (a->is_default_package != b->is_default_package) return false;
    if (!a->is_default_package
        && !core_string_eq(a->package_id, b->package_id)) return false;
    core_string_t left_name = left->module.identifier;
    core_string_t right_name = right->module.identifier;
    size_t left_root = 0, right_root = 0;
    while (left_root < left_name.u8_bytes
           && left_name.data[left_root] != '.') left_root++;
    while (right_root < right_name.u8_bytes
           && right_name.data[right_root] != '.') right_root++;
    if (left_root == left_name.u8_bytes
        || right_root == right_name.u8_bytes)
        return left_root == left_name.u8_bytes
            && right_root == right_name.u8_bytes;
    return left_root == right_root
        && !memcmp(left_name.data, right_name.data, left_root);
}

core_string_t
n00bb_package_id(const core_translation_unit_t *unit)
{
    const n00bb_source_origin_t *origin = unit
        ? core_source_file_user_data(&unit->source) : nullptr;
    if (!origin || origin->is_default_package)
        return CORE_STRING_STATIC("default");
    return origin->package_id;
}

static core_module_resolve_status_t
read_inventory_source(n00bb_resolver_t *resolver, size_t root_index,
                      core_string_t relative, core_source_file_t *out)
{
    const core_source_snapshot_t *snapshot = core_snapshot_inventory_find(
        resolver->inventory, root_index, relative);
    if (!snapshot) return CORE_MODULE_NOT_FOUND;
    return core_source_file_init(out, core_source_snapshot_path(snapshot),
                                 core_source_snapshot_contents(snapshot),
                                 core_source_snapshot_size(snapshot))
        ? CORE_MODULE_RESOLVED : CORE_MODULE_RESOLVE_ERROR;
}

static core_module_resolve_status_t
resolve_file(void *context, const core_module_spec_t *module,
             core_source_file_t *out)
{
    n00bb_resolver_t *resolver = context;
    char *relative = module_relative_path(module);
    if (!relative) return CORE_MODULE_RESOLVE_ERROR;

    for (size_t i = 0; i < resolver->roots.len; i++) {
        n00bb_root_t *root = &resolver->roots.data[i];
        static const char *extensions[] = {".n", ".n00b"};
        for (size_t e = 0; e < sizeof(extensions) / sizeof(extensions[0]); e++) {
            core_buffer_t path = {0};
            core_buffer_printf(&path, "%s%s", relative, extensions[e]);
            char *candidate = join_path(resolver->roots.data[i].path.data,
                                        path.data);
            core_module_resolve_status_t status = resolver->manifests_indexed
                ? read_inventory_source(resolver, i,
                    core_string_borrow(path.data, path.byte_len), out)
                : read_source_path(candidate, out);
            core_buffer_release(&path);
            core_free(candidate);
            if (status == CORE_MODULE_NOT_FOUND) continue;
            if (status == CORE_MODULE_RESOLVED) {
                core_string_t name = module->identifier;
                const char *separator = memchr(name.data, '.', name.u8_bytes);
                if (separator) {
                    core_buffer_t package = {0};
                    core_buffer_printf(&package, "%.*s/%.*s",
                        (int)root->path.u8_bytes, root->path.data,
                        (int)(separator - name.data), name.data);
                    set_source_origin(out,
                        core_string_borrow(package.data, package.byte_len),
                        core_string_borrow(name.data,
                            (size_t)(separator - name.data)),
                        false);
                    core_buffer_release(&package);
                }
                else set_source_origin(out, root->path, (core_string_t){0},
                                       root->is_default_package);
            }
            core_free(relative);
            return status;
        }
    }
    core_free(relative);
    return CORE_MODULE_NOT_FOUND;
}

static bool
snapshot_may_contain_package(const core_source_snapshot_t *snapshot)
{
    const char *bytes = core_source_snapshot_contents(snapshot);
    size_t size = core_source_snapshot_size(snapshot);
    static const char keyword[] = "package";
    for (size_t i = 0; i + sizeof(keyword) - 1 <= size; i++)
        if (!memcmp(bytes + i, keyword, sizeof(keyword) - 1))
            return true;
    return false;
}

static bool
index_manifest_module(void *opaque, size_t root_index,
                      core_string_t relative,
                      const core_source_snapshot_t *snapshot)
{
    n00bb_resolver_t *resolver = opaque;
    if (!snapshot_may_contain_package(snapshot)) return true;
    size_t suffix = relative.u8_bytes >= 2
        && !memcmp(relative.data + relative.u8_bytes - 2, ".n", 2)
        ? 2 : 5;
    core_buffer_t identifier = {0};
    for (size_t i = 0; i < relative.u8_bytes - suffix; i++)
        core_buffer_putc(&identifier,
            relative.data[i] == '/' || relative.data[i] == '\\'
                ? '.' : relative.data[i]);
    core_module_spec_t module = {0};
    bool initialized = identifier.byte_len
        && core_module_spec_init(&module,
            core_string_borrow(identifier.data, identifier.byte_len));
    core_buffer_release(&identifier);
    if (!initialized) return false;
    core_string_t path = core_source_snapshot_path(snapshot);
    for (size_t i = 0; i < resolver->manifests.len; i++)
        if (core_string_eq(resolver->manifests.data[i].path, path)) {
            core_module_spec_release(&module);
            return true;
        }
    core_string_t owned_path = core_string_dup(path);
    core_string_t owned_relative = core_string_dup(relative);
    if (!owned_path.data || !owned_relative.data) {
        core_free(owned_path.data);
        core_free(owned_relative.data);
        core_module_spec_release(&module);
        return false;
    }
    core_list_push(resolver->manifests,
        ((n00bb_manifest_candidate_t){.path = owned_path,
                                      .relative = owned_relative,
                                      .root_index = root_index,
                                      .module = module}));
    return true;
}

bool
n00bb_resolver_prepare_packages(n00bb_resolver_t *resolver)
{
    if (!resolver || !core_snapshot_inventory_refresh(resolver->inventory))
        return false;
    uint64_t generation = core_snapshot_inventory_generation(
        resolver->inventory);
    if (resolver->manifests_indexed
        && resolver->manifests_generation == generation) return true;
    for (size_t i = 0; i < resolver->manifests.len; i++) {
        core_free(resolver->manifests.data[i].path.data);
        core_free(resolver->manifests.data[i].relative.data);
        core_module_spec_release(&resolver->manifests.data[i].module);
    }
    resolver->manifests.len = 0;
    resolver->manifests_indexed = false;
    if (!core_snapshot_inventory_visit(resolver->inventory,
            index_manifest_module, resolver)) return false;
    resolver->manifests_generation = generation;
    resolver->manifests_indexed = true;
    return true;
}

static bool
syntax_index_has_name(const core_buffer_t *names, core_string_t wanted)
{
    size_t start = 0;
    while (start < names->byte_len) {
        const char *end = memchr(names->data + start, '\n',
                                 names->byte_len - start);
        if (!end) return false;
        size_t length = (size_t)(end - (names->data + start));
        if (length == wanted.u8_bytes
            && !memcmp(names->data + start, wanted.data, length))
            return true;
        start += length + 1;
    }
    return false;
}

void
n00bb_resolver_store_package_syntax(n00bb_resolver_t *resolver,
                                    core_string_t source_path,
                                    const void *names, size_t size)
{
    if (!resolver || !resolver->package_syntax_profile || (!names && size))
        return;
    for (size_t i = 0; i < resolver->manifests.len; i++) {
        n00bb_manifest_candidate_t *candidate = &resolver->manifests.data[i];
        if (!core_string_eq(candidate->path, source_path)) continue;
        core_buffer_t cached = {0};
        bool exists = core_snapshot_inventory_product_read(
            resolver->inventory, candidate->root_index, candidate->relative,
            PACKAGE_SYNTAX_KIND, resolver->package_syntax_profile, &cached);
        core_buffer_release(&cached);
        if (!exists)
            (void)core_snapshot_inventory_product_write(
                resolver->inventory, candidate->root_index,
                candidate->relative, PACKAGE_SYNTAX_KIND,
                resolver->package_syntax_profile, names, size);
        return;
    }
}

static core_module_resolve_status_t
discover_files(void *context, const core_module_spec_t *module,
               core_module_discovery_emit_fn emit, void *emit_context)
{
    n00bb_resolver_t *resolver = context;
    if (!resolver || !module || !emit) return CORE_MODULE_RESOLVE_ERROR;
    if (!resolver->manifests_indexed
        && !n00bb_resolver_prepare_packages(resolver))
        return CORE_MODULE_RESOLVE_ERROR;
    bool found = false;
    for (size_t i = 0; i < resolver->manifests.len; i++) {
        n00bb_manifest_candidate_t *candidate = &resolver->manifests.data[i];
        if (resolver->package_syntax_profile) {
            core_buffer_t names = {0};
            bool cached = core_snapshot_inventory_product_read(
                resolver->inventory, candidate->root_index,
                candidate->relative, PACKAGE_SYNTAX_KIND,
                resolver->package_syntax_profile, &names);
            bool matches = !cached || syntax_index_has_name(
                &names, module->identifier);
            core_buffer_release(&names);
            if (!matches) continue;
        }
        found = true;
        if (!emit(emit_context, &candidate->module))
            return CORE_MODULE_RESOLVE_ERROR;
    }
    return found ? CORE_MODULE_RESOLVED : CORE_MODULE_NOT_FOUND;
}

bool
n00bb_resolver_has_source_module(n00bb_resolver_t *resolver,
                                 core_string_t identifier)
{
    core_module_spec_t module = {0};
    core_source_file_t source = {0};
    bool found = core_module_spec_init(&module, identifier)
        && resolve_file(resolver, &module, &source) == CORE_MODULE_RESOLVED;
    core_source_file_release(&source);
    core_module_spec_release(&module);
    return found;
}

bool
n00bb_resolver_internal_import(n00bb_resolver_t *resolver,
                               const core_translation_unit_t *importer,
                               core_string_t name,
                               core_module_spec_t *resolved)
{
    if (!resolver || !importer || !resolved) return false;
    const n00bb_source_origin_t *origin =
        core_source_file_user_data(&importer->source);
    if (!origin || origin->is_default_package) return false;
    core_module_spec_t requested = {0};
    if (!core_module_spec_init(&requested, name)) return false;
    char *relative = module_relative_path(&requested);
    core_module_spec_release(&requested);
    if (!relative) return false;
    bool found = false;
    static const char *extensions[] = {".n", ".n00b"};
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        core_buffer_t path = {0};
        core_buffer_printf(&path, "%s%s", relative, extensions[i]);
        char *candidate = join_path(origin->package_id.data, path.data);
        struct stat info = {0};
        found = candidate && !stat(candidate, &info) && S_ISREG(info.st_mode);
        core_free(candidate);
        core_buffer_release(&path);
        if (found) break;
    }
    core_free(relative);
    if (!found) return false;
    core_string_t package_namespace = n00bb_package_namespace(importer);
    core_buffer_t canonical = {0};
    core_buffer_printf(&canonical, "%.*s%s%.*s",
                       (int)package_namespace.u8_bytes, package_namespace.data,
                       package_namespace.u8_bytes ? "." : "",
                       (int)name.u8_bytes, name.data);
    bool ok = core_module_spec_init(resolved,
        core_string_borrow(canonical.data, canonical.byte_len));
    core_buffer_release(&canonical);
    return ok;
}

core_module_resolve_status_t
n00bb_locate_operand(n00bb_resolver_t *resolver, const char *operand,
                    core_module_spec_t *module_out,
                    core_source_file_t *source_out)
{
    if (!n00bb_resolver_supports_locator(resolver, operand, nullptr))
        return CORE_MODULE_RESOLVE_ERROR;
    if (!resolver || !module_out || !source_out
        || !n00bb_operand_module_spec(operand, module_out))
        return CORE_MODULE_RESOLVE_ERROR;
    *source_out = (core_source_file_t){0};
    core_module_resolve_status_t generated = read_generated_source(resolver, operand, source_out);
    if (generated != CORE_MODULE_NOT_FOUND) return generated;
    if (is_file_operand(operand)) {
        core_module_resolve_status_t status =
            read_source_path(operand, source_out);
        if (status == CORE_MODULE_RESOLVED)
            set_file_operand_origin(resolver, operand, source_out);
        return status;
    }
    core_module_resolver_t interface = n00bb_resolver_interface(resolver);
    return core_module_resolve(&interface, module_out, source_out);
}

core_string_t
n00bb_resolver_search_description(n00bb_resolver_t *resolver,
                                 const char *operand)
{
    core_buffer_t description = {0};
    if (!resolver || !operand) return (core_string_t){0};
    core_string_t scheme = {0};
    if (!n00bb_resolver_supports_locator(resolver, operand, &scheme)) {
        core_buffer_printf(&description,
            "  unsupported source locator scheme: %.*s",
            (int)scheme.u8_bytes, scheme.data);
        return core_string_from_raw(description.data,
                                    (int64_t)description.byte_len);
    }
    if (is_file_operand(operand)) {
        core_buffer_printf(&description, "  checked: %s", operand);
    }
    else {
        core_module_spec_t module = {0};
        char *relative = nullptr;
        if (n00bb_operand_module_spec(operand, &module))
            relative = module_relative_path(&module);
        if (relative) {
            for (size_t i = 0; i < resolver->roots.len; i++) {
                static const char *extensions[] = {".n", ".n00b"};
                for (size_t e = 0; e < sizeof(extensions) / sizeof(extensions[0]); e++) {
                    core_buffer_t path = {0};
                    core_buffer_printf(&path, "%s%s", relative, extensions[e]);
                    char *candidate = join_path(resolver->roots.data[i].path.data,
                                                path.data);
                    core_buffer_release(&path);
                    core_buffer_printf(&description, "%s  checked: %s",
                        description.byte_len ? "\n" : "", candidate);
                    core_free(candidate);
                }
            }
        }
        core_free(relative);
        core_module_spec_release(&module);
    }
    return core_string_from_raw(description.data,
                                (int64_t)description.byte_len);
}

char *
n00bb_grammar_path(const char *argv0)
{
#ifdef N00BB_GRAMMAR_PATH
    FILE *source = fopen(N00BB_GRAMMAR_PATH, "rb");
    if (source) {
        fclose(source);
        return core_strdup(N00BB_GRAMMAR_PATH);
    }
#endif
    char *exe = n00bb_executable_path(argv0);
    if (!exe) return nullptr;
    char *bin_dir = parent_path(exe);
    char *prefix = parent_path(bin_dir);
    char *candidate = join_path(prefix, "share/n00bb/n00b.bnf");
    FILE *installed = fopen(candidate, "rb");
    core_free(prefix);
    core_free(bin_dir);
    core_free(exe);
    if (!installed) {
        core_free(candidate);
        return nullptr;
    }
    fclose(installed);
    return candidate;
}

static uint64_t
package_syntax_profile(const char *argv0)
{
    char *grammar_path = n00bb_grammar_path(argv0);
    char *executable_path = n00bb_executable_path(argv0);
    FILE *grammar = grammar_path ? fopen(grammar_path, "rb") : nullptr;
    struct stat executable = {0};
    if (!grammar || !executable_path || stat(executable_path, &executable)) {
        if (grammar) fclose(grammar);
        core_free(grammar_path);
        core_free(executable_path);
        return 0;
    }
    core_buffer_t fingerprint = {0};
#ifdef __APPLE__
    long modified_ns = executable.st_mtimespec.tv_nsec;
    long changed_ns = executable.st_ctimespec.tv_nsec;
#elif defined(_WIN32)
    long modified_ns = 0, changed_ns = 0;
#else
    long modified_ns = executable.st_mtim.tv_nsec;
    long changed_ns = executable.st_ctim.tv_nsec;
#endif
    core_buffer_printf(&fingerprint,
        "package-syntax-v1:%llu:%llu:%lld:%lld.%ld:%lld.%ld:",
        (unsigned long long)executable.st_dev,
        (unsigned long long)executable.st_ino,
        (long long)executable.st_size,
        (long long)executable.st_mtime, modified_ns,
        (long long)executable.st_ctime, changed_ns);
    char chunk[8192];
    size_t count;
    while ((count = fread(chunk, 1, sizeof chunk, grammar)) != 0)
        core_buffer_append(&fingerprint, chunk, count);
    bool ok = !ferror(grammar);
    fclose(grammar);
    core_free(grammar_path);
    core_free(executable_path);
    uint64_t profile = ok
        ? core_xxh3_64bits_raw(fingerprint.data, fingerprint.byte_len) : 0;
    core_buffer_release(&fingerprint);
    return profile ? profile : (ok ? 1 : 0);
}

core_module_resolver_t
n00bb_resolver_interface(n00bb_resolver_t *resolver)
{
    return (core_module_resolver_t){.resolve = resolve_file,
                                    .discover = discover_files,
                                    .context = resolver};
}
