#include "toolchain.h"

#include "cache_location.h"
#include "config.h"
#include "n00bb_version.h"

#include "lib/alloc.h"
#include "lib/os.h"

#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef __APPLE__
#define N00BB_NATIVE_LINKER "/usr/bin/clang"
#else
#define N00BB_NATIVE_LINKER N00BB_LLVM_CLANG
#endif

static n00bb_toolchain_t toolchain;

#define under(root, relative) \
    core_os_path_join(root, CORE_STRING_STATIC(relative))

static core_string_t
base_name(core_string_t path)
{
    size_t end = path.u8_bytes;
    while (end > 1 && core_os_is_separator(path.data[end - 1])) end--;
    size_t start = end;
    while (start > 0 && !core_os_is_separator(path.data[start - 1])) start--;
    return core_string_dup(core_string_slice(path, start, end - start));
}

static core_string_t
store_of(core_string_t root)
{
    core_string_t toolchains = core_os_path_parent(root);
    core_string_t name = base_name(toolchains);
    core_string_t store = core_string_eq(name, CORE_STRING_STATIC("toolchains"))
        ? core_os_path_parent(toolchains) : (core_string_t){0};
    core_free(name.data);
    core_free(toolchains.data);
    return store;
}

static core_string_t
default_store(void)
{
#ifdef _WIN32
    core_string_t base = core_os_environment(CORE_STRING_STATIC("LOCALAPPDATA"));
    core_string_t store = base.u8_bytes
        ? under(base, "Programs\\n00b\\share\\n00b") : (core_string_t){0};
#else
    core_string_t base = core_os_environment(CORE_STRING_STATIC("HOME"));
    core_string_t store = base.u8_bytes
        ? under(base, ".local/share/n00b") : (core_string_t){0};
#endif
    core_free(base.data);
    return store;
}

static void
install_layout(core_string_t root, n00bb_toolchain_mode_t mode)
{
    core_string_t store = store_of(root);
    toolchain = (n00bb_toolchain_t){
        .mode = mode,
        .id = base_name(root),
        .root = root,
        .store = store.data ? store : default_store(),
        .stdlib = under(root, N00BB_LAYOUT_STDLIB),
        .runtime = under(root, N00BB_LAYOUT_RUNTIME),
        .grammar = under(root, N00BB_LAYOUT_GRAMMAR),
        .writ_profiles = under(root, N00BB_LAYOUT_WRIT_PROFILES),
        .writ_contracts = under(root, N00BB_LAYOUT_WRIT_CONTRACTS),
    };
    n00bb_cache_set_key(toolchain.id);
}

static void
dev_layout(void)
{
    core_string_t source = CORE_STRING_STATIC(N00BB_SOURCE_ROOT);
    core_buffer_t id = {0};
    core_buffer_printf(&id, "%s-%s", N00BB_VERSION, n00bb_build);
    toolchain = (n00bb_toolchain_t){
        .mode = N00BB_TOOLCHAIN_DEV,
        .id = core_string_dup(core_buffer_string(&id)),
        .root = core_string_dup(source),
        .store = default_store(),
        .stdlib = under(source, "lib"),
        .runtime = core_string_dup(CORE_STRING_STATIC(N00BB_BUILD_ROOT)),
        .grammar = under(source, "grammar/n00b.bnf"),
        .writ_profiles = core_string_dup(
            CORE_STRING_STATIC(N00BB_WRIT_PROFILE_DIR)),
        .writ_contracts = core_string_dup(
            CORE_STRING_STATIC(N00BB_WRIT_CONTRACT_DIR)),
    };
    core_buffer_release(&id);
}

static core_string_t
installed_root(core_string_t argv0)
{
    core_string_t exe = core_os_executable_path(argv0);
    if (!exe.data) return exe;
    core_string_t bin = core_os_path_parent(exe);
    core_string_t root = core_os_path_parent(bin);
    bool installed = n00bb_toolchain_is_root(root);
    core_free(bin.data);
    core_free(exe.data);
    if (installed) return root;
    core_free(root.data);
    return (core_string_t){0};
}

