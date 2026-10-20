#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1

#include "driver.h"
#include "cache_location.h"

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
    bool interface_module;
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

static const core_string_t source_extensions[] = {
    CORE_STRING_STATIC(".n"),
    CORE_STRING_STATIC(".n00b"),
};

static core_string_t join_path(core_string_t a, core_string_t b);
static size_t source_suffix_length(core_string_t path);
static uint64_t package_syntax_profile(core_string_t argv0);
static void set_source_origin(core_source_file_t *source,
                              core_string_t package_id,
                              core_string_t package_namespace,
                              bool is_default_package);
#define PACKAGE_SYNTAX_KIND CORE_STRING_STATIC("n00bb.package-syntax.v1")
static bool
accept_source_path(void *context, core_string_t relative)
{
    (void)context;
    return core_string_ends_with(relative, CORE_STRING_STATIC(".n"))
        || core_string_ends_with(relative, CORE_STRING_STATIC(".n00b"));
}

static bool
descend_source_directory(void *context, core_string_t absolute)
{
    (void)context;
    core_string_t marker = join_path(absolute,
                                     CORE_STRING_STATIC("meson-private"));
    char *native = core_string_to_cstr(marker);
    struct stat info = {0};
    bool build_tree = native && !stat(native, &info) && S_ISDIR(info.st_mode);
    core_free(native);
    core_free(marker.data);
    return !build_tree;
}
#ifdef _WIN32
static core_string_t wide_to_utf8(const wchar_t *wide, int length);
static wchar_t *utf8_to_wide(core_string_t utf8);
#endif

#ifdef _WIN32
static core_string_t
wide_to_utf8(const wchar_t *wide, int length)
{
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0,
                                   nullptr, nullptr);
    if (bytes <= 0) return (core_string_t){0};
    char *utf8 = core_alloc_size(1, (size_t)bytes + 1);
    WideCharToMultiByte(CP_UTF8, 0, wide, length, utf8, bytes, nullptr, nullptr);
    utf8[bytes] = '\0';
    size_t used = length < 0 ? (size_t)bytes - 1 : (size_t)bytes;
    return (core_string_t){.data = utf8, .u8_bytes = used};
}

static wchar_t *
utf8_to_wide(core_string_t utf8)
{
    if (!utf8.data || utf8.u8_bytes > INT32_MAX) return nullptr;
    int chars = utf8.u8_bytes
        ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data,
                              (int)utf8.u8_bytes, nullptr, 0)
        : 0;
    if (chars < 0 || (utf8.u8_bytes && !chars)) return nullptr;
    wchar_t *wide = core_alloc_array(wchar_t, (size_t)chars + 1);
    if (chars)
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data,
                            (int)utf8.u8_bytes, wide, chars);
    wide[chars] = 0;
    return wide;
}
#endif

static bool
is_path_separator(char c)
{
    return c == '/' || c == '\\';
}

static bool
path_is_absolute(core_string_t path)
{
#ifdef _WIN32
    return (path.u8_bytes && is_path_separator(path.data[0]))
        || (path.u8_bytes >= 3 && path.data[1] == ':'
            && is_path_separator(path.data[2]));
#else
    return path.u8_bytes && path.data[0] == '/';
#endif
}

static core_string_t
canonical_path(core_string_t path)
{
#ifdef _WIN32
    wchar_t *wide = utf8_to_wide(path);
    if (!wide) return (core_string_t){0};
    wchar_t *full = _wfullpath(nullptr, wide, 0);
    core_free(wide);
    if (!full) return (core_string_t){0};
    core_string_t result = wide_to_utf8(full, -1);
    free(full);
    return result;
#else
    char *native = core_string_to_cstr(path);
    char *resolved = native ? realpath(native, nullptr) : nullptr;
    core_free(native);
    core_string_t result = core_string_from_cstr(resolved);
    free(resolved);
    return result;
#endif
}

static FILE *
open_source(core_string_t path)
{
#ifdef _WIN32
    wchar_t *wide = utf8_to_wide(path);
    if (!wide) return nullptr;
    FILE *file = _wfopen(wide, L"rb");
    core_free(wide);
    return file;
#else
    char *native = core_string_to_cstr(path);
    FILE *file = native ? fopen(native, "rb") : nullptr;
    int saved = errno;
    core_free(native);
    errno = saved;
    return file;
#endif
}

