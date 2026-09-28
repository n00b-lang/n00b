#define _XOPEN_SOURCE 700

#include "driver.h"

#include <errno.h>
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
#include "lib/list.h"

typedef struct {
    core_string_t path;
    bool is_default_package;
} n00bb_root_t;
core_list_decl(n00bb_root_t);

typedef struct {
    core_string_t package_id;
    bool is_default_package;
} n00bb_source_origin_t;

struct n00bb_resolver_t {
    core_list_t(n00bb_root_t) roots;
};

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

static char *
executable_path(const char *argv0)
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
    char *exe = executable_path(argv0);
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

n00bb_resolver_t *
n00bb_resolver_new(const char *argv0)
{
    char *cwd = cwd_path();
    if (!cwd) return nullptr;
    n00bb_resolver_t *resolver = core_alloc(n00bb_resolver_t);
    resolver->roots = core_list_new(n00bb_root_t);
    add_root(resolver, cwd, true);
    add_environment_roots(resolver, cwd);
    add_executable_library_roots(resolver, argv0);
    core_free(cwd);
    return resolver;
}

void
n00bb_resolver_free(n00bb_resolver_t *resolver)
{
    if (!resolver) return;
    for (size_t i = 0; i < resolver->roots.len; i++)
        core_free(resolver->roots.data[i].path.data);
    core_list_free(resolver->roots);
    core_free(resolver);
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
    core_buffer_puts(relative, ".n00b");
    return core_buffer_take(relative);
}

static bool
has_path_separator(const char *path)
{
    return strchr(path, '/') || strchr(path, '\\');
}

static bool
has_n00b_suffix(const char *path)
{
    size_t len = strlen(path);
    return len >= 5 && !strcmp(path + len - 5, ".n00b");
}

static bool
is_file_operand(const char *operand)
{
    if (has_path_separator(operand) || has_n00b_suffix(operand)) return true;
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
        if (len >= 5 && !strcmp(name + len - 5, ".n00b")) len -= 5;
        if (!len) return false;
        return core_module_spec_init(out, core_string_borrow(name, len));
    }
    return core_module_spec_init(out,
                                 core_string_borrow(operand, strlen(operand)));
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
    core_free(origin);
}

static void
set_source_origin(core_source_file_t *source, const char *package_id,
                  bool is_default_package)
{
    n00bb_source_origin_t *origin = core_alloc(n00bb_source_origin_t);
    origin->package_id = core_string_from_cstr(package_id ? package_id : "");
    origin->is_default_package = is_default_package;
    core_source_file_set_user_data(source, origin, free_source_origin);
}

void
n00bb_source_file_set_package(core_source_file_t *source,
                              const char *package_id)
{
    bool is_default = !package_id || !strcmp(package_id, "default");
    set_source_origin(source, package_id, is_default);
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
    if (a->is_default_package || b->is_default_package)
        return a->is_default_package && b->is_default_package;
    return core_string_eq(a->package_id, b->package_id);
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
resolve_file(void *context, const core_module_spec_t *module,
             core_source_file_t *out)
{
    n00bb_resolver_t *resolver = context;
    char *relative = module_relative_path(module);
    if (!relative) return CORE_MODULE_RESOLVE_ERROR;

    for (size_t i = 0; i < resolver->roots.len; i++) {
        char *candidate = join_path(resolver->roots.data[i].path.data, relative);
        core_module_resolve_status_t status = read_source_path(candidate, out);
        core_free(candidate);
        if (status == CORE_MODULE_NOT_FOUND) continue;
        if (status == CORE_MODULE_RESOLVED) {
            n00bb_root_t *root = &resolver->roots.data[i];
            set_source_origin(out, root->path.data,
                              root->is_default_package);
        }
        core_free(relative);
        return status;
    }
    core_free(relative);
    return CORE_MODULE_NOT_FOUND;
}

core_module_resolve_status_t
n00bb_locate_operand(n00bb_resolver_t *resolver, const char *operand,
                    core_module_spec_t *module_out,
                    core_source_file_t *source_out)
{
    if (!resolver || !module_out || !source_out
        || !n00bb_operand_module_spec(operand, module_out))
        return CORE_MODULE_RESOLVE_ERROR;
    *source_out = (core_source_file_t){0};
    if (is_file_operand(operand)) {
        core_module_resolve_status_t status =
            read_source_path(operand, source_out);
        if (status == CORE_MODULE_RESOLVED)
            set_source_origin(source_out, nullptr, true);
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
                char *candidate = join_path(resolver->roots.data[i].path.data,
                                            relative);
                core_buffer_printf(&description, "%s  checked: %s",
                    i ? "\n" : "", candidate);
                core_free(candidate);
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
    char *exe = executable_path(argv0);
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

core_module_resolver_t
n00bb_resolver_interface(n00bb_resolver_t *resolver)
{
    return (core_module_resolver_t){.resolve = resolve_file,
                                    .context = resolver};
}
