#include <stdint.h>

#include "n00b.h"
#include "adt/option.h"
#include "adt/result.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include "compiler/objfile/bstream.h"
#include "compiler/objfile/elf.h"
#include "compiler/objfile/elf_rewrite_admit.h"
#include "compiler/objfile/elf_types.h"
#include "text/strings/string_ops.h"
#include "util/assert.h"

#include "objfile_elf_casegen.h"

#define N00B_TEST_REQUIRE(expr) n00b_require((expr), #expr)

static n00b_elf_binary_t *
parse_generated(n00b_test_elf_generator_t generator)
{
    n00b_test_elf_case_t test_case = {
        .generator = generator,
    };

    n00b_buffer_t  *buf    = n00b_test_elf_case_generate(&test_case);
    n00b_bstream_t *stream = n00b_bstream_new(buf);
    auto            parsed = n00b_elf_parse(stream);

    N00B_TEST_REQUIRE(n00b_result_is_ok(parsed));
    return n00b_result_get(parsed);
}

static n00b_elf_binary_t *
parse_buffer(n00b_buffer_t *buf)
{
    n00b_bstream_t *stream = n00b_bstream_new(buf);
    auto            parsed = n00b_elf_parse(stream);

    N00B_TEST_REQUIRE(n00b_result_is_ok(parsed));
    return n00b_result_get(parsed);
}

static n00b_elf_binary_t *
parse_valid_minimal_exec(void)
{
    return parse_buffer(n00b_test_elf_minimal_exec(0x400080,
                                                  0,
                                                  0x400000,
                                                  512,
                                                  512,
                                                  true,
                                                  false,
                                                  true,
                                                  false));
}

static n00b_elf_binary_t *
parse_valid_minimal_exec_with_overlay(void)
{
    return parse_buffer(n00b_test_elf_minimal_exec(0x400080,
                                                  0,
                                                  0x400000,
                                                  512,
                                                  512,
                                                  true,
                                                  false,
                                                  true,
                                                  true));
}

static n00b_elf_binary_t *
parse_valid_minimal_dyn(void)
{
    n00b_buffer_t *buf = n00b_test_elf_minimal_exec(0x80,
                                                    0,
                                                    0,
                                                    512,
                                                    512,
                                                    true,
                                                    false,
                                                    true,
                                                    false);
    n00b_test_elf_put16((uint8_t *)buf->data + 16, ET_DYN);
    return parse_buffer(buf);
}

static n00b_elf_rewrite_admit_metadata_request_t
default_request(void)
{
    return (n00b_elf_rewrite_admit_metadata_request_t){
        .section_name    = r".n00b.test",
        .payload_size    = 16,
        .file_alignment  = 8,
        .section_type    = SHT_PROGBITS,
        .section_flags   = 0,
        .policy          = {
            .flags = N00B_ELF_REWRITE_ADMIT_POLICY_STRICT_LOADER_PRESERVATION
                   | N00B_ELF_REWRITE_ADMIT_POLICY_PRESERVE_OVERLAY,
        },
    };
}

static void
assert_string_eq(n00b_string_t *actual, n00b_string_t *expected)
{
    N00B_TEST_REQUIRE(actual != nullptr);
    N00B_TEST_REQUIRE(expected != nullptr);
    N00B_TEST_REQUIRE(n00b_unicode_str_eq(actual, expected));
}

static void
assert_non_empty(n00b_string_t *s)
{
    N00B_TEST_REQUIRE(s != nullptr);
    N00B_TEST_REQUIRE(s->u8_bytes > 0);
}

static void
assert_admit_err(n00b_result_t(n00b_elf_rewrite_admit_result_t) result,
                 n00b_err_t                                    err)
{
    N00B_TEST_REQUIRE(n00b_result_is_err(result));
    N00B_TEST_REQUIRE(n00b_result_get_err(result) == err);
}

static n00b_elf_rewrite_admit_result_t
require_accepted_result(
    n00b_result_t(n00b_elf_rewrite_admit_result_t) result)
{
    N00B_TEST_REQUIRE(n00b_result_is_ok(result));
    n00b_elf_rewrite_admit_result_t admit = n00b_result_get(result);

    N00B_TEST_REQUIRE(admit.outcome
                      == N00B_ELF_REWRITE_ADMIT_OUTCOME_ACCEPTED);
    N00B_TEST_REQUIRE(admit.rejection_reason
                      == N00B_ELF_REWRITE_ADMIT_REJECT_NONE);
    N00B_TEST_REQUIRE(n00b_option_is_set(admit.placement));
    return admit;
}