static core_string_t
join_path(core_string_t a, core_string_t b)
{
    core_buffer_t *out = core_buffer_empty();
    core_buffer_append_string(out, a);
    if (out->byte_len && !is_path_separator(out->data[out->byte_len - 1]))
        core_buffer_putc(out, N00BB_PATH_SEP);
    core_buffer_append_string(out, b);
    return core_buffer_take_string(out);
}

static core_string_t
parent_path(core_string_t path)
{
    size_t len = path.u8_bytes;
    while (len > 1 && is_path_separator(path.data[len - 1])) len--;
#ifdef _WIN32
    if (len == 2 && path.data[1] == ':') {
        core_buffer_t *drive = core_buffer_empty();
        core_buffer_append(drive, path.data, 2);
        core_buffer_putc(drive, '\\');
        return core_buffer_take_string(drive);
    }
#endif
    core_string_t trimmed = core_string_slice(path, 0, len);
    int64_t slash = core_string_rfind_byte(trimmed, '/');
    int64_t backslash = core_string_rfind_byte(trimmed, '\\');
    if (slash < 0 || (backslash >= 0 && backslash > slash)) slash = backslash;
    if (slash < 0) return core_string_dup(CORE_STRING_STATIC("."));
    size_t keep = slash == 0 ? 1 : (size_t)slash;
#ifdef _WIN32
    if (slash == 2 && path.data[1] == ':') keep = 3;
#endif
    return core_string_from_raw(path.data, (int64_t)keep);
}

static core_string_t
cwd_path(void)
{
#ifdef _WIN32
    DWORD cap = GetCurrentDirectoryW(0, nullptr);
    wchar_t *wide = core_alloc_array(wchar_t, cap);
    DWORD n = GetCurrentDirectoryW(cap, wide);
    core_string_t path = n && n < cap ? wide_to_utf8(wide, (int)n)
                                      : (core_string_t){0};
    core_free(wide);
    return path;
#else
    char *native = getcwd(nullptr, 0);
    core_string_t path = core_string_from_cstr(native);
    free(native);
    return path;
#endif
}

core_string_t
n00bb_executable_path(core_string_t argv0)
{
#ifdef _WIN32
    DWORD cap = 256;
    wchar_t *wide = nullptr;
    for (;;) {
        wide = core_realloc(wide, cap * sizeof(*wide));
        DWORD n = GetModuleFileNameW(nullptr, wide, cap);
        if (!n) break;
        if (n < cap - 1) {
            core_string_t path = wide_to_utf8(wide, (int)n);
            core_free(wide);
            return path;
        }
        cap *= 2;
    }
    core_free(wide);
    return argv0.data ? canonical_path(argv0) : (core_string_t){0};
#elif defined(__APPLE__)
    uint32_t cap = 0;
    _NSGetExecutablePath(nullptr, &cap);
    char *path = core_alloc_size(1, cap + 1);
    if (_NSGetExecutablePath(path, &cap) == 0) {
        char *resolved = realpath(path, nullptr);
        core_string_t result = core_string_from_cstr(resolved ? resolved
                                                              : path);
        free(resolved);
        core_free(path);
        return result;
    }
    core_free(path);
    return argv0.data ? canonical_path(argv0) : (core_string_t){0};
#elif defined(__linux__)
    size_t cap = 256;
    char *path = nullptr;
    for (;;) {
        path = core_realloc(path, cap + 1);
        ssize_t n = readlink("/proc/self/exe", path, cap);
        if (n < 0) break;
        if ((size_t)n < cap) {
            core_string_t result = core_string_from_raw(path, (int64_t)n);
            core_free(path);
            return result;
        }
        cap *= 2;
    }
    core_free(path);
    return argv0.data ? canonical_path(argv0) : (core_string_t){0};
#else
    return argv0.data ? canonical_path(argv0) : (core_string_t){0};
#endif
}

static void
add_root(n00bb_resolver_t *resolver, core_string_t path,
         bool is_default_package)
{
    if (!path.data || !path.u8_bytes) return;
    core_string_t canonical = canonical_path(path);
    n00bb_root_t root = {
        .path = canonical.data ? canonical : core_string_dup(path),
        .is_default_package = is_default_package,
    };
    core_list_push(resolver->roots, root);
}

