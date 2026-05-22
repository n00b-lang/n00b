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
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>

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
    // Canonical idiom: build the list as a fully-initialized lvalue
    // (scan_kind / scan_cb / scan_user / allocator threaded), populate,
    // then struct-copy into the heap-allocated return shell.
    n00b_list_t(n00b_string_t *) lst = n00b_list_new(n00b_string_t *);
    size_t n = n00b_array_len(arr);

    for (size_t i = 0; i < n; i++) {
        n00b_list_push(lst, n00b_array_get(arr, i));
    }

    n00b_list_t(n00b_string_t *) *result =
        n00b_alloc(n00b_list_t(n00b_string_t *));
    *result = lst;
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
    struct passwd *pw;

    if (user == nullptr) {
        const char *home = getenv("HOME");

        if (home) {
            result = n00b_string_from_cstr(home);
        }
        else {
            pw = getpwent();
            result = (pw == nullptr)
                ? n00b_string_from_cstr("/")
                : n00b_string_from_cstr(pw->pw_dir);
        }
    }
    else {
        pw = getpwnam(user->data);
        if (pw == nullptr) {
            return user;
        }
        result = n00b_string_from_cstr(pw->pw_dir);
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

    // Canonical idiom: the list is internally-scoped (passed to
    // internal_normalize_and_join which only reads it), so use a
    // by-value lvalue and pass `&lst` to the consumer.
    n00b_list_t(n00b_string_t *) combined = n00b_list_new(n00b_string_t *);

    for (size_t i = 0; i < n00b_list_len(*home_parts); i++) {
        n00b_list_push(combined, n00b_list_get(*home_parts, i));
    }
    for (size_t i = 0; i < n00b_list_len(*parts); i++) {
        n00b_list_push(combined, n00b_list_get(*parts, i));
    }

    return internal_normalize_and_join(&combined);
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
    // Canonical idiom for by-pointer return: build the list as a
    // fully scan-info-threaded lvalue, populate via internal_path_walk
    // (which pushes via the result pointer), then struct-copy the
    // populated lvalue into the heap-allocated return shell. The
    // struct-copy carries scan_kind / scan_cb / scan_user / allocator
    // into the heap allocation so the GC sees the correct shape.
    n00b_list_t(n00b_string_t *) lst = n00b_list_new(n00b_string_t *);

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
        .result                  = &lst,
        .resolved                = n00b_resolve_path(dir),
    };

    internal_path_walk(&ctx);

    n00b_list_t(n00b_string_t *) *result =
        n00b_alloc(n00b_list_t(n00b_string_t *));
    *result = lst;
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
    // Canonical idiom for by-pointer return: populate a fully
    // scan-info-threaded lvalue first, then struct-copy into the
    // heap-allocated return shell so the GC sees the threaded
    // scan_kind / scan_cb / scan_user / allocator on the heap struct.
    n00b_list_t(n00b_string_t *) lst = n00b_list_new(n00b_string_t *);

    if (p && p->u8_bytes) {
        n00b_string_t *resolved = n00b_resolve_path(p);

        if (p->data[p->u8_bytes - 1] == '/'
            || resolved->data[resolved->u8_bytes - 1] == '/') {
            n00b_list_push(lst, resolved);
            n00b_list_push(lst, n00b_string_empty());
            n00b_list_push(lst, n00b_string_empty());
        }
        else {
            int n = rfind_slash(resolved);

            n00b_list_push(lst, n00b_unicode_str_slice(resolved, 0, n));

            n00b_string_t *filename =
                n00b_unicode_str_slice(resolved, n + 1, resolved->codepoints);

            int dot = rfind_period(filename);

            if (dot == -1) {
                n00b_list_push(lst, filename);
                n00b_list_push(lst, n00b_string_empty());
            }
            else {
                n00b_list_push(lst, n00b_unicode_str_slice(filename, 0, dot));
                n00b_list_push(lst,
                               n00b_unicode_str_slice(filename, dot + 1,
                                                      filename->codepoints));
            }
        }
    }

    n00b_list_t(n00b_string_t *) *result =
        n00b_alloc(n00b_list_t(n00b_string_t *));
    *result = lst;
    return result;
}

