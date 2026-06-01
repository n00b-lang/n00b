#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include "compiler/objfile/bstream.h"
#include "compiler/objfile/elf.h"

#include "objfile_elf_casegen.h"

static bool
parse_succeeds(n00b_buffer_t *buf, n00b_elf_binary_t **out)
{
    n00b_bstream_t *stream = n00b_bstream_new(buf);
    auto            result = n00b_elf_parse(stream);

    if (n00b_result_is_err(result)) {
        if (out != nullptr) {
            *out = nullptr;
        }
        return false;
    }

    if (out != nullptr) {
        *out = n00b_result_get(result);
    }
    return true;
}

static void
check_known_case(const n00b_test_elf_case_t *test_case)
{
    n00b_buffer_t *buf = n00b_test_elf_case_generate(test_case);
    assert(buf != nullptr);

    n00b_elf_binary_t *bin = nullptr;
    bool               ok  = parse_succeeds(buf, &bin);

    switch (test_case->expect_parse) {
    case N00B_TEST_ELF_PARSE_OK:
        assert(ok);
        assert(bin != nullptr);
        break;
    case N00B_TEST_ELF_PARSE_REJECT:
        assert(!ok);
        assert(bin == nullptr);
        break;
    }

    printf("  [PASS] %s (%s)\n",
           test_case->name,
           test_case->expect_reason);
}

static void
test_known_answers(void)
{
    size_t count = 0;

    for (size_t i = 0; i < n00b_test_elf_case_count; i++) {
        const n00b_test_elf_case_t *test_case = &n00b_test_elf_cases[i];

        if (test_case->state != N00B_TEST_ELF_CASE_KNOWN) {
            continue;
        }

        check_known_case(test_case);
        count++;
    }

    assert(count > 0);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running ELF known-answer tests...\n");
    test_known_answers();
    printf("All ELF known-answer tests passed.\n");
    return 0;
}