static void
add_environment_root(n00bb_resolver_t *resolver, core_string_t cwd,
                     core_string_t entry)
{
    core_string_t absolute = path_is_absolute(entry) ? core_string_dup(entry)
                                                     : join_path(cwd, entry);
    add_root(resolver, absolute, false);
    core_free(absolute.data);
}

static void
add_environment_roots(n00bb_resolver_t *resolver, core_string_t cwd)
{
#ifdef _WIN32
    DWORD cap = GetEnvironmentVariableW(L"N00B_PATH", nullptr, 0);
    if (!cap) return;
    wchar_t *wide = core_alloc_array(wchar_t, cap);
    DWORD n = GetEnvironmentVariableW(L"N00B_PATH", wide, cap);
    core_string_t value = n && n < cap ? wide_to_utf8(wide, (int)n)
                                       : (core_string_t){0};
    core_free(wide);
#else
    core_string_t value = core_string_from_cstr(getenv("N00B_PATH"));
#endif
    if (!value.data) return;
    size_t start = 0;
    for (size_t i = 0; i <= value.u8_bytes; i++) {
        if (i < value.u8_bytes && value.data[i] != N00BB_LIST_SEP) continue;
        if (i > start)
            add_environment_root(resolver, cwd,
                core_string_slice(value, start, i - start));
        start = i + 1;
    }
    core_free(value.data);
}

static void
add_executable_library_roots(n00bb_resolver_t *resolver, core_string_t argv0)
{
    core_string_t exe = n00bb_executable_path(argv0);
    if (!exe.data) return;
    core_string_t dir = parent_path(exe);
    for (;;) {
        core_string_t n00b = join_path(dir, CORE_STRING_STATIC("n00b"));
        core_string_t lib = join_path(n00b, CORE_STRING_STATIC("lib"));
        add_root(resolver, lib, false);
        core_free(lib.data);
        core_free(n00b.data);
        core_string_t parent = parent_path(dir);
        if (core_string_eq(parent, dir)) {
            core_free(parent.data);
            break;
        }
        core_free(dir.data);
        dir = parent;
    }
    core_free(dir.data);
    core_free(exe.data);
}

static void
add_bundled_library_root(n00bb_resolver_t *resolver, core_string_t argv0)
{
    core_string_t exe = n00bb_executable_path(argv0);
    if (!exe.data) return;
    core_string_t bin = parent_path(exe);
    core_string_t prefix = parent_path(bin);
    core_string_t lib = join_path(prefix, CORE_STRING_STATIC("lib"));
    add_root(resolver, lib, false);
    core_free(lib.data);
    core_free(prefix.data);
    core_free(bin.data);
    core_free(exe.data);
}