static n00b_elf_rewrite_admit_result_t
require_rejected_result(
    n00b_result_t(n00b_elf_rewrite_admit_result_t) result,
    n00b_elf_rewrite_admit_rejection_reason_t      reason)
{
    N00B_TEST_REQUIRE(n00b_result_is_ok(result));
    n00b_elf_rewrite_admit_result_t admit = n00b_result_get(result);

    N00B_TEST_REQUIRE(admit.outcome
                      == N00B_ELF_REWRITE_ADMIT_OUTCOME_REJECTED);
    N00B_TEST_REQUIRE(admit.rejection_reason == reason);
    N00B_TEST_REQUIRE(!n00b_option_is_set(admit.placement));
    return admit;
}

static void
test_valid_minimal_admission(void)
{
    n00b_elf_binary_t *bin = parse_valid_minimal_exec();
    n00b_elf_rewrite_admit_metadata_request_t request = default_request();

    n00b_elf_rewrite_admit_result_t admit =
        require_accepted_result(
            n00b_elf_rewrite_admit_metadata_insert(bin, &request));
    n00b_elf_rewrite_admit_placement_t placement =
        n00b_option_get(admit.placement);

    N00B_TEST_REQUIRE(admit.file_size == 512);
    N00B_TEST_REQUIRE(admit.effective_alignment == 8);
    N00B_TEST_REQUIRE(placement.kind
                      == N00B_ELF_REWRITE_ADMIT_PLACEMENT_EOF_TAIL);
    N00B_TEST_REQUIRE(placement.file_offset == 512);
    N00B_TEST_REQUIRE(placement.file_end == 528);
    N00B_TEST_REQUIRE(placement.payload_size == 16);
    N00B_TEST_REQUIRE(placement.file_alignment == 8);
}

static void
test_valid_dyn_admission(void)
{
    n00b_elf_binary_t *bin = parse_valid_minimal_dyn();
    n00b_elf_rewrite_admit_metadata_request_t request = default_request();

    n00b_elf_rewrite_admit_result_t admit =
        require_accepted_result(
            n00b_elf_rewrite_admit_metadata_insert(bin, &request));
    n00b_elf_rewrite_admit_placement_t placement =
        n00b_option_get(admit.placement);

    N00B_TEST_REQUIRE(bin->header.type == ET_DYN);
    N00B_TEST_REQUIRE(placement.kind
                      == N00B_ELF_REWRITE_ADMIT_PLACEMENT_EOF_TAIL);
    N00B_TEST_REQUIRE(placement.file_offset == 512);
    N00B_TEST_REQUIRE(placement.file_end == 528);
}