bool
n00bb_toolchain_is_root(core_string_t root)
{
    core_string_t marker = under(root, N00BB_LAYOUT_MARKER);
    bool found = core_os_is_regular_file(marker);
    core_free(marker.data);
    return found;
}

static bool
executable(core_string_t path)
{
    return core_os_is_regular_file(path) && core_os_is_executable(path);
}

char *
n00bb_toolchain_find_on_path(core_string_t name)
{
    core_string_t search = core_os_environment(CORE_STRING_STATIC("PATH"));
    char *found = nullptr;
    size_t start = 0;
    for (size_t i = 0; !found && i <= search.u8_bytes; i++) {
        if (i < search.u8_bytes && search.data[i] != CORE_OS_LIST_SEPARATOR) continue;
        if (i > start) {
            core_string_t candidate = core_os_path_join(
                core_string_slice(search, start, i - start), name);
            if (executable(candidate)) found = core_string_to_cstr(candidate);
            core_free(candidate.data);
        }
        start = i + 1;
    }
    core_free(search.data);
    return found;
}

bool
n00bb_toolchain_runnable(const char *program)
{
    core_string_t name = core_string_from_cstr(program);
    bool runnable = false;
    if (name.u8_bytes && core_string_find_byte(name, '/') >= 0)
        runnable = executable(name);
    else if (name.u8_bytes) {
        char *found = n00bb_toolchain_find_on_path(name);
        runnable = found != nullptr;
        free(found);
    }
    core_free(name.data);
    return runnable;
}

static char *
resolve_tool(n00bb_setting_id_t setting, core_string_t built, core_string_t name)
{
    core_string_t chosen = n00bb_setting_string(setting);
    if (chosen.u8_bytes) return core_string_to_cstr(chosen);
    if (!built.u8_bytes) return nullptr;
    if (executable(built)) return core_string_to_cstr(built);
    return n00bb_toolchain_find_on_path(name);
}

static char *
sibling_tool(const char *program, core_string_t name)
{
    core_string_t path = core_string_from_cstr(program);
    int64_t slash = path.data ? core_string_rfind_byte(path, '/') : -1;
    core_buffer_t sibling = {0};
    if (slash >= 0) {
        core_buffer_append(&sibling, path.data, (size_t)slash + 1);
        core_buffer_append_string(&sibling, name);
#ifdef _WIN32
        core_buffer_puts(&sibling, ".exe");
#endif
    }
    core_free(path.data);
    char *found = slash >= 0 && executable(core_buffer_string(&sibling))
        ? core_string_to_cstr(core_buffer_string(&sibling)) : nullptr;
    core_buffer_release(&sibling);
    return found;
}

void
n00bb_toolchain_init(core_string_t argv0)
{
    core_string_t home = core_os_environment(CORE_STRING_STATIC("N00B_HOME"));
    core_string_t root = home.u8_bytes ? (core_string_t){0}
                                       : installed_root(argv0);
    if (home.u8_bytes) install_layout(home, N00BB_TOOLCHAIN_HOME);
    else if (root.data) install_layout(root, N00BB_TOOLCHAIN_INSTALLED);
    else {
        core_free(home.data);
        dev_layout();
    }
    toolchain.clang = resolve_tool(N00BB_KEY_TOOLS_CLANG,
        CORE_STRING_STATIC(N00BB_LLVM_CLANG), CORE_STRING_STATIC("clang"));
    toolchain.cargo = resolve_tool(N00BB_KEY_TOOLS_CARGO,
        CORE_STRING_STATIC(N00BB_CARGO), CORE_STRING_STATIC("cargo"));
    toolchain.linker = resolve_tool(N00BB_KEY_TOOLS_CLANG,
        CORE_STRING_STATIC(N00BB_NATIVE_LINKER), CORE_STRING_STATIC("clang"));
    toolchain.archiver = sibling_tool(toolchain.clang,
                                      CORE_STRING_STATIC("llvm-ar"));
    if (!toolchain.archiver)
        toolchain.archiver = n00bb_toolchain_find_on_path(
            CORE_STRING_STATIC("ar"));
}

const n00bb_toolchain_t *
n00bb_toolchain(void)
{
    return &toolchain;
}

core_string_t
n00bb_toolchain_id(void)
{
    return toolchain.id;
}
