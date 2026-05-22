/**
 * @file path.h
 * @brief Filesystem path utilities — resolution, joining, walking, temp files.
 */

#pragma once

#include "core/alloc.h"
#include "adt/list.h"
#include "adt/array.h"
#include "adt/option.h"
#include "adt/result.h"
#include "text/strings/string_ops.h"

#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>
#include <stdlib.h>

typedef enum {
    N00B_FK_NOT_FOUND       = 0,
    N00B_FK_IS_REG_FILE     = S_IFREG,
    N00B_FK_IS_DIR          = S_IFDIR,
    N00B_FK_IS_FLINK        = S_IFLNK,
    N00B_FK_IS_DLINK        = S_IFLNK | S_IFDIR,
    N00B_FK_IS_SOCK         = S_IFSOCK,
    N00B_FK_IS_CHR_DEVICE   = S_IFCHR,
    N00B_FK_IS_BLOCK_DEVICE = S_IFBLK,
    N00B_FK_IS_FIFO         = S_IFIFO,
    N00B_FK_OTHER           = ~0,
} n00b_file_kind;

extern n00b_string_t *n00b_resolve_path(n00b_string_t *s);
extern n00b_string_t *n00b_path_tilde_expand(n00b_string_t *in);
extern n00b_string_t *n00b_get_user_dir(n00b_string_t *user);
extern n00b_string_t *n00b_get_current_directory(void);
extern bool           n00b_set_current_directory(n00b_string_t *s);
extern n00b_string_t *n00b_path_join(n00b_list_t(n00b_string_t *) *items);

/**
 * @brief Join a path from a typed-variadic argument tail.
 *
 * Ergonomic variadic builder over @c n00b_path_join. Accepts a
 * required leading component @p first followed by zero or more
 * additional `n00b_string_t *` pieces. Pieces are joined with `/`
 * separators per the same semantics as @c n00b_path_simple_join:
 * an absolute piece (one whose first byte is `/`) re-roots the
 * result.
 *
 * @param first  First path component (required; must be non-null).
 * @param ...    Additional `n00b_string_t *` pieces.
 *
 * @return A newly-allocated string carrying the joined path. Empty
 *         pieces are skipped; an absolute piece anywhere in the tail
 *         re-roots the join. With only @p first and no variadic
 *         tail, returns @p first verbatim.
 */
extern n00b_string_t *
n00b_path_join_v(n00b_string_t *first, n00b_string_t * +);

/**
 * @brief Resolve the XDG Base Directory `XDG_CONFIG_HOME` per the
 *        freedesktop.org spec.
 *
 * Returns the value of `$XDG_CONFIG_HOME` if set and non-empty;
 * otherwise returns `$HOME/.config`. Per the spec, an empty
 * `$XDG_CONFIG_HOME` is treated identically to unset.
 *
 * @return Spec-compliant config home (no trailing slash). Falls
 *         back to `/.config` if both `$XDG_CONFIG_HOME` and `$HOME`
 *         are unset/empty (an edge case the spec does not address).
 */
extern n00b_string_t *n00b_xdg_config_home(void);

/**
 * @brief Resolve the XDG Base Directory `XDG_DATA_HOME` per spec.
 *
 * Returns `$XDG_DATA_HOME` if set and non-empty; otherwise
 * `$HOME/.local/share`. Empty `$XDG_DATA_HOME` is treated as unset.
 *
 * @return Spec-compliant data home (no trailing slash).
 */
extern n00b_string_t *n00b_xdg_data_home(void);

/**
 * @brief Resolve the XDG Base Directory `XDG_CACHE_HOME` per spec.
 *
 * Returns `$XDG_CACHE_HOME` if set and non-empty; otherwise
 * `$HOME/.cache`. Empty `$XDG_CACHE_HOME` is treated as unset.
 *
 * @return Spec-compliant cache home (no trailing slash).
 */
extern n00b_string_t *n00b_xdg_cache_home(void);

/**
 * @brief Resolve the XDG Base Directory `XDG_STATE_HOME` per spec.
 *
 * Returns `$XDG_STATE_HOME` if set and non-empty; otherwise
 * `$HOME/.local/state`. Empty `$XDG_STATE_HOME` is treated as
 * unset.
 *
 * @return Spec-compliant state home (no trailing slash).
 */
extern n00b_string_t *n00b_xdg_state_home(void);

/**
 * @brief Resolve `XDG_RUNTIME_DIR` per spec.
 *
 * Unlike the `*_HOME` variants the spec defines no fallback for
 * runtime dir. Callers decide how to handle absence.
 *
 * @return The value of `$XDG_RUNTIME_DIR` if set and non-empty,
 *         else `nullptr`.
 */
