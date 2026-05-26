/**
 * @file style_registry_defaults.c
 * @brief Static dict-literal defaults for the rich-string style registry.
 *
 * This translation unit is compiled with `--ncc-static-init-helper=PATH`
 * (it lives in `n00b_dict_aware_src`) so the two `d{...}` literals
 * below are lowered by ncc and emitted as static dict images by the
 * helper.
 *
 * The bootstrap libn00b (linked into the static-init helper executable)
 * cannot itself depend on the helper, so it ships a different
 * translation unit (`style_registry_defaults_stub.c`) that satisfies
 * the same `n00b_str_registry_install_defaults` symbol with a no-op.
 * The helper never renders rich text, so the empty default set is
 * functionally correct there; the full libn00b consumed by
 * applications and tests gets the dict-literal version below.
 *
 * Behavior is identical to the original procedural `register_defaults`
 * in `style_registry.c`: same style names, same field values.  The
 * runtime mutable dicts (`style_dict`, `role_dict` in
 * `style_registry.c`) copy each style via `n00b_str_style_copy` on
 * registration, so the static const templates here are never mutated.
 */

#include "text/strings/style_registry.h"
#include "text/strings/text_style.h"
#include "text/strings/style_ops.h"
#include "core/string.h"
#include "adt/dict.h"

// ===================================================================
// Static defaults — one `static const n00b_text_style_t` per template.
//
// `n00b_text_style_t` is "all-zero except for the int8 palette/font
// indices which use -1 as the unset sentinel".  Each definition below
// initializes the -1 sentinels explicitly so the templates round-trip
// through `n00b_str_style_is_empty` exactly the same way the old
// `n00b_str_style_new()`-built heap templates did.  Tristate /
// text_case / font_hint fields default to zero, which matches
// `N00B_TRI_UNSPECIFIED` / `N00B_TEXT_CASE_NONE` / `N00B_FONT_DEFAULT`
// respectively.
// ===================================================================

#define N00B_STYLE_DEFAULTS                                            \
    .font_index    = -1,                                               \
    .fg_palette_ix = -1,                                               \
    .bg_palette_ix = -1

// --- Named styles ---
static const n00b_text_style_t style_em = {
    N00B_STYLE_DEFAULTS,
    .italic = N00B_TRI_YES,
};

static const n00b_text_style_t style_em2 = {
    N00B_STYLE_DEFAULTS,
    .bold = N00B_TRI_YES,
};

static const n00b_text_style_t style_em3 = {
    N00B_STYLE_DEFAULTS,
    .bold   = N00B_TRI_YES,
    .italic = N00B_TRI_YES,
};

static const n00b_text_style_t style_h1 = {
    N00B_STYLE_DEFAULTS,
    .bold      = N00B_TRI_YES,
    .text_case = N00B_TEXT_CASE_UPPER,
};

static const n00b_text_style_t style_h2 = {
    N00B_STYLE_DEFAULTS,
    .bold = N00B_TRI_YES,
};

static const n00b_text_style_t style_h3 = {
    N00B_STYLE_DEFAULTS,
    .bold   = N00B_TRI_YES,
    .italic = N00B_TRI_YES,
};

static const n00b_text_style_t style_hd_offset = {
    N00B_STYLE_DEFAULTS,
    .dim = N00B_TRI_YES,
};

static const n00b_text_style_t style_hd_ascii = {
    N00B_STYLE_DEFAULTS,
    .bold = N00B_TRI_YES,
};

// --- Roles ---
static const n00b_text_style_t style_code = {
    N00B_STYLE_DEFAULTS,
    .font_hint = N00B_FONT_MONO,
};

static const n00b_text_style_t style_heading = {
    N00B_STYLE_DEFAULTS,
    .bold = N00B_TRI_YES,
};

static const n00b_text_style_t style_body = {
    N00B_STYLE_DEFAULTS,
};

static const n00b_text_style_t style_error = {
    N00B_STYLE_DEFAULTS,
    .bold = N00B_TRI_YES,
};

static const n00b_text_style_t style_success = {
    N00B_STYLE_DEFAULTS,
    .bold = N00B_TRI_YES,
};

static const n00b_text_style_t style_muted = {
    N00B_STYLE_DEFAULTS,
    .dim = N00B_TRI_YES,
};

static const n00b_text_style_t style_link = {
    N00B_STYLE_DEFAULTS,
    .underline = N00B_TRI_YES,
};

static const n00b_text_style_t style_label = {
    N00B_STYLE_DEFAULTS,
    .bold = N00B_TRI_YES,
};

static const n00b_text_style_t style_button = {
    N00B_STYLE_DEFAULTS,
    .bold    = N00B_TRI_YES,
    .reverse = N00B_TRI_YES,
};

static const n00b_text_style_t style_input = {
    N00B_STYLE_DEFAULTS,
    .underline = N00B_TRI_YES,
};

// ===================================================================
// Static dict literals — r-string keys → const-style pointers.
//
// These dicts are populated by the build-time helper (WP-011) and
// iterated exactly once at init time; they are NEVER queried at
// runtime, so content-based hashing is not configured here.  The
// runtime mutable dicts in `style_registry.c` (`style_dict`,
// `role_dict`) are what answer lookups, and they remain string-keyed
// via `n00b_hash_cstring` as before.
// ===================================================================

static n00b_dict_t(n00b_string_t *, const n00b_text_style_t *)
    builtin_named_styles = d{
        r"em":             &style_em,
        r"em1":            &style_em,
        r"em2":            &style_em2,
        r"em3":            &style_em3,
        r"h1":             &style_h1,
        r"h2":             &style_h2,
        r"h3":             &style_h3,
        r"hexdump.offset": &style_hd_offset,
        r"hexdump.ascii":  &style_hd_ascii,
    };

static n00b_dict_t(n00b_string_t *, const n00b_text_style_t *)
    builtin_roles = d{
        r"@code":    &style_code,
        r"@mono":    &style_code,
        r"@heading": &style_heading,
        r"@body":    &style_body,
        r"@error":   &style_error,
        r"@success": &style_success,
        r"@muted":   &style_muted,
        r"@link":    &style_link,
        r"@label":   &style_label,
        r"@button":  &style_button,
        r"@input":   &style_input,
    };

// ===================================================================
// Defaults installer — invoked from `n00b_str_registry_init` in
// `style_registry.c` after the runtime mutable dicts are constructed.
// ===================================================================

void
n00b_str_registry_install_defaults(void)
{
    n00b_dict_foreach(&builtin_named_styles, key, value, {
        n00b_str_style_register((const char *)key->data, value);
    });

    n00b_dict_foreach(&builtin_roles, key, value, {
        n00b_str_role_register((const char *)key->data, value);
    });
}
