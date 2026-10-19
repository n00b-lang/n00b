#include "cache_location.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/string.h"

#include <limits.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#ifndef _WIN32
static bool
append_environment(core_buffer_t *out, const char *name, core_string_t suffix)
{
    core_string_t value = core_string_from_cstr(getenv(name));
    bool present = value.u8_bytes != 0;
    if (present) {
        if (suffix.u8_bytes == 0 && value.data[0] != '/') {
            char cwd[PATH_MAX];
            core_string_t working = core_string_from_cstr(
                getcwd(cwd, sizeof cwd));
            present = working.u8_bytes != 0;
            core_buffer_append_string(out, working);
            core_buffer_append_string(out, CORE_STRING_STATIC("/"));
            core_free(working.data);
        }
        if (present) {
            core_buffer_append_string(out, value);
            core_buffer_append_string(out, suffix);
        }
    }
    core_free(value.data);
    return present;
}
#endif

core_string_t
n00bb_cache_directory(core_string_t component)
{
#ifdef _WIN32
    (void)component;
    return (core_string_t){};
#else
    core_buffer_t directory = {0};
    bool found = append_environment(&directory, "N00B_CACHE",
                                    CORE_STRING_STATIC(""))
        || append_environment(&directory, "XDG_CACHE_HOME",
                              CORE_STRING_STATIC("/n00b"))
        || append_environment(&directory, "HOME",
                              CORE_STRING_STATIC("/.cache/n00b"));
    core_string_t result = {0};
    if (found) {
        core_buffer_append_string(&directory, CORE_STRING_STATIC("/"));
        core_buffer_append_string(&directory, component);
        result = core_string_dup(core_buffer_string(&directory));
    }
    core_buffer_release(&directory);
    return result;
#endif
}

core_string_t
n00bb_cache_mirror_directory(core_string_t component,
                             core_string_t absolute_path)
{
    if (!absolute_path.u8_bytes || absolute_path.data[0] != '/')
        return (core_string_t){};
    core_string_t base = n00bb_cache_directory(component);
    if (!base.data) return base;
    core_buffer_t directory = {0};
    core_buffer_append_string(&directory, base);
    core_buffer_append_string(&directory, absolute_path);
    core_string_t result = core_string_dup(core_buffer_string(&directory));
    core_buffer_release(&directory);
    core_free(base.data);
    return result;
}
