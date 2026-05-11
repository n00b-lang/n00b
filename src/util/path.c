#include "n00b.h"
#include "adt/list.h"
#include "adt/array.h"
#include "text/strings/string_ops.h"
#include "text/strings/format.h"
#include "core/random.h"
#include "core/gc.h"
#include "adt/result.h"
#include "util/path.h"

#include <stdio.h>
#include <errno.h>

#if defined(__MACH__)
#include <libproc.h>
#endif

#include <dirent.h>
#ifndef _WIN32
#include <pwd.h>
#endif
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
// ============================================================================
// Windows POSIX shims — minimal stubs so the rest of this file compiles.
// Behavior of POSIX-only operations on Windows is best-effort:
//   - lstat() falls back to stat() (no symlink kind on Windows here)
//   - readlink() / getgroups() return failure
//   - geteuid() returns 0
//   - mkdir() drops the mode argument
// ============================================================================
#include <direct.h>

#ifndef S_IXUSR
#define S_IXUSR 0100
#endif
#ifndef S_IXGRP
#define S_IXGRP 0010
#endif
#ifndef S_IXOTH
#define S_IXOTH 0001
#endif
#ifndef NGROUPS_MAX
#define NGROUPS_MAX 32
#endif

typedef int n00b_uid_t;
typedef int n00b_gid_t;
#define uid_t n00b_uid_t
#define gid_t n00b_gid_t

static inline int n00b_lstat_compat(const char *p, struct stat *st) {
    return stat(p, st);
}
#define lstat(p, st) n00b_lstat_compat((p), (st))

static inline int n00b_readlink_compat(const char *p, char *b, size_t n) {
    (void)p; (void)b; (void)n;
    errno = EINVAL;
    return -1;
}
#define readlink(p, b, n) n00b_readlink_compat((p), (b), (n))

static inline n00b_uid_t n00b_geteuid_compat(void) { return 0; }
#define geteuid() n00b_geteuid_compat()

static inline int n00b_getgroups_compat(int n, n00b_gid_t *g) {
    (void)n; (void)g;
    return 0;
}
#define getgroups(n, g) n00b_getgroups_compat((n), (g))

// Windows mkdir() takes only one argument; drop the mode.
#define mkdir(p, m) _mkdir(p)
#endif // _WIN32

// ============================================================================
// Helpers
// ============================================================================

static n00b_string_t *cached_slash;
static n00b_string_t *cached_period;

static inline void
ensure_cached(void)
{
    if (!cached_slash) {
        cached_slash  = n00b_string_from_cstr("/");
        cached_period = n00b_string_from_cstr(".");
        n00b_gc_register_root(cached_slash);
        n00b_gc_register_root(cached_period);
    }
}

static n00b_list_t(n00b_string_t *) *
split_on_slash(n00b_string_t *s)
{
    ensure_cached();

    n00b_array_t(n00b_string_t *) arr = n00b_unicode_str_split(s, cached_slash);
    n00b_list_t(n00b_string_t *) *result = n00b_alloc(n00b_list_t(n00b_string_t *));
    *result = n00b_list_new(n00b_string_t *);
    size_t n = n00b_array_len(arr);

    for (size_t i = 0; i < n; i++) {
        n00b_list_push(*result, n00b_array_get(arr, i));
    }

    return result;
}

static int
rfind_slash(n00b_string_t *s)
{
    ensure_cached();
    n00b_option_t(int32_t) r = n00b_unicode_str_find(s, cached_slash, .reverse = true);
    return n00b_option_is_set(r) ? n00b_option_get(r) : -1;
}

static int
rfind_period(n00b_string_t *s)
{
    ensure_cached();
    n00b_option_t(int32_t) r = n00b_unicode_str_find(s, cached_period, .reverse = true);
    return n00b_option_is_set(r) ? n00b_option_get(r) : -1;
}

static int
find_slash(n00b_string_t *s)
{
    ensure_cached();
    n00b_option_t(int32_t) r = n00b_unicode_str_find(s, cached_slash);
    return n00b_option_is_set(r) ? n00b_option_get(r) : -1;
}

// ============================================================================
// Basic directory operations
// ============================================================================

n00b_string_t *
n00b_get_current_directory(void)
{
    char buf[PATH_MAX + 1];
    return n00b_string_from_cstr(getcwd(buf, PATH_MAX));
}

