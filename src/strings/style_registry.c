#include "strings/style_registry.h"
#include "strings/rich_desc.h"
#include "core/alloc.h"
#include "core/gc.h"
#include "core/hash.h"
#include "core/rt_access.h"
#include <string.h>
#include <assert.h>

// We store two dicts in the runtime: one for named styles, one for roles.
// Both are string-keyed (via n00b_hash_cstring) and store n00b_text_style_t *.

static n00b_dict_untyped_t *style_dict = nullptr;
static n00b_dict_untyped_t *role_dict  = nullptr;

// ===================================================================
// Internal helpers
// ===================================================================

static n00b_dict_untyped_t *
new_string_dict(void)
{
    n00b_dict_untyped_t *d = n00b_alloc(n00b_dict_untyped_t);
    n00b_dict_untyped_init(d, .hash = n00b_hash_cstring);
    return d;
}

static void
register_into(n00b_dict_untyped_t *d, const char *name,
              const n00b_text_style_t *style)
{
    n00b_text_style_t *copy = n00b_str_style_copy(style);
    n00b_dict_untyped_put(d, name, copy);
}

static n00b_text_style_t *
lookup_from(n00b_dict_untyped_t *d, const char *name)
{
    if (!d) {
        return nullptr;
    }

    bool  found;
    void *val = n00b_dict_untyped_get(d, name, &found);
    return found ? (n00b_text_style_t *)val : nullptr;
}

// ===================================================================
// Default registrations
// ===================================================================

static void
register_defaults(void)
{
    // --- Named styles ---
    n00b_text_style_t *em = n00b_str_style_new();
    em->italic            = N00B_TRI_YES;
    n00b_str_style_register("em", em);
    n00b_str_style_register("em1", em);

    n00b_text_style_t *em2 = n00b_str_style_new();
    em2->bold              = N00B_TRI_YES;
    n00b_str_style_register("em2", em2);

    n00b_text_style_t *em3 = n00b_str_style_new();
    em3->bold              = N00B_TRI_YES;
    em3->italic            = N00B_TRI_YES;
    n00b_str_style_register("em3", em3);

    n00b_text_style_t *h1 = n00b_str_style_new();
    h1->bold              = N00B_TRI_YES;
    h1->text_case         = N00B_TEXT_CASE_UPPER;
    n00b_str_style_register("h1", h1);

    n00b_text_style_t *h2 = n00b_str_style_new();
    h2->bold              = N00B_TRI_YES;
    n00b_str_style_register("h2", h2);

    n00b_text_style_t *h3 = n00b_str_style_new();
    h3->bold              = N00B_TRI_YES;
    h3->italic            = N00B_TRI_YES;
    n00b_str_style_register("h3", h3);

    n00b_free(em);
    n00b_free(em2);
    n00b_free(em3);
    n00b_free(h1);
    n00b_free(h2);
    n00b_free(h3);

    // --- Text roles ---
    n00b_text_style_t *code = n00b_str_style_new();
    code->font_hint         = N00B_FONT_MONO;
    n00b_str_role_register("@code", code);
    n00b_str_role_register("@mono", code);

    n00b_text_style_t *heading = n00b_str_style_new();
    heading->bold              = N00B_TRI_YES;
    n00b_str_role_register("@heading", heading);

    n00b_text_style_t *body = n00b_str_style_new();
    n00b_str_role_register("@body", body);

    n00b_text_style_t *error = n00b_str_style_new();
    error->bold               = N00B_TRI_YES;
    n00b_str_role_register("@error", error);

    n00b_text_style_t *success = n00b_str_style_new();
    success->bold               = N00B_TRI_YES;
    n00b_str_role_register("@success", success);

    n00b_text_style_t *muted = n00b_str_style_new();
    muted->dim                = N00B_TRI_YES;
    n00b_str_role_register("@muted", muted);

    n00b_text_style_t *link = n00b_str_style_new();
    link->underline          = N00B_TRI_YES;
    n00b_str_role_register("@link", link);

    n00b_text_style_t *label = n00b_str_style_new();
    label->bold               = N00B_TRI_YES;
    n00b_str_role_register("@label", label);

    n00b_text_style_t *button = n00b_str_style_new();
    button->bold               = N00B_TRI_YES;
    button->reverse            = N00B_TRI_YES;
    n00b_str_role_register("@button", button);

    n00b_text_style_t *input = n00b_str_style_new();
    input->underline          = N00B_TRI_YES;
    n00b_str_role_register("@input", input);

    n00b_free(code);
    n00b_free(heading);
    n00b_free(body);
    n00b_free(error);
    n00b_free(success);
    n00b_free(muted);
    n00b_free(link);
    n00b_free(label);
    n00b_free(button);
    n00b_free(input);

    // --- Hexdump styles ---
    n00b_text_style_t *hd_offset = n00b_str_style_new();
    hd_offset->dim               = N00B_TRI_YES;
    n00b_str_style_register("hexdump.offset", hd_offset);

    n00b_text_style_t *hd_ascii = n00b_str_style_new();
    hd_ascii->bold               = N00B_TRI_YES;
    n00b_str_style_register("hexdump.ascii", hd_ascii);

    n00b_free(hd_offset);
    n00b_free(hd_ascii);
}

// ===================================================================
// Public API
// ===================================================================

void
n00b_str_registry_init(void)
{
    style_dict = new_string_dict();
    role_dict  = new_string_dict();

    n00b_gc_register_root(style_dict);
    n00b_gc_register_root(role_dict);

    register_defaults();
    n00b_rich_desc_cache_init();
}

void
n00b_str_style_register(const char *name, const n00b_text_style_t *style)
{
    assert(style_dict && "n00b_str_registry_init() must be called first");
    register_into(style_dict, name, style);
}

n00b_option_t(n00b_text_style_t *)
n00b_str_style_lookup(const char *name)
{
    return n00b_option_from_nullable(n00b_text_style_t *, lookup_from(style_dict, name));
}

void
n00b_str_role_register(const char *name, const n00b_text_style_t *style)
{
    assert(role_dict && "n00b_str_registry_init() must be called first");
    register_into(role_dict, name, style);
}

n00b_option_t(n00b_text_style_t *)
n00b_str_role_lookup(const char *name)
{
    return n00b_option_from_nullable(n00b_text_style_t *, lookup_from(role_dict, name));
}