// ============================================================================
// File finding / command paths
// ============================================================================

n00b_list_t(n00b_string_t *) *
n00b_find_file_in_program_path(n00b_string_t *cmd,
                                n00b_list_t(n00b_string_t *) *path_list)
{
    // Canonical idiom for by-pointer return: populate a fully
    // scan-info-threaded lvalue first, then struct-copy into the
    // heap-allocated return shell.
    n00b_list_t(n00b_string_t *) lst = n00b_list_new(n00b_string_t *);

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
            n00b_list_push(lst, full_path);
            break;
        default:
            break;
        }
    }

    n00b_list_t(n00b_string_t *) *result =
        n00b_alloc(n00b_list_t(n00b_string_t *));
    *result = lst;
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

// ============================================================================
// Typed-variadic path join
// ============================================================================
//
// User direction 2026-05-21: "if we use our typed varargs, we don't
// really need the array version. The implementation could even do a
// list wrapper around the input for join." The variadic accepts a
// required leading component plus zero or more `n00b_string_t *`
// pieces, packs them into a by-value private-list lvalue, and
// delegates to the existing list-based `n00b_path_join` core.

n00b_string_t *
n00b_path_join_v(n00b_string_t *first, n00b_string_t * +)
{
    n00b_list_t(n00b_string_t *) parts =
        n00b_list_new_private(n00b_string_t *);

    n00b_list_push(parts, first);

    unsigned int count = n00b_remaining_vargs(vargs);
    for (unsigned int i = 0; i < count; i++) {
        n00b_string_t *piece = (n00b_string_t *)n00b_vargs_next(vargs);
        n00b_list_push(parts, piece);
    }

    return n00b_path_join(&parts);
}

// ============================================================================
// XDG Base Directory resolvers
// ============================================================================
//
// Per https://specifications.freedesktop.org/basedir-spec/latest/ —
// the `*_HOME` variants fall back to a `$HOME/`-relative default
// when the corresponding env var is unset or empty; `XDG_RUNTIME_DIR`
// has no spec-mandated fallback. Trailing slashes are stripped per
// the spec's "no trailing slash" convention.
//
// `getenv` use here matches the existing path-module pattern at
// `acquire_base_tmp_dir` and `n00b_get_program_search_path`.

static n00b_string_t *
xdg_home_or_fallback(const char *env_name, const char *home_relative)
{
    const char *v = getenv(env_name);
    if (v != nullptr && v[0] != '\0') {
        return remove_extra_slashes(n00b_string_from_cstr(v));
    }
    // Spec: empty $XDG_*_HOME = treated as unset, use $HOME-relative
    // fallback. n00b_get_user_dir(nullptr) returns $HOME (or pwent
    // fallback), already with trailing slashes stripped.
    return remove_extra_slashes(
        n00b_cformat("«#»«#»",
                     n00b_get_user_dir(nullptr),
                     n00b_string_from_cstr(home_relative)));
}

n00b_string_t *
n00b_xdg_config_home(void)
{
    return xdg_home_or_fallback("XDG_CONFIG_HOME", "/.config");
}

n00b_string_t *
n00b_xdg_data_home(void)
{
    return xdg_home_or_fallback("XDG_DATA_HOME", "/.local/share");
}

n00b_string_t *
n00b_xdg_cache_home(void)
{
    return xdg_home_or_fallback("XDG_CACHE_HOME", "/.cache");
}

n00b_string_t *
n00b_xdg_state_home(void)
{
    return xdg_home_or_fallback("XDG_STATE_HOME", "/.local/state");
}

n00b_string_t *
n00b_xdg_runtime_dir(void)
{
    const char *v = getenv("XDG_RUNTIME_DIR");
    if (v == nullptr || v[0] == '\0') {
        return nullptr;
    }
    return remove_extra_slashes(n00b_string_from_cstr(v));
}