bool
n00b_set_current_directory(n00b_string_t *s)
{
    return chdir(s->data) == 0;
}

// ============================================================================
// Temp files / dirs
// ============================================================================

static n00b_string_t *base_tmp_dir;

static n00b_string_t *
acquire_base_tmp_dir(void)
{
    if (base_tmp_dir) {
        return base_tmp_dir;
    }

    const char *v;

    if ((v = getenv("TMPDIR")) && *v) {
        base_tmp_dir = n00b_string_from_cstr(v);
    }
    else if ((v = getenv("TMP")) && *v) {
        base_tmp_dir = n00b_string_from_cstr(v);
    }
    else if ((v = getenv("TEMP")) && *v) {
        base_tmp_dir = n00b_string_from_cstr(v);
    }
    else {
        base_tmp_dir = n00b_string_from_cstr("/tmp/");
    }

    n00b_gc_register_root(base_tmp_dir);
    return base_tmp_dir;
}

static inline n00b_string_t *
construct_random_name(n00b_string_t *prefix, n00b_string_t *suffix)
{
    n00b_string_t *tmpdir = acquire_base_tmp_dir();
    uint64_t       bytes  = n00b_rand64();
    char           hex[17];

    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)bytes);
    n00b_string_t *random_string = n00b_string_from_cstr(hex);

    if (prefix || suffix) {
        if (!prefix) prefix = n00b_string_empty();
        if (!suffix) suffix = n00b_string_empty();
        random_string = n00b_cformat("«#»«#»«#»",
                                      prefix, random_string, suffix);
    }

    return n00b_path_simple_join(tmpdir, random_string);
}

n00b_result_t(n00b_string_t *)
n00b_new_temp_dir(n00b_string_t *prefix, n00b_string_t *suffix)
{
    n00b_string_t *dirname = construct_random_name(prefix, suffix);

    if (mkdir(dirname->data, 0774)) {
        return n00b_result_err(n00b_string_t *, errno);
    }

    return n00b_result_ok(n00b_string_t *, dirname);
}

n00b_string_t *
n00b_get_temp_root(void)
{
    return acquire_base_tmp_dir();
}

// ============================================================================
// Path normalization
// ============================================================================

static n00b_string_t *
remove_extra_slashes(n00b_string_t *result)
{
    int i = result->codepoints;

    while (i > 1 && result->data[--i] == '/') {
        result->data[i]  = 0;
        result->codepoints--;
        result->u8_bytes--;
    }

    return result;
}

n00b_string_t *
n00b_get_user_dir(n00b_string_t *user)
{
    n00b_string_t *result;
#ifndef _WIN32
    struct passwd *pw;
#endif

    if (user == nullptr) {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home) home = getenv("USERPROFILE");
#endif

        if (home) {
            result = n00b_string_from_cstr(home);
        }
        else {
#ifndef _WIN32
            pw = getpwent();
            result = (pw == nullptr)
                ? n00b_string_from_cstr("/")
                : n00b_string_from_cstr(pw->pw_dir);
#else
            result = n00b_string_from_cstr("C:\\");
#endif
        }
    }
    else {
#ifndef _WIN32
        pw = getpwnam(user->data);
        if (pw == nullptr) {
            return user;
        }
        result = n00b_string_from_cstr(pw->pw_dir);
#else
        // Looking up other users' home dirs isn't a portable concept on
        // Windows. Match the Unix fallback when the user is unknown.
        return user;
#endif
    }

    return remove_extra_slashes(result);
}

static n00b_string_t *
internal_normalize_and_join(n00b_list_t(n00b_string_t *) *pieces)
{
    int partlen = (int)n00b_list_len(*pieces);
    int nextout = 0;

    for (int i = 0; i < partlen; i++) {
        n00b_string_t *s = n00b_list_get(*pieces, i);

        if (s->codepoints == 0) continue;

        if (s->data[0] == '.') {
            if (s->codepoints == 1) continue;
            if (s->codepoints == 2 && s->data[1] == '.') {
                --nextout;
                continue;
            }
        }

        if (nextout < 0) return nullptr;

        n00b_list_set(*pieces, nextout++, s);
    }

    if (nextout == 0) {
        return n00b_string_from_cstr("/");
    }

    n00b_string_t *result = nullptr;

    for (int i = 0; i < nextout; i++) {
        n00b_string_t *s = n00b_list_get(*pieces, i);
        if (!s->codepoints) continue;

        if (!result) {
            result = n00b_cformat("/«#»", s);
        }
        else {
            result = n00b_cformat("«#»/«#»", result, s);
        }
    }

    return result;
}