extern n00b_string_t *n00b_xdg_runtime_dir(void);

/**
 * @brief Build a path under `$XDG_CONFIG_HOME/<app>/...`.
 *
 * Composes the XDG config base with @p app and the variadic
 * trailing pieces. Equivalent to
 * `n00b_path_join_v(n00b_xdg_config_home(), app, ...)`.
 *
 * @param app  Application namespace (required; must be non-null
 *             and non-empty).
 * @param ...  Additional `n00b_string_t *` path pieces.
 *
 * @return Joined path (no trailing slash).
 */
extern n00b_string_t *
n00b_xdg_config_path(n00b_string_t *app, n00b_string_t * +);

/**
 * @brief Build a path under `$XDG_DATA_HOME/<app>/...`.
 *
 * @param app  Application namespace.
 * @param ...  Additional `n00b_string_t *` path pieces.
 */
extern n00b_string_t *
n00b_xdg_data_path(n00b_string_t *app, n00b_string_t * +);

/**
 * @brief Build a path under `$XDG_CACHE_HOME/<app>/...`.
 *
 * @param app  Application namespace.
 * @param ...  Additional `n00b_string_t *` path pieces.
 */
extern n00b_string_t *
n00b_xdg_cache_path(n00b_string_t *app, n00b_string_t * +);

/**
 * @brief Build a path under `$XDG_STATE_HOME/<app>/...`.
 *
 * @param app  Application namespace.
 * @param ...  Additional `n00b_string_t *` path pieces.
 */
extern n00b_string_t *
n00b_xdg_state_path(n00b_string_t *app, n00b_string_t * +);

/**
 * @brief Build a path under `$XDG_RUNTIME_DIR/<app>/...`.
 *
 * Returns `nullptr` when `$XDG_RUNTIME_DIR` is unset/empty (the
 * spec mandates no fallback).
 *
 * @param app  Application namespace.
 * @param ...  Additional `n00b_string_t *` path pieces.
 *
 * @return Joined path, or `nullptr` if no runtime dir is defined.
 */
extern n00b_string_t *
n00b_xdg_runtime_path(n00b_string_t *app, n00b_string_t * +);

/**
 * @brief Combined path-canonicalization with composable steps.
 *
 * Applies the following transformations in order:
 *   1. `$VAR` / `${VAR}` environment-variable expansion
 *      (controlled by @p expand_env_vars).
 *   2. Leading `~` / `~user` home-directory expansion
 *      (controlled by @p expand_tilde).
 *   3. Absolute-path rooting via the current working directory
 *      (controlled by @p make_absolute).
 *   4. Optional `realpath()` symlink resolution
 *      (controlled by @p resolve_symlinks).
 *
 * @param p  Input path. May contain env-var references, a leading
 *           tilde, and `.`/`..` components.
 *
 * @kw expand_env_vars   Expand `$VAR` / `${VAR}` (default: true).
 *                       Unknown variables expand to the empty
 *                       string.
 * @kw expand_tilde      Expand a leading `~` / `~user` to the
 *                       associated home directory (default: true).
 * @kw make_absolute     If the path is not yet absolute after
 *                       earlier steps, prefix the cwd (default:
 *                       true).
 * @kw resolve_symlinks  Run `realpath()` on the result, collapsing
 *                       symlinks and `.`/`..` (default: false).
 *
 * @return A new string carrying the canonicalized path. With
 *         @p resolve_symlinks = true and the path missing on disk,
 *         returns the pre-realpath value.
 */
extern n00b_string_t *
_n00b_path_canonical(n00b_string_t *p) _kargs {
    bool expand_env_vars  = true;
    bool expand_tilde     = true;
    bool make_absolute    = true;
    bool resolve_symlinks = false;
};

#define n00b_path_canonical(p, ...) \
    _n00b_path_canonical(p __VA_OPT__(,) __VA_ARGS__)

extern n00b_file_kind n00b_get_file_kind(n00b_string_t *p);

extern n00b_list_t(n00b_string_t *) *
_n00b_path_walk(n00b_string_t *dir) _kargs {
    bool recurse        = true;
    bool yield_links    = false;
    bool yield_dirs     = false;
    bool ignore_special = true;
    bool follow_links   = false;
};

extern n00b_string_t *n00b_app_path(void);
extern n00b_string_t *n00b_path_trim_trailing_slashes(n00b_string_t *s);
extern n00b_result_t(n00b_string_t *) n00b_new_temp_dir(n00b_string_t *prefix,
                                                        n00b_string_t *suffix);
extern n00b_string_t *n00b_get_temp_root(void);
extern n00b_string_t *n00b_filename_from_path(n00b_string_t *s);