static void
test_stringifiers(void)
{
    assert_non_empty(
        n00b_elf_rewrite_admit_err_str(
            N00B_ELF_REWRITE_ADMIT_ERR_NULL_BINARY));
    assert_non_empty(
        n00b_elf_rewrite_admit_err_str(
            N00B_ELF_REWRITE_ADMIT_ERR_NULL_REQUEST));
    assert_non_empty(
        n00b_elf_rewrite_admit_err_str(
            N00B_ELF_REWRITE_ADMIT_ERR_NULL_SECTION_NAME));
    assert_non_empty(
        n00b_elf_rewrite_admit_err_str(
            N00B_ELF_REWRITE_ADMIT_ERR_ZERO_PAYLOAD_SIZE));
    assert_non_empty(
        n00b_elf_rewrite_admit_err_str(
            N00B_ELF_REWRITE_ADMIT_ERR_LAYOUT_SUBSTRATE));
    assert_non_empty(
        n00b_elf_rewrite_admit_err_str(
            N00B_ELF_REWRITE_ADMIT_ERR_OVERFLOW));
    assert_string_eq(n00b_elf_rewrite_admit_err_str(-1),
                     r"ELF rewrite admission: unknown error code");

    assert_non_empty(
        n00b_elf_rewrite_admit_policy_flag_str(
            N00B_ELF_REWRITE_ADMIT_POLICY_NONE));
    assert_non_empty(
        n00b_elf_rewrite_admit_policy_flag_str(
            N00B_ELF_REWRITE_ADMIT_POLICY_STRICT_LOADER_PRESERVATION));
    assert_non_empty(
        n00b_elf_rewrite_admit_policy_flag_str(
            N00B_ELF_REWRITE_ADMIT_POLICY_PRESERVE_OVERLAY));
    assert_non_empty(
        n00b_elf_rewrite_admit_policy_flag_str(
            N00B_ELF_REWRITE_ADMIT_POLICY_APPEND_AFTER_OVERLAY));
    assert_string_eq(
        n00b_elf_rewrite_admit_policy_flag_str(
            (n00b_elf_rewrite_admit_policy_flag_t)UINT32_MAX),
        r"unknown-elf-rewrite-admit-policy-flag");

    assert_string_eq(
        n00b_elf_rewrite_admit_outcome_str(
            N00B_ELF_REWRITE_ADMIT_OUTCOME_ACCEPTED),
        r"accepted");
    assert_string_eq(
        n00b_elf_rewrite_admit_outcome_str(
            N00B_ELF_REWRITE_ADMIT_OUTCOME_REJECTED),
        r"rejected");
    assert_string_eq(
        n00b_elf_rewrite_admit_outcome_str(
            (n00b_elf_rewrite_admit_outcome_t)UINT32_MAX),
        r"unknown-elf-rewrite-admit-outcome");

    assert_non_empty(
        n00b_elf_rewrite_admit_placement_kind_str(
            N00B_ELF_REWRITE_ADMIT_PLACEMENT_NONE));
    assert_non_empty(
        n00b_elf_rewrite_admit_placement_kind_str(
            N00B_ELF_REWRITE_ADMIT_PLACEMENT_EOF_TAIL));
    assert_non_empty(
        n00b_elf_rewrite_admit_placement_kind_str(
            N00B_ELF_REWRITE_ADMIT_PLACEMENT_FILE_GAP));
    assert_non_empty(
        n00b_elf_rewrite_admit_placement_kind_str(
            N00B_ELF_REWRITE_ADMIT_PLACEMENT_AFTER_OVERLAY));
    assert_string_eq(
        n00b_elf_rewrite_admit_placement_kind_str(
            (n00b_elf_rewrite_admit_placement_kind_t)UINT32_MAX),
        r"unknown-elf-rewrite-admit-placement-kind");

    assert_non_empty(
        n00b_elf_rewrite_admit_rejection_reason_str(
            N00B_ELF_REWRITE_ADMIT_REJECT_NONE));
    assert_non_empty(
        n00b_elf_rewrite_admit_rejection_reason_str(
            N00B_ELF_REWRITE_ADMIT_REJECT_NOT_YET_CHECKED));
    assert_non_empty(
        n00b_elf_rewrite_admit_rejection_reason_str(
            N00B_ELF_REWRITE_ADMIT_REJECT_RESERVED_SECTION_NAME));
    assert_non_empty(
        n00b_elf_rewrite_admit_rejection_reason_str(
            N00B_ELF_REWRITE_ADMIT_REJECT_SECTION_NOT_METADATA));
    assert_non_empty(
        n00b_elf_rewrite_admit_rejection_reason_str(
            N00B_ELF_REWRITE_ADMIT_REJECT_NO_SAFE_PLACEMENT));
    assert_non_empty(
        n00b_elf_rewrite_admit_rejection_reason_str(
            N00B_ELF_REWRITE_ADMIT_REJECT_FILE_COLLISION));
    assert_non_empty(
        n00b_elf_rewrite_admit_rejection_reason_str(
            N00B_ELF_REWRITE_ADMIT_REJECT_UNKNOWN_NONZERO_BYTES));
    assert_non_empty(
        n00b_elf_rewrite_admit_rejection_reason_str(
            N00B_ELF_REWRITE_ADMIT_REJECT_OVERLAY_POLICY));
    assert_non_empty(
        n00b_elf_rewrite_admit_rejection_reason_str(
            N00B_ELF_REWRITE_ADMIT_REJECT_PHTAB_OUTSIDE_LOAD));
    assert_non_empty(
        n00b_elf_rewrite_admit_rejection_reason_str(
            N00B_ELF_REWRITE_ADMIT_REJECT_PT_PHDR_MISSING));
    assert_non_empty(
        n00b_elf_rewrite_admit_rejection_reason_str(
            N00B_ELF_REWRITE_ADMIT_REJECT_PT_PHDR_INCONSISTENT));
    assert_non_empty(
        n00b_elf_rewrite_admit_rejection_reason_str(
            N00B_ELF_REWRITE_ADMIT_REJECT_ENTRY_OUTSIDE_LOAD));
    assert_non_empty(
        n00b_elf_rewrite_admit_rejection_reason_str(
            N00B_ELF_REWRITE_ADMIT_REJECT_ENTRY_MEMORY_ONLY));
    assert_non_empty(
        n00b_elf_rewrite_admit_rejection_reason_str(
            N00B_ELF_REWRITE_ADMIT_REJECT_LOADER_PRESERVATION));
    assert_string_eq(
        n00b_elf_rewrite_admit_rejection_reason_str(
            (n00b_elf_rewrite_admit_rejection_reason_t)UINT32_MAX),
        r"unknown-elf-rewrite-admit-rejection-reason");

}