n00b_string_t *
n00b_path_tilde_expand(n00b_string_t *in)
{
    ensure_cached();

    if (!in || !in->codepoints) {
        in = cached_slash;
    }

    if (in->data[0] != '~') {
        return internal_normalize_and_join(split_on_slash(in));
    }

    n00b_list_t(n00b_string_t *) *parts = split_on_slash(in);
    n00b_string_t *home = n00b_list_get(*parts, 0);

    n00b_list_t(n00b_string_t *) *home_parts;

    if (home->codepoints == 1) {
        n00b_list_set(*parts, 0, n00b_string_empty());
        home_parts = split_on_slash(n00b_get_user_dir(nullptr));
    }
    else {
        n00b_string_t *username =
            n00b_unicode_str_slice(home, 1, home->codepoints);
        n00b_list_set(*parts, 0, n00b_string_empty());
        home_parts = split_on_slash(n00b_get_user_dir(username));
    }

    n00b_list_t(n00b_string_t *) *combined = n00b_alloc(n00b_list_t(n00b_string_t *));
    *combined = n00b_list_new(n00b_string_t *);

    for (size_t i = 0; i < n00b_list_len(*home_parts); i++) {
        n00b_list_push(*combined, n00b_list_get(*home_parts, i));
    }
    for (size_t i = 0; i < n00b_list_len(*parts); i++) {
        n00b_list_push(*combined, n00b_list_get(*parts, i));
    }

    return internal_normalize_and_join(combined);
}

n00b_string_t *
n00b_resolve_path(n00b_string_t *s)
{
    if (s == nullptr || s->codepoints == 0) {
        return n00b_get_user_dir(nullptr);
    }

    switch (s->data[0]) {
    case '~':
        return n00b_path_tilde_expand(s);
    case '/':
        return internal_normalize_and_join(split_on_slash(s));
    default: {
        n00b_list_t(n00b_string_t *) *parts =
            split_on_slash(n00b_get_current_directory());
        n00b_list_t(n00b_string_t *) *rel = split_on_slash(s);
        size_t rn = n00b_list_len(*rel);

        for (size_t i = 0; i < rn; i++) {
            n00b_list_push(*parts, n00b_list_get(*rel, i));
        }

        return internal_normalize_and_join(parts);
    }
    }
}

// ============================================================================
// Path joining
// ============================================================================

n00b_string_t *
n00b_path_join(n00b_list_t(n00b_string_t *) *items)
{
    int len   = 0;
    int first = 0;
    int last  = (int)n00b_list_len(*items);
    int tmplen;

    for (int i = 0; i < last; i++) {
        n00b_string_t *tmp = n00b_list_get(*items, i);
        tmplen = (int)tmp->u8_bytes;

        if (tmplen == 0) continue;

        if (tmp->data[0] == '/') {
            len   = tmplen;
            first = i;
        }
        else {
            len += tmplen;
        }

        if ((i + 1 != last) && (tmp->data[tmplen - 1] != '/'))
            len++;
    }

    char *buf = n00b_alloc_array(char, len + 1);
    char *p   = buf;

    for (int i = first; i < last; i++) {
        n00b_string_t *tmp = n00b_list_get(*items, i);
        tmplen = (int)tmp->u8_bytes;

        if (tmplen == 0) continue;

        memcpy(p, tmp->data, tmplen);
        p += tmplen;

        if ((i + 1 != last) && (tmp->data[tmplen - 1] != '/'))
            *p++ = '/';
    }

    *p = '\0';
    return n00b_string_from_cstr(buf);
}

// ============================================================================
// File kind / stat
// ============================================================================

