#include "cache_location.h"

#include "lib/alloc.h"
#include "lib/os.h"
#include "lib/buffer.h"
#include "lib/string.h"

static bool
append_directory(core_buffer_t *out, core_string_t value, core_string_t suffix)
{
    bool present = value.u8_bytes != 0;
    if (present && !core_os_is_absolute(value)) {
        core_string_t working = core_os_current_directory();
        core_string_t joined = core_os_path_join(working, value);
        core_free(working.data);
        core_free(value.data);
        value = joined;
    }
    if (present) {
        core_string_t full = core_os_path_join(value, suffix);
        core_buffer_append_string(out, suffix.u8_bytes ? full : value);
        core_free(full.data);
    }
    core_free(value.data);
    return present;
}

static bool
append_environment(core_buffer_t *out, core_string_t name, core_string_t suffix)
{
    return append_directory(out, core_os_environment(name), suffix);
}

static core_string_t chosen_root;

void
n00bb_cache_set_root(core_string_t root)
{
    core_free(chosen_root.data);
    chosen_root = core_string_dup(root);
}

static bool
append_setting(core_buffer_t *out)
{
    return append_directory(out, core_string_dup(chosen_root),
                            CORE_STRING_STATIC(""));
}

static core_string_t cache_key = CORE_STRING_STATIC("dev");

void
n00bb_cache_set_key(core_string_t key)
{
    cache_key = key;
}

core_string_t
n00bb_cache_root(void)
{
    core_buffer_t directory = {0};
#ifdef _WIN32
    bool found = append_setting(&directory)
        || append_environment(&directory, CORE_STRING_STATIC("LOCALAPPDATA"),
                              CORE_STRING_STATIC("n00b/cache"));
#elif defined(__APPLE__)
    bool found = append_setting(&directory)
        || append_environment(&directory, CORE_STRING_STATIC("XDG_CACHE_HOME"),
                              CORE_STRING_STATIC("n00b"))
        || append_environment(&directory, CORE_STRING_STATIC("HOME"),
                              CORE_STRING_STATIC("Library/Caches/n00b"));
#else
    bool found = append_setting(&directory)
        || append_environment(&directory, CORE_STRING_STATIC("XDG_CACHE_HOME"),
                              CORE_STRING_STATIC("n00b"))
        || append_environment(&directory, CORE_STRING_STATIC("HOME"),
                              CORE_STRING_STATIC(".cache/n00b"));
#endif
    core_string_t result = found ? core_string_dup(core_buffer_string(&directory))
                                 : (core_string_t){0};
    core_buffer_release(&directory);
    return result;
}

core_string_t
n00bb_cache_directory(core_string_t component)
{
    core_string_t root = n00bb_cache_root();
    if (!root.data) return root;
    core_string_t keyed = core_os_path_join(root, cache_key);
    core_free(root.data);
    if (!component.u8_bytes) return keyed;
    core_string_t result = core_os_path_join(keyed, component);
    core_free(keyed.data);
    return result;
}

core_string_t
n00bb_cache_mirror_directory(core_string_t component,
                             core_string_t absolute_path)
{
    core_string_t relative = core_os_root_relative(absolute_path);
    if (!relative.data) return (core_string_t){};
    core_string_t base = n00bb_cache_directory(component);
    if (!base.data) {
        core_free(relative.data);
        return base;
    }
    core_buffer_t directory = {0};
    core_buffer_append_string(&directory, base);
    core_buffer_append(&directory, "/", 1);
    core_buffer_append_string(&directory, relative);
    core_free(relative.data);
    core_string_t result = core_string_dup(core_buffer_string(&directory));
    core_buffer_release(&directory);
    core_free(base.data);
    return result;
}

bool
n00bb_make_directories(core_string_t directory)
{
    if (!directory.data || !directory.u8_bytes) return false;
    for (size_t i = 1; i < directory.u8_bytes; i++)
        if (core_os_is_separator(directory.data[i]))
            core_os_make_directory(core_string_borrow(directory.data, i),
                                   0700);
    core_os_make_directory(directory, 0700);
    return core_os_is_directory(directory);
}