static void
test_invalid_inputs(void)
{
    n00b_elf_binary_t *bin = parse_valid_minimal_exec();
    n00b_elf_rewrite_admit_metadata_request_t request = default_request();

    assert_admit_err(
        n00b_elf_rewrite_admit_metadata_insert(nullptr, &request),
        N00B_ELF_REWRITE_ADMIT_ERR_NULL_BINARY);

    assert_admit_err(n00b_elf_rewrite_admit_metadata_insert(bin, nullptr),
                     N00B_ELF_REWRITE_ADMIT_ERR_NULL_REQUEST);

    request.section_name = nullptr;
    assert_admit_err(n00b_elf_rewrite_admit_metadata_insert(bin, &request),
                     N00B_ELF_REWRITE_ADMIT_ERR_NULL_SECTION_NAME);

    request = default_request();
    request.payload_size = 0;
    assert_admit_err(n00b_elf_rewrite_admit_metadata_insert(bin, &request),
                     N00B_ELF_REWRITE_ADMIT_ERR_ZERO_PAYLOAD_SIZE);

    n00b_elf_binary_t bad_bin = {};
    request = default_request();
    assert_admit_err(n00b_elf_rewrite_admit_metadata_insert(&bad_bin,
                                                            &request),
                     N00B_ELF_REWRITE_ADMIT_ERR_LAYOUT_SUBSTRATE);

}

static void
test_alignment_zero_is_byte_alignment(void)
{
    n00b_elf_binary_t *bin = parse_valid_minimal_exec();
    n00b_elf_rewrite_admit_metadata_request_t request = default_request();

    request.payload_size   = 9;
    request.file_alignment = 0;

    n00b_elf_rewrite_admit_result_t admit =
        require_accepted_result(
            n00b_elf_rewrite_admit_metadata_insert(bin, &request));
    n00b_elf_rewrite_admit_placement_t placement =
        n00b_option_get(admit.placement);

    N00B_TEST_REQUIRE(admit.file_size == 512);
    N00B_TEST_REQUIRE(admit.effective_alignment == 1);
    N00B_TEST_REQUIRE(placement.kind
                      == N00B_ELF_REWRITE_ADMIT_PLACEMENT_EOF_TAIL);
    N00B_TEST_REQUIRE(placement.file_offset == 512);
    N00B_TEST_REQUIRE(placement.file_end == 521);
    N00B_TEST_REQUIRE(placement.payload_size == 9);
    N00B_TEST_REQUIRE(placement.file_alignment == 1);

}