n00b_file_kind
n00b_get_file_kind(n00b_string_t *p)
{
    struct stat file_info;

    p = n00b_resolve_path(p);

    if (lstat(p->data, &file_info) != 0)
        return N00B_FK_NOT_FOUND;

    switch (file_info.st_mode & S_IFMT) {
    case S_IFREG:  return N00B_FK_IS_REG_FILE;
    case S_IFDIR:  return N00B_FK_IS_DIR;
    case S_IFSOCK: return N00B_FK_IS_SOCK;
    case S_IFCHR:  return N00B_FK_IS_CHR_DEVICE;
    case S_IFBLK:  return N00B_FK_IS_BLOCK_DEVICE;
    case S_IFIFO:  return N00B_FK_IS_FIFO;
    case S_IFLNK:
        if (stat(p->data, &file_info) != 0) return N00B_FK_NOT_FOUND;
        switch (file_info.st_mode & S_IFMT) {
        case S_IFREG: return N00B_FK_IS_FLINK;
        case S_IFDIR: return N00B_FK_IS_DLINK;
        default:      return N00B_FK_OTHER;
        }
    default: return N00B_FK_OTHER;
    }
}

// ============================================================================
// Directory walking
// ============================================================================

typedef struct {
    n00b_string_t                *sc_proc;
    n00b_string_t                *sc_dev;
    n00b_list_t(n00b_string_t *) *result;
    n00b_string_t                *resolved;
    bool                          recurse;
    bool                          yield_links;
    bool                          yield_dirs;
    bool                          follow_links;
    bool                          ignore_special;
    bool                          done_with_safety_checks;
    bool                          have_recursed;
} n00b_walk_ctx;

static n00b_string_t *
add_slash_if_needed(n00b_string_t *s)
{
    if (s->u8_bytes && s->data[s->u8_bytes - 1] == '/') return s;
    return n00b_cformat("«#»/", s);
}

static void
internal_path_walk(n00b_walk_ctx *ctx)
{
    DIR           *dirobj;
    struct dirent *entry;
    n00b_string_t *saved;
    struct stat    file_info;

    if (!ctx->done_with_safety_checks) {
        if (n00b_unicode_str_starts_with(ctx->resolved, ctx->sc_proc)) return;
        if (n00b_unicode_str_starts_with(ctx->resolved, ctx->sc_dev))  return;
        if (ctx->resolved->codepoints != 1) ctx->done_with_safety_checks = true;
    }

    if (lstat(ctx->resolved->data, &file_info) != 0) return;

    switch (file_info.st_mode & S_IFMT) {
    case S_IFREG:
        n00b_list_push(*ctx->result, ctx->resolved);
        return;

    case S_IFDIR:
        if (ctx->yield_dirs) {
            n00b_list_push(*ctx->result, ctx->resolved);
            return;
        }

actual_directory:
        if (!ctx->recurse) {
            if (ctx->have_recursed) return;
            ctx->have_recursed = true;
        }

        ctx->resolved = add_slash_if_needed(ctx->resolved);
        dirobj = opendir(ctx->resolved->data);
        if (dirobj == nullptr) return;

        saved = ctx->resolved;

        while (true) {
            entry = readdir(dirobj);
            if (entry == nullptr) {
                closedir(dirobj);
                ctx->resolved = saved;
                return;
            }

            if (!strcmp(entry->d_name, "..") || !strcmp(entry->d_name, "."))
                continue;

            ctx->resolved = n00b_unicode_str_cat(
                saved, n00b_string_from_cstr(entry->d_name));
            internal_path_walk(ctx);
        }

        ctx->resolved = saved;
        return;

    case S_IFLNK:
        if (stat(ctx->resolved->data, &file_info) != 0) return;

        switch (file_info.st_mode & S_IFMT) {
        case S_IFREG:
            if (ctx->follow_links && ctx->yield_links) {
                char buf[PATH_MAX + 1] = {0};
                int n = readlink(ctx->resolved->data, buf, PATH_MAX);
                if (n == -1) return;
                buf[n] = 0;
                n00b_list_push(*ctx->result,
                               n00b_resolve_path(n00b_string_from_cstr(buf)));
            }
            else if (ctx->yield_links) {
                n00b_list_push(*ctx->result, ctx->resolved);
            }
            return;

        case S_IFDIR:
            if (ctx->yield_dirs && ctx->yield_links)
                n00b_list_push(*ctx->result, ctx->resolved);

            if (!ctx->follow_links || !ctx->recurse) return;

            saved = ctx->resolved;
            char lbuf[PATH_MAX + 1] = {0};
            int ln = readlink(ctx->resolved->data, lbuf, PATH_MAX);
            if (ln == -1) return;
            lbuf[ln] = 0;

            ctx->resolved = n00b_resolve_path(n00b_string_from_cstr(lbuf));
            if (ctx->yield_dirs && !ctx->yield_links)
                n00b_list_push(*ctx->result, ctx->resolved);

            goto actual_directory;

        default:
            if (!ctx->ignore_special)
                n00b_list_push(*ctx->result, ctx->resolved);
            return;
        }

    default:
        if (!ctx->ignore_special)
            n00b_list_push(*ctx->result, ctx->resolved);
        return;
    }
}

