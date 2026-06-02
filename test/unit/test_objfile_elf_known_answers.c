#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include "adt/option.h"
#include "adt/result.h"
#include "compiler/objfile/bstream.h"
#include "compiler/objfile/elf.h"
#include "compiler/objfile/elf_layout.h"
#include "compiler/objfile/elf_rewrite_admit.h"
#include "util/assert.h"

#include "objfile_elf_casegen.h"

#define N00B_TEST_REQUIRE(expr) n00b_require((expr), #expr)

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
test_divergence_classification_metadata(void)
{
    bool saw_n00b_broader    = false;
    bool saw_shared_scope    = false;
    bool saw_diagnostic_only = false;

    for (size_t i = 0; i < n00b_test_elf_case_count; i++) {
        const n00b_test_elf_case_t *test_case = &n00b_test_elf_cases[i];
        const char *classification =
            n00b_test_elf_divergence_name(test_case->divergence);

        assert(classification != nullptr);
        assert(classification[0] != '\0');

        switch (test_case->divergence) {
        case N00B_TEST_ELF_DIVERGENCE_SHARED_SCOPE:
            saw_shared_scope = true;
            break;
        case N00B_TEST_ELF_DIVERGENCE_N00B_BROADER:
            saw_n00b_broader = true;
            break;
        case N00B_TEST_ELF_DIVERGENCE_BRANDON_NARROWER:
            break;
        case N00B_TEST_ELF_DIVERGENCE_DIAGNOSTIC_ONLY:
            saw_diagnostic_only = true;
            break;
        }
    }

    assert(saw_shared_scope);
    assert(saw_n00b_broader);
    assert(saw_diagnostic_only);
    printf("  [PASS] divergence classification metadata\n");
}

static n00b_elf_layout_t *
layout_for_case(const char *name)
{
    const n00b_test_elf_case_t *test_case = n00b_test_elf_case_by_name(name);
    assert(test_case != nullptr);

    n00b_buffer_t *buf = n00b_test_elf_case_generate(test_case);
    assert(buf != nullptr);

    n00b_elf_binary_t *bin = nullptr;
    assert(parse_succeeds(buf, &bin));
    assert(bin != nullptr);

    auto layout = n00b_elf_layout_build(bin);
    assert(n00b_result_is_ok(layout));
    return n00b_result_get(layout);
}

static size_t
count_file_kind(n00b_elf_layout_t              *layout,
                uint64_t                        start,
                uint64_t                        end,
                n00b_elf_layout_interval_kind_t kind)
{
    auto overlaps = n00b_elf_layout_file_overlaps(layout, start, end);
    assert(n00b_result_is_ok(overlaps));
    n00b_elf_layout_interval_list_t list = n00b_result_get(overlaps);

    size_t count = 0;
    for (uint64_t i = 0; i < list.count; i++) {
        if (list.items[i].kind == kind) {
            count++;
        }
    }

    return count;
}

static size_t
count_vaddr_kind(n00b_elf_layout_t              *layout,
                 uint64_t                        start,
                 uint64_t                        end,
                 n00b_elf_layout_interval_kind_t kind)
{
    auto overlaps = n00b_elf_layout_vaddr_overlaps(layout, start, end);
    assert(n00b_result_is_ok(overlaps));
    n00b_elf_layout_interval_list_t list = n00b_result_get(overlaps);

    size_t count = 0;
    for (uint64_t i = 0; i < list.count; i++) {
        if (list.items[i].kind == kind) {
            count++;
        }
    }

    return count;
}

static n00b_elf_layout_gap_t
require_file_gap(n00b_elf_layout_t *layout,
                 uint64_t           start,
                 uint64_t           end,
                 uint64_t           min_size,
                 uint64_t           alignment)
{
    auto gap = n00b_elf_layout_find_file_gap(layout,
                                             start,
                                             end,
                                             min_size,
                                             alignment);
    assert(n00b_result_is_ok(gap));
    n00b_option_t(n00b_elf_layout_gap_t) gap_opt = n00b_result_get(gap);
    assert(n00b_option_is_set(gap_opt));
    return n00b_option_get(gap_opt);
}