// Shared helper: join base + app + variadic-tail pieces into a list
// and delegate to n00b_path_join. base = nullptr (e.g. runtime-dir
// unset) propagates nullptr.
static n00b_string_t *
xdg_join_under(n00b_string_t *base,
               n00b_string_t *app,
               n00b_vargs_t  *tail_vargs)
{
    if (base == nullptr) {
        return nullptr;
    }

    n00b_list_t(n00b_string_t *) parts =
        n00b_list_new_private(n00b_string_t *);

    n00b_list_push(parts, base);
    n00b_list_push(parts, app);

    unsigned int count = n00b_remaining_vargs(tail_vargs);
    for (unsigned int i = 0; i < count; i++) {
        n00b_string_t *piece =
            (n00b_string_t *)n00b_vargs_next(tail_vargs);
        n00b_list_push(parts, piece);
    }

    return n00b_path_join(&parts);
}

n00b_string_t *
n00b_xdg_config_path(n00b_string_t *app, n00b_string_t * +)
{
    return xdg_join_under(n00b_xdg_config_home(), app, vargs);
}

n00b_string_t *
n00b_xdg_data_path(n00b_string_t *app, n00b_string_t * +)
{
    return xdg_join_under(n00b_xdg_data_home(), app, vargs);
}

n00b_string_t *
n00b_xdg_cache_path(n00b_string_t *app, n00b_string_t * +)
{
    return xdg_join_under(n00b_xdg_cache_home(), app, vargs);
}

n00b_string_t *
n00b_xdg_state_path(n00b_string_t *app, n00b_string_t * +)
{
    return xdg_join_under(n00b_xdg_state_home(), app, vargs);
}

n00b_string_t *
n00b_xdg_runtime_path(n00b_string_t *app, n00b_string_t * +)
{
    return xdg_join_under(n00b_xdg_runtime_dir(), app, vargs);
}

// ============================================================================
// n00b_path_canonical — combined env-var + tilde + absolute + realpath
// ============================================================================
//
// Composes the four canonicalization steps in order:
//   1. $VAR / ${VAR} env-var expansion (when expand_env_vars).
//   2. Leading ~ / ~user tilde expansion (when expand_tilde).
//   3. Absolute-rooting via cwd (when make_absolute).
//   4. realpath() symlink resolution (when resolve_symlinks).
//
// Env-var expansion is the only genuinely new piece; it walks the
// input scanning for `$NAME` or `${NAME}` markers, resolves each
// via getenv (matching the module's existing pattern), and emits
// the empty string for unresolved names.

static bool
is_env_var_char(char c, bool first)
{
    if (c == '_') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (!first && c >= '0' && c <= '9') return true;
    return false;
}