n00b_list_t(n00b_string_t *) *
_n00b_path_walk(n00b_string_t *dir) _kargs
{
    bool recurse        = true;
    bool yield_links    = false;
    bool yield_dirs     = false;
    bool ignore_special = true;
    bool follow_links   = false;
}
{
    n00b_list_t(n00b_string_t *) *result = n00b_alloc(n00b_list_t(n00b_string_t *));
    *result = n00b_list_new(n00b_string_t *);

    n00b_walk_ctx ctx = {
        .sc_proc                 = n00b_string_from_cstr("/proc/"),
        .sc_dev                  = n00b_string_from_cstr("/dev/"),
        .recurse                 = recurse,
        .yield_links             = yield_links,
        .yield_dirs              = yield_dirs,
        .follow_links            = follow_links,
        .ignore_special          = ignore_special,
        .done_with_safety_checks = false,
        .have_recursed           = false,
        .result                  = result,
        .resolved                = n00b_resolve_path(dir),
    };

    internal_path_walk(&ctx);
    return result;
}

// ============================================================================
// App path
// ============================================================================

#ifdef __linux__
n00b_string_t *
n00b_app_path(void)
{
    char buf[PATH_MAX];
    char proc_path[PATH_MAX];

    snprintf(proc_path, PATH_MAX, "/proc/%d/exe", getpid());
    ssize_t n = readlink(proc_path, buf, PATH_MAX - 1);
    if (n < 0) return n00b_string_from_cstr(".");
    buf[n] = 0;

    return n00b_resolve_path(n00b_string_from_cstr(buf));
}
#elif defined(__MACH__)
n00b_string_t *
n00b_app_path(void)
{
    char buf[PROC_PIDPATHINFO_MAXSIZE];
    proc_pidpath(getpid(), buf, PROC_PIDPATHINFO_MAXSIZE);
    return n00b_resolve_path(n00b_string_from_cstr(buf));
}
#elif defined(_WIN32)
// Forward-declare GetModuleFileNameA rather than #include <windows.h>:
// windows.h transitively pulls in MMX intrinsic headers that ncc cannot
// tokenize (compound-literal vector casts in mmintrin.h).
#define N00B_WIN_MAX_PATH 260
extern unsigned long __stdcall
GetModuleFileNameA(void *hModule, char *lpFilename, unsigned long nSize);

n00b_string_t *
n00b_app_path(void)
{
    char buf[N00B_WIN_MAX_PATH];
    unsigned long n = GetModuleFileNameA(NULL, buf, N00B_WIN_MAX_PATH);
    if (n == 0 || n == N00B_WIN_MAX_PATH) return n00b_string_from_cstr(".");
    buf[n] = 0;
    return n00b_resolve_path(n00b_string_from_cstr(buf));
}
#else
#error "Unsupported platform"
#endif

// ============================================================================
// Path trimming / stripping / extension
// ============================================================================

n00b_string_t *
n00b_path_trim_trailing_slashes(n00b_string_t *s)
{
    int b_len = (int)s->u8_bytes;

    if (!b_len || s->data[b_len - 1] != '/') return s;

    s = n00b_unicode_str_copy(s);

    do {
        s->data[--b_len] = 0;
        s->codepoints--;
        s->u8_bytes--;
    } while (b_len && s->data[b_len - 1] == '/');

    return s;
}

void
n00b_path_strip_slashes_both_ends(n00b_string_t *s)
{
    while (s->u8_bytes && s->data[0] == '/') {
        s->data++;
        s->u8_bytes--;
        s->codepoints--;
    }

    while (s->u8_bytes && s->data[s->u8_bytes - 1] == '/') {
        s->data[--s->u8_bytes] = 0;
        s->codepoints--;
    }
}