static n00b_elf_rewrite_admit_metadata_request_t
admission_request_make(n00b_test_elf_admission_request_t request)
{
    n00b_elf_rewrite_admit_metadata_request_t admission = {
        .section_name    = r".n00b.test",
        .payload_size    = 16,
        .file_alignment  = 8,
        .section_type    = SHT_PROGBITS,
        .section_flags   = 0,
        .policy          = {
            .flags = N00B_ELF_REWRITE_ADMIT_POLICY_PRESERVE_OVERLAY,
        },
    };

    switch (request) {
    case N00B_TEST_ELF_ADMISSION_STRICT_EOF:
        admission.policy.flags |=
            N00B_ELF_REWRITE_ADMIT_POLICY_STRICT_LOADER_PRESERVATION;
        break;
    case N00B_TEST_ELF_ADMISSION_RELAXED_PREFERRED_GAP:
        admission.preferred_file_offset = n00b_option_set(uint64_t, 288);
        admission.file_alignment        = 16;
        break;
    case N00B_TEST_ELF_ADMISSION_RELAXED_OVERLAY_APPEND:
        admission.policy.flags |=
            N00B_ELF_REWRITE_ADMIT_POLICY_APPEND_AFTER_OVERLAY;
        break;
    case N00B_TEST_ELF_ADMISSION_NONE:
    case N00B_TEST_ELF_ADMISSION_RELAXED_EOF:
    case N00B_TEST_ELF_ADMISSION_RELAXED_OVERLAY_PRESERVE:
        break;
    }

    return admission;
}

static void
check_admission_case(const n00b_test_elf_case_t *test_case,
                     bool                       *saw_phtab_outside_load,
                     bool                       *saw_entry_outside_load,
                     bool                       *saw_entry_memory_only,
                     bool                       *saw_pt_phdr_inconsistent,
                     bool                       *saw_overlay_policy,
                     bool                       *saw_nonzero_gap,
                     bool                       *saw_accepted_placement)
{
    n00b_buffer_t *buf = n00b_test_elf_case_generate(test_case);
    N00B_TEST_REQUIRE(buf != nullptr);

    n00b_elf_binary_t *bin = nullptr;
    N00B_TEST_REQUIRE(parse_succeeds(buf, &bin));
    N00B_TEST_REQUIRE(bin != nullptr);

    n00b_elf_rewrite_admit_metadata_request_t request =
        admission_request_make(test_case->admission_request);
    auto result = n00b_elf_rewrite_admit_metadata_insert(bin, &request);
    N00B_TEST_REQUIRE(n00b_result_is_ok(result));

    n00b_elf_rewrite_admit_result_t admit = n00b_result_get(result);
    N00B_TEST_REQUIRE(admit.outcome == test_case->admission_outcome);
    N00B_TEST_REQUIRE(admit.rejection_reason == test_case->admission_reason);

    n00b_elf_rewrite_admit_placement_kind_t placement_kind =
        N00B_ELF_REWRITE_ADMIT_PLACEMENT_NONE;
    if (n00b_option_is_set(admit.placement)) {
        n00b_elf_rewrite_admit_placement_t placement =
            n00b_option_get(admit.placement);
        placement_kind = placement.kind;
    }

    N00B_TEST_REQUIRE(placement_kind == test_case->admission_placement);

    switch (test_case->admission_reason) {
    case N00B_ELF_REWRITE_ADMIT_REJECT_PHTAB_OUTSIDE_LOAD:
        *saw_phtab_outside_load = true;
        break;
    case N00B_ELF_REWRITE_ADMIT_REJECT_ENTRY_OUTSIDE_LOAD:
        *saw_entry_outside_load = true;
        break;
    case N00B_ELF_REWRITE_ADMIT_REJECT_ENTRY_MEMORY_ONLY:
        *saw_entry_memory_only = true;
        break;
    case N00B_ELF_REWRITE_ADMIT_REJECT_PT_PHDR_INCONSISTENT:
        *saw_pt_phdr_inconsistent = true;
        break;
    case N00B_ELF_REWRITE_ADMIT_REJECT_OVERLAY_POLICY:
        *saw_overlay_policy = true;
        break;
    case N00B_ELF_REWRITE_ADMIT_REJECT_UNKNOWN_NONZERO_BYTES:
        *saw_nonzero_gap = true;
        break;
    case N00B_ELF_REWRITE_ADMIT_REJECT_NONE:
        N00B_TEST_REQUIRE(
            admit.outcome == N00B_ELF_REWRITE_ADMIT_OUTCOME_ACCEPTED);
        N00B_TEST_REQUIRE(test_case->admission_placement
                          != N00B_ELF_REWRITE_ADMIT_PLACEMENT_NONE);
        *saw_accepted_placement = true;
        break;
    default:
        break;
    }
}