extern n00b_list_t(n00b_string_t *) *
n00b_find_file_in_program_path(n00b_string_t *cmd,
                                n00b_list_t(n00b_string_t *) *path_list);

extern n00b_list_t(n00b_string_t *) *
n00b_find_command_paths(n00b_string_t *cmd,
                         n00b_list_t(n00b_string_t *) *path_list,
                         bool self_ok);

extern n00b_result_t(n00b_string_t *) n00b_rename(n00b_string_t *from,
                                                   n00b_string_t *to);
extern n00b_list_t(n00b_string_t *) *n00b_path_parts(n00b_string_t *p);

extern n00b_list_t(n00b_string_t *) *
_n00b_list_directory(n00b_string_t *dir) _kargs {
    n00b_string_t *extension   = nullptr;
    bool           files       = true;
    bool           directories = true;
    bool           links       = true;
    bool           specials    = true;
    bool           full_path   = false;
    bool           dot_files   = true;
};

extern n00b_string_t *n00b_path_get_extension(n00b_string_t *s);
extern n00b_string_t *n00b_path_remove_extension(n00b_string_t *s);
extern void           n00b_path_strip_slashes_both_ends(n00b_string_t *s);
extern n00b_string_t *n00b_path_chop_extension(n00b_string_t *s);

#define n00b_path_walk(x, ...) \
    _n00b_path_walk(x __VA_OPT__(,) __VA_ARGS__)
#define n00b_list_directory(x, ...) \
    _n00b_list_directory(x __VA_OPT__(,) __VA_ARGS__)

static inline bool
n00b_path_exists(n00b_string_t *s)
{
    return n00b_get_file_kind(s) != N00B_FK_NOT_FOUND;
}

static inline bool
n00b_path_is_file(n00b_string_t *s)
{
    switch (n00b_get_file_kind(s)) {
    case N00B_FK_IS_REG_FILE:
    case N00B_FK_IS_FLINK:
        return true;
    default:
        return false;
    }
}

static inline bool
n00b_path_is_directory(n00b_string_t *s)
{
    switch (n00b_get_file_kind(s)) {
    case N00B_FK_IS_DIR:
    case N00B_FK_IS_DLINK:
        return true;
    default:
        return false;
    }
}

static inline bool
n00b_path_is_link(n00b_string_t *s)
{
    switch (n00b_get_file_kind(s)) {
    case N00B_FK_IS_FLINK:
    case N00B_FK_IS_DLINK:
        return true;
    default:
        return false;
    }
}

static inline n00b_string_t *
n00b_get_home_directory(void)
{
    return n00b_get_user_dir(nullptr);
}

static inline bool
n00b_file_exists(n00b_string_t *filename)
{
    struct stat info;
    return stat(filename->data, &info) == 0;
}

static inline n00b_string_t *
n00b_path_simple_join(n00b_string_t *p1, n00b_string_t *p2)
{
    if (p2->u8_bytes && p2->data[0] == '/') {
        return p2;
    }

    if (!p1 || !p1->codepoints) {
        p1 = n00b_string_from_cstr("/");
    }

    n00b_list_t(n00b_string_t *) parts =
        n00b_list_new_private(n00b_string_t *);
    n00b_list_push(parts, p1);
    n00b_list_push(parts, p2);

    return n00b_path_join(&parts);
}

static inline n00b_string_t *
n00b_get_user_name(void)
{
    struct passwd *pw = getpwuid(getuid());
    return n00b_string_from_cstr(pw->pw_name);
}

static inline n00b_list_t(n00b_string_t *) *
n00b_get_program_search_path(void)
{
    const char *path = getenv("PATH");

    n00b_list_t(n00b_string_t *) *result = n00b_alloc(n00b_list_t(n00b_string_t *));
    *result = n00b_list_new(n00b_string_t *);

    if (!path) return result;

    n00b_string_t *ps = n00b_string_from_cstr(path);
    n00b_array_t(n00b_string_t *) parts =
        n00b_unicode_str_split(ps, n00b_string_from_cstr(":"));

    for (size_t i = 0; i < n00b_array_len(parts); i++) {
        n00b_list_push(*result, n00b_array_get(parts, i));
    }

    return result;
}

static inline n00b_option_t(n00b_string_t *)
n00b_find_first_command_path(n00b_string_t *s,
                              n00b_list_t(n00b_string_t *) *l,
                              bool self)
{
    n00b_list_t(n00b_string_t *) *resolved =
        n00b_find_command_paths(s, l, self);

    if (!n00b_list_len(*resolved)) {
        return n00b_option_none(n00b_string_t *);
    }

    return n00b_option_set(n00b_string_t *, n00b_list_get(*resolved, 0));
}