n00b_string_t *
n00b_path_chop_extension(n00b_string_t *s)
{
    int n = rfind_period(s);
    int m = rfind_slash(s);

    if (n <= m) return n00b_string_empty();

    n00b_string_t *result = n00b_unicode_str_slice(s, n, s->codepoints);
    s->codepoints -= result->codepoints;
    s->u8_bytes   -= result->u8_bytes;
    s->data[s->u8_bytes] = 0;

    return result;
}

n00b_string_t *
n00b_filename_from_path(n00b_string_t *s)
{
    if (find_slash(s) == -1) return s;

    n00b_string_t *resolved = n00b_resolve_path(s);
    n00b_path_strip_slashes_both_ends(resolved);

    int n = rfind_slash(resolved);
    if (n == -1) return resolved;

    return n00b_unicode_str_slice(resolved, n + 1, resolved->codepoints);
}

n00b_string_t *
n00b_path_get_extension(n00b_string_t *s)
{
    int n = rfind_period(s);
    int m = rfind_slash(s);

    if (n <= m) return n00b_string_empty();
    return n00b_unicode_str_slice(s, n, s->codepoints);
}

n00b_string_t *
n00b_path_remove_extension(n00b_string_t *s)
{
    int n = rfind_period(s);
    int m = rfind_slash(s);

    if (n <= m) return s;
    return n00b_unicode_str_slice(s, 0, n);
}

n00b_list_t(n00b_string_t *) *
n00b_path_parts(n00b_string_t *p)
{
    n00b_list_t(n00b_string_t *) *result = n00b_alloc(n00b_list_t(n00b_string_t *));
    *result = n00b_list_new(n00b_string_t *);

    if (!p || !p->u8_bytes) return result;

    n00b_string_t *resolved = n00b_resolve_path(p);

    if (p->data[p->u8_bytes - 1] == '/'
        || resolved->data[resolved->u8_bytes - 1] == '/') {
        n00b_list_push(*result, resolved);
        n00b_list_push(*result, n00b_string_empty());
        n00b_list_push(*result, n00b_string_empty());
        return result;
    }

    int n = rfind_slash(resolved);

    n00b_list_push(*result, n00b_unicode_str_slice(resolved, 0, n));

    n00b_string_t *filename =
        n00b_unicode_str_slice(resolved, n + 1, resolved->codepoints);

    int dot = rfind_period(filename);

    if (dot == -1) {
        n00b_list_push(*result, filename);
        n00b_list_push(*result, n00b_string_empty());
        return result;
    }

    n00b_list_push(*result, n00b_unicode_str_slice(filename, 0, dot));
    n00b_list_push(*result,
                   n00b_unicode_str_slice(filename, dot + 1,
                                          filename->codepoints));
    return result;
}

// ============================================================================
// File finding / command paths
// ============================================================================

n00b_list_t(n00b_string_t *) *
n00b_find_file_in_program_path(n00b_string_t *cmd,
                                n00b_list_t(n00b_string_t *) *path_list)
{
    n00b_list_t(n00b_string_t *) *result = n00b_alloc(n00b_list_t(n00b_string_t *));
    *result = n00b_list_new(n00b_string_t *);

    if (!path_list) {
        path_list = n00b_get_program_search_path();
    }

    cmd = n00b_filename_from_path(cmd);

    size_t n = n00b_list_len(*path_list);

    for (size_t i = 0; i < n; i++) {
        n00b_string_t *dir       = n00b_list_get(*path_list, i);
        dir                      = n00b_resolve_path(dir);
        n00b_string_t *full_path = n00b_path_simple_join(dir, cmd);

        switch (n00b_get_file_kind(full_path)) {
        case N00B_FK_IS_REG_FILE:
        case N00B_FK_IS_FLINK:
            n00b_list_push(*result, full_path);
            break;
        default:
            break;
        }
    }

    return result;
}

