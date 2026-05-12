// Smoke test for the TOML parser.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "parsers/toml.h"

static void
check_parse(const char *src, bool expect_ok, const char *label)
{
    n00b_string_t *src_s = n00b_string_from_cstr(src);
    auto r = n00b_toml_parse(src_s);
    bool ok = n00b_result_is_ok(r);
    if (ok != expect_ok) {
        printf("FAIL: %s — expect_ok=%d, got_ok=%d\n",
               label, expect_ok, ok);
        if (!ok) {
            n00b_string_t *err = n00b_toml_last_error();
            printf("      error: %s\n", err ? err->data : "(none)");
        }
        assert(false);
    }
    printf("PASS: %s\n", label);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    // Empty doc.
    check_parse("", true, "empty");

    // Simple key=value.
    check_parse("name = \"foo\"\n", true, "simple-string");
    check_parse("count = 42\n", true, "simple-int");
    check_parse("flag = true\n", true, "simple-bool");

    // Array.
    check_parse("xs = [1, 2, 3]\n", true, "array-int");
    check_parse("xs = [\"a\", \"b\"]\n", true, "array-string");

    // Multi-line basic string.
    check_parse("p = \"\"\"line1\\nline2\"\"\"\n", true, "multiline-basic");

    // Literal string.
    check_parse("p = '\\d+'\n", true, "literal-string");

    // Array-of-tables.
    check_parse("[[test]]\nname = \"a\"\n[[test]]\nname = \"b\"\n",
                true, "array-of-tables");

    // Round-trip an array-of-tables and read values back.
    {
        n00b_string_t *src = n00b_string_from_cstr(
            "[[test]]\n"
            "name = \"hello\"\n"
            "pattern = \"a+\"\n"
            "[[test]]\n"
            "name = \"world\"\n"
            "pattern = \"b+\"\n");
        auto r = n00b_toml_parse(src);
        assert(n00b_result_is_ok(r));
        n00b_toml_node_t *root = n00b_result_get(r);
        n00b_toml_node_t *arr  = n00b_toml_table_array_of(root, "test");
        assert(arr != nullptr);
        assert(n00b_toml_array_len(arr) == 2);

        n00b_toml_node_t *t0 = n00b_toml_array_get(arr, 0);
        n00b_toml_node_t *name0 = n00b_toml_table_get_cstr(t0, "name");
        n00b_toml_node_t *pat0  = n00b_toml_table_get_cstr(t0, "pattern");
        assert(name0 && n00b_toml_type(name0) == N00B_TOML_STRING);
        assert(pat0  && n00b_toml_type(pat0)  == N00B_TOML_STRING);
        assert(strcmp(n00b_toml_as_string(name0)->data, "hello") == 0);
        assert(strcmp(n00b_toml_as_string(pat0)->data,  "a+")    == 0);
        printf("PASS: aot-readback\n");
    }

    // Parse error reports.
    check_parse("key =\n", false, "missing-value");
    check_parse("[unterminated\n", false, "unterminated-header");

    n00b_shutdown();
    printf("toml smoke: all OK\n");
    return 0;
}