static void
test_no_mutation_baseline(void)
{
    n00b_elf_binary_t *bin = parse_valid_minimal_exec();
    n00b_elf_rewrite_admit_metadata_request_t request = default_request();

    uint16_t           header_shnum = bin->header.shnum;
    uint16_t           header_phnum = bin->header.phnum;
    uint64_t           header_shoff = bin->header.shoff;
    uint64_t           header_phoff = bin->header.phoff;
    uint32_t           num_sections = bin->num_sections;
    uint32_t           num_segments = bin->num_segments;
    n00b_elf_section_t *sections    = bin->sections;
    n00b_elf_segment_t *segments    = bin->segments;
    n00b_buffer_t      *overlay     = bin->overlay;
    size_t             file_size    = n00b_buffer_len(bin->stream->buf);
    uint64_t           shstr_offset = bin->sections[1].offset;
    uint64_t           shstr_size   = bin->sections[1].size;
    uint64_t           load_filesz  = bin->segments[0].filesz;

    n00b_elf_rewrite_admit_result_t admit =
        require_accepted_result(
            n00b_elf_rewrite_admit_metadata_insert(bin, &request));
    n00b_elf_rewrite_admit_placement_t placement =
        n00b_option_get(admit.placement);

    N00B_TEST_REQUIRE(placement.kind
                      == N00B_ELF_REWRITE_ADMIT_PLACEMENT_EOF_TAIL);
    N00B_TEST_REQUIRE(placement.file_offset == 512);
    N00B_TEST_REQUIRE(placement.file_end == 528);
    N00B_TEST_REQUIRE(admit.file_size == file_size);
    N00B_TEST_REQUIRE(admit.policy.flags == request.policy.flags);

    N00B_TEST_REQUIRE(bin->header.shnum == header_shnum);
    N00B_TEST_REQUIRE(bin->header.phnum == header_phnum);
    N00B_TEST_REQUIRE(bin->header.shoff == header_shoff);
    N00B_TEST_REQUIRE(bin->header.phoff == header_phoff);
    N00B_TEST_REQUIRE(bin->num_sections == num_sections);
    N00B_TEST_REQUIRE(bin->num_segments == num_segments);
    N00B_TEST_REQUIRE(bin->sections == sections);
    N00B_TEST_REQUIRE(bin->segments == segments);
    N00B_TEST_REQUIRE(bin->overlay == overlay);
    N00B_TEST_REQUIRE(n00b_buffer_len(bin->stream->buf) == file_size);
    N00B_TEST_REQUIRE(bin->sections[1].offset == shstr_offset);
    N00B_TEST_REQUIRE(bin->sections[1].size == shstr_size);
    N00B_TEST_REQUIRE(bin->segments[0].filesz == load_filesz);

}

static void
test_loader_preservation_rejections(void)
{
    n00b_elf_rewrite_admit_metadata_request_t request = default_request();

    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(
            parse_generated(N00B_TEST_ELF_GEN_VALID_MINIMAL_EXEC),
            &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_PT_PHDR_MISSING);

    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(
            parse_generated(N00B_TEST_ELF_GEN_PHTAB_NOT_IN_LOAD),
            &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_PHTAB_OUTSIDE_LOAD);

    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(
            parse_buffer(n00b_test_elf_minimal_exec(0x500000,
                                                    0,
                                                    0x400000,
                                                    512,
                                                    512,
                                                    true,
                                                    false,
                                                    true,
                                                    false)),
            &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_ENTRY_OUTSIDE_LOAD);

    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(
            parse_buffer(n00b_test_elf_minimal_exec(0x400101,
                                                    0,
                                                    0x400000,
                                                    0x100,
                                                    0x2000,
                                                    true,
                                                    false,
                                                    true,
                                                    false)),
            &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_ENTRY_MEMORY_ONLY);

    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(
            parse_generated(N00B_TEST_ELF_GEN_PT_PHDR_BAD_SIZE),
            &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_PT_PHDR_INCONSISTENT);
}

static void
test_overlay_policy(void)
{
    n00b_elf_rewrite_admit_metadata_request_t request = default_request();

    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(
            parse_valid_minimal_exec_with_overlay(),
            &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_OVERLAY_POLICY);

    request.policy.flags |= N00B_ELF_REWRITE_ADMIT_POLICY_APPEND_AFTER_OVERLAY;
    n00b_elf_rewrite_admit_result_t admit =
        require_accepted_result(
            n00b_elf_rewrite_admit_metadata_insert(
                parse_valid_minimal_exec_with_overlay(),
                &request));
    n00b_elf_rewrite_admit_placement_t placement =
        n00b_option_get(admit.placement);

    N00B_TEST_REQUIRE(placement.kind
                      == N00B_ELF_REWRITE_ADMIT_PLACEMENT_AFTER_OVERLAY);
    N00B_TEST_REQUIRE(placement.file_offset == 528);
    N00B_TEST_REQUIRE(placement.file_end == 544);
}