n00b_list_t(n00b_string_t *) *
n00b_find_command_paths(n00b_string_t *cmd,
                         n00b_list_t(n00b_string_t *) *path_list,
                         bool self_ok)
{
    n00b_list_t(n00b_string_t *) *result =
        n00b_find_file_in_program_path(cmd, path_list);

    int            n          = (int)n00b_list_len(*result);
    n00b_string_t *my_path    = nullptr;
    uid_t          my_euid    = geteuid();
    int            num_groups = -1;
    gid_t          groups[NGROUPS_MAX];

    if (!self_ok) {
        my_path = n00b_app_path();
    }

    while (n--) {
        n00b_string_t *path = n00b_list_get(*result, n);

        if (!self_ok && n00b_unicode_str_eq(path, my_path)) {
            n00b_list_delete(*result, n);
            continue;
        }

        struct stat file_info;

        if (stat(path->data, &file_info) != 0) {
            n00b_list_delete(*result, n);
            continue;
        }

        int exe_bits = file_info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH);

        if (!exe_bits) {
            n00b_list_delete(*result, n);
            continue;
        }

        if (exe_bits & S_IXOTH) continue;
        if ((exe_bits & S_IXUSR) && file_info.st_uid == my_euid) continue;

        if (num_groups < 0) {
            num_groups = getgroups(NGROUPS_MAX, groups);
        }

        gid_t program_gid = file_info.st_gid;

        for (int i = 0; i < num_groups; i++) {
            if (program_gid == groups[i]) goto on_success;
        }

        n00b_list_delete(*result, n);
on_success:
        continue;
    }

    return result;
}

// ============================================================================
// Rename
// ============================================================================

n00b_result_t(n00b_string_t *)
n00b_rename(n00b_string_t *from, n00b_string_t *to)
{
    from = n00b_resolve_path(from);
    to   = n00b_resolve_path(to);

    if (!n00b_file_exists(from)) {
        return n00b_result_err(n00b_string_t *, ENOENT);
    }

    if (n00b_file_exists(to)) {
        n00b_list_t(n00b_string_t *) *parts = n00b_path_parts(to);

        if ((int)n00b_list_len(*parts) < 3) {
            return n00b_result_err(n00b_string_t *, EINVAL);
        }

        n00b_string_t *base = n00b_list_get(*parts, 0);
        n00b_string_t *name = n00b_list_get(*parts, 1);
        n00b_string_t *ext  = n00b_list_get(*parts, 2);
        int            i    = 0;

        if (ext->codepoints) {
            ext = n00b_cformat(".«#»", ext);
        }

        do {
            to = n00b_cformat("«#»/«#».«#:d»«#»", base, name, ++i, ext);
        } while (n00b_file_exists(to));
    }

    if (rename(from->data, to->data)) {
        return n00b_result_err(n00b_string_t *, errno);
    }

    return n00b_result_ok(n00b_string_t *, to);
}

// ============================================================================
// List directory
// ============================================================================

n00b_list_t(n00b_string_t *) *
_n00b_list_directory(n00b_string_t *dir) _kargs
{
    n00b_string_t *extension   = nullptr;
    bool           files       = true;
    bool           directories = true;
    bool           links       = true;
    bool           specials    = true;
    bool           full_path   = false;
    bool           dot_files   = true;
}
{
    dir         = n00b_resolve_path(dir);
    DIR *dirent = opendir(dir->data);

    n00b_list_t(n00b_string_t *) *result = n00b_alloc(n00b_list_t(n00b_string_t *));
    *result = n00b_list_new(n00b_string_t *);

    if (!dirent) return result;

    if (extension && extension->codepoints && extension->data[0] != '.') {
        extension = n00b_cformat(".«#»", extension);
    }

    while (true) {
        struct dirent *entry = readdir(dirent);

        if (!entry) {
            closedir(dirent);
            return result;
        }

        if (!strcmp(entry->d_name, "..") || !strcmp(entry->d_name, "."))
            continue;

        if (!dot_files && *entry->d_name == '.')
            continue;

        n00b_string_t *fname = n00b_string_from_cstr(entry->d_name);
        n00b_string_t *full  = n00b_path_simple_join(dir, fname);
        struct stat    file_info;
        bool           add = false;

        if (lstat(full->data, &file_info) != 0) continue;

        switch (file_info.st_mode & S_IFMT) {
        case S_IFREG: add = files;       break;
        case S_IFDIR: add = directories;  break;
        case S_IFLNK: add = links;       break;
        default:      add = specials;     break;
        }

        if (!add) continue;

        if (extension && !n00b_unicode_str_ends_with(fname, extension))
            continue;

        n00b_list_push(*result, full_path ? full : fname);
    }
}