static void
test_layout_known_answers(void)
{
    n00b_elf_layout_t *layout = layout_for_case("phtab_not_in_load");
    assert(count_file_kind(layout,
                           64,
                           120,
                           N00B_ELF_LAYOUT_INTERVAL_PHTAB) == 1);
    assert(count_file_kind(layout,
                           64,
                           120,
                           N00B_ELF_LAYOUT_INTERVAL_SEGMENT_FILE) == 0);

    layout = layout_for_case("entry_in_mem_not_file");
    assert(count_file_kind(layout,
                           0x101,
                           0x102,
                           N00B_ELF_LAYOUT_INTERVAL_SEGMENT_FILE) == 0);
    assert(count_vaddr_kind(layout,
                            0x400101,
                            0x400102,
                            N00B_ELF_LAYOUT_INTERVAL_SEGMENT_MEMORY) == 1);

    layout = layout_for_case("pt_phdr_bad_size");
    assert(count_file_kind(layout,
                           64,
                           176,
                           N00B_ELF_LAYOUT_INTERVAL_PHTAB) == 1);

    layout = layout_for_case("overlay_after_segments");
    assert(count_file_kind(layout,
                           512,
                           528,
                           N00B_ELF_LAYOUT_INTERVAL_OVERLAY) == 1);
    n00b_elf_layout_gap_t gap = require_file_gap(layout, 512, 528, 8, 1);
    assert(gap.kind == N00B_ELF_LAYOUT_GAP_OVERLAY);

    layout = layout_for_case("layout_nonzero_unknown");
    gap    = require_file_gap(layout, 288, 320, 4, 16);
    assert(gap.kind == N00B_ELF_LAYOUT_GAP_UNKNOWN_NONZERO);

    layout = layout_for_case("layout_classification");
    assert(count_vaddr_kind(layout,
                            0x400600,
                            0x400620,
                            N00B_ELF_LAYOUT_INTERVAL_SECTION_NOBITS_MEMORY) == 1);
    assert(count_file_kind(layout,
                           576,
                           608,
                           N00B_ELF_LAYOUT_INTERVAL_SECTION_NOBITS_MEMORY) == 0);

    printf("  [PASS] layout known-answer probes\n");
}

static void
test_admission_known_answers(void)
{
    bool saw_phtab_outside_load = false;
    bool saw_entry_outside_load = false;
    bool saw_entry_memory_only = false;
    bool saw_pt_phdr_inconsistent = false;
    bool saw_overlay_policy = false;
    bool saw_nonzero_gap = false;
    bool saw_accepted_placement = false;
    size_t count = 0;

    for (size_t i = 0; i < n00b_test_elf_case_count; i++) {
        const n00b_test_elf_case_t *test_case = &n00b_test_elf_cases[i];

        if (!n00b_test_elf_case_has_admission(test_case)) {
            continue;
        }

        N00B_TEST_REQUIRE(test_case->expect_parse == N00B_TEST_ELF_PARSE_OK);
        check_admission_case(test_case,
                             &saw_phtab_outside_load,
                             &saw_entry_outside_load,
                             &saw_entry_memory_only,
                             &saw_pt_phdr_inconsistent,
                             &saw_overlay_policy,
                             &saw_nonzero_gap,
                             &saw_accepted_placement);
        count++;
    }

    N00B_TEST_REQUIRE(count > 0);
    N00B_TEST_REQUIRE(saw_phtab_outside_load);
    N00B_TEST_REQUIRE(saw_entry_outside_load);
    N00B_TEST_REQUIRE(saw_entry_memory_only);
    N00B_TEST_REQUIRE(saw_pt_phdr_inconsistent);
    N00B_TEST_REQUIRE(saw_overlay_policy);
    N00B_TEST_REQUIRE(saw_nonzero_gap);
    N00B_TEST_REQUIRE(saw_accepted_placement);
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
    test_divergence_classification_metadata();
    test_layout_known_answers();
    test_admission_known_answers();
    printf("All ELF known-answer tests passed.\n");
    return 0;
}
