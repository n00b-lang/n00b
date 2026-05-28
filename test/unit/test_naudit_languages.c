/*
 * WP-009 Phase 1 regression test — built-in language registry.
 *
 * Exercises four scenarios against
 * `naudit/languages.h`:
 *
 *   1. `lookup_language_by_name(r"c")` returns the C descriptor.
 *   2. `lookup_language_by_extension(r".c", nullptr)` returns the
 *      C descriptor (built-in default).
 *   3. `lookup_language_by_extension(r".cc", nullptr)` returns
 *      nullptr (`.cc` is not in C's built-in defaults).
 *   4. With a project override mapping `.cc` → `c`, the same
 *      lookup returns the C descriptor.
 *
 * Plus a smoke check on `n00b_naudit_all_languages` and a
 * tokenizer-registry lookup ensuring the `"c"` tokenizer
 * triple is registered.
 *
 * Bootstrap shape mirrors test_naudit_module.c per the relaxed
 * test convention — libc <assert.h> + <stdio.h> are allowed for
 * harness scaffolding (NCC.md "NO LIBC ALLOWED" exemption for
 * test files).
 */

#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"
#include "adt/list.h"
#include "adt/dict.h"
#include "text/strings/string_ops.h"

#include "naudit/naudit.h"
#include "naudit/languages.h"
#include "naudit/tokenizer_registry.h"

static void
test_lookup_by_name(void)
{
    n00b_naudit_language_info_t *row =
        n00b_naudit_lookup_language_by_name(r"c");
    assert(!!row);
    assert(!!row->name);
    assert(n00b_unicode_str_eq(row->name, r"c"));
    assert(!!row->grammar_path);
    assert(row->grammar_path->u8_bytes > 0);
    assert(!!row->tokenizer_name);
    assert(n00b_unicode_str_eq(row->tokenizer_name, r"c"));
    assert(!!row->default_extensions);
    assert(n00b_list_len(*row->default_extensions) >= 2);
    printf("  [PASS] lookup_language_by_name(c)\n");

    /* Unknown name returns nullptr. */
    n00b_naudit_language_info_t *miss =
        n00b_naudit_lookup_language_by_name(r"definitely_not_a_language");
    assert(!miss);
    printf("  [PASS] lookup_language_by_name(unknown) -> nullptr\n");
}

static void
test_lookup_by_extension_default(void)
{
    n00b_naudit_language_info_t *row_c =
        n00b_naudit_lookup_language_by_extension(r".c", nullptr);
    assert(!!row_c);
    assert(n00b_unicode_str_eq(row_c->name, r"c"));

    n00b_naudit_language_info_t *row_h =
        n00b_naudit_lookup_language_by_extension(r".h", nullptr);
    assert(!!row_h);
    assert(n00b_unicode_str_eq(row_h->name, r"c"));
    /* Both must point at the same registry row. */
    assert(row_c == row_h);
    printf("  [PASS] lookup_language_by_extension(.c, .h) -> c\n");

    /* `.cc` is NOT a built-in C default. */
    n00b_naudit_language_info_t *row_cc =
        n00b_naudit_lookup_language_by_extension(r".cc", nullptr);
    assert(!row_cc);
    printf("  [PASS] lookup_language_by_extension(.cc, nullptr) -> nullptr\n");
}

static void
test_lookup_by_extension_override(void)
{
    /*
     * Build a project-override dict mapping `.cc` to `c`. The
     * lookup should now return the C descriptor for `.cc`.
     */
    n00b_dict_t(n00b_string_t *, n00b_string_t *) *overrides =
        n00b_alloc(n00b_dict_t(n00b_string_t *, n00b_string_t *));
    n00b_dict_init(overrides,
                   .hash          = n00b_string_hash,
                   .skip_obj_hash = true);
    n00b_string_t *ext  = r".cc";
    n00b_string_t *lang = r"c";
    n00b_dict_put(overrides, ext, lang);

    n00b_naudit_language_info_t *row =
        n00b_naudit_lookup_language_by_extension(r".cc", overrides);
    assert(!!row);
    assert(n00b_unicode_str_eq(row->name, r"c"));
    printf("  [PASS] lookup_language_by_extension(.cc, overrides=.cc->c) -> c\n");

    /* Override wins over a built-in match too: re-map `.c` to a
     * known language (still `c` here — the override is a no-op
     * but exercises the override-precedence path). */
    n00b_string_t *ext2 = r".c";
    n00b_dict_put(overrides, ext2, lang);
    n00b_naudit_language_info_t *row2 =
        n00b_naudit_lookup_language_by_extension(r".c", overrides);
    assert(!!row2);
    assert(n00b_unicode_str_eq(row2->name, r"c"));
    printf("  [PASS] lookup_language_by_extension(.c, overrides) -> c\n");
}

static void
test_all_languages(void)
{
    n00b_list_t(n00b_naudit_language_info_t *) *all =
        n00b_naudit_all_languages();
    assert(!!all);
    int64_t n = n00b_list_len(*all);
    assert(n >= 1);

    /* At least one entry whose name == r"c" must be present. */
    bool saw_c = false;
    for (int64_t i = 0; i < n; i++) {
        n00b_naudit_language_info_t *row = n00b_list_get(*all, i);
        if (row && row->name && n00b_unicode_str_eq(row->name, r"c")) {
            saw_c = true;
        }
    }
    assert(saw_c);
    printf("  [PASS] all_languages includes c (n=%lld)\n",
           (long long)n);
}

static void
test_tokenizer_registry(void)
{
    n00b_naudit_tokenizer_info_t *tok =
        n00b_naudit_lookup_tokenizer(r"c");
    assert(!!tok);
    assert(!!tok->name);
    assert(n00b_unicode_str_eq(tok->name, r"c"));
    assert(!!tok->scan_cb);
    assert(!!tok->state_new);
    /* `reset_cb` may legitimately be nullptr for some tokenizers,
     * but the C tokenizer registers a real reset. */
    assert(!!tok->reset_cb);
    /* Smoke: invoking state_new returns non-null. */
    void *state = tok->state_new();
    assert(!!state);
    printf("  [PASS] lookup_tokenizer(c) returns full triple\n");

    n00b_naudit_tokenizer_info_t *miss =
        n00b_naudit_lookup_tokenizer(r"definitely_not_a_tokenizer");
    assert(!miss);
    printf("  [PASS] lookup_tokenizer(unknown) -> nullptr\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    bool ok = n00b_audit_module_init();
    assert(ok);

    test_lookup_by_name();
    test_lookup_by_extension_default();
    test_lookup_by_extension_override();
    test_all_languages();
    test_tokenizer_registry();

    printf("All n00b-audit WP-009 Phase 1 language registry checks passed.\n");
    return 0;
}
