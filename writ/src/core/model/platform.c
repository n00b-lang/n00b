#include "model.h"
#include "lib/os.h"

typedef struct {
    const char *triple;
    const char *os;
    const char *arch;
    const char *abi;
    const char *libc;
    const char *stdlib;
    const char *data_model;
    const char *object_format;
} platform_row_t;

static const platform_row_t platforms[] = {
    {"aarch64-apple-darwin", "macos", "aarch64", "darwin", "apple", "libcxx", "lp64", "macho"},
    {"x86_64-apple-darwin", "macos", "x86_64", "darwin", "apple", "libcxx", "lp64", "macho"},
    {"aarch64-linux-gnu", "linux", "aarch64", "gnu", "glibc", "libstdcxx", "lp64", "elf"},
    {"x86_64-linux-gnu", "linux", "x86_64", "gnu", "glibc", "libstdcxx", "lp64", "elf"},
    {"x86_64-pc-windows-msvc", "windows", "x86_64", "msvc", "msvcrt", "msvc", "llp64", "coff"},
    {"x86_64-w64-windows-gnu", "windows", "x86_64", "gnu", "msvcrt", "libcxx", "llp64", "coff"},
};

static const struct {
    const char *alias;
    const char *triple;
} aliases[] = {
    {"aarch64-unknown-linux-gnu", "aarch64-linux-gnu"},
    {"x86_64-unknown-linux-gnu", "x86_64-linux-gnu"},
    {"arm64-apple-darwin", "aarch64-apple-darwin"},
    {"arm64-apple-macosx", "aarch64-apple-darwin"},
    {"x86_64-pc-windows-gnu", "x86_64-w64-windows-gnu"},
    {"x86_64-w64-mingw32", "x86_64-w64-windows-gnu"},
};

static writ_sym_t
sym(writ_session_t *session, const char *text)
{
    return writ_intern(session, text, strlen(text));
}

static void
platform_release(void *object)
{
    writ_platform_t *platform = object;
    core_dict_free(&platform->versions);
    core_dict_free(&platform->features);
}

writ_status_t
writ_platform_from_triple(writ_session_t *session, writ_str_t triple, writ_platform_t **out)
{
    if (!session || !out) {
        return WRIT_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (writ_str_eq(triple, writ_cstr(aliases[i].alias))) {
            triple = writ_cstr(aliases[i].triple);
        }
    }
    for (size_t i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++) {
        const platform_row_t *row = &platforms[i];
        if (!writ_str_eq(triple, writ_cstr(row->triple))) {
            continue;
        }
        writ_platform_t *platform = writ_alloc(session, sizeof(writ_platform_t));
        *platform = (writ_platform_t){
            .session       = session,
            .triple        = sym(session, row->triple),
            .os            = sym(session, row->os),
            .arch          = sym(session, row->arch),
            .abi           = sym(session, row->abi),
            .libc          = sym(session, row->libc),
            .stdlib        = sym(session, row->stdlib),
            .data_model    = sym(session, row->data_model),
            .object_format = sym(session, row->object_format),
            .pointer_width = 64,
            .long_width    =!strcmp(row->data_model, "llp64") ? 4 : 8,
        };
        core_dict_init(&platform->versions, nullptr, nullptr);
        core_dict_init(&platform->features, nullptr, nullptr);
        writ_on_session_free(session, platform_release, platform);
        *out = platform;
        return WRIT_OK;
    }
    return WRIT_INVALID_ARGUMENT;
}

void
writ_platform_binary_io(void)
{
    core_os_binary_io();
}

writ_str_t
writ_platform_host_triple(void)
{
#if defined(__APPLE__) && defined(__aarch64__)
    return writ_cstr("aarch64-apple-darwin");
#elif defined(__APPLE__)
    return writ_cstr("x86_64-apple-darwin");
#elif defined(__linux__) && defined(__aarch64__)
    return writ_cstr("aarch64-linux-gnu");
#elif defined(__linux__)
    return writ_cstr("x86_64-linux-gnu");
#elif defined(__MINGW32__)
    return writ_cstr("x86_64-w64-windows-gnu");
#else
    return writ_cstr("x86_64-pc-windows-msvc");
#endif
}

writ_str_t
writ_platform_triple(const writ_platform_t *platform)
{
    return platform ? writ_sym_str(platform->session, platform->triple) : writ_cstr("");
}

int64_t
writ_version_value(writ_str_t text)
{
    int64_t parts[3] = {0, 0, 0};
    size_t part = 0;
    for (size_t i = 0; i < text.length && part < 3; i++) {
        if (text.data[i] == '.') {
            part++;
        } else if (text.data[i] >= '0' && text.data[i] <= '9') {
            parts[part] = parts[part] * 10 + (text.data[i] - '0');
        }
    }
    return parts[0] * 1000000 + parts[1] * 1000 + parts[2];
}

writ_status_t
writ_platform_set_version(writ_platform_t *platform, writ_str_t library, writ_str_t version)
{
    if (!platform || !library.data || !version.data) {
        return WRIT_INVALID_ARGUMENT;
    }
    core_dict_put(&platform->versions, writ_intern_str(platform->session, library),
                  (void *)(intptr_t)writ_version_value(version));
    return WRIT_OK;
}

writ_status_t
writ_platform_set_feature(writ_platform_t *platform, writ_str_t feature, bool enabled)
{
    if (!platform || !feature.data) {
        return WRIT_INVALID_ARGUMENT;
    }
    core_dict_put(&platform->features, writ_intern_str(platform->session, feature), (void *)(intptr_t)enabled);
    return WRIT_OK;
}

static const struct {
    const char *name;
    size_t      offset;
} facts[] = {
    {"os", offsetof(writ_platform_t, os)},
    {"arch", offsetof(writ_platform_t, arch)},
    {"abi", offsetof(writ_platform_t, abi)},
    {"libc", offsetof(writ_platform_t, libc)},
    {"stdlib", offsetof(writ_platform_t, stdlib)},
    {"platform", offsetof(writ_platform_t, triple)},
};

bool
writ_platform_fact(const writ_platform_t *platform, writ_sym_t name, int64_t *value)
{
    writ_str_t text = writ_sym_str(platform->session, name);
    for (size_t i = 0; i < sizeof(facts) / sizeof(facts[0]); i++) {
        if (writ_str_eq(text, writ_cstr(facts[i].name))) {
            *value = *(const writ_sym_t *)((const char *)platform + facts[i].offset);
            return true;
        }
    }
    if (writ_str_eq(text, writ_cstr("pointer_width"))) {
        *value = platform->pointer_width;
        return true;
    }
    return false;
}

bool
writ_glob_match(writ_str_t pattern, writ_str_t text)
{
    size_t p = 0;
    size_t t = 0;
    size_t star = SIZE_MAX;
    size_t mark = 0;
    while (t < text.length) {
        if (p < pattern.length && pattern.data[p] == '*') {
            star = p++;
            mark = t;
        } else if (p < pattern.length && pattern.data[p] == text.data[t]) {
            p++;
            t++;
        } else if (star != SIZE_MAX) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.length && pattern.data[p] == '*') {
        p++;
    }
    return p == pattern.length;
}

size_t
writ_glob_literal_prefix(writ_str_t pattern)
{
    size_t length = 0;
    while (length < pattern.length && pattern.data[length] != '*') {
        length++;
    }
    return length;
}