static void
test_preferred_file_placement_classification(void)
{
    n00b_elf_rewrite_admit_metadata_request_t request = default_request();

    request.policy.flags = N00B_ELF_REWRITE_ADMIT_POLICY_PRESERVE_OVERLAY;
    request.preferred_file_offset = n00b_option_set(uint64_t, 288);
    request.payload_size          = 16;
    request.file_alignment        = 16;
    n00b_elf_rewrite_admit_result_t admit =
        require_accepted_result(
            n00b_elf_rewrite_admit_metadata_insert(
                parse_generated(N00B_TEST_ELF_GEN_LAYOUT_CLASSIFICATION),
                &request));
    n00b_elf_rewrite_admit_placement_t placement =
        n00b_option_get(admit.placement);
    N00B_TEST_REQUIRE(placement.kind
                      == N00B_ELF_REWRITE_ADMIT_PLACEMENT_FILE_GAP);
    N00B_TEST_REQUIRE(placement.file_offset == 288);
    N00B_TEST_REQUIRE(placement.file_end == 304);

    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(
            parse_generated(N00B_TEST_ELF_GEN_LAYOUT_NONZERO_UNKNOWN),
            &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_UNKNOWN_NONZERO_BYTES);

    request = default_request();
    request.preferred_file_offset = n00b_option_set(uint64_t, 64);
    request.payload_size          = 8;
    request.file_alignment        = 8;
    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(
            parse_valid_minimal_exec(),
            &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_FILE_COLLISION);

    request = default_request();
    request.policy.flags |= N00B_ELF_REWRITE_ADMIT_POLICY_APPEND_AFTER_OVERLAY;
    request.preferred_file_offset = n00b_option_set(uint64_t, 512);
    request.payload_size          = 8;
    request.file_alignment        = 8;
    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(
            parse_valid_minimal_exec_with_overlay(),
            &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_OVERLAY_POLICY);
}

static void
test_reserved_section_names(void)
{
    n00b_elf_rewrite_admit_metadata_request_t request = default_request();

    request.section_name = r".chalk.mark";
    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(parse_valid_minimal_exec(),
                                                &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_RESERVED_SECTION_NAME);

    request = default_request();
    request.section_name = r".chalk.free";
    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(parse_valid_minimal_exec(),
                                                &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_RESERVED_SECTION_NAME);

    request = default_request();
    request.section_name = r".0c001.guard";
    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(parse_valid_minimal_exec(),
                                                &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_RESERVED_SECTION_NAME);
}

static void
test_reserved_target_sections(void)
{
    n00b_elf_rewrite_admit_metadata_request_t request = default_request();
    n00b_elf_binary_t *bin = parse_valid_minimal_exec();

    bin->sections[1].name = r".chalk.mark";
    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(bin, &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_RESERVED_SECTION_NAME);

    bin = parse_valid_minimal_exec();
    bin->sections[1].name = r".chalk.free";
    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(bin, &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_RESERVED_SECTION_NAME);

    bin = parse_valid_minimal_exec();
    bin->sections[1].name = r".0c001.guard";
    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(bin, &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_RESERVED_SECTION_NAME);

    bin = parse_valid_minimal_exec();
    bin->sections[1].type = 0xc001;
    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(bin, &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_RESERVED_SECTION_NAME);
}

static void
test_non_metadata_requests(void)
{
    n00b_elf_rewrite_admit_metadata_request_t request = default_request();

    request.section_flags = SHF_ALLOC;
    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(parse_valid_minimal_exec(),
                                                &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_SECTION_NOT_METADATA);

    request = default_request();
    request.section_type = SHT_NOBITS;
    require_rejected_result(
        n00b_elf_rewrite_admit_metadata_insert(parse_valid_minimal_exec(),
                                                &request),
        N00B_ELF_REWRITE_ADMIT_REJECT_SECTION_NOT_METADATA);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    test_stringifiers();
    test_invalid_inputs();
    test_valid_minimal_admission();
    test_valid_dyn_admission();
    test_alignment_zero_is_byte_alignment();
    test_no_mutation_baseline();
    test_loader_preservation_rejections();
    test_overlay_policy();
    test_preferred_file_placement_classification();
    test_reserved_section_names();
    test_reserved_target_sections();
    test_non_metadata_requests();
    return 0;
}