static n00b_string_t *
expand_env_vars_impl(n00b_string_t *in)
{
    if (in == nullptr || in->u8_bytes == 0) {
        return in;
    }

    // Scratch buffer: worst-case the result is bounded by
    // (input bytes) + (sum of env-var values). We don't know the
    // upper bound a priori; build with a power-of-two grow.
    size_t cap = (size_t)in->u8_bytes * 2 + 16;
    char  *buf = n00b_alloc_array(char, cap);
    size_t off = 0;

    for (size_t i = 0; i < (size_t)in->u8_bytes; ) {
        char c = in->data[i];

        if (c != '$') {
            if (off + 1 >= cap) {
                cap *= 2;
                char *nb = n00b_alloc_array(char, cap);
                memcpy(nb, buf, off);
                buf = nb;
            }
            buf[off++] = c;
            i++;
            continue;
        }

        // `$` — try to parse a name.
        size_t name_start;
        size_t name_end;
        size_t consumed;
        bool   braced = false;

        if (i + 1 < (size_t)in->u8_bytes && in->data[i + 1] == '{') {
            // ${NAME}
            braced     = true;
            name_start = i + 2;
            name_end   = name_start;
            while (name_end < (size_t)in->u8_bytes
                   && in->data[name_end] != '}') {
                name_end++;
            }
            if (name_end >= (size_t)in->u8_bytes) {
                // No closing brace; emit `$` literally and continue.
                if (off + 1 >= cap) {
                    cap *= 2;
                    char *nb = n00b_alloc_array(char, cap);
                    memcpy(nb, buf, off);
                    buf = nb;
                }
                buf[off++] = '$';
                i++;
                continue;
            }
            consumed = (name_end - i) + 1; // include closing `}`
        }
        else {
            // $NAME — accept [A-Za-z_][A-Za-z0-9_]*.
            name_start = i + 1;
            if (name_start >= (size_t)in->u8_bytes
                || !is_env_var_char(in->data[name_start], true)) {
                // Lone `$` — emit literally.
                if (off + 1 >= cap) {
                    cap *= 2;
                    char *nb = n00b_alloc_array(char, cap);
                    memcpy(nb, buf, off);
                    buf = nb;
                }
                buf[off++] = '$';
                i++;
                continue;
            }
            name_end = name_start + 1;
            while (name_end < (size_t)in->u8_bytes
                   && is_env_var_char(in->data[name_end], false)) {
                name_end++;
            }
            consumed = name_end - i;
        }

        // Look up the env var and emit its value.
        size_t name_len = name_end - name_start;
        char   name_buf[256];
        if (name_len < sizeof(name_buf)) {
            memcpy(name_buf, in->data + name_start, name_len);
            name_buf[name_len] = '\0';
            const char *val = getenv(name_buf);
            if (val != nullptr) {
                size_t val_len = strlen(val);
                while (off + val_len + 1 >= cap) {
                    cap *= 2;
                    char *nb = n00b_alloc_array(char, cap);
                    memcpy(nb, buf, off);
                    buf = nb;
                }
                memcpy(buf + off, val, val_len);
                off += val_len;
            }
            // Unset / unknown → emit nothing (POSIX-shell convention).
        }
        // name_len >= sizeof(name_buf): pathological; emit nothing.

        i += consumed;
        (void)braced;
    }

    buf[off] = '\0';
    return n00b_string_from_raw(buf, (int64_t)off);
}

n00b_string_t *
_n00b_path_canonical(n00b_string_t *p) _kargs
{
    bool expand_env_vars  = true;
    bool expand_tilde     = true;
    bool make_absolute    = true;
    bool resolve_symlinks = false;
}
{
    if (p == nullptr) {
        return nullptr;
    }

    n00b_string_t *cur = p;

    if (expand_env_vars) {
        cur = expand_env_vars_impl(cur);
    }

    if (expand_tilde && cur != nullptr && cur->u8_bytes > 0
        && cur->data[0] == '~') {
        cur = n00b_path_tilde_expand(cur);
    }

    if (make_absolute && cur != nullptr && cur->u8_bytes > 0
        && cur->data[0] != '/') {
        cur = n00b_path_simple_join(n00b_get_current_directory(), cur);
    }

    if (resolve_symlinks && cur != nullptr) {
        char  buf[PATH_MAX + 1];
        char *r = realpath(cur->data, buf);
        if (r != nullptr) {
            cur = n00b_string_from_cstr(r);
        }
        // realpath failure (e.g., path doesn't exist): preserve cur.
    }

    return cur;
}

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

    // Canonical idiom for by-pointer return: populate a fully
    // scan-info-threaded lvalue first, then struct-copy into the
    // heap-allocated return shell at the end.
    n00b_list_t(n00b_string_t *) lst = n00b_list_new(n00b_string_t *);

    if (dirent) {
        if (extension && extension->codepoints && extension->data[0] != '.') {
            extension = n00b_cformat(".«#»", extension);
        }

        while (true) {
            struct dirent *entry = readdir(dirent);

            if (!entry) {
                closedir(dirent);
                break;
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

            n00b_list_push(lst, full_path ? full : fname);
        }
    }

    n00b_list_t(n00b_string_t *) *result =
        n00b_alloc(n00b_list_t(n00b_string_t *));
    *result = lst;
    return result;
}