n00bb_resolver_t *
n00bb_resolver_new(core_string_t argv0)
{
    core_string_t cwd = cwd_path();
    if (!cwd.data) return nullptr;
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
    core_snapshot_inventory_set_descend(resolver->inventory,
                                        descend_source_directory);
    if (resolver->inventory && !getenv("N00BB_NO_PACKAGE_CACHE")) {
        core_string_t packages = n00bb_cache_directory(
            CORE_STRING_STATIC("packages"));
        if (packages.data && core_snapshot_inventory_enable_cache(
                resolver->inventory, packages, 2))
            resolver->package_syntax_profile = package_syntax_profile(argv0);
        core_free(packages.data);
    }
    core_free(roots);
    core_free(cwd.data);
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
    core_string_t prefix = CORE_STRING_STATIC("<generated:");
    if (path.u8_bytes != prefix.u8_bytes + module->identifier.u8_bytes + 1
        || !core_string_starts_with(path, prefix)
        || !core_string_eq(core_string_slice(path, prefix.u8_bytes,
                                             module->identifier.u8_bytes),
                           module->identifier)) return nullptr;
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
read_generated_source(n00bb_resolver_t *resolver, core_string_t operand, core_source_file_t *source_out)
{
    core_module_resolve_status_t status = CORE_MODULE_NOT_FOUND;
    pthread_mutex_lock(&resolver->generated_lock);
    for (size_t i = 0; i < resolver->generated.len; i++) {
        n00bb_generated_module_t *module = &resolver->generated.data[i];
        if (!core_string_eq(operand, module->identifier)) continue;
        core_buffer_t path = {0};
        core_buffer_puts(&path, "<generated:");
        core_buffer_append_string(&path, operand);
        core_buffer_putc(&path, '>');
        bool ok = core_source_file_init(source_out, core_buffer_string(&path),
                                        module->text);
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

static core_string_t
module_relative_path(const core_module_spec_t *module)
{
    core_string_t id = module->identifier;
    if (!id.data || !id.u8_bytes || memchr(id.data, '\0', id.u8_bytes))
        return (core_string_t){0};
    core_buffer_t *relative = core_buffer_empty();
    size_t segment_start = 0;
    for (size_t i = 0; i <= id.u8_bytes; i++) {
        if (i < id.u8_bytes && id.data[i] != '.') {
            if (id.data[i] == '/' || id.data[i] == '\\') {
                core_buffer_free(relative);
                return (core_string_t){0};
            }
#ifdef _WIN32
            if (id.data[i] == ':') {
                core_buffer_free(relative);
                return (core_string_t){0};
            }
#endif
            continue;
        }
        size_t segment_len = i - segment_start;
        if (!segment_len || (segment_len == 1 && id.data[segment_start] == '.')
            || (segment_len == 2 && id.data[segment_start] == '.'
                && id.data[segment_start + 1] == '.')) {
            core_buffer_free(relative);
            return (core_string_t){0};
        }
        if (segment_start) core_buffer_putc(relative, N00BB_PATH_SEP);
        core_buffer_append(relative, id.data + segment_start, segment_len);
        segment_start = i + 1;
    }
    return core_buffer_take_string(relative);
}

static bool
has_path_separator(core_string_t path)
{
    return core_string_find_byte(path, '/') >= 0
        || core_string_find_byte(path, '\\') >= 0;
}

static size_t
source_suffix_length(core_string_t path)
{
    if (core_string_ends_with(path, CORE_STRING_STATIC(".n"))) return 2;
    if (core_string_ends_with(path, CORE_STRING_STATIC(".n00b"))) return 5;
    return 0;
}

static bool
scheme_letter(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static core_string_t
locator_scheme(core_string_t operand)
{
    if (!operand.data || !operand.u8_bytes || !scheme_letter(operand.data[0]))
        return (core_string_t){0};
    size_t i = 1;
    while (i < operand.u8_bytes
           && (scheme_letter(operand.data[i])
               || (operand.data[i] >= '0' && operand.data[i] <= '9')
               || operand.data[i] == '+' || operand.data[i] == '-'
               || operand.data[i] == '.')) i++;
    return i + 2 < operand.u8_bytes && operand.data[i] == ':'
            && operand.data[i + 1] == '/' && operand.data[i + 2] == '/'
        ? core_string_slice(operand, 0, i) : (core_string_t){0};
}

bool
n00bb_resolver_supports_locator(n00bb_resolver_t *resolver,
                                core_string_t operand,
                                core_string_t *unsupported_scheme)
{
    if (unsupported_scheme) *unsupported_scheme = (core_string_t){0};
    if (!resolver || !operand.data) return false;
    core_string_t scheme = locator_scheme(operand);
    if (!scheme.data) return true;
    if (unsupported_scheme) *unsupported_scheme = scheme;
    return false;
}

static void
set_file_operand_origin(const n00bb_resolver_t *resolver, core_string_t operand,
                        core_source_file_t *source)
{
    core_string_t canonical = canonical_path(operand);
    core_string_t directory = canonical.data ? parent_path(canonical)
                                             : (core_string_t){0};
    const n00bb_root_t *owner = nullptr;
    for (size_t i = 0; directory.data && !owner && i < resolver->roots.len; i++)
        if (core_string_eq(resolver->roots.data[i].path, directory))
            owner = &resolver->roots.data[i];
    if (owner)
        set_source_origin(source, owner->path, (core_string_t){0},
                          owner->is_default_package);
    else set_source_origin(source, (core_string_t){0}, (core_string_t){0},
                           true);
    core_free(directory.data);
    core_free(canonical.data);
}

static bool
is_file_operand(core_string_t operand)
{
    if (has_path_separator(operand) || source_suffix_length(operand)) return true;
    FILE *file = open_source(operand);
    if (!file) return false;
    fclose(file);
    return true;
}

bool
n00bb_operand_module_spec(core_string_t operand, core_module_spec_t *out)
{
    if (!operand.data || !operand.u8_bytes || !out) return false;
    if (is_file_operand(operand)) {
        int64_t slash = core_string_rfind_byte(operand, '/');
        int64_t backslash = core_string_rfind_byte(operand, '\\');
        if (slash < 0 || (backslash >= 0 && backslash > slash))
            slash = backslash;
        size_t start = slash >= 0 ? (size_t)slash + 1 : 0;
        core_string_t name = core_string_slice(operand, start,
                                               operand.u8_bytes - start);
        name.u8_bytes -= source_suffix_length(name);
        if (!th_name_syntax_valid(name)) return false;
        return core_module_spec_init(out, name);
    }
    return th_name_syntax_valid(operand)
        && core_module_spec_init(out, operand);
}

static core_module_resolve_status_t
read_source_path(core_string_t path, core_source_file_t *out)
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
    core_string_t physical = canonical_path(path);
    bool initialized = core_source_file_init(out,
        physical.data ? physical : path,
        core_string_borrow(contents, (size_t)length));
    core_free(physical.data);
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
                              core_string_t package_id)
{
    core_string_t id = package_id.data ? package_id : CORE_STRING_STATIC("");
    bool is_default = !package_id.data
        || core_string_eq(package_id, CORE_STRING_STATIC("default"));
    size_t base = id.u8_bytes;
    while (base && id.data[base - 1] != '/' && id.data[base - 1] != '\\')
        base--;
    core_string_t package_namespace = is_default ? (core_string_t){0}
        : core_string_borrow(id.data + base, id.u8_bytes - base);
    set_source_origin(source, id, package_namespace, is_default);
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

static size_t
unit_root_index(const n00bb_resolver_t *resolver,
                const core_translation_unit_t *unit)
{
    const n00bb_source_origin_t *origin = unit
        ? core_source_file_user_data(&unit->source) : nullptr;
    if (!resolver || !origin) return SIZE_MAX;
    size_t best = SIZE_MAX;
    size_t best_length = 0;
    for (size_t i = 0; i < resolver->roots.len; i++) {
        const n00bb_root_t *root = &resolver->roots.data[i];
        if (origin->is_default_package) {
            if (root->is_default_package) return i;
            continue;
        }
        core_string_t id = origin->package_id;
        size_t length = root->path.u8_bytes;
        bool prefix = id.u8_bytes >= length
            && !memcmp(id.data, root->path.data, length)
            && (id.u8_bytes == length || is_path_separator(id.data[length]));
        if (prefix && (best == SIZE_MAX || length > best_length)) {
            best = i;
            best_length = length;
        }
    }
    return best;
}

bool
n00bb_resolver_same_root(const n00bb_resolver_t *resolver,
                         const core_translation_unit_t *left,
                         const core_translation_unit_t *right)
{
    size_t left_root = unit_root_index(resolver, left);
    return left_root != SIZE_MAX
        && left_root == unit_root_index(resolver, right);
}

void
n00bb_source_file_mark_interface(core_source_file_t *source,
                                 uint64_t source_hash)
{
    n00bb_source_origin_t *origin = source
        ? core_source_file_user_data(source) : nullptr;
    if (!origin) return;
    origin->interface_module = true;
    core_source_file_set_identity(source, source_hash);
}

bool
n00bb_source_file_is_interface(const core_source_file_t *source)
{
    const n00bb_source_origin_t *origin = source
        ? core_source_file_user_data(source) : nullptr;
    return origin && origin->interface_module;
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
    core_string_t contents = core_source_snapshot_contents(snapshot);
    return core_source_file_init(out, core_source_snapshot_path(snapshot),
                                 core_string_borrow(contents.data,
                                     core_source_snapshot_size(snapshot)))
        ? CORE_MODULE_RESOLVED : CORE_MODULE_RESOLVE_ERROR;
}

static core_module_resolve_status_t
resolve_file(void *context, const core_module_spec_t *module,
             core_source_file_t *out)
{
    n00bb_resolver_t *resolver = context;
    core_string_t relative = module_relative_path(module);
    if (!relative.data) return CORE_MODULE_RESOLVE_ERROR;

    for (size_t i = 0; i < resolver->roots.len; i++) {
        n00bb_root_t *root = &resolver->roots.data[i];
        for (size_t e = 0; e < sizeof(source_extensions) / sizeof(source_extensions[0]); e++) {
            core_buffer_t path = {0};
            core_buffer_append_string(&path, relative);
            core_buffer_append_string(&path, source_extensions[e]);
            core_string_t candidate = join_path(resolver->roots.data[i].path,
                                                core_buffer_string(&path));
            core_module_resolve_status_t status = resolver->manifests_indexed
                ? read_inventory_source(resolver, i,
                    core_buffer_string(&path), out)
                : read_source_path(candidate, out);
            core_buffer_release(&path);
            core_free(candidate.data);
            if (status == CORE_MODULE_NOT_FOUND) continue;
            if (status == CORE_MODULE_RESOLVED) {
                core_string_t name = module->identifier;
                int64_t separator = core_string_find_byte(name, '.');
                if (separator >= 0) {
                    core_buffer_t package = {0};
                    core_buffer_printf(&package, "%.*s/%.*s",
                        (int)root->path.u8_bytes, root->path.data,
                        (int)separator, name.data);
                    set_source_origin(out, core_buffer_string(&package),
                        core_string_slice(name, 0, (size_t)separator),
                        false);
                    core_buffer_release(&package);
                }
                else set_source_origin(out, root->path, (core_string_t){0},
                                       root->is_default_package);
            }
            core_free(relative.data);
            return status;
        }
    }
    core_free(relative.data);
    return CORE_MODULE_NOT_FOUND;
}

static bool
snapshot_may_contain_package(const core_source_snapshot_t *snapshot)
{
    core_string_t contents = core_source_snapshot_contents(snapshot);
    size_t size = core_source_snapshot_size(snapshot);
    static const char keyword[] = "package";
    for (size_t i = 0; i + sizeof(keyword) - 1 <= size; i++)
        if (!memcmp(contents.data + i, keyword, sizeof(keyword) - 1))
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
    size_t suffix = core_string_ends_with(relative, CORE_STRING_STATIC(".n"))
        ? 2 : 5;
    core_buffer_t identifier = {0};
    for (size_t i = 0; i < relative.u8_bytes - suffix; i++)
        core_buffer_putc(&identifier,
            relative.data[i] == '/' || relative.data[i] == '\\'
                ? '.' : relative.data[i]);
    core_module_spec_t module = {0};
    bool initialized = identifier.byte_len
        && core_module_spec_init(&module, core_buffer_string(&identifier));
    core_buffer_release(&identifier);
    if (!initialized) return false;
    core_string_t path = core_source_snapshot_path(snapshot);
    for (size_t i = 0; i < resolver->manifests.len; i++) {
        n00bb_manifest_candidate_t *known = &resolver->manifests.data[i];
        if (!core_string_eq(known->path, path)) continue;
        if (relative.u8_bytes >= known->relative.u8_bytes) {
            core_module_spec_release(&module);
            return true;
        }
        core_string_t nearer = core_string_dup(relative);
        if (!nearer.data) {
            core_module_spec_release(&module);
            return false;
        }
        core_free(known->relative.data);
        core_module_spec_release(&known->module);
        known->relative = nearer;
        known->root_index = root_index;
        known->module = module;
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

static bool
regular_file_exists(core_string_t path)
{
    char *native = core_string_to_cstr(path);
    struct stat info = {0};
    bool found = native && !stat(native, &info) && S_ISREG(info.st_mode);
    core_free(native);
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
    core_string_t relative = module_relative_path(&requested);
    core_module_spec_release(&requested);
    if (!relative.data) return false;
    bool found = false;
    for (size_t i = 0; i < sizeof(source_extensions) / sizeof(source_extensions[0]); i++) {
        core_buffer_t path = {0};
        core_buffer_append_string(&path, relative);
        core_buffer_append_string(&path, source_extensions[i]);
        core_string_t candidate = join_path(origin->package_id,
                                            core_buffer_string(&path));
        found = candidate.data && regular_file_exists(candidate);
        core_free(candidate.data);
        core_buffer_release(&path);
        if (found) break;
    }
    core_free(relative.data);
    if (!found) return false;
    core_string_t package_namespace = n00bb_package_namespace(importer);
    core_buffer_t canonical = {0};
    core_buffer_printf(&canonical, "%.*s%s%.*s",
                       (int)package_namespace.u8_bytes, package_namespace.data,
                       package_namespace.u8_bytes ? "." : "",
                       (int)name.u8_bytes, name.data);
    bool ok = core_module_spec_init(resolved, core_buffer_string(&canonical));
    core_buffer_release(&canonical);
    return ok;
}

core_module_resolve_status_t
n00bb_locate_operand(n00bb_resolver_t *resolver, core_string_t operand,
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
                                 core_string_t operand)
{
    core_buffer_t description = {0};
    if (!resolver || !operand.data) return (core_string_t){0};
    core_string_t scheme = {0};
    if (!n00bb_resolver_supports_locator(resolver, operand, &scheme)) {
        core_buffer_printf(&description,
            "  unsupported source locator scheme: %.*s",
            (int)scheme.u8_bytes, scheme.data);
        return core_string_from_raw(description.data,
                                    (int64_t)description.byte_len);
    }
    if (is_file_operand(operand)) {
        core_buffer_printf(&description, "  checked: %.*s",
                           (int)operand.u8_bytes, operand.data);
    }
    else {
        core_module_spec_t module = {0};
        core_string_t relative = {0};
        if (n00bb_operand_module_spec(operand, &module))
            relative = module_relative_path(&module);
        if (relative.data) {
            for (size_t i = 0; i < resolver->roots.len; i++) {
                for (size_t e = 0; e < sizeof(source_extensions) / sizeof(source_extensions[0]); e++) {
                    core_buffer_t path = {0};
                    core_buffer_append_string(&path, relative);
                    core_buffer_append_string(&path, source_extensions[e]);
                    core_string_t candidate = join_path(
                        resolver->roots.data[i].path,
                        core_buffer_string(&path));
                    core_buffer_release(&path);
                    core_buffer_printf(&description, "%s  checked: %.*s",
                        description.byte_len ? "\n" : "",
                        (int)candidate.u8_bytes, candidate.data);
                    core_free(candidate.data);
                }
            }
        }
        core_free(relative.data);
        core_module_spec_release(&module);
    }
    return core_string_from_raw(description.data,
                                (int64_t)description.byte_len);
}

static bool
readable_file(core_string_t path)
{
    char *native = core_string_to_cstr(path);
    FILE *file = native ? fopen(native, "rb") : nullptr;
    core_free(native);
    if (!file) return false;
    fclose(file);
    return true;
}

core_string_t
n00bb_grammar_path(core_string_t argv0)
{
#ifdef N00BB_GRAMMAR_PATH
    FILE *source = fopen(N00BB_GRAMMAR_PATH, "rb");
    if (source) {
        fclose(source);
        return core_string_dup(CORE_STRING_STATIC(N00BB_GRAMMAR_PATH));
    }
#endif
    core_string_t exe = n00bb_executable_path(argv0);
    if (!exe.data) return (core_string_t){0};
    core_string_t bin_dir = parent_path(exe);
    core_string_t prefix = parent_path(bin_dir);
    core_string_t candidate = join_path(prefix,
        CORE_STRING_STATIC("share/n00bb/n00b.bnf"));
    bool installed = readable_file(candidate);
    core_free(prefix.data);
    core_free(bin_dir.data);
    core_free(exe.data);
    if (!installed) {
        core_free(candidate.data);
        return (core_string_t){0};
    }
    return candidate;
}

static uint64_t
package_syntax_profile(core_string_t argv0)
{
    core_string_t grammar_path = n00bb_grammar_path(argv0);
    core_string_t executable_path = n00bb_executable_path(argv0);
    char *native_grammar = core_string_to_cstr(grammar_path);
    char *native_executable = core_string_to_cstr(executable_path);
    FILE *grammar = native_grammar ? fopen(native_grammar, "rb") : nullptr;
    struct stat executable = {0};
    bool missing = !grammar || !native_executable
        || stat(native_executable, &executable);
    core_free(native_grammar);
    core_free(native_executable);
    core_free(grammar_path.data);
    core_free(executable_path.data);
    if (missing) {
        if (grammar) fclose(grammar);
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
